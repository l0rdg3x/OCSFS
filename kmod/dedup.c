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
	u32                 io_errs;
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static u64 dedup_hash_block(struct super_block *sb, u64 phys_block,
			    u32 *io_errs)
{
	struct buffer_head *bh = sb_bread(sb, phys_block);
	u64 hash;

	if (!bh) {
		if (io_errs) {
			(*io_errs)++;
			if (*io_errs <= 5)
				pr_warn_ratelimited("ocsfs: dedup: I/O error on block %llu\n",
						    phys_block);
		}
		return 0;
	}
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

	if (flags & (OCSFS_EXT_COMPRESSED | OCSFS_EXT_ENCRYPTED))
		return 0; /* compressed/encrypted extents cannot be content-deduplicated */

	for (i = 0; i < length; i++) {
		u64 phys = physical + i;
		u64 hash = dedup_hash_block(sb, phys, &dc->io_errs);
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

	/* Free the duplicate physical block inside a journal transaction so that a
	 * crash between the btree replace and this free is recoverable: the
	 * uncommitted txn is rolled back on replay, leaving a stale-refcount block
	 * that fsck can reclaim.  Fall back to non-journaled free if txn_begin
	 * fails (transient OOM) — the existing stale-refcount fsck path still
	 * handles that case. */
	ocsfs_refcount_dec(inode->i_sb, p->dup_phys, 1, &should_free);
	if (should_free) {
		struct ocsfs_txn *txn = ocsfs_txn_begin(inode->i_sb);

		if (!IS_ERR(txn)) {
			ocsfs_free_blocks_txn(txn, p->dup_phys, 1);
			ocsfs_txn_commit(txn);
		} else {
			ocsfs_free_blocks(inode->i_sb, p->dup_phys, 1);
		}
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * ARCH-6: Background dedup scrub daemon (delayed_work)
 * Enabled when OCSFS_FEATURE_RO_COMPAT_DEDUP_SCRUB is set.
 * Every 5 minutes: scans a batch of in-memory regular file inodes
 * and calls ocsfs_dedup_file on each.
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_DEDUP_SCRUB_INTERVAL_JIFFIES  (5 * 60 * HZ)
#define OCSFS_DEDUP_SCRUB_BATCH             64

static void ocsfs_dedup_scrub_fn(struct work_struct *work)
{
	struct ocsfs_sb_info *sbi = container_of(to_delayed_work(work),
						  struct ocsfs_sb_info,
						  s_dedup_scrub_work);
	struct super_block *sb = sbi->s_sb;
	struct inode *batch[OCSFS_DEDUP_SCRUB_BATCH];
	struct inode *inode;
	u64 bytes_total = 0;
	int n = 0, i;

	if (sb->s_flags & SB_RDONLY)
		goto reschedule;

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		if (n >= OCSFS_DEDUP_SCRUB_BATCH)
			break;
		if (!S_ISREG(inode->i_mode) || inode->i_nlink == 0)
			continue;
		if (igrab(inode))
			batch[n++] = inode;
	}
	spin_unlock(&sb->s_inode_list_lock);

	for (i = 0; i < n; i++) {
		u64 saved = 0;

		ocsfs_dedup_file(batch[i], &saved);
		bytes_total += saved;
		iput(batch[i]);
	}

	if (bytes_total)
		pr_debug("ocsfs: scrub pass freed %llu bytes via dedup\n",
			 bytes_total);

reschedule:
	queue_delayed_work(system_wq, &sbi->s_dedup_scrub_work,
			   OCSFS_DEDUP_SCRUB_INTERVAL_JIFFIES);
}

void ocsfs_dedup_scrub_start(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	if (!(sbi->s_feature_ro_compat & OCSFS_FEATURE_RO_COMPAT_DEDUP_SCRUB))
		return;

	INIT_DELAYED_WORK(&sbi->s_dedup_scrub_work, ocsfs_dedup_scrub_fn);
	queue_delayed_work(system_wq, &sbi->s_dedup_scrub_work,
			   OCSFS_DEDUP_SCRUB_INTERVAL_JIFFIES);
}

void ocsfs_dedup_scrub_stop(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	if (!(sbi->s_feature_ro_compat & OCSFS_FEATURE_RO_COMPAT_DEDUP_SCRUB))
		return;

	cancel_delayed_work_sync(&sbi->s_dedup_scrub_work);
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
	if (dc.io_errs)
		pr_warn_ratelimited("ocsfs: dedup ino=%llu: %u I/O error(s) during scan\n",
				    oi->i_disk_ino, dc.io_errs);
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
	if (sbi->s_clustered)
		ocsfs_flush_inode_locked(inode, true);
	inode_unlock(inode);

	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);

out_free_ht:
	dedup_free_pairs(&dc);
	dedup_free_ht(&dc);

	*bytes_deduped = saved;
	return ret;
}
