// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — content-based block deduplication.
 *
 * Requires btree-extent inode. Two-phase design: phase 1 scans all extents
 * read-only and builds an in-memory hash table; phase 2 applies replacements
 * (refcount_inc canonical, extent_btree_replace, free_blocks duplicate).
 * Crash between replace and free leaves a space leak recoverable by fsck.
 */
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/xxhash.h>
#include "ocsfs.h"

#define DEDUP_HT_SIZE  4096U   /* hash table buckets — power of two */

/* ── In-memory hash table entry ──────────────────────────────────────────── */

struct dedup_entry {
	u64               hash;
	u64               phys_block;
	struct dedup_entry *next;
};

/* ── Dedup work item (collected in phase 1, applied in phase 2) ─────────── */

struct dedup_pair {
	u64              logical_block; /* logical block in the file */
	u64              dup_phys;      /* physical block to free */
	u64              can_phys;      /* canonical block to share */
	struct dedup_pair *next;
};

/* ── Per-call context (ht heap-allocated to avoid large stack frame) ──────── */

struct dedup_ctx {
	struct inode       *inode;
	struct dedup_entry **ht;    /* kzalloc'd: DEDUP_HT_SIZE pointers */
	struct dedup_pair   *pairs; /* singly-linked, newest first */
	u32                 n_pairs;
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static u64 dedup_hash_block(struct super_block *sb, u64 phys_block)
{
	struct buffer_head *bh = sb_bread(sb, phys_block);
	u64 hash;

	if (!bh)
		return 0;
	hash = xxh64(bh->b_data, bh->b_size, 0xDE0DC45EULL);
	brelse(bh);
	return hash;
}

static bool dedup_blocks_equal(struct super_block *sb, u64 a, u64 b)
{
	struct buffer_head *bha, *bhb;
	bool eq;

	bha = sb_bread(sb, a);
	if (!bha)
		return false;
	bhb = sb_bread(sb, b);
	if (!bhb) {
		brelse(bha);
		return false;
	}
	eq = memcmp(bha->b_data, bhb->b_data, bha->b_size) == 0;
	brelse(bhb);
	brelse(bha);
	return eq;
}

static void dedup_free_ht(struct dedup_ctx *dc)
{
	u32 i;

	if (!dc->ht)
		return;

	for (i = 0; i < DEDUP_HT_SIZE; i++) {
		struct dedup_entry *e = dc->ht[i];

		while (e) {
			struct dedup_entry *tmp = e->next;

			kfree(e);
			e = tmp;
		}
	}
	kfree(dc->ht);
	dc->ht = NULL;
}

static void dedup_free_pairs(struct dedup_ctx *dc)
{
	struct dedup_pair *p = dc->pairs;

	while (p) {
		struct dedup_pair *tmp = p->next;

		kfree(p);
		p = tmp;
	}
	dc->pairs = NULL;
}

/* ── Phase 1: extent scan callback ─────────────────────────────────────────
 * Called by ocsfs_extent_btree_iterate for each (logical, physical, length, flags).
 * Hashes each block; on match, records a dedup_pair. Does NOT modify btree.
 */
static int dedup_scan_extent(u64 logical, u64 physical, u32 length,
			     u16 flags, void *ctx_)
{
	struct dedup_ctx *dc = ctx_;
	struct super_block *sb = dc->inode->i_sb;
	u32 i;

	(void)flags; /* extent flags not relevant for dedup scanning */

	for (i = 0; i < length; i++) {
		u64 phys = physical + i;
		u64 hash = dedup_hash_block(sb, phys);
		u32 slot;
		struct dedup_entry *e;

		if (!hash)
			continue; /* skip unreadable blocks */

		slot = (u32)(hash & (DEDUP_HT_SIZE - 1));

		for (e = dc->ht[slot]; e; e = e->next) {
			if (e->hash == hash &&
			    dedup_blocks_equal(sb, e->phys_block, phys)) {
				/* Duplicate found — record for phase 2 */
				struct dedup_pair *p;

				p = kmalloc(sizeof(*p), GFP_KERNEL);
				if (!p)
					return -ENOMEM;
				p->logical_block = logical + i;
				p->dup_phys      = phys;
				p->can_phys      = e->phys_block;
				p->next          = dc->pairs;
				dc->pairs        = p;
				dc->n_pairs++;
				goto next_block;
			}
		}

		/* Not seen before — record as canonical */
		e = kmalloc(sizeof(*e), GFP_KERNEL);
		if (!e)
			return -ENOMEM;
		e->hash       = hash;
		e->phys_block = phys;
		e->next       = dc->ht[slot];
		dc->ht[slot]  = e;

next_block:;
	}
	return 0;
}

/* ── Phase 2: apply one dedup pair ──────────────────────────────────────── */

static int dedup_apply_pair(struct inode *inode, const struct dedup_pair *p)
{
	struct ocsfs_extent orig;
	u64 offset_in_ext;
	bool should_free;
	int ret;

	ret = ocsfs_extent_lookup(inode, p->logical_block, &orig);
	if (ret)
		return ret;

	/* Verify the physical block still matches — another op may have changed it */
	offset_in_ext = p->logical_block - orig.logical_block;
	if (orig.physical_block + offset_in_ext != p->dup_phys)
		return 0; /* already remapped; skip */

	ret = ocsfs_refcount_inc(inode->i_sb, p->can_phys, 1);
	if (ret)
		return ret;

	ret = ocsfs_extent_btree_replace(inode, &orig, offset_in_ext, 1,
					 p->can_phys);
	if (ret) {
		ocsfs_refcount_dec(inode->i_sb, p->can_phys, 1, NULL);
		return ret;
	}

	/* Free the duplicate physical block.
	 * Crash here leaves a space leak; fsck detects stale refcount and frees. */
	ocsfs_refcount_dec(inode->i_sb, p->dup_phys, 1, &should_free);
	if (should_free)
		ocsfs_free_blocks(inode->i_sb, p->dup_phys, 1);

	return 0;
}

/* ── Public entry point ─────────────────────────────────────────────────── */

int ocsfs_dedup_file(struct inode *inode, u64 *bytes_deduped)
{
	struct ocsfs_sb_info    *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi  = OCSFS_I(inode);
	struct dedup_ctx         dc  = {};
	struct dedup_pair       *p;
	int ret = 0;
	u64 saved = 0;

	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (oi->i_extent_tree_root == 0)
		return -EOPNOTSUPP; /* inline extents; file too small for dedup */

	dc.inode = inode;
	dc.ht = kzalloc(DEDUP_HT_SIZE * sizeof(struct dedup_entry *), GFP_KERNEL);
	if (!dc.ht)
		return -ENOMEM;

	/* Cluster: hold EX DLM lock for the duration */
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			goto out_free_ht;
	}

	inode_lock(inode);

	/* Phase 1: read-only scan */
	ret = ocsfs_extent_btree_iterate(inode, dedup_scan_extent, &dc);
	if (ret)
		goto out_unlock;

	dedup_free_ht(&dc);   /* hash table no longer needed */

	/* Phase 2: apply replacements */
	for (p = dc.pairs; p; p = p->next) {
		int r = dedup_apply_pair(inode, p);

		if (r == 0)
			saved += inode->i_sb->s_blocksize;
		else if (r != -ENOENT)
			pr_warn_ratelimited("ocsfs: dedup pair failed ino=%llu lb=%llu: %d\n",
					    oi->i_disk_ino, p->logical_block, r);
	}

	inode->i_blocks -= saved / 512;
	mark_inode_dirty(inode);

out_unlock:
	inode_unlock(inode);

	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);

out_free_ht:
	dedup_free_pairs(&dc);
	dedup_free_ht(&dc);

	*bytes_deduped = saved;
	return ret;
}
