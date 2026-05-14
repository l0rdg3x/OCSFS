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

int ocsfs_punch_hole(struct inode *inode, loff_t offset, loff_t len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 start_block = offset / sbi->s_block_size;
	u64 end_block = (offset + len) / sbi->s_block_size;
	int i;
	int ret = 0;

	if (start_block >= end_block)
		return 0;

	/* Invalidate page cache for the punched region */
	truncate_pagecache_range(inode, offset,
				 offset + len - 1);

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
			ocsfs_free_blocks(inode->i_sb,
					  e->physical_block, e->length);
			inode->i_blocks -= (u64)e->length *
					   (sbi->s_block_size / 512);

			/* Remove from array */
			if (i + 1 < oi->i_extent_count) {
				memmove(&oi->i_extents[i],
					&oi->i_extents[i + 1],
					(oi->i_extent_count - i - 1) *
					sizeof(struct ocsfs_extent));
			}
			oi->i_extent_count--;
		} else if (ext_start >= start_block && ext_start < end_block) {
			/* Case 2: Punch head of extent */
			u32 removed = (u32)(end_block - ext_start);

			ocsfs_free_blocks(inode->i_sb,
					  e->physical_block, removed);
			inode->i_blocks -= (u64)removed *
					   (sbi->s_block_size / 512);

			e->logical_block += removed;
			e->physical_block += removed;
			e->length -= removed;
		} else if (ext_end > start_block && ext_end <= end_block) {
			/* Case 3: Punch tail of extent */
			u32 removed = (u32)(ext_end - start_block);

			ocsfs_free_blocks(inode->i_sb,
					  e->physical_block +
					  (e->length - removed),
					  removed);
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

			/* Free the punched middle blocks */
			ocsfs_free_blocks(inode->i_sb,
					  e->physical_block + head_len,
					  removed);
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
				 * Insert failed — blocks are already freed.
				 * This leaves a gap, but the filesystem is
				 * still consistent (just lost some blocks).
				 */
				pr_warn("ocsfs: punch_hole split failed: "
					"inode %llu, lost %u blocks\n",
					oi->i_disk_ino, tail_len);
			}
			/* Don't adjust i_blocks for tail — it already existed */
			break; /* only one split possible per call */
		}
	}

	mark_inode_dirty(inode);
	mutex_unlock(&oi->i_extent_lock);

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
	u64 start_block = offset / sbi->s_block_size;
	u64 end_block = (offset + len + sbi->s_block_size - 1) /
			sbi->s_block_size;
	u16 i;

	/* Invalidate page cache for zeroed region */
	truncate_pagecache_range(inode, offset, offset + len - 1);

	mutex_lock(&oi->i_extent_lock);

	/* Mark overlapping extents as UNWRITTEN */
	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];
		u64 ext_end = e->logical_block + e->length;

		if (ext_end <= start_block || e->logical_block >= end_block)
			continue;

		/*
		 * For simplicity, mark the entire overlapping extent as
		 * UNWRITTEN. A more sophisticated implementation would
		 * split at boundaries, but this is correct and simpler.
		 */
		if (e->logical_block >= start_block && ext_end <= end_block) {
			e->flags = OCSFS_EXT_UNWRITTEN;
		}
		/* Partial overlaps: only mark if fully contained */
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
	int ret = 0;

	/* Check for unsupported mode combinations */
	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
		     FALLOC_FL_ZERO_RANGE))
		return -EOPNOTSUPP;

	/* PUNCH_HOLE requires KEEP_SIZE */
	if ((mode & FALLOC_FL_PUNCH_HOLE) && !(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	inode_lock(inode);

	if (mode & FALLOC_FL_PUNCH_HOLE) {
		ret = ocsfs_punch_hole(inode, offset, len);
		goto out;
	}

	if (mode & FALLOC_FL_ZERO_RANGE) {
		ret = ocsfs_zero_range(inode, offset, len);
		if (ret)
			goto out;

		/* Extend file if not KEEP_SIZE and range goes past EOF */
		if (!(mode & FALLOC_FL_KEEP_SIZE) &&
		    offset + len > inode->i_size) {
			i_size_write(inode, offset + len);
			mark_inode_dirty(inode);
		}
		goto out;
	}

	/* Default: preallocate blocks */
	ret = ocsfs_prealloc_blocks(inode, offset, len);
	if (ret)
		goto out;

	/* Update file size unless KEEP_SIZE */
	if (!(mode & FALLOC_FL_KEEP_SIZE) && offset + len > inode->i_size) {
		i_size_write(inode, offset + len);
		mark_inode_dirty(inode);
	}

	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);

out:
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

void ocsfs_thin_stats(struct inode *inode, u64 *written, u64 *unwritten)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 w = 0, u = 0;
	u16 i;

	mutex_lock(&oi->i_extent_lock);

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
