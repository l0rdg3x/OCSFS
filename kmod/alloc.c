// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — alloc.c
 * Extent preallocation and smart allocation strategies.
 *
 * Phase 3: Higher-level allocation policy on top of bitmap.c:
 *   - Multi-block contiguous allocation with configurable hints
 *   - Per-inode preallocation window (speculative allocation)
 *   - AG-local allocation affinity (keep inode data in home AG)
 *   - Goal-oriented allocation (try to extend existing extents)
 *
 * bitmap.c handles the raw bitmap scanning; this layer adds the
 * allocation strategy that makes large sequential I/O fast.
 */

#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * ALLOCATION HINTS
 *
 * The allocator uses hints to place blocks optimally:
 *   - Goal block:  try to allocate near this physical block
 *   - Home AG:     prefer the inode's home AG
 *   - Prealloc:    speculatively allocate extra blocks for sequential writes
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Default preallocation size (in blocks).
 * Sequential writes will speculatively allocate this many extra blocks
 * to reduce fragmentation from repeated small allocations.
 */
#define OCSFS_PREALLOC_MIN_BLOCKS	8
#define OCSFS_PREALLOC_MAX_BLOCKS	256
#define OCSFS_PREALLOC_DEFAULT		32

/*
 * Compute a preallocation hint based on file size and write pattern.
 * Larger files get larger preallocation windows, up to the maximum.
 */
static u32 ocsfs_prealloc_hint(struct inode *inode, u32 requested)
{
	u64 file_blocks;
	u32 hint;

	/* For O_DIRECT or small requested sizes, don't preallocate */
	if (requested >= OCSFS_PREALLOC_MAX_BLOCKS)
		return requested;

	file_blocks = inode->i_blocks / (OCSFS_SB(inode->i_sb)->s_block_size / 512);

	/*
	 * Scale preallocation with file size:
	 *   < 64 blocks:    8 block prealloc
	 *   < 256 blocks:  32 block prealloc
	 *   < 4096 blocks: 128 block prealloc
	 *   >= 4096:       256 block prealloc
	 */
	if (file_blocks < 64)
		hint = OCSFS_PREALLOC_MIN_BLOCKS;
	else if (file_blocks < 256)
		hint = OCSFS_PREALLOC_DEFAULT;
	else if (file_blocks < 4096)
		hint = 128;
	else
		hint = OCSFS_PREALLOC_MAX_BLOCKS;

	return max_t(u32, hint, requested);
}

/*
 * Try to find a goal block — the physical block just past the last
 * extent of this inode. Allocating near the goal keeps the file
 * physically contiguous on disk.
 */
static u64 ocsfs_find_goal_block(struct inode *inode)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u16 i;
	u64 best_end = 0;

	/* Find the extent with the highest logical_block */
	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];
		u64 end = e->physical_block + e->length;

		if (end > best_end)
			best_end = end;
	}

	return best_end; /* 0 means no goal */
}

/* ═══════════════════════════════════════════════════════════════
 * GOAL-ORIENTED ALLOCATION
 *
 * Tries to allocate blocks near the goal. Falls back to the
 * AG-level allocator in bitmap.c if goal-local fails.
 * ═══════════════════════════════════════════════════════════════ */

/*
 * ocsfs_alloc_near_goal() — Try to allocate @count blocks starting
 * at @goal. If the exact goal region is occupied, falls back to
 * the standard bitmap allocator.
 *
 * The goal is a physical block number; we find which AG it belongs
 * to and try that AG first, then fall back to any AG.
 */
static int ocsfs_alloc_near_goal(struct super_block *sb, u64 goal,
				 u32 ag_hint, u32 count, u64 *block_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u32 goal_ag;
	int ret;

	if (goal == 0)
		return ocsfs_alloc_blocks(sb, ag_hint, count, block_out);

	/* Determine which AG the goal block is in */
	for (goal_ag = 0; goal_ag < sbi->s_ag_count; goal_ag++) {
		struct ocsfs_ag_info *ag = &sbi->s_ags[goal_ag];

		if (goal >= ag->block_start &&
		    goal < ag->block_start + ag->block_count)
			break;
	}

	if (goal_ag >= sbi->s_ag_count)
		goal_ag = ag_hint;

	/* Try the goal AG first */
	ret = ocsfs_alloc_blocks(sb, goal_ag, count, block_out);
	if (ret == 0)
		return 0;

	/* Fall back to home AG if different */
	if (goal_ag != ag_hint) {
		ret = ocsfs_alloc_blocks(sb, ag_hint, count, block_out);
		if (ret == 0)
			return 0;
	}

	/* ocsfs_alloc_blocks already tries all AGs on fallback */
	return -ENOSPC;
}

/* ═══════════════════════════════════════════════════════════════
 * PUBLIC API: SMART BLOCK ALLOCATION
 * ═══════════════════════════════════════════════════════════════ */

/*
 * ocsfs_alloc_extent() — High-level extent allocation.
 *
 * Allocates blocks for @inode at logical offset @logical_block.
 * Uses goal-oriented allocation and preallocation hints to minimize
 * fragmentation.
 *
 * @inode:          target inode
 * @logical_block:  starting logical block in the file
 * @requested:      minimum number of blocks needed
 * @allocated:      [out] actual number of blocks allocated (may be > requested)
 * @phys_out:       [out] starting physical block number
 * @flags:          OCSFS_EXT_WRITTEN or OCSFS_EXT_UNWRITTEN
 *
 * Caller must hold oi->i_extent_lock.
 */
int ocsfs_alloc_extent(struct inode *inode, u64 logical_block,
		       u32 requested, u32 *allocated, u64 *phys_out,
		       u16 flags)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 goal;
	u32 try_count;
	int ret;

	/* Compute allocation size with preallocation */
	if (flags & OCSFS_EXT_UNWRITTEN) {
		/* fallocate: allocate exactly what was asked */
		try_count = requested;
	} else {
		/* Regular write: add preallocation */
		try_count = ocsfs_prealloc_hint(inode, requested);
	}

	/* Find a goal block based on existing extents */
	goal = ocsfs_find_goal_block(inode);

	/* Try to allocate the full amount near the goal */
	ret = ocsfs_alloc_near_goal(inode->i_sb, goal, oi->i_ag,
				    try_count, phys_out);
	if (ret && try_count > requested) {
		/* Preallocation failed, try just what's needed */
		try_count = requested;
		ret = ocsfs_alloc_near_goal(inode->i_sb, goal, oi->i_ag,
					    try_count, phys_out);
	}
	if (ret && try_count > 1) {
		/* Even that failed, try single block */
		try_count = 1;
		ret = ocsfs_alloc_near_goal(inode->i_sb, goal, oi->i_ag,
					    1, phys_out);
	}
	if (ret)
		return ret;

	*allocated = try_count;

	/* Insert the extent into the inode's extent map */
	ret = ocsfs_extent_insert(inode, logical_block, *phys_out,
				  try_count, flags);
	if (ret) {
		ocsfs_free_blocks(inode->i_sb, *phys_out, try_count);
		return ret;
	}

	/* Update block count */
	inode->i_blocks += (u64)try_count * (sbi->s_block_size / 512);
	mark_inode_dirty(inode);

	return 0;
}

/*
 * ocsfs_prealloc_blocks() — Speculatively preallocate blocks for
 * fallocate(FALLOC_FL_KEEP_SIZE).
 *
 * Allocates UNWRITTEN extents that don't change the file size.
 * These are converted to WRITTEN on the first write to each block.
 */
int ocsfs_prealloc_blocks(struct inode *inode, u64 offset, u64 len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 start_block = offset / sbi->s_block_size;
	u64 end_block = (offset + len + sbi->s_block_size - 1) /
			sbi->s_block_size;
	u64 cur;
	int ret = 0;

	mutex_lock(&oi->i_extent_lock);

	for (cur = start_block; cur < end_block; ) {
		struct ocsfs_extent ext;
		u32 alloc_len;
		u32 allocated;
		u64 phys;

		/* Check if this block is already mapped */
		ret = ocsfs_extent_lookup(inode, cur, &ext);
		if (ret == 0 && ext.physical_block != 0) {
			/* Already allocated — skip past this extent */
			u64 ext_end = ext.logical_block + ext.length;

			cur = ext_end;
			continue;
		}

		/* Allocate blocks up to end_block or next mapped extent */
		alloc_len = (u32)min_t(u64, end_block - cur,
				       OCSFS_PREALLOC_MAX_BLOCKS);

		ret = ocsfs_alloc_extent(inode, cur, alloc_len,
					 &allocated, &phys,
					 OCSFS_EXT_UNWRITTEN);
		if (ret)
			break;

		cur += allocated;
	}

	mutex_unlock(&oi->i_extent_lock);
	return ret;
}
