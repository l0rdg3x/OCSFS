// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — compress_file.c
 * File-level and extent-level compression operations.
 *
 * Split from compress.c to keep both files under 500 lines.
 *
 * ocsfs_extent_decompress_for_write(): decompress before write to avoid
 *   leaving COMPRESSED flag set over raw (uncompressed) data.
 * ocsfs_compress_file(): called from fsync; lazily compresses all inline
 *   uncompressed extents using the per-file algorithm.
 */

#include "ocsfs.h"
#include <linux/lz4.h>
#include <linux/zstd.h>

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
	/* st_blocks: physical blocks grew from old_phys_blks to e->length */
	inode->i_blocks += (u64)(e->length - old_phys_blks) *
			   (sbi->s_block_size / 512);
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
		/* st_blocks: physical blocks shrank from old_len to comp_blocks */
		inode->i_blocks -= (u64)(old_len - comp_blocks) *
				   (sbi->s_block_size / 512);
		mark_inode_dirty(inode);

		/* MEDIO-V3-10: journal the inode update BEFORE freeing old blocks.
		 * This ensures that on crash we never have the inode pointing at
		 * freed (possibly reused) blocks.  On txn failure skip the free —
		 * a space leak is recoverable by fsck; corruption is not. */
		{
			bool do_free = false;
			struct ocsfs_txn *ctxn = ocsfs_txn_begin(sb);

			if (!IS_ERR(ctxn)) {
				int jr = ocsfs_flush_inode_locked(inode, true);

				if (!jr) {
					ocsfs_txn_commit(ctxn);
					do_free = true;
				} else {
					ocsfs_txn_abort(ctxn);
					pr_warn_ratelimited(
						"ocsfs: compress_file: journal failed (%d), "
						"old blocks kept (space leak)\n", jr);
				}
			} else {
				pr_warn_ratelimited(
					"ocsfs: compress_file: txn_begin failed, "
					"old blocks kept (space leak)\n");
			}
			if (do_free)
				ocsfs_free_blocks(sb, old_phys, old_len);
		}
	}
unlock:
	mutex_unlock(&oi->i_extent_lock);
	return ret;
}
