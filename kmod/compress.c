// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — compress.c
 * Inline compression support (LZ4 and ZSTD).
 *
 * Phase 4: Optional per-file compression.
 *
 * Design:
 *   - Compression is per-file, controlled by inode flags
 *   - Two algorithms: LZ4 (fast, default) and ZSTD (better ratio)
 *   - Compression operates on extent-sized chunks
 *   - O_DIRECT I/O bypasses compression entirely (VM data path)
 *   - Only buffered I/O is compressed (ISOs, templates, backups)
 *
 * On-disk format:
 *   Compressed extents have OCSFS_EXT_COMPRESSED flag set.
 *   The extent stores:
 *     - e_logical_block:  uncompressed logical offset (file space)
 *     - e_physical_block: on-disk location of compressed data
 *     - e_length:         number of COMPRESSED blocks on disk
 *     - e_flags:          OCSFS_EXT_COMPRESSED | algorithm ID
 *
 *   The uncompressed size is derived from the next extent's
 *   logical_block (or i_size for the last extent).
 *
 * The Linux kernel provides LZ4 and ZSTD libraries natively.
 */

#include "ocsfs.h"
#include <linux/lz4.h>
#include <linux/zstd.h>

/* ═══════════════════════════════════════════════════════════════
 * COMPRESSION ALGORITHM INTERFACE
 * ═══════════════════════════════════════════════════════════════ */

/* Compression algorithm IDs stored in upper bits of extent flags */
#define OCSFS_COMPRESS_NONE	0
#define OCSFS_COMPRESS_LZ4	1
#define OCSFS_COMPRESS_ZSTD	2

#define OCSFS_EXT_COMPRESSED	0x0004  /* extent flag: data is compressed */
#define OCSFS_EXT_COMP_ALGO_MASK 0x0018 /* bits 3-4: algorithm ID */
#define OCSFS_EXT_COMP_ALGO_SHIFT 3

static inline u8 ocsfs_ext_comp_algo(u16 flags)
{
	return (flags & OCSFS_EXT_COMP_ALGO_MASK) >> OCSFS_EXT_COMP_ALGO_SHIFT;
}

static inline u16 ocsfs_ext_set_comp_algo(u16 flags, u8 algo)
{
	flags &= ~OCSFS_EXT_COMP_ALGO_MASK;
	flags |= ((u16)algo << OCSFS_EXT_COMP_ALGO_SHIFT);
	return flags;
}

/* ═══════════════════════════════════════════════════════════════
 * LZ4 COMPRESSION
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_lz4_compress(const void *src, unsigned int src_len,
			      void *dst, unsigned int *dst_len,
			      void *workspace)
{
	int ret;

	ret = LZ4_compress_default(src, dst, src_len, *dst_len, workspace);
	if (ret <= 0)
		return -EINVAL;

	*dst_len = ret;
	return 0;
}

static int ocsfs_lz4_decompress(const void *src, unsigned int src_len,
				void *dst, unsigned int dst_len)
{
	int ret;

	ret = LZ4_decompress_safe(src, dst, src_len, dst_len);
	if (ret < 0)
		return -EINVAL;

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * ZSTD COMPRESSION
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_zstd_compress(const void *src, unsigned int src_len,
			       void *dst, unsigned int *dst_len,
			       void *workspace)
{
	size_t ret;
	zstd_cctx *cctx;
	zstd_parameters params;

	params = zstd_get_params(3, src_len); /* level 3: fast */

	cctx = zstd_init_cctx(workspace,
			       zstd_cctx_workspace_bound(&params.cParams));
	if (!cctx)
		return -ENOMEM;

	ret = zstd_compress_cctx(cctx, dst, *dst_len, src, src_len, &params);
	if (zstd_is_error(ret))
		return -EINVAL;

	*dst_len = ret;
	return 0;
}

static int ocsfs_zstd_decompress(const void *src, unsigned int src_len,
				 void *dst, unsigned int dst_len)
{
	size_t ret;
	zstd_dctx *dctx;
	void *workspace;
	size_t wksp_size;

	wksp_size = zstd_dctx_workspace_bound();
	workspace = kvmalloc(wksp_size, GFP_NOFS);
	if (!workspace)
		return -ENOMEM;

	dctx = zstd_init_dctx(workspace, wksp_size);
	if (!dctx) {
		kvfree(workspace);
		return -ENOMEM;
	}

	ret = zstd_decompress_dctx(dctx, dst, dst_len, src, src_len);
	kvfree(workspace);

	if (zstd_is_error(ret))
		return -EINVAL;

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * COMPRESSION DISPATCH
 * ═══════════════════════════════════════════════════════════════ */

/*
 * ocsfs_compress_data() — Compress a data buffer.
 *
 * @algo:     OCSFS_COMPRESS_LZ4 or OCSFS_COMPRESS_ZSTD
 * @src:      uncompressed data
 * @src_len:  length of uncompressed data
 * @dst:      output buffer (must be at least src_len bytes)
 * @dst_len:  [in] buffer size, [out] compressed size
 *
 * Returns 0 on success. If compression doesn't save space,
 * returns -EINVAL (caller should store uncompressed).
 */
int ocsfs_compress_data(u8 algo, const void *src, unsigned int src_len,
			void *dst, unsigned int *dst_len)
{
	void *workspace;
	size_t wksp_size;
	int ret;

	switch (algo) {
	case OCSFS_COMPRESS_LZ4:
		wksp_size = LZ4_MEM_COMPRESS;
		workspace = kvmalloc(wksp_size, GFP_NOFS);
		if (!workspace)
			return -ENOMEM;

		ret = ocsfs_lz4_compress(src, src_len, dst, dst_len,
					 workspace);
		kvfree(workspace);
		break;

	case OCSFS_COMPRESS_ZSTD: {
		zstd_parameters params = zstd_get_params(3, src_len);

		wksp_size = zstd_cctx_workspace_bound(&params.cParams);
		workspace = kvmalloc(wksp_size, GFP_NOFS);
		if (!workspace)
			return -ENOMEM;

		ret = ocsfs_zstd_compress(src, src_len, dst, dst_len,
					  workspace);
		kvfree(workspace);
		break;
	}

	default:
		return -EINVAL;
	}

	if (ret)
		return ret;

	/* Only use compressed version if it actually saves space */
	if (*dst_len >= src_len)
		return -EINVAL; /* compression didn't help */

	return 0;
}

/*
 * ocsfs_decompress_data() — Decompress a data buffer.
 *
 * @algo:     OCSFS_COMPRESS_LZ4 or OCSFS_COMPRESS_ZSTD
 * @src:      compressed data
 * @src_len:  length of compressed data
 * @dst:      output buffer
 * @dst_len:  expected uncompressed length
 */
int ocsfs_decompress_data(u8 algo, const void *src, unsigned int src_len,
			  void *dst, unsigned int dst_len)
{
	switch (algo) {
	case OCSFS_COMPRESS_LZ4:
		return ocsfs_lz4_decompress(src, src_len, dst, dst_len);

	case OCSFS_COMPRESS_ZSTD:
		return ocsfs_zstd_decompress(src, src_len, dst, dst_len);

	default:
		return -EINVAL;
	}
}

/* ═══════════════════════════════════════════════════════════════
 * COMPRESSED EXTENT I/O
 *
 * Read path:
 *   1. Read compressed data from disk
 *   2. Decompress into page cache
 *
 * Write path:
 *   1. Compress data from page cache
 *   2. Allocate blocks for compressed size
 *   3. Write compressed data to disk
 *   4. Update extent with COMPRESSED flag
 * ═══════════════════════════════════════════════════════════════ */

/*
 * ocsfs_compress_extent_read() — Read and decompress a compressed extent.
 *
 * @inode:   the file being read
 * @ext:     the compressed extent descriptor
 * @pages:   array of pages to fill with decompressed data
 * @nr_pages: number of pages
 *
 * Reads the compressed blocks from disk, decompresses, and fills
 * the provided pages.
 */
int ocsfs_compress_extent_read(struct inode *inode,
			       struct ocsfs_extent *ext,
			       struct page **pages, unsigned int nr_pages)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u8 algo;
	void *comp_buf;
	void *decomp_buf;
	u32 comp_size;
	u32 decomp_size;
	struct buffer_head *bh;
	u32 i;
	u32 copied;
	int ret;

	if (!(ext->flags & OCSFS_EXT_COMPRESSED))
		return -EINVAL;

	algo = ocsfs_ext_comp_algo(ext->flags);
	comp_size = ext->length * sbi->s_block_size;
	decomp_size = nr_pages * PAGE_SIZE;

	/* Allocate buffers */
	comp_buf = kvmalloc(comp_size, GFP_NOFS);
	if (!comp_buf)
		return -ENOMEM;

	decomp_buf = kvmalloc(decomp_size, GFP_NOFS);
	if (!decomp_buf) {
		kvfree(comp_buf);
		return -ENOMEM;
	}

	/* Read compressed blocks from disk */
	copied = 0;
	for (i = 0; i < ext->length && copied < comp_size; i++) {
		bh = sb_bread(sb, ext->physical_block + i);
		if (!bh) {
			ret = -EIO;
			goto out;
		}

		memcpy(comp_buf + copied, bh->b_data,
		       min_t(u32, sbi->s_block_size, comp_size - copied));
		copied += sbi->s_block_size;
		brelse(bh);
	}

	/* Decompress */
	ret = ocsfs_decompress_data(algo, comp_buf, comp_size,
				    decomp_buf, decomp_size);
	if (ret)
		goto out;

	/* Fill pages with decompressed data */
	for (i = 0; i < nr_pages; i++) {
		void *kaddr = kmap_local_page(pages[i]);

		memcpy(kaddr, decomp_buf + i * PAGE_SIZE,
		       min_t(u32, PAGE_SIZE, decomp_size - i * PAGE_SIZE));
		kunmap_local(kaddr);
		SetPageUptodate(pages[i]);
	}

	ret = 0;

out:
	kvfree(decomp_buf);
	kvfree(comp_buf);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * FILE COMPRESSION CONTROL
 *
 * Per-file compression enable/disable via inode flags.
 * ═══════════════════════════════════════════════════════════════ */

/* Inode flag bits for compression (stored in i_flags) */
#define OCSFS_IFLAG_COMPRESS_LZ4    0x0020
#define OCSFS_IFLAG_COMPRESS_ZSTD   0x0040
#define OCSFS_IFLAG_NOCOMPRESS      0x0080  /* explicitly disabled */

/*
 * ocsfs_get_compression_algo() — Get the compression algorithm for a file.
 *
 * Returns OCSFS_COMPRESS_NONE if compression is disabled.
 */
u8 ocsfs_get_compression_algo(struct inode *inode)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);

	if (oi->i_flags & OCSFS_IFLAG_NOCOMPRESS)
		return OCSFS_COMPRESS_NONE;

	if (oi->i_flags & OCSFS_IFLAG_COMPRESS_ZSTD)
		return OCSFS_COMPRESS_ZSTD;

	if (oi->i_flags & OCSFS_IFLAG_COMPRESS_LZ4)
		return OCSFS_COMPRESS_LZ4;

	return OCSFS_COMPRESS_NONE;
}

/*
 * ocsfs_set_compression() — Enable/disable compression on a file.
 *
 * @algo: OCSFS_COMPRESS_NONE, OCSFS_COMPRESS_LZ4, or OCSFS_COMPRESS_ZSTD
 */
int ocsfs_set_compression(struct inode *inode, u8 algo)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);

	/* Only regular files can be compressed */
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	/* Clear all compression flags */
	oi->i_flags &= ~(OCSFS_IFLAG_COMPRESS_LZ4 |
			  OCSFS_IFLAG_COMPRESS_ZSTD |
			  OCSFS_IFLAG_NOCOMPRESS);

	switch (algo) {
	case OCSFS_COMPRESS_NONE:
		oi->i_flags |= OCSFS_IFLAG_NOCOMPRESS;
		break;
	case OCSFS_COMPRESS_LZ4:
		oi->i_flags |= OCSFS_IFLAG_COMPRESS_LZ4;
		break;
	case OCSFS_COMPRESS_ZSTD:
		oi->i_flags |= OCSFS_IFLAG_COMPRESS_ZSTD;
		break;
	default:
		return -EINVAL;
	}

	mark_inode_dirty(inode);
	return 0;
}

/*
 * ocsfs_compress_stats() — Get compression statistics for a file.
 *
 * @disk_size:   actual on-disk blocks (compressed)
 * @logical_size: logical blocks (uncompressed)
 */
void ocsfs_compress_stats(struct inode *inode, u64 *disk_size,
			  u64 *logical_size)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	u64 disk = 0, logical = 0;
	u16 i;

	mutex_lock(&oi->i_extent_lock);

	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];

		disk += e->length;

		if (e->flags & OCSFS_EXT_COMPRESSED) {
			/*
			 * For compressed extents, the logical size is
			 * larger than the physical. We estimate based
			 * on position in the file.
			 */
			if (i + 1 < oi->i_extent_count) {
				logical += oi->i_extents[i + 1].logical_block -
					   e->logical_block;
			} else {
				u64 file_blocks = (i_size_read(inode) +
						   sbi->s_block_size - 1) /
						  sbi->s_block_size;
				logical += file_blocks - e->logical_block;
			}
		} else {
			logical += e->length;
		}
	}

	mutex_unlock(&oi->i_extent_lock);

	*disk_size = disk;
	*logical_size = logical;
}
