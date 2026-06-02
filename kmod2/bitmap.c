// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — bitmap.c
 * Per-AG block allocation via a 1-bit-per-block bitmap (AG-relative).
 * Single-node (Plan 1): each AG's bitmap is serialised by ag_lock.
 */
#include "ocsfs.h"
#include <linux/vmalloc.h>

/* Load an AG's whole bitmap into a kmalloc buffer. Caller holds ag_lock. */
static u8 *read_ag_bitmap(struct super_block *sb, struct ocsfs2_ag_info *ai,
			  struct buffer_head ***bhs_out)
{
	struct buffer_head **bhs;
	u8 *buf;
	u64 first_blk = ai->bitmap_off / sb->s_blocksize;
	u64 i;

	bhs = kcalloc(ai->bitmap_blocks, sizeof(*bhs), GFP_NOFS);
	if (!bhs)
		return NULL;
	buf = kvmalloc(ai->bitmap_blocks * sb->s_blocksize, GFP_NOFS);
	if (!buf) {
		kfree(bhs);
		return NULL;
	}
	for (i = 0; i < ai->bitmap_blocks; i++) {
		bhs[i] = sb_bread(sb, first_blk + i);
		if (!bhs[i]) {
			while (i--)
				brelse(bhs[i]);
			kvfree(buf);
			kfree(bhs);
			return NULL;
		}
		memcpy(buf + i * sb->s_blocksize, bhs[i]->b_data, sb->s_blocksize);
	}
	*bhs_out = bhs;
	return buf;
}

static void release_bhs(struct ocsfs2_ag_info *ai, struct buffer_head **bhs)
{
	u64 i;

	for (i = 0; i < ai->bitmap_blocks; i++)
		brelse(bhs[i]);
	kfree(bhs);
}

/* Write back only the bitmap blocks touched by [bit_lo, bit_hi). */
static void flush_bitmap_range(struct super_block *sb, struct ocsfs2_ag_info *ai,
			       struct buffer_head **bhs, const u8 *buf,
			       u64 bit_lo, u64 bit_hi)
{
	u64 blo = (bit_lo / 8) / sb->s_blocksize;
	u64 bhi = ((bit_hi - 1) / 8) / sb->s_blocksize;
	u64 i;

	for (i = blo; i <= bhi && i < ai->bitmap_blocks; i++) {
		memcpy(bhs[i]->b_data, buf + i * sb->s_blocksize, sb->s_blocksize);
		mark_buffer_dirty(bhs[i]);
	}
}

static inline bool test_bit_le8(const u8 *bm, u64 i)
{
	return bm[i >> 3] & (1u << (i & 7));
}
static inline void set_bit_le8(u8 *bm, u64 i)   { bm[i >> 3] |=  (u8)(1u << (i & 7)); }
static inline void clear_bit_le8(u8 *bm, u64 i) { bm[i >> 3] &= (u8)~(1u << (i & 7)); }

int ocsfs2_alloc_blocks(struct super_block *sb, u32 ag_hint, u32 count,
			u64 *block_out)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 tried;

	if (count == 0)
		return -EINVAL;

	for (tried = 0; tried < sbi->s_ag_count; tried++) {
		u32 ag = (ag_hint + tried) % sbi->s_ag_count;
		struct ocsfs2_ag_info *ai = &sbi->s_ags[ag];
		struct buffer_head **bhs;
		u8 *bm;
		u64 run_start = 0, run_len = 0, i;
		bool found = false;

		mutex_lock(&ai->ag_lock);
		if (ai->free_blocks < count) {
			mutex_unlock(&ai->ag_lock);
			continue;
		}
		bm = read_ag_bitmap(sb, ai, &bhs);
		if (!bm) {
			mutex_unlock(&ai->ag_lock);
			return -EIO;
		}
		for (i = ai->next_blk_hint; i < ai->block_count; i++) {
			if (test_bit_le8(bm, i)) {
				run_len = 0;
				continue;
			}
			if (run_len == 0)
				run_start = i;
			if (++run_len == count) {
				found = true;
				break;
			}
		}
		if (!found && ai->next_blk_hint != 0) {
			/* wrap: rescan from the start of the AG */
			run_len = 0;
			for (i = 0; i < ai->block_count; i++) {
				if (test_bit_le8(bm, i)) { run_len = 0; continue; }
				if (run_len == 0) run_start = i;
				if (++run_len == count) { found = true; break; }
			}
		}
		if (!found) {
			release_bhs(ai, bhs);
			kvfree(bm);
			mutex_unlock(&ai->ag_lock);
			continue;
		}
		for (i = 0; i < count; i++)
			set_bit_le8(bm, run_start + i);
		flush_bitmap_range(sb, ai, bhs, bm, run_start, run_start + count);
		release_bhs(ai, bhs);
		kvfree(bm);

		ai->free_blocks -= count;
		ai->next_blk_hint = run_start + count;
		mutex_unlock(&ai->ag_lock);

		spin_lock(&sbi->s_free_lock);
		sbi->s_free_blocks -= count;
		spin_unlock(&sbi->s_free_lock);

		*block_out = ai->block_start + run_start;
		return 0;
	}
	return -ENOSPC;
}

void ocsfs2_free_blocks(struct super_block *sb, u64 block, u32 count)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 ag;
	struct ocsfs2_ag_info *ai;
	struct buffer_head **bhs;
	u8 *bm;
	u64 rel, i;
	u32 freed = 0;

	if (count == 0)
		return;
	/* locate the AG containing this block */
	for (ag = 0; ag < sbi->s_ag_count; ag++) {
		ai = &sbi->s_ags[ag];
		if (block >= ai->block_start &&
		    block < ai->block_start + ai->block_count)
			break;
	}
	if (ag >= sbi->s_ag_count) {
		pr_err_ratelimited("ocsfs2: free_blocks: block %llu out of range\n",
				   (unsigned long long)block);
		return;
	}
	ai = &sbi->s_ags[ag];
	rel = block - ai->block_start;
	if (rel + count > ai->block_count) {
		pr_err_ratelimited("ocsfs2: free_blocks: range crosses AG boundary\n");
		count = ai->block_count - rel;
	}

	mutex_lock(&ai->ag_lock);
	bm = read_ag_bitmap(sb, ai, &bhs);
	if (!bm) {
		mutex_unlock(&ai->ag_lock);
		return;
	}
	for (i = 0; i < count; i++) {
		if (test_bit_le8(bm, rel + i)) {
			clear_bit_le8(bm, rel + i);
			freed++;
		}
	}
	flush_bitmap_range(sb, ai, bhs, bm, rel, rel + count);
	release_bhs(ai, bhs);
	kvfree(bm);

	ai->free_blocks += freed;
	if (rel < ai->next_blk_hint)
		ai->next_blk_hint = rel;
	mutex_unlock(&ai->ag_lock);

	spin_lock(&sbi->s_free_lock);
	sbi->s_free_blocks += freed;
	spin_unlock(&sbi->s_free_lock);
}
