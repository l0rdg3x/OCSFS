// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — refcount.c
 * Extent reference counting for Copy-on-Write snapshots.
 *
 * Phase 4: Per-AG refcount tree stored on disk.
 *
 * Design:
 *   Each AG has a refcount area on disk (after the block bitmap).
 *   The refcount area is a flat array of __le32 entries, one per
 *   physical block in the AG. Only blocks with refcount > 1 need
 *   an entry (refcount 1 is the default for allocated blocks).
 *
 *   To save space, we use a sparse representation: a B+ tree
 *   mapping (physical_block → refcount). Only blocks with
 *   refcount > 1 have entries. For Phase 4 MVP, we use a simpler
 *   approach: a per-AG hash table stored in dedicated blocks.
 *
 * Refcount lifecycle:
 *   alloc_blocks: refcount = 1 (implicit, no entry needed)
 *   snapshot:     refcount++ (entry created if refcount goes to 2)
 *   write (CoW):  refcount-- on old blocks, alloc new with refcount 1
 *   free:         if refcount > 1, refcount--; else actually free
 *
 * The refcount table uses a simple on-disk hash map with chaining.
 * Each bucket is a linked list of (block, refcount) pairs.
 */

#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * REFCOUNT TABLE LAYOUT
 *
 * The refcount table occupies OCSFS_REFCOUNT_BLOCKS_PER_AG blocks
 * at the end of each AG's metadata area. Each block contains a
 * flat array of ocsfs_disk_refcount entries.
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_REFCOUNT_BLOCKS_PER_AG	16
#define OCSFS_REFCOUNT_MAGIC		0x52454643  /* 'REFC' */

/*
 * On-disk refcount entry — 16 bytes each.
 * Stored in sorted arrays within each refcount block.
 */
struct ocsfs_disk_refcount {
	__le64	rc_block;	/* physical block number (0 = unused slot) */
	__le32	rc_count;	/* reference count (0 = free entry) */
	__le32	rc_checksum;
} __packed;

/* Entries per block = block_size / 16 */
static inline u32 ocsfs_rc_entries_per_block(struct ocsfs_sb_info *sbi)
{
	return sbi->s_block_size / sizeof(struct ocsfs_disk_refcount);
}

/* Hash a physical block number to a refcount block index */
static inline u32 ocsfs_rc_hash(u64 phys_block, u32 nr_blocks)
{
	u64 h = phys_block * 0x9e3779b97f4a7c15ULL;
	return (u32)(h % nr_blocks);
}

/*
 * Find the AG and local block offset for a physical block.
 */
static struct ocsfs_ag_info *ocsfs_block_to_ag(struct ocsfs_sb_info *sbi,
					       u64 phys_block,
					       u64 *local_block)
{
	u32 ag_no;

	for (ag_no = 0; ag_no < sbi->s_ag_count; ag_no++) {
		struct ocsfs_ag_info *ag = &sbi->s_ags[ag_no];

		if (phys_block >= ag->block_start &&
		    phys_block < ag->block_start + ag->block_count) {
			*local_block = phys_block - ag->block_start;
			return ag;
		}
	}

	return NULL;
}

/*
 * Get the disk block number for a refcount table block in an AG.
 * The refcount table is stored after the bitmap area.
 */
static u64 ocsfs_rc_table_block(struct ocsfs_sb_info *sbi,
				struct ocsfs_ag_info *ag, u32 bucket)
{
	u64 bitmap_end = (ag->bitmap_off + ag->bitmap_size) /
			 sbi->s_block_size;
	u64 inode_table_blocks = (ag->inode_count * OCSFS_INODE_SIZE +
				  sbi->s_block_size - 1) / sbi->s_block_size;
	u64 rc_start = (ag->inode_table_off / sbi->s_block_size) +
		       inode_table_blocks;

	return rc_start + bucket;
}

/* ═══════════════════════════════════════════════════════════════
 * REFCOUNT OPERATIONS
 * ═══════════════════════════════════════════════════════════════ */

/*
 * ocsfs_refcount_get() — Get the refcount for a physical block.
 *
 * Returns 1 (default) if no explicit entry exists.
 */
int ocsfs_refcount_get(struct super_block *sb, u64 phys_block,
		       u32 *refcount_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_ag_info *ag;
	u64 local;
	u32 bucket;
	u64 rc_block;
	struct buffer_head *bh;
	struct ocsfs_disk_refcount *entries;
	u32 nr_entries;
	u32 i;

	ag = ocsfs_block_to_ag(sbi, phys_block, &local);
	if (!ag) {
		*refcount_out = 0;
		return -EINVAL;
	}

	bucket = ocsfs_rc_hash(phys_block, OCSFS_REFCOUNT_BLOCKS_PER_AG);
	rc_block = ocsfs_rc_table_block(sbi, ag, bucket);

	bh = sb_bread(sb, rc_block);
	if (!bh) {
		/* No refcount table block — default refcount is 1 */
		*refcount_out = 1;
		return 0;
	}

	entries = (struct ocsfs_disk_refcount *)bh->b_data;
	nr_entries = ocsfs_rc_entries_per_block(sbi);

	for (i = 0; i < nr_entries; i++) {
		if (le64_to_cpu(entries[i].rc_block) == phys_block &&
		    le32_to_cpu(entries[i].rc_count) > 0) {
			*refcount_out = le32_to_cpu(entries[i].rc_count);
			brelse(bh);
			return 0;
		}
	}

	brelse(bh);

	/* No entry found — default refcount is 1 for allocated blocks */
	*refcount_out = 1;
	return 0;
}

/*
 * ocsfs_refcount_set() — Set the refcount for a physical block.
 *
 * If count <= 1, removes the entry (1 is the default).
 * If count > 1, creates or updates an entry.
 */
static int ocsfs_refcount_set(struct super_block *sb, u64 phys_block,
			      u32 count)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_ag_info *ag;
	u64 local;
	u32 bucket;
	u64 rc_block;
	struct buffer_head *bh;
	struct ocsfs_disk_refcount *entries;
	u32 nr_entries;
	u32 i;
	int free_slot = -1;

	ag = ocsfs_block_to_ag(sbi, phys_block, &local);
	if (!ag)
		return -EINVAL;

	bucket = ocsfs_rc_hash(phys_block, OCSFS_REFCOUNT_BLOCKS_PER_AG);
	rc_block = ocsfs_rc_table_block(sbi, ag, bucket);

	bh = sb_bread(sb, rc_block);
	if (!bh)
		return -EIO;

	entries = (struct ocsfs_disk_refcount *)bh->b_data;
	nr_entries = ocsfs_rc_entries_per_block(sbi);

	/* Search for existing entry or free slot */
	for (i = 0; i < nr_entries; i++) {
		u64 blk = le64_to_cpu(entries[i].rc_block);
		u32 cnt = le32_to_cpu(entries[i].rc_count);

		if (blk == phys_block && cnt > 0) {
			/* Found existing entry */
			if (count <= 1) {
				/* Remove entry (revert to default) */
				entries[i].rc_block = 0;
				entries[i].rc_count = 0;
			} else {
				entries[i].rc_count = cpu_to_le32(count);
			}

			entries[i].rc_checksum = cpu_to_le32(
				ocsfs_crc32c(0, &entries[i],
					     sizeof(*entries) - 4));
			mark_buffer_dirty(bh);
			brelse(bh);
			return 0;
		}

		if (free_slot < 0 && (blk == 0 || cnt == 0))
			free_slot = i;
	}

	/* No existing entry found */
	if (count <= 1) {
		/* Nothing to do — 1 is the default */
		brelse(bh);
		return 0;
	}

	if (free_slot < 0) {
		brelse(bh);
		pr_warn("ocsfs: refcount table full for AG %u\n", ag->ag_no);
		return -ENOSPC;
	}

	/* Create new entry */
	entries[free_slot].rc_block = cpu_to_le64(phys_block);
	entries[free_slot].rc_count = cpu_to_le32(count);
	entries[free_slot].rc_checksum = cpu_to_le32(
		ocsfs_crc32c(0, &entries[free_slot],
			     sizeof(*entries) - 4));

	mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

/*
 * ocsfs_refcount_inc() — Increment refcount on a range of blocks.
 *
 * For snapshot creation: all shared extents go from 1 → 2
 * (or n → n+1 for multi-layer snapshots).
 */
int ocsfs_refcount_inc(struct super_block *sb, u64 phys_block, u32 len)
{
	u32 i;
	int ret;

	for (i = 0; i < len; i++) {
		u32 current_rc;

		ret = ocsfs_refcount_get(sb, phys_block + i, &current_rc);
		if (ret)
			return ret;

		ret = ocsfs_refcount_set(sb, phys_block + i, current_rc + 1);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * ocsfs_refcount_dec() — Decrement refcount on a range of blocks.
 *
 * @should_free: set to true if refcount drops to 0 (caller should
 *               free the blocks).
 *
 * For snapshot deletion and CoW: decrements shared extent refcounts.
 */
int ocsfs_refcount_dec(struct super_block *sb, u64 phys_block, u32 len,
		       bool *should_free)
{
	u32 i;
	int ret;
	bool all_free = true;

	for (i = 0; i < len; i++) {
		u32 current_rc;

		ret = ocsfs_refcount_get(sb, phys_block + i, &current_rc);
		if (ret)
			return ret;

		if (current_rc <= 1) {
			/* Already at 1 or 0 — will be freed */
			ret = ocsfs_refcount_set(sb, phys_block + i, 0);
		} else {
			ret = ocsfs_refcount_set(sb, phys_block + i,
						 current_rc - 1);
			all_free = false;
		}

		if (ret)
			return ret;
	}

	if (should_free)
		*should_free = all_free;

	return 0;
}

/*
 * ocsfs_refcount_init_ag() — Initialize the refcount table for an AG.
 *
 * Called during mkfs or when first enabling snapshots.
 * Zeroes the refcount blocks.
 */
int ocsfs_refcount_init_ag(struct super_block *sb, u32 ag_no)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_ag_info *ag = &sbi->s_ags[ag_no];
	u32 i;

	for (i = 0; i < OCSFS_REFCOUNT_BLOCKS_PER_AG; i++) {
		u64 rc_block = ocsfs_rc_table_block(sbi, ag, i);
		struct buffer_head *bh;

		bh = sb_getblk(sb, rc_block);
		if (!bh)
			return -ENOMEM;

		lock_buffer(bh);
		memset(bh->b_data, 0, sbi->s_block_size);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		brelse(bh);
	}

	return 0;
}
