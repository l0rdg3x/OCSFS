// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — refcount.c
 * Per-AG B+ tree refcount for Copy-on-Write and dedup (ARCH-5).
 *
 * Requires OCSFS_FEATURE_INCOMPAT_RC_BTREE_PER_AG.
 * Key: physical block number (u64)
 * Value: reference count (u64, implicit 1 = no entry)
 *
 * DLM locking: ag_rc_lock_res (EX for write, SH for read) serializes
 * all B+ tree operations within an AG.  CAS cannot cover multi-block
 * B+ tree splits, so DLM is the correct coordination primitive here.
 */

#include "ocsfs.h"
#include "ocsfs_btree.h"

/* ═══════════════════════════════════════════════════════════════
 * B+ TREE I/O CALLBACKS
 * ═══════════════════════════════════════════════════════════════ */

struct ocsfs_rc_io_ctx {
	struct super_block *sb;
	u32 ag_no;
	struct ocsfs_txn *txn; /* non-NULL: add bh to txn instead of sync */
};

static int rc_bt_read(void *ctx, u64 block, void *buf, u32 size)
{
	struct ocsfs_rc_io_ctx *rctx = ctx;
	struct super_block *sb = rctx->sb;
	struct buffer_head *bh;

	/*
	 * Always read through the buffer cache.  The old forced disk re-read for
	 * cross-node coherence broke read-your-own-writes (a just-allocated
	 * refcount-btree node read back as zeros during reflink/snapshot/dedup)
	 * and could block in __bh_read on a dirty/locked node while holding a
	 * lock.  Cross-node coherence of refcount-btree metadata must be handled
	 * at DLM acquisition — TODO for the multi-node testbed.  See
	 * ext_btree_read() for the full rationale.
	 */
	bh = sb_bread(sb, block);
	if (!bh)
		return -EIO;
	memcpy(buf, bh->b_data, size);
	brelse(bh);
	return 0;
}

static int rc_bt_write(void *ctx, u64 block, const void *buf, u32 size)
{
	struct ocsfs_rc_io_ctx *rctx = ctx;
	struct super_block *sb = rctx->sb;
	struct buffer_head *bh;

	bh = sb_getblk(sb, block);
	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memcpy(bh->b_data, buf, size);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);

	/* ARCH-N2: if a transaction is active, add bh to it instead of
	 * syncing directly so btree writes are crash-consistent. */
	if (rctx->txn) {
		int ret = ocsfs_txn_add_bh(rctx->txn, bh);

		brelse(bh);
		return ret;
	}
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

static int rc_bt_alloc(void *ctx, u64 *out_block)
{
	struct ocsfs_rc_io_ctx *rctx = ctx;

	/*
	 * When invoked inside a transaction (the normal case — rc_apply_delta
	 * opens one), allocate through that transaction.  Using the plain
	 * ocsfs_alloc_blocks() here would call ocsfs_txn_begin() again while the
	 * caller already holds the journal lock for the open transaction, which
	 * self-deadlocks (observed as cp + the writeback worker stuck in
	 * ocsfs_txn_begin during reflink/snapshot/dedup).
	 */
	if (rctx->txn)
		return ocsfs_alloc_blocks_txn(rctx->txn, rctx->sb, rctx->ag_no,
					      1, out_block);
	return ocsfs_alloc_blocks(rctx->sb, rctx->ag_no, 1, out_block);
}

static int rc_bt_free(void *ctx, u64 block)
{
	struct ocsfs_rc_io_ctx *rctx = ctx;

	if (rctx->txn)
		return ocsfs_free_blocks_txn(rctx->txn, block, 1);
	ocsfs_free_blocks(rctx->sb, block, 1);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * INTERNAL HELPERS
 * ═══════════════════════════════════════════════════════════════ */

static struct ocsfs_ag_info *ocsfs_block_to_ag(struct ocsfs_sb_info *sbi,
					       u64 phys_block)
{
	u32 ag_no;

	for (ag_no = 0; ag_no < sbi->s_ag_count; ag_no++) {
		struct ocsfs_ag_info *ag = &sbi->s_ags[ag_no];

		if (phys_block >= ag->block_start &&
		    phys_block < ag->block_start + ag->block_count)
			return ag;
	}
	return NULL;
}

static int rc_btree_open_or_create(struct ocsfs_btree *bt,
				   struct super_block *sb,
				   struct ocsfs_ag_info *ag,
				   struct ocsfs_rc_io_ctx *rctx)
{
	u32 bsz = OCSFS_SB(sb)->s_block_size;

	if (ag->rc_btree_root)
		return ocsfs_btree_open(bt, ag->rc_btree_root, bsz,
					rc_bt_read, rc_bt_write,
					rc_bt_alloc, rc_bt_free, rctx);

	return ocsfs_btree_create(bt, bsz, rc_bt_read, rc_bt_write,
				  rc_bt_alloc, rc_bt_free, rctx);
}

/*
 * Write updated B+ tree root back to the AG descriptor block.
 * Must be called with ag_rc_lock_res held EX.
 */
static int rc_btree_persist_root(struct super_block *sb,
				 struct ocsfs_ag_info *ag, u64 new_root,
				 struct ocsfs_txn *txn)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 off = sbi->s_ag_desc_off + (u64)ag->ag_no * sizeof(struct ocsfs_disk_ag);
	u64 blk = ocsfs_byte_to_block(sbi, off);
	struct buffer_head *bh;
	struct ocsfs_disk_ag *dag;
	int ret;

	/*
	 * Read-modify-write of the AG descriptor: we touch only ag_rc_btree_root
	 * but recompute the CRC over the whole block, so every other field must
	 * already be valid.  sb_bread (not sb_getblk) guarantees the buffer is
	 * uptodate — otherwise an uncached AG descriptor (e.g. after drop_caches)
	 * would be overwritten with garbage carrying a valid CRC, corrupting the
	 * AG ("bad magic" at next mount).
	 */
	bh = sb_bread(sb, blk);
	if (!bh)
		return -EIO;

	dag = (struct ocsfs_disk_ag *)bh->b_data;
	dag->ag_rc_btree_root = cpu_to_le64(new_root);
	dag->ag_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, dag, offsetof(struct ocsfs_disk_ag, ag_checksum)));
	mark_buffer_dirty(bh);

	if (txn) {
		ret = ocsfs_txn_add_bh(txn, bh);
		brelse(bh);
	} else {
		sync_dirty_buffer(bh);
		brelse(bh);
		ret = 0;
	}

	if (ret == 0)
		ag->rc_btree_root = new_root;
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_refcount_get(struct super_block *sb, u64 phys_block,
		       u32 *refcount_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_ag_info *ag;
	struct ocsfs_rc_io_ctx rctx;
	struct ocsfs_btree bt;
	u64 val;
	int ret;

	if (!(sbi->s_feature_incompat & OCSFS_FEATURE_INCOMPAT_RC_BTREE_PER_AG)) {
		*refcount_out = 1;
		return -EOPNOTSUPP;
	}

	ag = ocsfs_block_to_ag(sbi, phys_block);
	if (!ag) {
		*refcount_out = 0;
		return -EINVAL;
	}

	rctx.sb    = sb;
	rctx.ag_no = ag->ag_no;
	rctx.txn   = NULL;   /* read-only path: rc_bt_alloc must not see a stale txn */

	ret = ocsfs_lock_acquire(sb, &ag->ag_rc_lock_res, OCSFS_LOCK_SH);
	if (ret)
		return ret;

	if (!ag->rc_btree_root) {
		*refcount_out = 1;
		ocsfs_lock_release(sb, &ag->ag_rc_lock_res);
		return 0;
	}

	ret = ocsfs_btree_open(&bt, ag->rc_btree_root, sbi->s_block_size,
			       rc_bt_read, rc_bt_write,
			       rc_bt_alloc, rc_bt_free, &rctx);
	if (ret) {
		ocsfs_lock_release(sb, &ag->ag_rc_lock_res);
		return ret;
	}

	ret = ocsfs_btree_search(&bt, phys_block, &val);
	if (ret == -ENOENT) {
		*refcount_out = 1; /* implicit: allocated but not shared */
		ret = 0;
	} else if (ret == 0) {
		*refcount_out = (u32)val;
	}

	ocsfs_lock_release(sb, &ag->ag_rc_lock_res);
	return ret;
}

static int rc_apply_delta(struct super_block *sb, u64 phys_block,
			  int delta, u32 *result_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_ag_info *ag;
	struct ocsfs_rc_io_ctx rctx;
	struct ocsfs_btree bt;
	struct ocsfs_txn *txn;
	u64 old_val = 0;
	u32 old_count, new_count;
	bool existed;
	int ret;

	ag = ocsfs_block_to_ag(sbi, phys_block);
	if (!ag)
		return -EINVAL;

	rctx.sb    = sb;
	rctx.ag_no = ag->ag_no;

	ret = ocsfs_lock_acquire(sb, &ag->ag_rc_lock_res, OCSFS_LOCK_EX);
	if (ret)
		return ret;

	/* ARCH-N2: wrap btree writes in a journal transaction so a crash
	 * mid-split cannot leave the refcount tree in an inconsistent state. */
	txn = ocsfs_txn_begin(sb);
	if (IS_ERR(txn)) {
		ret = PTR_ERR(txn);
		goto out_unlock;
	}
	rctx.txn = txn;

	ret = rc_btree_open_or_create(&bt, sb, ag, &rctx);
	if (ret)
		goto out_abort;

	ret = ocsfs_btree_search(&bt, phys_block, &old_val);
	existed   = (ret == 0);
	old_count = existed ? (u32)old_val : 1;

	if (delta >= 0)
		new_count = old_count + (u32)delta;
	else
		new_count = (old_count > (u32)(-delta)) ?
			    old_count - (u32)(-delta) : 0;

	if (existed) {
		ret = ocsfs_btree_delete(&bt, phys_block);
		if (ret)
			goto out_abort;
	}

	if (new_count > 1) {
		ret = ocsfs_btree_insert(&bt, phys_block, (u64)new_count);
		if (ret)
			goto out_abort;
	}

	if (bt.root_block != ag->rc_btree_root) {
		ret = rc_btree_persist_root(sb, ag, bt.root_block, txn);
		if (ret)
			goto out_abort;
	}

	ret = ocsfs_txn_commit(txn);
	if (ret == 0 && result_out)
		*result_out = new_count;
	goto out_unlock;

out_abort:
	ocsfs_txn_abort(txn);
out_unlock:
	ocsfs_lock_release(sb, &ag->ag_rc_lock_res);
	return ret;
}

int ocsfs_refcount_inc(struct super_block *sb, u64 phys_block, u32 len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u32 i;

	if (!(sbi->s_feature_incompat & OCSFS_FEATURE_INCOMPAT_RC_BTREE_PER_AG))
		return -EOPNOTSUPP;

	for (i = 0; i < len; i++) {
		int ret = rc_apply_delta(sb, phys_block + i, +1, NULL);

		if (ret)
			return ret;
	}
	return 0;
}

int ocsfs_refcount_dec(struct super_block *sb, u64 phys_block, u32 len,
		       bool *should_free)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	bool all_free = true;
	u32 i;

	if (!(sbi->s_feature_incompat & OCSFS_FEATURE_INCOMPAT_RC_BTREE_PER_AG))
		return -EOPNOTSUPP;

	for (i = 0; i < len; i++) {
		u32 result = 1;
		int ret = rc_apply_delta(sb, phys_block + i, -1, &result);

		if (ret)
			return ret;
		if (result > 0)
			all_free = false;
	}
	if (should_free)
		*should_free = all_free;
	return 0;
}

/*
 * ocsfs_free_blocks_rc — refcount-aware free of an extent's physical blocks.
 *
 * For a SHARED block (refcount > 1, i.e. reflinked / deduped / snapshotted) it
 * drops one reference and leaves the block allocated for the other owners.  A
 * non-shared block is freed normally.  Callers (truncate / unlink) used to call
 * ocsfs_free_blocks() directly, which cleared the bitmap of shared canonical
 * blocks out from under their other owners (latent corruption once the block
 * was reallocated) and left the refcount entry stale so the dedup GC could
 * never reclaim it.
 *
 * MUST be called WITHOUT an open journal transaction: refcount_dec acquires the
 * AG refcount lock before beginning its own txn, the opposite order to a
 * truncate that already holds j_lock — so callers collect the extents and free
 * them after committing the btree/inode update (see ocsfs_extent_truncate).
 *
 * Fast path: a run of consecutive non-shared blocks is freed in one bulk call,
 * and on a volume with no sharing in this AG refcount_get is O(1), so the common
 * (never-shared) file keeps deleting at bulk speed.
 */
void ocsfs_free_blocks_rc(struct super_block *sb, u64 phys, u32 len)
{
	u32 i = 0;

	while (i < len) {
		u32 rc = 1;

		if (ocsfs_refcount_get(sb, phys + i, &rc) == 0 && rc > 1) {
			ocsfs_refcount_dec(sb, phys + i, 1, NULL);
			i++;
		} else {
			u32 run = 1;

			while (i + run < len) {
				u32 rc2 = 1;

				if (ocsfs_refcount_get(sb, phys + i + run,
						       &rc2) == 0 && rc2 > 1)
					break;
				run++;
			}
			ocsfs_free_blocks(sb, phys + i, run);
			i += run;
		}
	}
}

/*
 * ocsfs_refcount_init_ag — crea il B+ tree radice per un AG.
 * Chiamato da mkfs o al primo montaggio con INCOMPAT_RC_BTREE_PER_AG.
 */
int ocsfs_refcount_init_ag(struct super_block *sb, u32 ag_no)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_ag_info *ag  = &sbi->s_ags[ag_no];
	struct ocsfs_rc_io_ctx rctx = { .sb = sb, .ag_no = ag_no };
	struct ocsfs_btree bt;
	int ret;

	if (!(sbi->s_feature_incompat & OCSFS_FEATURE_INCOMPAT_RC_BTREE_PER_AG))
		return 0;

	if (ag->rc_btree_root)
		return 0; /* già inizializzato */

	ret = ocsfs_btree_create(&bt, sbi->s_block_size,
				 rc_bt_read, rc_bt_write,
				 rc_bt_alloc, rc_bt_free, &rctx);
	if (ret)
		return ret;

	return rc_btree_persist_root(sb, ag, bt.root_block, NULL);
}
