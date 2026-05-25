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
 *     - e_length:         LOGICAL block count (range coverage, unchanged by compression)
 *     - e_checksum:       PHYSICAL compressed block count (reused field, formerly 0)
 *     - e_flags:          OCSFS_EXT_COMPRESSED | algorithm ID
 *
 *   In-memory: ocsfs_extent.phys_length mirrors e_checksum for COMPRESSED extents.
 *
 * The Linux kernel provides LZ4 and ZSTD libraries natively.
 */

#include "ocsfs.h"
#include <linux/lz4.h>
#include <linux/zstd.h>

/* ═══════════════════════════════════════════════════════════════
 * COMPRESSION ALGORITHM INTERFACE
 * (OCSFS_COMPRESS_*, OCSFS_EXT_COMPRESSED, and ocsfs_ext_comp_algo()
 *  are defined in ocsfs.h so that iomap.c can also use them)
 * ═══════════════════════════════════════════════════════════════ */

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
				 void *dst, unsigned int dst_len,
				 void *workspace, size_t wksp_size)
{
	zstd_dctx *dctx;
	size_t ret;

	dctx = zstd_init_dctx(workspace, wksp_size);
	if (!dctx)
		return -ENOMEM;

	ret = zstd_decompress_dctx(dctx, dst, dst_len, src, src_len);
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
int ocsfs_decompress_data(struct super_block *sb, u8 algo,
			  const void *src, unsigned int src_len,
			  void *dst, unsigned int dst_len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	int ret;

	switch (algo) {
	case OCSFS_COMPRESS_LZ4:
		return ocsfs_lz4_decompress(src, src_len, dst, dst_len);

	case OCSFS_COMPRESS_ZSTD:
		mutex_lock(&sbi->s_decompress_lock);
		if (!sbi->s_decompress_wksp) {
			size_t sz = zstd_dctx_workspace_bound();

			sbi->s_decompress_wksp = kvmalloc(sz, GFP_NOFS);
			if (!sbi->s_decompress_wksp) {
				mutex_unlock(&sbi->s_decompress_lock);
				return -ENOMEM;
			}
			sbi->s_decompress_wksp_sz = sz;
		}
		ret = ocsfs_zstd_decompress(src, src_len, dst, dst_len,
					    sbi->s_decompress_wksp,
					    sbi->s_decompress_wksp_sz);
		mutex_unlock(&sbi->s_decompress_lock);
		return ret;

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
	u32 phys_blks;
	size_t comp_size;
	size_t decomp_size;
	struct buffer_head *bh;
	u32 i;
	size_t copied;
	int ret;

	if (!(ext->flags & OCSFS_EXT_COMPRESSED))
		return -EINVAL;

	algo = ocsfs_ext_comp_algo(ext->flags);
	/* phys_length is the compressed block count; fall back to length
	 * for extents written before the phys_length field was introduced. */
	phys_blks = (ext->phys_length > 0) ? (u32)ext->phys_length : ext->length;
	comp_size = (size_t)phys_blks * sbi->s_block_size;
	decomp_size = (size_t)nr_pages * PAGE_SIZE;

	/*
	 * Cap allocation size to prevent OOM DoS via a corrupted or malicious
	 * extent descriptor with a huge length field.  A corrupt image with
	 * ext->length = 0x3FFFFF would otherwise trigger a kvmalloc of ~4 GiB,
	 * stalling the entire system.  1 MiB per extent is a generous ceiling.
	 */
	if (comp_size > (1u << 20) || decomp_size > (1u << 20))
		return -EFBIG;

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
	for (i = 0; i < phys_blks && copied < comp_size; i++) {
		bh = sb_bread(sb, ext->physical_block + i);
		if (!bh) {
			ret = -EIO;
			goto out;
		}

		memcpy(comp_buf + copied, bh->b_data,
		       min_t(size_t, sbi->s_block_size, comp_size - copied));
		copied += sbi->s_block_size;
		brelse(bh);
	}

	/* Decompress */
	ret = ocsfs_decompress_data(sb, algo, comp_buf, comp_size,
				    decomp_buf, decomp_size);
	if (ret)
		goto out;

	/* Fill pages with decompressed data */
	for (i = 0; i < nr_pages; i++) {
		void *kaddr = kmap_local_page(pages[i]);

		memcpy(kaddr, decomp_buf + i * PAGE_SIZE,
		       min_t(size_t, PAGE_SIZE, decomp_size - i * PAGE_SIZE));
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
struct compress_stats_ctx { u64 disk; };

static int compress_stats_iter(u64 logical, u64 physical, u32 length,
				u16 flags, void *ctx)
{
	struct compress_stats_ctx *cs = ctx;

	(void)logical; (void)physical; (void)flags;
	cs->disk += length;
	return 0;
}

void ocsfs_compress_stats(struct inode *inode, u64 *disk_size,
			  u64 *logical_size)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	u64 disk = 0, logical = 0;
	u16 i;

	/*
	 * For btree-backed inodes, the inline i_extents[] is empty.
	 * Use the iterate API for the physical (disk) sum; the total logical
	 * blocks equal ceil(i_size / block_size) regardless of compression.
	 */
	mutex_lock(&oi->i_extent_lock);

	if (oi->i_extent_tree_root) {
		struct compress_stats_ctx cs = {};

		ocsfs_extent_btree_iterate(inode, compress_stats_iter, &cs);
		mutex_unlock(&oi->i_extent_lock);
		*disk_size    = cs.disk;
		*logical_size = (i_size_read(inode) + sbi->s_block_size - 1) /
				sbi->s_block_size;
		return;
	}

	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];

		disk += ((e->flags & OCSFS_EXT_COMPRESSED) && e->phys_length)
			? e->phys_length : e->length;

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

/*
 * ocsfs_extent_decompress_for_write() — Decompress a compressed extent before write.
 *
 * When a write targets a compressed extent, we must decompress it first.
 * Otherwise the VFS would write raw (uncompressed) data to the compressed
 * physical blocks while keeping the COMPRESSED flag set, making the extent
 * unreadable.
 *
 * Caller holds oi->i_extent_lock.  The extent is updated in-place in
 * oi->i_extents[]; old compressed blocks are freed.  Returns 0 on success.
 * On failure the extent is left unchanged (still compressed).
 */
int ocsfs_extent_decompress_for_write(struct inode *inode, u64 logical_block)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_extent *e = NULL;
	u8 algo;
	u32 phys_blks;
	size_t comp_size, decomp_size;
	void *comp_buf, *decomp_buf;
	struct buffer_head *bh;
	u64 new_phys, old_phys;
	u32 old_phys_blks;
	u32 i, j;
	size_t copied;
	int ret;

	/* btree-backed inodes don't use compression — nothing to do */
	if (oi->i_extent_tree_root)
		return 0;

	/* Find the extent in the inline array */
	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *cur = &oi->i_extents[i];

		if (logical_block >= cur->logical_block &&
		    logical_block < cur->logical_block + cur->length) {
			e = cur;
			break;
		}
	}
	if (!e || !(e->flags & OCSFS_EXT_COMPRESSED))
		return 0; /* nothing to decompress */

	algo      = ocsfs_ext_comp_algo(e->flags);
	phys_blks = e->phys_length ? (u32)e->phys_length : e->length;
	comp_size   = (size_t)phys_blks  * sbi->s_block_size;
	decomp_size = (size_t)e->length  * sbi->s_block_size;

	if (comp_size > (1u << 20) || decomp_size > (1u << 20))
		return -EFBIG;

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
	for (j = 0; j < phys_blks && copied < comp_size; j++) {
		bh = sb_bread(sb, e->physical_block + j);
		if (!bh) {
			ret = -EIO;
			goto out;
		}
		memcpy(comp_buf + copied, bh->b_data,
		       min_t(size_t, sbi->s_block_size, comp_size - copied));
		copied += sbi->s_block_size;
		brelse(bh);
	}

	ret = ocsfs_decompress_data(sb, algo, comp_buf, comp_size,
				    decomp_buf, decomp_size);
	if (ret)
		goto out;

	/* Allocate blocks for the full uncompressed extent */
	ret = ocsfs_alloc_blocks(sb, 0, e->length, &new_phys);
	if (ret)
		goto out;

	/* Write uncompressed data to new blocks */
	for (j = 0; j < e->length; j++) {
		unsigned int off   = j * sbi->s_block_size;
		unsigned int chunk = min_t(unsigned int,
					   sbi->s_block_size, decomp_size - off);

		bh = sb_getblk(sb, new_phys + j);
		if (!bh) {
			ocsfs_free_blocks(sb, new_phys, e->length);
			ret = -EIO;
			goto out;
		}
		lock_buffer(bh);
		memcpy(bh->b_data, decomp_buf + off, chunk);
		if (chunk < sbi->s_block_size)
			memset(bh->b_data + chunk, 0, sbi->s_block_size - chunk);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}

	/* Update extent in-place: uncompressed, new physical blocks */
	old_phys      = e->physical_block;
	old_phys_blks = phys_blks;
	e->physical_block = new_phys;
	e->flags &= ~(OCSFS_EXT_COMPRESSED | OCSFS_EXT_COMP_ALGO_MASK);
	e->phys_length    = 0;
	mark_inode_dirty(inode);

	ocsfs_free_blocks(sb, old_phys, old_phys_blks);
	ret = 0;
out:
	kvfree(decomp_buf);
	kvfree(comp_buf);
	return ret;
}

/*
 * ocsfs_compress_file() — Lazily compress all uncompressed inline extents.
 *
 * Called from ocsfs_fsync(). First flushes dirty pages to disk (uncompressed),
 * then re-reads each inline extent, compresses in memory, allocates fewer
 * blocks, and updates the in-memory extent map.  If compression does not
 * reduce size the extent is left as-is.
 *
 * Btree-backed inodes are skipped: the btree encoding does not yet support
 * separate logical/physical lengths required for compressed extents.
 *
 * Key invariant: e->length always stores the LOGICAL block count (range
 * coverage) so that ocsfs_extent_lookup() range checks remain correct.
 * e->phys_length stores the physical (compressed) block count and is
 * persisted in e_checksum on disk.
 */
int ocsfs_compress_file(struct inode *inode)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u8 algo = ocsfs_get_compression_algo(inode);
	u16 i;
	int ret = 0;

	if (algo == OCSFS_COMPRESS_NONE)
		return 0;

	ret = filemap_write_and_wait(inode->i_mapping);
	if (ret)
		return ret;

	mutex_lock(&oi->i_extent_lock);
	if (oi->i_extent_tree_root)
		goto unlock; /* btree path not yet supported; data safe uncompressed */

	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];
		void *raw, *comp;
		unsigned int raw_size, comp_size;
		u32 comp_blocks, j;
		u64 new_phys, old_phys;
		u32 old_len;

		if ((e->flags & (OCSFS_EXT_COMPRESSED | OCSFS_EXT_UNWRITTEN)) ||
		    !e->physical_block)
			continue;

		raw_size = (unsigned int)e->length * sbi->s_block_size;
		if (raw_size > (1u << 20))
			continue;

		raw = kvmalloc(raw_size, GFP_NOFS);
		if (!raw) { ret = -ENOMEM; break; }

		for (j = 0; j < e->length; j++) {
			struct buffer_head *bh = sb_bread(sb, e->physical_block + j);

			if (!bh) {
				kvfree(raw);
				ret = -EIO;
				goto unlock;
			}
			memcpy(raw + (size_t)j * sbi->s_block_size,
			       bh->b_data, sbi->s_block_size);
			brelse(bh);
		}

		comp = kvmalloc(raw_size, GFP_NOFS);
		if (!comp) { kvfree(raw); ret = -ENOMEM; break; }

		comp_size = raw_size;
		if (ocsfs_compress_data(algo, raw, raw_size, comp, &comp_size) ||
		    comp_size >= raw_size) {
			kvfree(raw);
			kvfree(comp);
			continue;
		}
		kvfree(raw);

		comp_blocks = (comp_size + sbi->s_block_size - 1) / sbi->s_block_size;
		if (ocsfs_alloc_blocks(sb, 0, comp_blocks, &new_phys)) {
			kvfree(comp);
			continue;
		}

		for (j = 0; j < comp_blocks; j++) {
			struct buffer_head *bh = sb_getblk(sb, new_phys + j);
			unsigned int off   = j * sbi->s_block_size;
			unsigned int chunk = min_t(unsigned int,
						   comp_size - off,
						   sbi->s_block_size);

			if (!bh) {
				kvfree(comp);
				ocsfs_free_blocks(sb, new_phys, comp_blocks);
				ret = -EIO;
				goto unlock;
			}
			lock_buffer(bh);
			memcpy(bh->b_data, comp + off, chunk);
			if (chunk < sbi->s_block_size)
				memset(bh->b_data + chunk, 0,
				       sbi->s_block_size - chunk);
			set_buffer_uptodate(bh);
			mark_buffer_dirty(bh);
			unlock_buffer(bh);
			sync_dirty_buffer(bh);
			brelse(bh);
		}
		kvfree(comp);

		old_phys = e->physical_block;
		old_len  = e->length;
		e->physical_block = new_phys;
		e->phys_length    = (u16)comp_blocks; /* physical compressed blocks */
		/* e->length remains old_len: logical block count for range checks */
		e->flags = ocsfs_ext_set_comp_algo(
				e->flags | OCSFS_EXT_COMPRESSED, algo);
		mark_inode_dirty(inode);
		ocsfs_free_blocks(sb, old_phys, old_len);
	}
unlock:
	mutex_unlock(&oi->i_extent_lock);
	return ret;
}
