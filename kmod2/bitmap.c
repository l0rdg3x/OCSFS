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
	struct ocsfs2_txn *txn = ocsfs2_current_txn();
	u64 blo = (bit_lo / 8) / sb->s_blocksize;
	u64 bhi = ((bit_hi - 1) / 8) / sb->s_blocksize;
	u64 i;

	for (i = blo; i <= bhi && i < ai->bitmap_blocks; i++) {
		if (txn)
			ocsfs2_txn_get(txn, bhs[i]);  /* snapshot before overwrite */
		memcpy(bhs[i]->b_data, buf + i * sb->s_blocksize, sb->s_blocksize);
		mark_buffer_dirty(bhs[i]);
		if (!txn)
			sync_dirty_buffer(bhs[i]);    /* data path: bitmap durable ahead of inode */
	}
}

static inline bool test_bit_le8(const u8 *bm, u64 i)
{
	return bm[i >> 3] & (1u << (i & 7));
}
static inline void set_bit_le8(u8 *bm, u64 i)   { bm[i >> 3] |=  (u8)(1u << (i & 7)); }
static inline void clear_bit_le8(u8 *bm, u64 i) { bm[i >> 3] &= (u8)~(1u << (i & 7)); }

#define OCSFS2_BM_CAW_RETRIES  16

/* Clustered allocation: claim a run of @count free blocks via CAW on a single
 * bitmap block, so two nodes never double-allocate (lock-free, coherent — no
 * meta lease needed even on the data path). Runs stay within one bitmap block
 * (32768 blocks), well above the 2048-block allocation cap. */
static int clustered_alloc(struct super_block *sb, u32 ag_hint, u32 count,
			   u64 *block_out)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 bs = sb->s_blocksize, bpb = bs * 8;
	u8 *old, *new;
	u32 tried;
	int ret = -ENOSPC;

	old = kmalloc(bs, GFP_NOFS);
	new = kmalloc(bs, GFP_NOFS);
	if (!old || !new) { kfree(old); kfree(new); return -ENOMEM; }

	for (tried = 0; tried < sbi->s_ag_count && ret == -ENOSPC; tried++) {
		u32 ag = (ag_hint + tried) % sbi->s_ag_count;
		struct ocsfs2_ag_info *ai = &sbi->s_ags[ag];
		u64 b;

		mutex_lock(&ai->ag_lock);
		for (b = 0; (u64)b * bpb < ai->block_count; b++) {
			u64 base = (u64)b * bpb;
			u32 blk_bits = (u32)min_t(u64, bpb, ai->block_count - base);
			u64 bbyte = ai->bitmap_off + (u64)b * bs;
			int attempt;

			for (attempt = 0; attempt < OCSFS2_BM_CAW_RETRIES; attempt++) {
				u32 run_start = 0, run_len = 0, i;
				bool found = false;

				if (ocsfs2_cl_bio(sb, bbyte, old, bs, REQ_OP_READ)) {
					ret = -EIO;
					goto unlock;
				}
				for (i = 0; i < blk_bits; i++) {
					if (test_bit_le8(old, i)) { run_len = 0; continue; }
					if (run_len == 0) run_start = i;
					if (++run_len == count) { found = true; break; }
				}
				if (!found)
					break;   /* not in this block; try next */
				memcpy(new, old, bs);
				for (i = 0; i < count; i++)
					set_bit_le8(new, run_start + i);
				if (ocsfs2_scsi_caw(sb, bbyte / bs, old, new, bs) == 0) {
					ai->free_blocks -= count;   /* per-node hint */
					*block_out = ai->block_start + base + run_start;
					ret = 0;
					goto unlock;
				}
				/* miscompare: a peer changed this block, re-read + retry */
			}
		}
unlock:
		mutex_unlock(&ai->ag_lock);
	}
	kfree(old);
	kfree(new);
	return ret;
}

/* Clustered free: clear the bits of [block, block+count) via CAW per touched
 * bitmap block (coherent against concurrent allocators). */
static void clustered_free(struct super_block *sb, u64 block, u32 count)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 bs = sb->s_blocksize, bpb = bs * 8, ag;
	struct ocsfs2_ag_info *ai;
	u8 *old, *new;
	u64 rel, done = 0;

	for (ag = 0; ag < sbi->s_ag_count; ag++) {
		ai = &sbi->s_ags[ag];
		if (block >= ai->block_start &&
		    block < ai->block_start + ai->block_count)
			break;
	}
	if (ag >= sbi->s_ag_count)
		return;
	ai = &sbi->s_ags[ag];
	rel = block - ai->block_start;
	if (rel + count > ai->block_count)
		count = ai->block_count - rel;

	old = kmalloc(bs, GFP_NOFS);
	new = kmalloc(bs, GFP_NOFS);
	if (!old || !new) { kfree(old); kfree(new); return; }

	mutex_lock(&ai->ag_lock);
	while (done < count) {
		u64 cur = rel + done;
		u64 b = cur / bpb;
		u64 bbyte = ai->bitmap_off + b * bs;
		u32 first = cur % bpb;
		u32 n = (u32)min_t(u64, count - done, bpb - first);   /* bits in this block */
		int attempt;

		for (attempt = 0; attempt < OCSFS2_BM_CAW_RETRIES; attempt++) {
			u32 i, cleared = 0;

			if (ocsfs2_cl_bio(sb, bbyte, old, bs, REQ_OP_READ))
				goto next;
			memcpy(new, old, bs);
			for (i = 0; i < n; i++)
				if (test_bit_le8(new, first + i)) {
					clear_bit_le8(new, first + i);
					cleared++;
				}
			if (ocsfs2_scsi_caw(sb, bbyte / bs, old, new, bs) == 0) {
				ai->free_blocks += cleared;
				break;
			}
		}
next:
		done += n;
	}
	mutex_unlock(&ai->ag_lock);
	kfree(old);
	kfree(new);
}

int ocsfs2_alloc_blocks(struct super_block *sb, u32 ag_hint, u32 count,
			u64 *block_out)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 tried;

	if (count == 0)
		return -EINVAL;
	if (sbi->s_cluster)
		return clustered_alloc(sb, ag_hint, count, block_out);

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
	if (sbi->s_cluster) {
		clustered_free(sb, block, count);
		return;
	}
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
