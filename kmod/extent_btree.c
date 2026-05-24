// SPDX-License-Identifier: GPL-2.0-only
/* OCSFS — extent_btree.c: B+ tree overflow for OCSFS_INLINE_EXTENTS+.
 * Value: bits[0..39]=phys, bits[40..61]=len(22-bit), bits[62..63]=flags.
 * Max: ~4 PB phys @ 4 KiB/block; ~16 GiB per extent. */

#include "ocsfs.h"
#include "ocsfs_btree.h"

/* ── value encoding ── */

static inline u64 ext_encode(u64 phys, u32 len, u16 flags)
{
	return (phys & 0xFFFFFFFFFFULL) |
	       ((u64)(len   & 0x3FFFFFU) << 40) |
	       ((u64)(flags & 0x3U)      << 62);
}

static inline u64  ext_phys(u64 v)  { return v & 0xFFFFFFFFFFULL; }
static inline u32  ext_len(u64 v)   { return (u32)((v >> 40) & 0x3FFFFFU); }
static inline u16  ext_flags(u64 v) { return (u16)((v >> 62) & 0x3U); }

/* ── I/O callbacks ── */

struct ext_btree_ctx {
	struct super_block *sb;
	struct ocsfs_txn   *txn;
};

static int ext_btree_read(void *ctx, u64 block, void *buf, u32 size)
{
	struct ext_btree_ctx *ec = ctx;
	struct buffer_head *bh;

	if (OCSFS_SB(ec->sb)->s_clustered) {
		/*
		 * In cluster mode the page cache is not coherent across nodes.
		 * After another node takes EX, modifies, and flushes the btree
		 * nodes, our next read must bypass our stale cached copy.
		 */
		bh = sb_getblk(ec->sb, block);
		if (!bh)
			return -EIO;
		clear_buffer_uptodate(bh);
		if (bh_read(bh, 0) < 0) {
			brelse(bh);
			return -EIO;
		}
	} else {
		bh = sb_bread(ec->sb, block);
		if (!bh)
			return -EIO;
	}
	memcpy(buf, bh->b_data, size);
	brelse(bh);
	return 0;
}

static int ext_btree_write(void *ctx, u64 block, const void *buf, u32 size)
{
	struct ext_btree_ctx *ec = ctx;
	struct buffer_head *bh;
	int ret = 0;

	/*
	 * txn path: capture real BEFORE-image; in cluster mode force a fresh
	 * read so the journal records the true on-disk state, not a stale
	 * cached copy from before another node's modification.
	 */
	if (ec->txn) {
		if (OCSFS_SB(ec->sb)->s_clustered) {
			bh = sb_getblk(ec->sb, block);
			if (!bh)
				return -EIO;
			clear_buffer_uptodate(bh);
			if (bh_read(bh, 0) < 0) {
				brelse(bh);
				return -EIO;
			}
		} else {
			bh = sb_bread(ec->sb, block);
			if (!bh)
				return -EIO;
		}
	} else {
		bh = sb_getblk(ec->sb, block);
		if (!bh)
			return -EIO;
	}

	if (ec->txn) {
		ret = ocsfs_txn_add_bh(ec->txn, bh);
		if (ret) { brelse(bh); return ret; }
	}

	lock_buffer(bh);
	memcpy(bh->b_data, buf, size);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);

	if (!ec->txn)
		mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

static int ext_btree_alloc(void *ctx, u64 *out)
{
	struct ext_btree_ctx *ec = ctx;

	if (ec->txn)
		return ocsfs_alloc_blocks_txn(ec->txn, ec->sb, 0, 1, out);
	return ocsfs_alloc_blocks(ec->sb, 0, 1, out);
}

static int ext_btree_free(void *ctx, u64 block)
{
	struct ext_btree_ctx *ec = ctx;

	if (ec->txn)
		return ocsfs_free_blocks_txn(ec->txn, block, 1);
	ocsfs_free_blocks(ec->sb, block, 1);
	return 0;
}

/* ── open / create helpers ── */

static int ext_btree_open(struct inode *inode, struct ocsfs_btree *bt,
			  struct ext_btree_ctx *ec)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	ec->sb  = inode->i_sb;
	ec->txn = NULL;
	return ocsfs_btree_open(bt, oi->i_extent_tree_root,
				OCSFS_SB(inode->i_sb)->s_block_size,
				ext_btree_read, ext_btree_write,
				ext_btree_alloc, ext_btree_free, ec);
}

static int ext_btree_create(struct inode *inode, struct ocsfs_btree *bt,
			    struct ext_btree_ctx *ec)
{
	ec->sb = inode->i_sb;
	return ocsfs_btree_create(bt, OCSFS_SB(inode->i_sb)->s_block_size,
				  ext_btree_read, ext_btree_write,
				  ext_btree_alloc, ext_btree_free, ec);
}

/* ── PUBLIC API ── */

int ocsfs_extent_btree_lookup(struct inode *inode, u64 logical,
			      struct ocsfs_extent *out)
{
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	u64 key, val;
	int ret = ext_btree_open(inode, &bt, &ec);

	if (ret)
		return ret;
	ret = ocsfs_btree_search_le(&bt, logical, &key, &val);
	if (ret || logical >= key + ext_len(val))
		return -ENOENT;
	out->logical_block  = key;
	out->physical_block = ext_phys(val);
	out->length         = ext_len(val);
	out->flags          = ext_flags(val);
	return 0;
}

int ocsfs_extent_btree_insert(struct inode *inode, u64 logical, u64 physical,
			      u32 len, u16 flags)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_txn *txn;
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	int ret = ext_btree_open(inode, &bt, &ec);

	if (ret)
		return ret;
	txn = ocsfs_txn_begin(inode->i_sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);
	ec.txn = txn;
	ret = ocsfs_btree_insert(&bt, logical, ext_encode(physical, len, flags));
	if (ret) {
		ocsfs_txn_abort(txn);
		return ret;
	}
	oi->i_extent_tree_root = bt.root_block;
	ret = ocsfs_txn_commit(txn);
	if (!ret)
		mark_inode_dirty(inode);
	return ret;
}

/*
 * Migrate all inline extents to a new B+ tree.
 * On success: oi->i_extent_tree_root is set, oi->i_extent_count zeroed.
 * On failure: tree is partially built (blocks leaked) — caller must handle.
 */
int ocsfs_extent_btree_migrate(struct inode *inode)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_txn *txn;
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	int i, ret;

	txn = ocsfs_txn_begin(inode->i_sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);
	ec.txn = txn;

	ret = ext_btree_create(inode, &bt, &ec);
	if (ret)
		goto abort;

	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];
		u64 val = ext_encode(e->physical_block, e->length, e->flags);

		ret = ocsfs_btree_insert(&bt, e->logical_block, val);
		if (ret) {
			pr_err("ocsfs: inode %llu: migrate extent %d failed: %d\n",
			       oi->i_disk_ino, i, ret);
			goto abort;
		}
	}

	oi->i_extent_tree_root = bt.root_block;
	oi->i_extent_count     = 0;
	return ocsfs_txn_commit(txn);

abort:
	ocsfs_txn_abort(txn);
	return ret;
}

/* ── truncate helper ── */

struct ext_trunc_ctx {
	u64  keys[64];
	u32  count;
};

static int ext_trunc_collect(u64 key, u64 val, void *ctx)
{
	struct ext_trunc_ctx *tc = ctx;

	(void)val;
	if (tc->count < ARRAY_SIZE(tc->keys))
		tc->keys[tc->count++] = key;
	return (tc->count >= ARRAY_SIZE(tc->keys)) ? 1 : 0;
}

int ocsfs_extent_btree_truncate(struct inode *inode, u64 from_block)
{
	struct ocsfs_inode_info *oi  = OCSFS_I(inode);
	struct super_block      *sb  = inode->i_sb;
	struct ocsfs_sb_info    *sbi = OCSFS_SB(sb);
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	struct ext_trunc_ctx tc;
	struct ocsfs_txn *txn;
	u64 key, val;
	u32 i;
	int ret;

	ret = ext_btree_open(inode, &bt, &ec);
	if (ret)
		return ret;

	txn = ocsfs_txn_begin(sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);

	ec.txn = txn;

	/* Shrink any extent that straddles from_block */
	if (from_block > 0 &&
	    !ocsfs_btree_search_le(&bt, from_block - 1, &key, &val)) {
		u32 elen = ext_len(val);

		if (key + elen > from_block) {
			u32 keep  = (u32)(from_block - key);
			u32 freed = elen - keep;

			ret = ocsfs_free_blocks_txn(txn,
						    ext_phys(val) + keep, freed);
			if (ret)
				goto abort;
			inode->i_blocks -= (u64)freed * (sbi->s_block_size / 512);
			ret = ocsfs_btree_delete(&bt, key);
			if (ret)
				goto abort;
			ret = ocsfs_btree_insert(&bt, key,
						 ext_encode(ext_phys(val), keep,
							    ext_flags(val)));
			if (ret)
				goto abort;
			oi->i_extent_tree_root = bt.root_block;
		}
	}

	/* Delete all extents fully beyond from_block */
	{
		int safety = 1000;

		do {
			tc.count = 0;
			ocsfs_btree_range_scan(&bt, from_block, U64_MAX,
					       ext_trunc_collect, &tc);
			for (i = 0; i < tc.count; i++) {
				u64 v;

				if (!ocsfs_btree_search(&bt, tc.keys[i], &v)) {
					ret = ocsfs_free_blocks_txn(txn,
								    ext_phys(v),
								    ext_len(v));
					if (ret)
						goto abort;
					inode->i_blocks -= (u64)ext_len(v) *
							   (sbi->s_block_size / 512);
					ret = ocsfs_btree_delete(&bt, tc.keys[i]);
					if (ret)
						goto abort;
					oi->i_extent_tree_root = bt.root_block;
				}
			}
			if (tc.count > 0 && --safety <= 0) {
				pr_err("ocsfs: extent_btree_truncate: loop limit reached "
				       "(inode %llu) — btree may be corrupt, run fsck\n",
				       oi->i_disk_ino);
				ret = -EUCLEAN;
				goto abort;
			}
		} while (tc.count > 0);
	}

	ret = ocsfs_txn_commit(txn);
	if (!ret)
		mark_inode_dirty(inode);
	return ret;

abort:
	ocsfs_txn_abort(txn);
	return ret;
}

/* ── convert unwritten ── */

int ocsfs_extent_btree_convert_unwritten(struct inode *inode, u64 logical, u32 len)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_txn *txn;
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	u64 key, val, ext_end, cvt_end;
	u64 phys;
	u32 elen;
	u16 eflags;
	int ret;

	ret = ext_btree_open(inode, &bt, &ec);
	if (ret)
		return ret;

	ret = ocsfs_btree_search_le(&bt, logical, &key, &val);
	if (ret)
		return 0;

	eflags = ext_flags(val);
	if (!(eflags & OCSFS_EXT_UNWRITTEN))
		return 0;

	phys    = ext_phys(val);
	elen    = ext_len(val);
	ext_end = key + elen;
	cvt_end = logical + len;

	if (logical >= ext_end || cvt_end <= key)
		return 0;

	txn = ocsfs_txn_begin(inode->i_sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);
	ec.txn = txn;

	ret = ocsfs_btree_delete(&bt, key);
	if (ret)
		goto abort;
	oi->i_extent_tree_root = bt.root_block;

	if (key < logical) {
		u32 head_len = (u32)(logical - key);

		ret = ocsfs_btree_insert(&bt, key,
				   ext_encode(phys, head_len, OCSFS_EXT_UNWRITTEN));
		if (ret)
			goto abort;
		oi->i_extent_tree_root = bt.root_block;
	}

	{
		u64 m_key  = (logical > key) ? logical : key;
		u64 m_phys = phys + (m_key - key);
		u64 m_end  = (cvt_end < ext_end) ? cvt_end : ext_end;
		u32 m_len  = (u32)(m_end - m_key);

		ret = ocsfs_btree_insert(&bt, m_key,
				   ext_encode(m_phys, m_len, OCSFS_EXT_WRITTEN));
		if (ret)
			goto abort;
		oi->i_extent_tree_root = bt.root_block;
	}

	if (ext_end > cvt_end) {
		u64 tail_key  = cvt_end;
		u64 tail_phys = phys + (tail_key - key);
		u32 tail_len  = (u32)(ext_end - tail_key);

		ret = ocsfs_btree_insert(&bt, tail_key,
				   ext_encode(tail_phys, tail_len,
					      OCSFS_EXT_UNWRITTEN));
		if (ret)
			goto abort;
		oi->i_extent_tree_root = bt.root_block;
	}

	ret = ocsfs_txn_commit(txn);
	if (!ret)
		mark_inode_dirty(inode);
	return ret;

abort:
	ocsfs_txn_abort(txn);
	return ret;
}

/* ── replace for CoW: delete original, re-insert head / CoW'd / tail ── */

int ocsfs_extent_btree_replace(struct inode *inode,
				const struct ocsfs_extent *orig,
				u64 offset_in_ext, u32 cow_len, u64 new_phys)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_txn *txn;
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	int ret;

	ret = ext_btree_open(inode, &bt, &ec);
	if (ret)
		return ret;

	txn = ocsfs_txn_begin(inode->i_sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);
	ec.txn = txn;

	ret = ocsfs_btree_delete(&bt, orig->logical_block);
	if (ret)
		goto abort;
	oi->i_extent_tree_root = bt.root_block;

	if (offset_in_ext > 0) {
		ret = ocsfs_btree_insert(&bt, orig->logical_block,
				ext_encode(orig->physical_block,
					   (u32)offset_in_ext, orig->flags));
		if (ret)
			goto abort;
		oi->i_extent_tree_root = bt.root_block;
	}

	ret = ocsfs_btree_insert(&bt, orig->logical_block + offset_in_ext,
			ext_encode(new_phys, cow_len, OCSFS_EXT_WRITTEN));
	if (ret)
		goto abort;
	oi->i_extent_tree_root = bt.root_block;

	if (offset_in_ext + cow_len < orig->length) {
		u64 tail_log  = orig->logical_block + offset_in_ext + cow_len;
		u64 tail_phys = orig->physical_block + offset_in_ext + cow_len;
		u32 tail_len  = orig->length - (u32)offset_in_ext - cow_len;

		ret = ocsfs_btree_insert(&bt, tail_log,
				ext_encode(tail_phys, tail_len, orig->flags));
		if (ret)
			goto abort;
		oi->i_extent_tree_root = bt.root_block;
	}

	ret = ocsfs_txn_commit(txn);
	if (!ret)
		mark_inode_dirty(inode);
	return ret;

abort:
	ocsfs_txn_abort(txn);
	return ret;
}

/* ── clear: remove all entries from the B+ tree WITHOUT freeing data blocks ── */

int ocsfs_extent_btree_clear(struct inode *inode)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	struct ext_trunc_ctx tc;
	struct ocsfs_txn *txn;
	int ret = ext_btree_open(inode, &bt, &ec);
	u32 i;

	if (ret)
		return ret;

	txn = ocsfs_txn_begin(inode->i_sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);
	ec.txn = txn;

	do {
		tc.count = 0;
		ocsfs_btree_range_scan(&bt, 0, U64_MAX, ext_trunc_collect, &tc);
		for (i = 0; i < tc.count; i++) {
			ret = ocsfs_btree_delete(&bt, tc.keys[i]);
			if (ret)
				goto abort;
			oi->i_extent_tree_root = bt.root_block;
		}
	} while (tc.count > 0);

	oi->i_extent_tree_root = 0;
	ret = ocsfs_txn_commit(txn);
	if (!ret)
		mark_inode_dirty(inode);
	return ret;

abort:
	ocsfs_txn_abort(txn);
	return ret;
}

/* ── iterate / count ── */

struct ext_iter_state { ocsfs_extent_iter_fn fn; void *ctx; };

static int ext_iter_wrap(u64 k, u64 v, void *c)
{
	struct ext_iter_state *s = c;
	return s->fn(k, ext_phys(v), ext_len(v), ext_flags(v), s->ctx);
}

int ocsfs_extent_btree_iterate(struct inode *inode,
			       ocsfs_extent_iter_fn fn, void *ctx)
{
	struct ext_iter_state s = { fn, ctx };
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	int ret = ext_btree_open(inode, &bt, &ec);

	if (ret)
		return ret;
	ocsfs_btree_range_scan(&bt, 0, U64_MAX, ext_iter_wrap, &s);
	return 0;
}

/*
 * Return the physical block just past the last extent in the btree.
 * Uses a floor-search for key=U64_MAX — finds the entry with the
 * largest logical_block in O(log n) without a full scan.
 * Returns 0 if the btree is empty or on error (caller falls back to no-goal).
 */
u64 ocsfs_extent_btree_goal_block(struct inode *inode)
{
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	u64 key = 0, val = 0;

	if (ext_btree_open(inode, &bt, &ec))
		return 0;
	if (ocsfs_btree_search_le(&bt, U64_MAX, &key, &val))
		return 0;
	return ext_phys(val) + ext_len(val);
}

static int ext_count_cb(u64 key, u64 val, void *ctx)
	{ (void)key; *(u64 *)ctx += ext_len(val); return 0; }

int ocsfs_extent_btree_count(struct inode *inode, u64 *count)
{
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	int ret = ext_btree_open(inode, &bt, &ec);

	if (ret)
		return ret;
	*count = 0;
	ocsfs_btree_range_scan(&bt, 0, U64_MAX, ext_count_cb, count);
	return 0;
}

/* ── punch_hole: free extents overlapping [start_block, end_block) ── */

int ocsfs_extent_btree_punch_hole(struct inode *inode,
				  u64 start_block, u64 end_block)
{
	struct ocsfs_inode_info *oi  = OCSFS_I(inode);
	struct super_block      *sb  = inode->i_sb;
	struct ocsfs_sb_info    *sbi = OCSFS_SB(sb);
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	struct ext_trunc_ctx tc;
	struct ocsfs_txn *txn;
	u64 search_from;
	int safety = 1000;
	u32 i;
	int ret;

	ret = ext_btree_open(inode, &bt, &ec);
	if (ret)
		return ret;

	txn = ocsfs_txn_begin(sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);
	ec.txn = txn;

	/* search_from: may need to find an extent starting just before start_block */
	search_from = start_block > 0 ? start_block - 1 : 0;

	do {
		u64 key, val;

		tc.count = 0;
		ocsfs_btree_range_scan(&bt, search_from, end_block,
				       ext_trunc_collect, &tc);
		if (tc.count == 0)
			break;

		for (i = 0; i < tc.count; i++) {
			if (ocsfs_btree_search(&bt, tc.keys[i], &val))
				continue;

			key = tc.keys[i];
			{
				u64 ext_end = key + ext_len(val);
				u64 phys    = ext_phys(val);
				u16 flags   = ext_flags(val);

				/* Skip extents entirely before start_block */
				if (ext_end <= start_block)
					continue;

				ret = ocsfs_btree_delete(&bt, key);
				if (ret)
					goto abort;
				oi->i_extent_tree_root = bt.root_block;

				/* Head portion: key .. start_block */
				if (key < start_block) {
					u32 keep = (u32)(start_block - key);

					ret = ocsfs_btree_insert(&bt, key,
							ext_encode(phys, keep, flags));
					if (ret)
						goto abort;
					oi->i_extent_tree_root = bt.root_block;
					/* free only the punched part */
					phys += keep;
				}

				/* Tail portion: end_block .. ext_end */
				if (ext_end > end_block) {
					u64 tail_off  = end_block - key;
					u32 tail_len  = (u32)(ext_end - end_block);
					u64 tail_phys = ext_phys(val) + tail_off;

					ret = ocsfs_btree_insert(&bt, end_block,
							ext_encode(tail_phys, tail_len, flags));
					if (ret)
						goto abort;
					oi->i_extent_tree_root = bt.root_block;
					/* free only up to end_block */
				}

				/* Free the punched physical blocks */
				{
					u64 free_start = (key < start_block)
							? start_block : key;
					u64 free_end   = (ext_end > end_block)
							? end_block : ext_end;
					u32 free_len   = (u32)(free_end - free_start);

					if (free_len && !(flags & OCSFS_EXT_UNWRITTEN)) {
						ret = ocsfs_free_blocks_txn(txn,
								phys, free_len);
						if (ret)
							goto abort;
						inode->i_blocks -= (u64)free_len *
								   (sbi->s_block_size / 512);
					}
				}
			}
		}
		if (--safety <= 0) {
			ret = -EUCLEAN;
			goto abort;
		}
	} while (tc.count > 0);

	ret = ocsfs_txn_commit(txn);
	if (!ret)
		mark_inode_dirty(inode);
	return ret;

abort:
	ocsfs_txn_abort(txn);
	return ret;
}

/* ── zero_range: mark extents UNWRITTEN in [start_block, end_block) ── */

int ocsfs_extent_btree_zero_range(struct inode *inode,
				  u64 start_block, u64 end_block)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct super_block      *sb = inode->i_sb;
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	struct ext_trunc_ctx tc;
	struct ocsfs_txn *txn;
	int safety = 1000;
	u32 i;
	int ret;

	ret = ext_btree_open(inode, &bt, &ec);
	if (ret)
		return ret;

	txn = ocsfs_txn_begin(sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);
	ec.txn = txn;

	do {
		u64 key, val;

		tc.count = 0;
		ocsfs_btree_range_scan(&bt,
				       start_block > 0 ? start_block - 1 : 0,
				       end_block, ext_trunc_collect, &tc);
		if (tc.count == 0)
			break;

		for (i = 0; i < tc.count; i++) {
			if (ocsfs_btree_search(&bt, tc.keys[i], &val))
				continue;

			key = tc.keys[i];
			{
				u64 ext_end = key + ext_len(val);
				u64 phys    = ext_phys(val);

				if (ext_end <= start_block)
					continue;

				ret = ocsfs_btree_delete(&bt, key);
				if (ret)
					goto abort;
				oi->i_extent_tree_root = bt.root_block;

				/* Head before start_block: keep as-is */
				if (key < start_block) {
					u32 hlen = (u32)(start_block - key);

					ret = ocsfs_btree_insert(&bt, key,
							ext_encode(phys, hlen, ext_flags(val)));
					if (ret)
						goto abort;
					oi->i_extent_tree_root = bt.root_block;
				}

				/* Middle: mark UNWRITTEN */
				{
					u64 m_key  = (key > start_block) ? key : start_block;
					u64 m_phys = phys + (m_key - key);
					u64 m_end  = (ext_end < end_block) ? ext_end : end_block;
					u32 m_len  = (u32)(m_end - m_key);

					ret = ocsfs_btree_insert(&bt, m_key,
							ext_encode(m_phys, m_len,
								   OCSFS_EXT_UNWRITTEN));
					if (ret)
						goto abort;
					oi->i_extent_tree_root = bt.root_block;
				}

				/* Tail after end_block: keep as-is */
				if (ext_end > end_block) {
					u64 t_off  = end_block - key;
					u64 t_phys = phys + t_off;
					u32 t_len  = (u32)(ext_end - end_block);

					ret = ocsfs_btree_insert(&bt, end_block,
							ext_encode(t_phys, t_len, ext_flags(val)));
					if (ret)
						goto abort;
					oi->i_extent_tree_root = bt.root_block;
				}
			}
		}
		if (--safety <= 0) {
			ret = -EUCLEAN;
			goto abort;
		}
	} while (tc.count > 0);

	ret = ocsfs_txn_commit(txn);
	if (!ret)
		mark_inode_dirty(inode);
	return ret;

abort:
	ocsfs_txn_abort(txn);
	return ret;
}
