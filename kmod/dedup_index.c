// SPDX-License-Identifier: GPL-2.0
/*
 * OCSFS — dedup_index.c
 *
 * Global cross-file dedup index (a "DDT"): a single on-disk B+ tree keyed by
 * 64-bit block fingerprint, mapping fingerprint -> canonical physical block.
 * Rooted at sbi->s_dedup_index_root (persisted in the superblock).
 *
 * Reference model (the key correctness invariant):
 *   Every block recorded in the index holds ONE extra refcount reference (the
 *   "index reference").  Therefore a canonical block's total refcount is
 *   (number of file extents pointing at it) + 1.  Consequences:
 *     - it is never freed while indexed (refcount >= 1 always), so an index
 *       entry can never dangle onto a free/reused block;
 *     - once a second reference appears (refcount >= 2) every write to it is
 *       forced through CoW, so its content is immutable — no read-then-share
 *       race with a concurrent in-place write;
 *     - when the last *file* reference goes away the refcount collapses to the
 *       implicit-1 state (no refcount-btree entry), which ocsfs_refcount_get()
 *       reports as 1.  GC treats "refcount == 1" as "files == 0, index-only"
 *       and reclaims the entry + the block.
 *
 * Crash ordering: the index reference (refcount_inc) is made durable BEFORE the
 * index entry is inserted, and GC removes the index entry BEFORE dropping the
 * reference.  A crash in either window leaves at worst a stale refcount /
 * orphan block that fsck reclaims — never corruption.
 */

#include "ocsfs.h"
#include "ocsfs_btree.h"

struct didx_ctx {
	struct super_block *sb;
	struct ocsfs_txn   *txn;   /* non-NULL: route btree node writes through it */
};

/* ── B+ tree node I/O callbacks (mirror refcount.c's rc_bt_*) ───────────── */

static int didx_bt_read(void *ctx, u64 block, void *buf, u32 size)
{
	struct didx_ctx *dc = ctx;
	struct buffer_head *bh = sb_bread(dc->sb, block);

	if (!bh)
		return -EIO;
	memcpy(buf, bh->b_data, size);
	brelse(bh);
	return 0;
}

static int didx_bt_write(void *ctx, u64 block, const void *buf, u32 size)
{
	struct didx_ctx *dc = ctx;
	struct buffer_head *bh = sb_getblk(dc->sb, block);

	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memcpy(bh->b_data, buf, size);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);

	if (dc->txn) {
		int ret = ocsfs_txn_add_bh(dc->txn, bh);

		brelse(bh);
		return ret;
	}
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

static int didx_bt_alloc(void *ctx, u64 *out_block)
{
	struct didx_ctx *dc = ctx;

	/* Allocate index nodes from AG 0 (spills to other AGs when full).  Must
	 * use the txn-aware allocator when a txn is open to avoid re-entering
	 * ocsfs_txn_begin() under the journal lock (self-deadlock). */
	if (dc->txn)
		return ocsfs_alloc_blocks_txn(dc->txn, dc->sb, 0, 1, out_block);
	return ocsfs_alloc_blocks(dc->sb, 0, 1, out_block);
}

static int didx_bt_free(void *ctx, u64 block)
{
	struct didx_ctx *dc = ctx;

	if (dc->txn)
		return ocsfs_free_blocks_txn(dc->txn, block, 1);
	ocsfs_free_blocks(dc->sb, block, 1);
	return 0;
}

/* ── superblock root persistence ────────────────────────────────────────── */

static void didx_persist_root(struct super_block *sb, u64 new_root,
			      struct ocsfs_txn *txn)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	sbi->s_dedup_index_root = new_root;
	sbi->s_ds->s_dedup_index_root = cpu_to_le64(new_root);
	sbi->s_ds->s_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, sbi->s_ds, OCSFS_SUPERBLOCK_SIZE - 4));
	mark_buffer_dirty(sbi->s_sbh);
	if (txn)
		ocsfs_txn_add_bh(txn, sbi->s_sbh);
	else
		sync_dirty_buffer(sbi->s_sbh);
}

static int didx_open_or_create(struct ocsfs_btree *bt, struct super_block *sb,
			       struct didx_ctx *dc)
{
	u32 bsz = OCSFS_SB(sb)->s_block_size;

	if (OCSFS_SB(sb)->s_dedup_index_root)
		return ocsfs_btree_open(bt, OCSFS_SB(sb)->s_dedup_index_root, bsz,
					didx_bt_read, didx_bt_write,
					didx_bt_alloc, didx_bt_free, dc);
	return ocsfs_btree_create(bt, bsz, didx_bt_read, didx_bt_write,
				  didx_bt_alloc, didx_bt_free, dc);
}

/* ── public API ─────────────────────────────────────────────────────────── */

/*
 * Look up a fingerprint.  Returns 0 and *canonical on hit, -ENOENT on miss.
 * Read-only; takes the index lock for a consistent snapshot.
 */
int ocsfs_dedup_index_lookup(struct super_block *sb, u64 fp, u64 *canonical)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct didx_ctx dc = { .sb = sb, .txn = NULL };
	struct ocsfs_btree bt;
	u64 val;
	int ret;

	mutex_lock(&sbi->s_dedup_index_lock);
	if (!sbi->s_dedup_index_root) {
		mutex_unlock(&sbi->s_dedup_index_lock);
		return -ENOENT;
	}
	ret = ocsfs_btree_open(&bt, sbi->s_dedup_index_root, sbi->s_block_size,
			       didx_bt_read, didx_bt_write,
			       didx_bt_alloc, didx_bt_free, &dc);
	if (ret) {
		mutex_unlock(&sbi->s_dedup_index_lock);
		return ret;
	}
	ret = ocsfs_btree_search(&bt, fp, &val);
	mutex_unlock(&sbi->s_dedup_index_lock);
	if (ret == 0)
		*canonical = val;
	return ret;
}

/*
 * Record `phys` as the canonical block for fingerprint `fp` and take the index
 * reference (refcount_inc).  No-op if `fp` is already mapped (the existing
 * canonical wins; caller should dedup against it instead).
 */
int ocsfs_dedup_index_insert_canonical(struct super_block *sb, u64 fp, u64 phys)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct didx_ctx dc = { .sb = sb };
	struct ocsfs_btree bt;
	struct ocsfs_txn *txn;
	u64 existing;
	int ret;

	/* Index lock is held across refcount_inc + insert.  Both open and commit
	 * their own journal transactions sequentially (never nested), and the
	 * lock order is always s_dedup_index_lock -> j_lock — no path takes them
	 * in the other order, so this cannot deadlock. */
	mutex_lock(&sbi->s_dedup_index_lock);

	if (sbi->s_dedup_index_root) {
		ret = ocsfs_btree_open(&bt, sbi->s_dedup_index_root,
				       sbi->s_block_size, didx_bt_read,
				       didx_bt_write, didx_bt_alloc,
				       didx_bt_free, &dc);
		if (ret)
			goto out_unlock;
		if (ocsfs_btree_search(&bt, fp, &existing) == 0) {
			ret = 0;            /* already canonical — caller dedups */
			goto out_unlock;
		}
	}

	/* Index reference FIRST, made durable before the entry exists. */
	ret = ocsfs_refcount_inc(sb, phys, 1);
	if (ret)
		goto out_unlock;

	txn = ocsfs_txn_begin(sb);
	if (IS_ERR(txn)) {
		ret = PTR_ERR(txn);
		goto out_undo;
	}
	dc.txn = txn;

	ret = didx_open_or_create(&bt, sb, &dc);
	if (ret) {
		ocsfs_txn_abort(txn);
		goto out_undo;
	}
	ret = ocsfs_btree_insert(&bt, fp, phys);
	if (ret) {
		ocsfs_txn_abort(txn);
		goto out_undo;
	}
	if (bt.root_block != sbi->s_dedup_index_root)
		didx_persist_root(sb, bt.root_block, txn);

	ret = ocsfs_txn_commit(txn);
	if (ret)
		goto out_undo;

	mutex_unlock(&sbi->s_dedup_index_lock);
	return 0;

out_undo:
	ocsfs_refcount_dec(sb, phys, 1, NULL);
out_unlock:
	mutex_unlock(&sbi->s_dedup_index_lock);
	return ret;
}

/* ── garbage collection ─────────────────────────────────────────────────── */

#define DIDX_GC_BATCH 256

struct didx_gc_collect {
	u64 fps[DIDX_GC_BATCH];
	u64 blks[DIDX_GC_BATCH];
	u32 n;
};

static int didx_gc_scan_cb(u64 key, u64 value, void *ctx)
{
	struct didx_gc_collect *c = ctx;

	if (c->n >= DIDX_GC_BATCH)
		return 1;   /* stop scan; remaining entries handled next pass */
	c->fps[c->n]  = key;
	c->blks[c->n] = value;
	c->n++;
	return 0;
}

/*
 * Reclaim index-only canonicals: entries whose block has refcount 1 (no file
 * references it any more).  Removes the index entry, then drops the index
 * reference (refcount 1 -> 0 -> free).  Best-effort and bounded to one batch
 * per call; the scrub re-invokes it.
 */
int ocsfs_dedup_index_gc(struct super_block *sb, u64 *bytes_freed)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct didx_ctx dc = { .sb = sb, .txn = NULL };
	struct didx_gc_collect *col;
	struct ocsfs_btree bt;
	u64 freed = 0;
	u32 i;
	int ret = 0;

	if (bytes_freed)
		*bytes_freed = 0;

	mutex_lock(&sbi->s_dedup_index_lock);
	if (!sbi->s_dedup_index_root) {
		mutex_unlock(&sbi->s_dedup_index_lock);
		return 0;
	}
	col = kzalloc(sizeof(*col), GFP_KERNEL);
	if (!col) {
		mutex_unlock(&sbi->s_dedup_index_lock);
		return -ENOMEM;
	}
	ret = ocsfs_btree_open(&bt, sbi->s_dedup_index_root, sbi->s_block_size,
			       didx_bt_read, didx_bt_write,
			       didx_bt_alloc, didx_bt_free, &dc);
	if (ret) {
		mutex_unlock(&sbi->s_dedup_index_lock);
		kfree(col);
		return ret;
	}
	ocsfs_btree_range_scan(&bt, 0, U64_MAX, didx_gc_scan_cb, col);
	mutex_unlock(&sbi->s_dedup_index_lock);

	for (i = 0; i < col->n; i++) {
		u32 rc = 0;

		if (ocsfs_refcount_get(sb, col->blks[i], &rc) != 0 || rc != 1)
			continue;   /* still referenced by >=1 file, keep */

		/* Remove the index entry first (crash order), then drop the
		 * index reference which frees the now-unreferenced block. */
		mutex_lock(&sbi->s_dedup_index_lock);
		{
			struct ocsfs_txn *txn = ocsfs_txn_begin(sb);

			if (!IS_ERR(txn)) {
				dc.txn = txn;
				if (sbi->s_dedup_index_root &&
				    ocsfs_btree_open(&bt, sbi->s_dedup_index_root,
						     sbi->s_block_size,
						     didx_bt_read, didx_bt_write,
						     didx_bt_alloc, didx_bt_free,
						     &dc) == 0 &&
				    ocsfs_btree_delete(&bt, col->fps[i]) == 0) {
					if (bt.root_block != sbi->s_dedup_index_root)
						didx_persist_root(sb, bt.root_block, txn);
					if (ocsfs_txn_commit(txn) == 0) {
						mutex_unlock(&sbi->s_dedup_index_lock);
						ocsfs_refcount_dec(sb, col->blks[i],
								   1, NULL);
						freed += sb->s_blocksize;
						continue;
					}
				} else {
					ocsfs_txn_abort(txn);
				}
			}
		}
		mutex_unlock(&sbi->s_dedup_index_lock);
	}

	kfree(col);
	if (bytes_freed)
		*bytes_freed = freed;
	return ret;
}
