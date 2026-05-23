// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — iomap.c
 * iomap-based I/O for the data path.
 *
 * Phase 3: Replace buffer_head-based I/O with iomap for:
 *   - Direct I/O (O_DIRECT) — zero-copy between userspace and block device
 *   - Buffered I/O — iomap-based page cache management
 *   - Readahead — iomap-driven readahead for sequential patterns
 *
 * iomap is the modern Linux VFS I/O path (used by XFS, ext4, btrfs).
 * It maps file logical offsets to physical device offsets and lets
 * the VFS handle the actual I/O submission.
 */

#include "ocsfs.h"
#include <linux/iomap.h>

/* OCSFS_MIN_PREALLOC_BLOCKS defined in ocsfs.h */

/* ═══════════════════════════════════════════════════════════════
 * IOMAP BEGIN / END CALLBACKS
 *
 * These are the core iomap callbacks. They translate file offsets
 * into device offsets using the extent map.
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_iomap_begin(struct inode *inode, loff_t pos, loff_t length,
			     unsigned flags, struct iomap *iomap,
			     struct iomap *srcmap)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_extent ext;
	u64 logical_block = pos / sbi->s_block_size;
	u64 end_block;
	int ret;

	mutex_lock(&oi->i_extent_lock);

	ret = ocsfs_extent_lookup(inode, logical_block, &ext);

	if (ret == 0 && ext.physical_block != 0) {
		/* Found an existing extent */
		u64 offset_in_ext = logical_block - ext.logical_block;
		u64 remaining_blocks = ext.length - offset_in_ext;
		loff_t mapped_len;

		/*
		 * CoW: if writing to a shared extent (refcount > 1 from a
		 * snapshot), copy the blocks before mapping them for write.
		 * Caller (ocsfs_file_write_iter) holds DLM EX; we hold
		 * i_extent_lock — satisfies ocsfs_cow_extent's contract.
		 * Re-lookup after CoW since the extent map changed.
		 */
		if ((flags & IOMAP_WRITE) &&
		    !(ext.flags & OCSFS_EXT_UNWRITTEN) &&
		    ocsfs_needs_cow(inode->i_sb, ext.physical_block)) {
			u32 cow_blocks = min_t(u32, (u32)remaining_blocks,
				(u32)((length + sbi->s_block_size - 1) /
				      sbi->s_block_size));

			ret = ocsfs_cow_extent(inode, logical_block, cow_blocks);
			if (ret) {
				mutex_unlock(&oi->i_extent_lock);
				return ret;
			}
			ret = ocsfs_extent_lookup(inode, logical_block, &ext);
			if (ret || ext.physical_block == 0) {
				mutex_unlock(&oi->i_extent_lock);
				return ret ? ret : -EIO;
			}
			offset_in_ext    = logical_block - ext.logical_block;
			remaining_blocks = ext.length - offset_in_ext;
		}

		mapped_len = (loff_t)remaining_blocks * sbi->s_block_size;
		iomap->addr = (ext.physical_block + offset_in_ext) *
			      (u64)sbi->s_block_size;
		iomap->length = min_t(loff_t, length, mapped_len);
		iomap->bdev = inode->i_sb->s_bdev;
		iomap->offset = pos;

		if (ext.flags & OCSFS_EXT_UNWRITTEN) {
			iomap->type = IOMAP_UNWRITTEN;

			/* For writes: convert UNWRITTEN → WRITTEN */
			if (flags & IOMAP_WRITE) {
				ocsfs_extent_convert_unwritten(inode,
					ext.logical_block + offset_in_ext,
					min_t(u32, remaining_blocks,
					      (length + sbi->s_block_size - 1) /
					      sbi->s_block_size));
			}
		} else {
			iomap->type = IOMAP_MAPPED;
		}

		mutex_unlock(&oi->i_extent_lock);
		return 0;
	}

	/* No mapping exists */
	if (!(flags & IOMAP_WRITE)) {
		/* Read from hole — return zeroes */
		iomap->type = IOMAP_HOLE;
		iomap->addr = IOMAP_NULL_ADDR;
		iomap->offset = pos;

		/* Calculate hole length to next extent or end of file */
		end_block = (inode->i_size + sbi->s_block_size - 1) /
			    sbi->s_block_size;
		iomap->length = min_t(loff_t, length,
				      (loff_t)(end_block - logical_block) *
				      sbi->s_block_size);
		if (iomap->length <= 0)
			iomap->length = sbi->s_block_size;

		mutex_unlock(&oi->i_extent_lock);
		return 0;
	}

	/* Write to unallocated region — allocate blocks */
	{
		u64 phys;
		u32 alloc_blocks;
		u32 write_blocks, try_blocks;

		/*
		 * Staged fallback: try OCSFS_MIN_PREALLOC_BLOCKS first to
		 * amortize txn overhead across many small writes (e.g. VM I/O).
		 * If pre-alloc fails, retry with the exact write size, then
		 * with a single block as last resort.
		 */
		write_blocks = max_t(u32,
			(length + sbi->s_block_size - 1) / sbi->s_block_size,
			1);
		try_blocks = max_t(u32, write_blocks, OCSFS_MIN_PREALLOC_BLOCKS);

		ret = ocsfs_alloc_blocks(inode->i_sb, oi->i_ag,
					 try_blocks, &phys);
		if (ret && try_blocks > write_blocks) {
			try_blocks = write_blocks;
			ret = ocsfs_alloc_blocks(inode->i_sb, oi->i_ag,
						 try_blocks, &phys);
		}
		if (ret && try_blocks > 1) {
			try_blocks = 1;
			ret = ocsfs_alloc_blocks(inode->i_sb, oi->i_ag,
						 1, &phys);
		}
		if (ret) {
			mutex_unlock(&oi->i_extent_lock);
			return ret;
		}

		alloc_blocks = try_blocks;

		ret = ocsfs_extent_insert(inode, logical_block, phys,
					  alloc_blocks, OCSFS_EXT_WRITTEN);
		if (ret) {
			ocsfs_free_blocks(inode->i_sb, phys, alloc_blocks);
			mutex_unlock(&oi->i_extent_lock);
			return ret;
		}

		inode->i_blocks += (u64)alloc_blocks *
				   (sbi->s_block_size / 512);

		iomap->addr = phys * (u64)sbi->s_block_size;
		iomap->length = (loff_t)alloc_blocks * sbi->s_block_size;
		iomap->type = IOMAP_MAPPED;
		iomap->flags |= IOMAP_F_NEW;
		iomap->bdev = inode->i_sb->s_bdev;
		iomap->offset = pos;
	}

	mutex_unlock(&oi->i_extent_lock);
	return 0;
}

static int ocsfs_iomap_end(struct inode *inode, loff_t pos, loff_t length,
			   ssize_t written, unsigned flags,
			   struct iomap *iomap)
{
	/* Update file size if we wrote past the end */
	if ((flags & IOMAP_WRITE) && written > 0) {
		loff_t new_size = pos + written;

		if (new_size > inode->i_size) {
			i_size_write(inode, new_size);
			mark_inode_dirty(inode);
		}
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * IOMAP OPERATIONS TABLES
 * ═══════════════════════════════════════════════════════════════ */

const struct iomap_ops ocsfs_iomap_ops = {
	.iomap_begin    = ocsfs_iomap_begin,
	.iomap_end      = ocsfs_iomap_end,
};

/* Direct I/O uses the same iomap ops */
const struct iomap_ops ocsfs_dio_iomap_ops = {
	.iomap_begin    = ocsfs_iomap_begin,
	.iomap_end      = ocsfs_iomap_end,
};

/* ═══════════════════════════════════════════════════════════════
 * IOMAP-BASED FILE OPERATIONS
 *
 * These replace the buffer_head-based read_iter/write_iter when
 * iomap is available (all modern kernels).
 * ═══════════════════════════════════════════════════════════════ */

ssize_t ocsfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	ssize_t ret;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_SH);
		if (ret)
			return ret;
		ocsfs_inode_refresh(inode);
		invalidate_inode_pages2(inode->i_mapping);
	}

	if (iocb->ki_flags & IOCB_DIRECT)
		ret = iomap_dio_rw(iocb, to, &ocsfs_dio_iomap_ops,
				   NULL, 0, NULL, 0);
	else
		ret = filemap_read(iocb, to, 0);

	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);

	return ret;
}

ssize_t ocsfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	ssize_t ret;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;
		/*
		 * Invalidate the page cache so we start with clean pages.
		 * Another node may have written to this file since our last
		 * access; dropping stale pages prevents us from writing
		 * outdated data or exposing stale reads post-write.
		 */
		invalidate_inode_pages2(inode->i_mapping);
	}

	inode_lock(inode);

	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	if (iocb->ki_flags & IOCB_DIRECT) {
		ret = iomap_dio_rw(iocb, from, &ocsfs_dio_iomap_ops,
				   NULL, 0, NULL, 0);
	} else {
		/*
		 * iomap_file_buffered_write (kernel >= 6.0, 5-arg form) updates
		 * iocb->ki_pos internally before returning. Do NOT add ret here
		 * again — that would double-advance the file position.
		 */
		ret = iomap_file_buffered_write(iocb, from,
						&ocsfs_iomap_ops, NULL, NULL);
	}

out:
	inode_unlock(inode);

	if (ret > 0) {
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
		mark_inode_dirty(inode);
	}

	if (sbi->s_clustered) {
		/*
		 * Flush dirty pages to the block device BEFORE releasing EX.
		 * Without this, another node that acquires SH immediately after
		 * our release would call invalidate_inode_pages2 and then read
		 * from the block device, seeing the pre-write data because our
		 * pages haven't been written back yet.
		 *
		 * Buffered writes only: direct I/O already bypasses the page
		 * cache and writes synchronously to the block device.
		 */
		if (ret > 0 && !(iocb->ki_flags & IOCB_DIRECT))
			filemap_write_and_wait(inode->i_mapping);

		/*
		 * Flush inode metadata (i_size, mtime, updated extent map from
		 * any CoW that occurred) before releasing EX.  Another node's
		 * ocsfs_iget reads the inode from disk after acquiring SH; if
		 * the updated extent map is not on disk yet it will see stale
		 * block mappings and silently read wrong data.
		 */
		if (ret > 0) {
			int fr = ocsfs_flush_inode_locked(inode, true);
			if (fr)
				pr_warn_ratelimited(
					"ocsfs: write_iter inode flush failed (%d)\n",
					fr);
		}

		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	}

	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * IOMAP-BASED ADDRESS SPACE OPERATIONS
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_iomap_read_folio(struct file *file, struct folio *folio)
{
	iomap_bio_read_folio(folio, &ocsfs_iomap_ops);
	return 0;
}

static void ocsfs_iomap_readahead(struct readahead_control *rac)
{
	iomap_bio_readahead(rac, &ocsfs_iomap_ops);
}

static sector_t ocsfs_iomap_bmap(struct address_space *mapping, sector_t bno)
{
	return iomap_bmap(mapping, bno, &ocsfs_iomap_ops);
}

/*
 * Writeback map_blocks callback: find the physical mapping for a page being
 * written back. Flags=0 (no IOMAP_WRITE) so we only look up existing extents
 * without allocating new blocks or triggering CoW. Dirty pages always have
 * blocks allocated from the original write path; holes would produce
 * IOMAP_HOLE and iomap_writepages skips them safely.
 *
 * Passing IOMAP_WRITE here would be wrong in clustered mode: writepages is
 * called by the VM without holding DLM EX, but ocsfs_cow_extent requires it.
 */
static int ocsfs_map_blocks(struct iomap_writepage_ctx *wpc,
			    struct inode *inode, loff_t pos)
{
	if (wpc->iomap.length && wpc->iomap.offset <= pos &&
	    wpc->iomap.offset + wpc->iomap.length > pos)
		return 0;
	return ocsfs_iomap_begin(inode, pos, inode->i_sb->s_blocksize,
				 0, &wpc->iomap, NULL);
}

static const struct iomap_writeback_ops ocsfs_writeback_ops = {
	.map_blocks = ocsfs_map_blocks,
};

static int ocsfs_writepages(struct address_space *mapping,
			    struct writeback_control *wbc)
{
	struct iomap_writepage_ctx wpc = {};

	return iomap_writepages(mapping, wbc, &wpc, &ocsfs_writeback_ops);
}

const struct address_space_operations ocsfs_iomap_aops = {
	.dirty_folio      = iomap_dirty_folio,
	.invalidate_folio = iomap_invalidate_folio,
	.release_folio    = iomap_release_folio,
	.read_folio       = ocsfs_iomap_read_folio,
	.readahead        = ocsfs_iomap_readahead,
	.writepages       = ocsfs_writepages,
	.bmap             = ocsfs_iomap_bmap,
};
