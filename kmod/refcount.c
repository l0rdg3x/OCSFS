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
};

static int rc_bt_read(void *ctx, u64 block, void *buf, u32 size)
{
	struct super_block *sb = ((struct ocsfs_rc_io_ctx *)ctx)->sb;
	struct buffer_head *bh;

	bh = sb_getblk(sb, block);
	if (!bh)
		return -EIO;
	clear_buffer_uptodate(bh);
	if (bh_read(bh, 0) < 0) {
		brelse(bh);
		return -EIO;
	}
	memcpy(buf, bh->b_data, size);
	brelse(bh);
	return 0;
}

static int rc_bt_write(void *ctx, u64 block, const void *buf, u32 size)
{
	struct super_block *sb = ((struct ocsfs_rc_io_ctx *)ctx)->sb;
	struct buffer_head *bh;

	bh = sb_getblk(sb, block);
	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memcpy(bh->b_data, buf, size);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

static int rc_bt_alloc(void *ctx, u64 *out_block)
{
	struct ocsfs_rc_io_ctx *rctx = ctx;

	return ocsfs_alloc_blocks(rctx->sb, rctx->ag_no, 1, out_block);
}

static int rc_bt_free(void *ctx, u64 block)
{
	struct ocsfs_rc_io_ctx *rctx = ctx;

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
				 struct ocsfs_ag_info *ag, u64 new_root)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 off = sbi->s_ag_desc_off + (u64)ag->ag_no * sizeof(struct ocsfs_disk_ag);
	u64 blk = ocsfs_byte_to_block(sbi, off);
	struct buffer_head *bh;
	struct ocsfs_disk_ag *dag;

	bh = sb_getblk(sb, blk);
	if (!bh)
		return -EIO;

	dag = (struct ocsfs_disk_ag *)bh->b_data;
	dag->ag_rc_btree_root = cpu_to_le64(new_root);
	dag->ag_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, dag, offsetof(struct ocsfs_disk_ag, ag_checksum)));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	ag->rc_btree_root = new_root;
	return 0;
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

	ret = rc_btree_open_or_create(&bt, sb, ag, &rctx);
	if (ret)
		goto out_unlock;

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
			goto out_persist;
	}

	if (new_count > 1) {
		ret = ocsfs_btree_insert(&bt, phys_block, (u64)new_count);
		if (ret)
			goto out_persist;
	}
	ret = 0;

out_persist:
	if (bt.root_block != ag->rc_btree_root)
		rc_btree_persist_root(sb, ag, bt.root_block);

	if (ret == 0 && result_out)
		*result_out = new_count;
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

	return rc_btree_persist_root(sb, ag, bt.root_block);
}
