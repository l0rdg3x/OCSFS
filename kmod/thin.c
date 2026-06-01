// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — thin.c
 * Thin provisioning support.
 *
 * Phase 3: Lazy allocation with UNMAP/DISCARD support:
 *   - UNWRITTEN extents for fallocate preallocation
 *   - Punch hole (FALLOC_FL_PUNCH_HOLE) to free blocks in the middle of a file
 *   - DISCARD passthrough to underlying storage (SSD TRIM / SAN UNMAP)
 *   - Zero range support for efficient sparse file creation
 *
 * Thin provisioning means blocks are only allocated on first write,
 * and can be returned to the pool via punch_hole/DISCARD.
 */

#include <linux/types.h>
#include <linux/falloc.h>
#include <linux/pagemap.h>
#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * PUNCH HOLE
 *
 * Deallocate blocks in the range [offset, offset+len), creating
 * a hole in the file. File size is not changed.
 *
 * This is the core of thin provisioning reclaim — VM images that
 * have been zeroed or trimmed can return blocks to the pool.
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Zero a sub-block byte range [byte_off, byte_off+byte_len) that lies entirely
 * within ONE logical block, physically (the block stays allocated).  Used for
 * the partial edges of a punch/zero range.  Caller holds i_extent_lock.  A hole
 * or an UNWRITTEN block already reads as zero, so those are no-ops.
 */
static int ocsfs_zero_within_block(struct inode *inode, loff_t byte_off,
				   loff_t byte_len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	u64 lblk = (u64)byte_off / sbi->s_block_size;
	u32 boff = (u32)((u64)byte_off % sbi->s_block_size);
	struct ocsfs_extent ext;
	struct buffer_head *bh;
	u64 phys;
	int ret;

	if (byte_len <= 0)
		return 0;
	if (boff + byte_len > sbi->s_block_size)        /* defensive: keep in-block */
		byte_len = sbi->s_block_size - boff;

	ret = ocsfs_extent_lookup(inode, lblk, &ext);
	if (ret || ext.physical_block == 0)
		return 0;                               /* hole — already zero */
	if (ext.flags & OCSFS_EXT_UNWRITTEN)
		return 0;                               /* unwritten — reads zero */
	if (ext.flags & OCSFS_EXT_COMPRESSED) {
		ret = ocsfs_extent_decompress_for_write(inode, lblk);
		if (ret)
			return ret;
		ret = ocsfs_extent_lookup(inode, lblk, &ext);
		if (ret || ext.physical_block == 0)
			return ret;
	}
	phys = ext.physical_block + (lblk - ext.logical_block);
	bh = sb_bread(inode->i_sb, phys);
	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memset(bh->b_data + boff, 0, byte_len);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

int ocsfs_punch_hole(struct inode *inode, loff_t offset, loff_t len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 bs = sbi->s_block_size;
	loff_t pend = offset + len;
	/* Whole-block range that is FULLY inside the punch: [full_start, full_end).
	 * Partially-covered blocks at the edges stay allocated; their punched bytes
	 * are zeroed in place below (the previous code used floor()/floor() for
	 * both ends, which (a) did nothing for a sub-block punch within one block
	 * and (b) could deallocate a partially-covered edge block, losing data
	 * outside the hole). */
	u64 full_start = (u64)(offset + bs - 1) / bs;
	u64 full_end   = (u64)pend / bs;
	u64 start_block = full_start;
	u64 end_block = full_end;
	loff_t head_end = (loff_t)full_start * bs;
	loff_t tail_start = (loff_t)full_end * bs;
	int i;
	int ret = 0;

	if (len <= 0)
		return 0;

	/* Invalidate page cache for the punched region */
	truncate_pagecache_range(inode, offset, pend - 1);

	/* Zero the partial (sub-block) bytes at the head and tail. */
	mutex_lock(&oi->i_extent_lock);
	if (offset < head_end)
		ocsfs_zero_within_block(inode, offset,
					min_t(loff_t, pend, head_end) - offset);
	if (pend > tail_start && tail_start >= head_end)
		ocsfs_zero_within_block(inode, max_t(loff_t, offset, tail_start),
					pend - max_t(loff_t, offset, tail_start));
	mutex_unlock(&oi->i_extent_lock);

	if (oi->i_extent_tree_root)
		return ocsfs_extent_btree_punch_hole(inode, full_start, full_end);

	if (start_block >= end_block)
		return 0;   /* only partial bytes punched — no whole blocks to free */

	/* ALTO-N1: journal-before-free — collect blocks to free, modify extents
	 * in memory, journal the inode, THEN free.  Prevents cross-link on crash
	 * (without this, free commits to bitmap before inode flush, so crashed
	 * block can be reallocated while this inode still references it). */
	struct { u64 phys; u32 count; } deferred_frees[OCSFS_INLINE_EXTENTS + 1];
	int nfrees = 0;

	mutex_lock(&oi->i_extent_lock);

	/*
	 * Walk extents backwards (safe for removal/shrink).
	 * For each extent overlapping [start_block, end_block):
	 *   - Fully contained:    remove the extent, free blocks
	 *   - Head overlap:       shrink from front
	 *   - Tail overlap:       shrink from back
	 *   - Middle punch:       split into two extents
	 */
	for (i = oi->i_extent_count - 1; i >= 0; i--) {
		struct ocsfs_extent *e = &oi->i_extents[i];
		u64 ext_start = e->logical_block;
		u64 ext_end = e->logical_block + e->length;

		if (ext_end <= start_block || ext_start >= end_block)
			continue; /* no overlap */

		if (ext_start >= start_block && ext_end <= end_block) {
			/* Case 1: Entire extent is punched */
			u32 phys = ocsfs_ext_phys_blocks(e);

			deferred_frees[nfrees].phys  = e->physical_block;
			deferred_frees[nfrees].count = phys;
			nfrees++;
			inode->i_blocks -= (u64)phys * (sbi->s_block_size / 512);

			/* Remove from array */
			if (i + 1 < oi->i_extent_count) {
				memmove(&oi->i_extents[i],
					&oi->i_extents[i + 1],
					(oi->i_extent_count - i - 1) *
					sizeof(struct ocsfs_extent));
			}
			oi->i_extent_count--;
		} else if (e->flags & OCSFS_EXT_COMPRESSED) {
			/*
			 * Partial punch on a compressed extent: physical blocks
			 * cannot be sliced at an arbitrary logical boundary.
			 * Decompress in-place first, then re-visit this index.
			 */
			int dr = ocsfs_extent_decompress_for_write(
					inode, e->logical_block);
			if (dr) {
				ret = dr;
				break;
			}
			i++; /* re-visit after loop decrements i */
			continue;
		} else if (ext_start >= start_block && ext_start < end_block) {
			/* Case 2: Punch head of extent */
			u32 removed = (u32)(end_block - ext_start);

			deferred_frees[nfrees].phys  = e->physical_block;
			deferred_frees[nfrees].count = removed;
			nfrees++;
			inode->i_blocks -= (u64)removed *
					   (sbi->s_block_size / 512);

			e->logical_block += removed;
			e->physical_block += removed;
			e->length -= removed;
		} else if (ext_end > start_block && ext_end <= end_block) {
			/* Case 3: Punch tail of extent */
			u32 removed = (u32)(ext_end - start_block);

			deferred_frees[nfrees].phys  = e->physical_block +
						       (e->length - removed);
			deferred_frees[nfrees].count = removed;
			nfrees++;
			inode->i_blocks -= (u64)removed *
					   (sbi->s_block_size / 512);

			e->length -= removed;
		} else {
			/* Case 4: Punch hole in middle — split extent */
			u32 head_len = (u32)(start_block - ext_start);
			u32 tail_len = (u32)(ext_end - end_block);
			u64 tail_phys = e->physical_block +
					(end_block - ext_start);
			u32 removed = (u32)(end_block - start_block);

			deferred_frees[nfrees].phys  = e->physical_block + head_len;
			deferred_frees[nfrees].count = removed;
			nfrees++;
			inode->i_blocks -= (u64)removed *
					   (sbi->s_block_size / 512);

			/* Shrink current extent to head */
			e->length = head_len;

			/* Insert new extent for tail (after the hole) */
			ret = ocsfs_extent_insert(inode, end_block,
						  tail_phys, tail_len,
						  e->flags);
			if (ret) {
				/*
				 * Insert failed — deferred_free still holds the
				 * middle blocks; they will be freed below after
				 * inode flush, so no cross-link on crash.
				 */
				pr_warn("ocsfs: punch_hole split failed: "
					"inode %llu, lost %u blocks\n",
					oi->i_disk_ino, tail_len);
			}
			/* Don't adjust i_blocks for tail — it already existed */
			break; /* only one split possible per call */
		}
	}

	/* Journal inode with updated extent map BEFORE freeing blocks */
	if (nfrees > 0) {
		int fr = ocsfs_flush_inode_locked(inode, false);

		if (fr)
			pr_warn_ratelimited(
				"ocsfs: punch_hole inode flush failed (%d)\n",
				fr);
	}
	mark_inode_dirty(inode);
	mutex_unlock(&oi->i_extent_lock);

	/* Free blocks only after inode is safely on disk */
	{
		int k;

		for (k = 0; k < nfrees; k++)
			ocsfs_free_blocks(inode->i_sb,
					  deferred_frees[k].phys,
					  deferred_frees[k].count);
	}
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * ZERO RANGE
 *
 * Zero a range of the file without deallocating. If the range
 * covers allocated blocks, convert them to UNWRITTEN (reads
 * return zeroes but blocks stay allocated).
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_zero_range(struct inode *inode, loff_t offset, loff_t len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 bs = sbi->s_block_size;
	loff_t pend = offset + len;
	/* Whole blocks FULLY inside [offset, pend): [full_start, full_end).
	 * The partial edge blocks keep their data outside the range; only their
	 * in-range bytes are zeroed in place.  The previous code used
	 * floor(offset/bs) for start_block and zeroed/UNWROTE whole edge blocks,
	 * destroying the preserved prefix/suffix bytes when offset or pend fell
	 * mid-block (caught by fsx: a zero_range starting mid-block wiped the
	 * block's leading bytes). */
	u64 full_start = (u64)(offset + bs - 1) / bs;
	u64 full_end   = (u64)pend / bs;
	loff_t head_end   = (loff_t)full_start * bs;
	loff_t tail_start = (loff_t)full_end * bs;
	u16 i;

	if (len <= 0)
		return 0;

	/* Invalidate page cache for the zeroed region */
	truncate_pagecache_range(inode, offset, pend - 1);

	/* Zero the partial (sub-block) bytes at the head and tail in place,
	 * preserving the surrounding bytes (same helper the punch path uses). */
	mutex_lock(&oi->i_extent_lock);
	if (offset < head_end)
		ocsfs_zero_within_block(inode, offset,
					min_t(loff_t, pend, head_end) - offset);
	if (pend > tail_start && tail_start >= head_end)
		ocsfs_zero_within_block(inode, max_t(loff_t, offset, tail_start),
					pend - max_t(loff_t, offset, tail_start));
	mutex_unlock(&oi->i_extent_lock);

	if (oi->i_extent_tree_root) {
		if (full_start < full_end)
			return ocsfs_extent_btree_zero_range(inode, full_start,
							     full_end);
		return 0;
	}

	if (full_start >= full_end)
		return 0;   /* only partial edges — no whole blocks to zero */

	mutex_lock(&oi->i_extent_lock);

	/* Whole-block interior only: mark UNWRITTEN (reads return zeros) or
	 * physically zero the full blocks that intersect [full_start, full_end). */
	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];
		u64 ext_end = e->logical_block + e->length;

		if (ext_end <= full_start || e->logical_block >= full_end)
			continue;
		if (!e->physical_block || (e->flags & OCSFS_EXT_UNWRITTEN))
			continue;   /* hole / unwritten already reads as zero */

		if (e->logical_block >= full_start && ext_end <= full_end) {
			/* Fully inside the interior: reads return zeros. */
			e->flags = OCSFS_EXT_UNWRITTEN;
		} else if (!(e->flags & OCSFS_EXT_COMPRESSED)) {
			/* Spans the interior boundary — physically zero the full
			 * blocks that intersect [full_start, full_end). */
			u64 zs   = max(e->logical_block, full_start);
			u64 ze   = min(ext_end, full_end);
			u64 phys = e->physical_block + (zs - e->logical_block);
			u64 k;

			for (k = 0; k < ze - zs; k++) {
				struct buffer_head *zbh =
					sb_getblk(inode->i_sb, phys + k);

				if (!zbh)
					continue;
				lock_buffer(zbh);
				memset(zbh->b_data, 0, sbi->s_block_size);
				set_buffer_uptodate(zbh);
				mark_buffer_dirty(zbh);
				unlock_buffer(zbh);
				sync_dirty_buffer(zbh);
				brelse(zbh);
			}
		}
	}

	/* ALTO-N1: journal flag change before returning */
	{
		int fr = ocsfs_flush_inode_locked(inode, false);

		if (fr)
			pr_warn_ratelimited(
				"ocsfs: zero_range inode flush failed (%d)\n",
				fr);
	}
	mark_inode_dirty(inode);
	mutex_unlock(&oi->i_extent_lock);

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * FALLOCATE
 *
 * Implements the fallocate(2) system call:
 *   - Default:           preallocate blocks (UNWRITTEN)
 *   - KEEP_SIZE:         preallocate but don't change i_size
 *   - PUNCH_HOLE:        deallocate blocks in a range
 *   - ZERO_RANGE:        zero a range (keep blocks allocated)
 *
 * This is the key thin provisioning API for VM disk images.
 * ═══════════════════════════════════════════════════════════════ */

long ocsfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	int ret = 0;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
		     FALLOC_FL_ZERO_RANGE))
		return -EOPNOTSUPP;

	if ((mode & FALLOC_FL_PUNCH_HOLE) && !(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	inode_lock(inode);

	/*
	 * All three operations (punch_hole, zero_range, prealloc) modify the
	 * inode's extent map. In clustered mode, hold DLM EX across the entire
	 * operation and flush before releasing so other nodes see a coherent
	 * extent map immediately after we release EX.
	 */
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			goto out_inode_unlock;
	}

	/* Punch and zero_range change the data visible through the extent map
	 * (data -> hole/zeros).  Flush any dirty pages in the range first so the
	 * on-disk extent work is consistent, then (after the op succeeds, in
	 * out:) drop the now-stale clean pages so a buffered read reflects the
	 * new state instead of returning pre-punch/zero cached data.  The
	 * clustered lr_inv_lo/hi mechanism only covers *peer* nodes; the local
	 * page cache must be invalidated here too (caught by fsx). */
	if (mode & (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE))
		filemap_write_and_wait_range(inode->i_mapping, offset,
					     offset + len - 1);

	if (mode & FALLOC_FL_PUNCH_HOLE) {
		ret = ocsfs_punch_hole(inode, offset, len);
		goto out;
	}

	if (mode & FALLOC_FL_ZERO_RANGE) {
		ret = ocsfs_zero_range(inode, offset, len);
		if (ret)
			goto out;
		if (!(mode & FALLOC_FL_KEEP_SIZE) &&
		    offset + len > inode->i_size) {
			i_size_write(inode, offset + len);
			mark_inode_dirty(inode);
		}
		goto out;
	}

	ret = ocsfs_prealloc_blocks(inode, offset, len);
	if (ret)
		goto out;

	if (!(mode & FALLOC_FL_KEEP_SIZE) && offset + len > inode->i_size) {
		i_size_write(inode, offset + len);
		mark_inode_dirty(inode);
	}

	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);

out:
	/* Drop stale clean pages over the punched/zeroed range on the local
	 * mapping; subsequent buffered reads repopulate from the updated extent
	 * map (hole -> zeros, or the zeroed blocks). */
	if (ret == 0 && (mode & (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE))) {
		filemap_invalidate_lock(inode->i_mapping);
		invalidate_inode_pages2_range(inode->i_mapping,
			offset >> PAGE_SHIFT,
			(offset + len - 1) >> PAGE_SHIFT);
		filemap_invalidate_unlock(inode->i_mapping);
	}

	if (sbi->s_clustered) {
		if (ret == 0) {
			int fr;

			/* ALTO-V3-2: record the modified block range so the next SH
			 * acquirer can do selective page cache invalidation (ARCH-7). */
			oi->i_lock_res.lr_inv_lo = (u64)(offset / sbi->s_block_size);
			oi->i_lock_res.lr_inv_hi = (u64)((offset + len +
							   sbi->s_block_size - 1) /
							  sbi->s_block_size);

			fr = ocsfs_flush_inode_locked(inode, true);
			if (fr)
				pr_warn_ratelimited(
					"ocsfs: fallocate inode flush failed (%d)\n", fr);
		}
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	}
out_inode_unlock:
	inode_unlock(inode);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * DISCARD / UNMAP PASSTHROUGH
 *
 * When blocks are freed (via punch_hole, truncate, or unlink),
 * issue a discard request to the underlying block device so the
 * SAN/SSD can reclaim the physical space.
 *
 * This is called from ocsfs_free_blocks() in bitmap.c if the
 * block device supports discard.
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_discard_blocks(struct super_block *sb, u64 block, u32 count)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	sector_t start_sector;
	sector_t nr_sectors;

	/* Check if the device supports discard */
	if (!bdev_max_discard_sectors(sb->s_bdev))
		return 0; /* silently skip — not an error */

	start_sector = block * (sbi->s_block_size / 512);
	nr_sectors = (sector_t)count * (sbi->s_block_size / 512);

	return blkdev_issue_discard(sb->s_bdev, start_sector, nr_sectors,
				    GFP_NOFS);
}

/* ═══════════════════════════════════════════════════════════════
 * THIN PROVISIONING REPORTING
 *
 * Helper to calculate actual (written) vs reserved (preallocated)
 * block counts for an inode.
 * ═══════════════════════════════════════════════════════════════ */

struct thin_stats_ctx { u64 written; u64 unwritten; };

static int thin_stats_iter(u64 logical, u64 physical, u32 length,
			   u16 flags, void *ctx)
{
	struct thin_stats_ctx *ts = ctx;

	(void)logical; (void)physical;
	if (flags & OCSFS_EXT_UNWRITTEN)
		ts->unwritten += length;
	else
		ts->written += length;
	return 0;
}

void ocsfs_thin_stats(struct inode *inode, u64 *written, u64 *unwritten)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 w = 0, u = 0;
	u16 i;

	mutex_lock(&oi->i_extent_lock);

	if (oi->i_extent_tree_root) {
		struct thin_stats_ctx ts = {};

		ocsfs_extent_btree_iterate(inode, thin_stats_iter, &ts);
		mutex_unlock(&oi->i_extent_lock);
		*written   = ts.written;
		*unwritten = ts.unwritten;
		return;
	}

	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];

		if (e->flags & OCSFS_EXT_UNWRITTEN)
			u += e->length;
		else
			w += e->length;
	}

	mutex_unlock(&oi->i_extent_lock);

	*written = w;
	*unwritten = u;
}
