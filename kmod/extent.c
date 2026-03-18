// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — extent.c
 * Inline extent management for the kernel module.
 *
 * Phase 1: only inline extents (up to OCSFS_INLINE_EXTENTS per inode).
 * Phase 3: UNWRITTEN extent support (convert on write for thin provisioning).
 * Future: B+ tree overflow for files with many extents.
 *
 * Extents are kept sorted by logical_block. Adjacent extents are merged
 * when possible to minimize the number of entries.
 */

#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * EXTENT LOOKUP
 *
 * Find the extent covering @logical_block. If found, fills @ext_out.
 * Returns 0 on success, -ENOENT if no extent covers the block.
 * Caller must hold i_extent_lock.
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_extent_lookup(struct inode *inode, u64 logical_block,
			struct ocsfs_extent *ext_out)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u16 i;

	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];

		if (logical_block >= e->logical_block &&
		    logical_block < e->logical_block + e->length) {
			*ext_out = *e;
			return 0;
		}
	}

	/* No mapping — return a zero extent (hole) */
	memset(ext_out, 0, sizeof(*ext_out));
	return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════
 * EXTENT INSERT
 *
 * Insert a new extent [logical, logical+len) → [physical, physical+len).
 * Attempts to merge with adjacent extents.
 * Caller must hold i_extent_lock.
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_extent_insert(struct inode *inode, u64 logical, u64 physical,
			u32 len, u16 flags)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u16 i, pos;

	/* Try to merge with an existing extent */
	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];

		/* Merge at the end of extent i? */
		if (e->flags == flags &&
		    logical == e->logical_block + e->length &&
		    physical == e->physical_block + e->length) {
			e->length += len;
			goto try_merge_next;
		}

		/* Merge at the beginning of extent i? */
		if (e->flags == flags &&
		    logical + len == e->logical_block &&
		    physical + len == e->physical_block) {
			e->logical_block = logical;
			e->physical_block = physical;
			e->length += len;
			mark_inode_dirty(inode);
			return 0;
		}
	}

	/* No merge possible — insert a new extent */
	if (oi->i_extent_count >= OCSFS_INLINE_EXTENTS) {
		pr_warn("ocsfs: inode %llu: too many extents (%u)\n",
			oi->i_disk_ino, oi->i_extent_count);
		return -ENOSPC;
	}

	/* Find insertion position (keep sorted by logical_block) */
	pos = oi->i_extent_count;
	for (i = 0; i < oi->i_extent_count; i++) {
		if (logical < oi->i_extents[i].logical_block) {
			pos = i;
			break;
		}
	}

	/* Shift entries right */
	if (pos < oi->i_extent_count) {
		memmove(&oi->i_extents[pos + 1], &oi->i_extents[pos],
			(oi->i_extent_count - pos) * sizeof(struct ocsfs_extent));
	}

	oi->i_extents[pos].logical_block = logical;
	oi->i_extents[pos].physical_block = physical;
	oi->i_extents[pos].length = len;
	oi->i_extents[pos].flags = flags;
	oi->i_extent_count++;

	mark_inode_dirty(inode);
	return 0;

try_merge_next:
	/* After extending extent i at the end, see if we can absorb extent i+1 */
	if (i + 1 < oi->i_extent_count) {
		struct ocsfs_extent *next = &oi->i_extents[i + 1];
		struct ocsfs_extent *cur = &oi->i_extents[i];

		if (cur->flags == next->flags &&
		    cur->logical_block + cur->length == next->logical_block &&
		    cur->physical_block + cur->length == next->physical_block) {
			cur->length += next->length;
			/* Remove extent i+1 */
			if (i + 2 < oi->i_extent_count) {
				memmove(&oi->i_extents[i + 1],
					&oi->i_extents[i + 2],
					(oi->i_extent_count - i - 2) *
					sizeof(struct ocsfs_extent));
			}
			oi->i_extent_count--;
		}
	}

	mark_inode_dirty(inode);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * EXTENT TRUNCATE
 *
 * Remove all extents covering blocks >= @from_block.
 * Frees the underlying disk blocks.
 * Caller must hold i_extent_lock.
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_extent_truncate(struct inode *inode, u64 from_block)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	int i;

	for (i = oi->i_extent_count - 1; i >= 0; i--) {
		struct ocsfs_extent *e = &oi->i_extents[i];

		if (e->logical_block >= from_block) {
			/* Entire extent is beyond truncation point — free it */
			ocsfs_free_blocks(sb, e->physical_block, e->length);
			inode->i_blocks -= (u64)e->length *
					   (sbi->s_block_size / 512);
			oi->i_extent_count--;
		} else if (e->logical_block + e->length > from_block) {
			/* Extent partially overlaps — shrink it */
			u32 keep = (u32)(from_block - e->logical_block);
			u32 freed = e->length - keep;

			ocsfs_free_blocks(sb,
					  e->physical_block + keep,
					  freed);
			inode->i_blocks -= (u64)freed *
					   (sbi->s_block_size / 512);
			e->length = keep;
		}
	}

	mark_inode_dirty(inode);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * CONVERT UNWRITTEN → WRITTEN
 *
 * Phase 3: When a write lands on an UNWRITTEN (preallocated) extent,
 * convert the affected range to WRITTEN. This may split an extent
 * if only part of it is being written.
 *
 * Called from iomap_begin() when writing to UNWRITTEN extents.
 * Caller must hold i_extent_lock.
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_extent_convert_unwritten(struct inode *inode, u64 logical_block,
				   u32 len)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u16 i;

	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];
		u64 ext_end = e->logical_block + e->length;

		if (!(e->flags & OCSFS_EXT_UNWRITTEN))
			continue;

		/* Check if this extent overlaps the convert range */
		if (logical_block >= ext_end ||
		    logical_block + len <= e->logical_block)
			continue;

		/*
		 * Case 1: Convert the entire extent.
		 * The simple and common case — the write covers the
		 * whole UNWRITTEN extent.
		 */
		if (logical_block <= e->logical_block &&
		    logical_block + len >= ext_end) {
			e->flags = OCSFS_EXT_WRITTEN;
			mark_inode_dirty(inode);
			continue;
		}

		/*
		 * Case 2: Convert a prefix of the extent.
		 * Split into [WRITTEN prefix] + [UNWRITTEN tail].
		 */
		if (logical_block <= e->logical_block &&
		    logical_block + len < ext_end) {
			u32 cvt_len = (u32)(logical_block + len -
					    e->logical_block);
			u64 tail_logical = e->logical_block + cvt_len;
			u64 tail_physical = e->physical_block + cvt_len;
			u32 tail_len = e->length - cvt_len;

			/* Shrink current to converted prefix */
			e->length = cvt_len;
			e->flags = OCSFS_EXT_WRITTEN;

			/* Insert the UNWRITTEN tail */
			ocsfs_extent_insert(inode, tail_logical,
					    tail_physical, tail_len,
					    OCSFS_EXT_UNWRITTEN);
			mark_inode_dirty(inode);
			return 0;
		}

		/*
		 * Case 3: Convert a suffix of the extent.
		 * Split into [UNWRITTEN head] + [WRITTEN suffix].
		 */
		if (logical_block > e->logical_block &&
		    logical_block + len >= ext_end) {
			u32 head_len = (u32)(logical_block - e->logical_block);
			u64 cvt_physical = e->physical_block + head_len;
			u32 cvt_len = e->length - head_len;

			/* Shrink current to UNWRITTEN head */
			e->length = head_len;

			/* Insert the WRITTEN suffix */
			ocsfs_extent_insert(inode, logical_block,
					    cvt_physical, cvt_len,
					    OCSFS_EXT_WRITTEN);
			mark_inode_dirty(inode);
			continue;
		}

		/*
		 * Case 4: Convert the middle (3-way split).
		 * [UNWRITTEN head] + [WRITTEN middle] + [UNWRITTEN tail]
		 */
		{
			u32 head_len = (u32)(logical_block - e->logical_block);
			u32 cvt_len = len;
			u32 tail_len = (u32)(ext_end - (logical_block + len));
			u64 cvt_physical = e->physical_block + head_len;
			u64 tail_logical = logical_block + len;
			u64 tail_physical = e->physical_block + head_len +
					    cvt_len;

			/* Shrink current to UNWRITTEN head */
			e->length = head_len;

			/* Insert WRITTEN middle */
			ocsfs_extent_insert(inode, logical_block,
					    cvt_physical, cvt_len,
					    OCSFS_EXT_WRITTEN);

			/* Insert UNWRITTEN tail */
			ocsfs_extent_insert(inode, tail_logical,
					    tail_physical, tail_len,
					    OCSFS_EXT_UNWRITTEN);

			mark_inode_dirty(inode);
			return 0;
		}
	}

	mark_inode_dirty(inode);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * COUNT BLOCKS — sum of all extent lengths
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_extent_count_blocks(struct inode *inode, u64 *count)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 total = 0;
	u16 i;

	for (i = 0; i < oi->i_extent_count; i++)
		total += oi->i_extents[i].length;

	*count = total;
	return 0;
}
