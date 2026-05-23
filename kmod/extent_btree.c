// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — extent_btree.c
 * B+ tree overflow for files with more than OCSFS_INLINE_EXTENTS extents.
 *
 * Value encoding: bits[0..39]=physical_block, bits[40..61]=length, bits[62..63]=flags.
 * Max addressable: ~4 PB (40-bit phys @ 4 KiB blocks), ~16 GiB per extent (22-bit len).
 */

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
	struct buffer_head *bh   = sb_bread(ec->sb, block);

	if (!bh)
		return -EIO;
	memcpy(buf, bh->b_data, size);
	brelse(bh);
	return 0;
}

static int ext_btree_write(void *ctx, u64 block, const void *buf, u32 size)
{
	struct ext_btree_ctx *ec = ctx;
	struct buffer_head *bh   = sb_getblk(ec->sb, block);

	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memcpy(bh->b_data, buf, size);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	if (ec->txn) {
		int r = ocsfs_txn_add_bh(ec->txn, bh);
		brelse(bh);
		return r;
	}
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
	struct ocsfs_sb_info *sbi   = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);

	ec->sb  = inode->i_sb;
	ec->txn = NULL;
	return ocsfs_btree_open(bt, oi->i_extent_tree_root, sbi->s_block_size,
				ext_btree_read, ext_btree_write,
				ext_btree_alloc, ext_btree_free, ec);
}

static int ext_btree_create(struct inode *inode, struct ocsfs_btree *bt,
			    struct ext_btree_ctx *ec)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);

	ec->sb = inode->i_sb;
	return ocsfs_btree_create(bt, sbi->s_block_size,
				  ext_btree_read, ext_btree_write,
				  ext_btree_alloc, ext_btree_free, ec);
}

/* ═══════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_extent_btree_lookup(struct inode *inode, u64 logical,
			      struct ocsfs_extent *out)
{
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	u64 key, val;
	int ret;

	ret = ext_btree_open(inode, &bt, &ec);
	if (ret)
		return ret;

	ret = ocsfs_btree_search_le(&bt, logical, &key, &val);
	if (ret)
		return -ENOENT;

	if (logical >= key + ext_len(val))
		return -ENOENT;  /* hole */

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
	int ret;

	ret = ext_btree_open(inode, &bt, &ec);
	if (ret)
		return ret;

	txn = ocsfs_txn_begin(inode->i_sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);
	ec.txn = txn;

	ret = ocsfs_btree_insert(&bt, logical,
				 ext_encode(physical, len, flags));
	oi->i_extent_tree_root = bt.root_block;
	if (ret) {
		ocsfs_txn_abort(txn);
		return ret;
	}
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
			ocsfs_btree_delete(&bt, key);
			ocsfs_btree_insert(&bt, key,
					   ext_encode(ext_phys(val), keep,
						      ext_flags(val)));
			oi->i_extent_tree_root = bt.root_block;
		}
	}

	/* Delete all extents fully beyond from_block */
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
				ocsfs_btree_delete(&bt, tc.keys[i]);
				oi->i_extent_tree_root = bt.root_block;
			}
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

/* ── convert unwritten ── */

int ocsfs_extent_btree_convert_unwritten(struct inode *inode, u64 logical, u32 len)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
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

	ocsfs_btree_delete(&bt, key);
	oi->i_extent_tree_root = bt.root_block;

	if (key < logical) {
		u32 head_len = (u32)(logical - key);

		ocsfs_btree_insert(&bt, key,
				   ext_encode(phys, head_len, OCSFS_EXT_UNWRITTEN));
		oi->i_extent_tree_root = bt.root_block;
	}

	{
		u64 m_key  = (logical > key) ? logical : key;
		u64 m_phys = phys + (m_key - key);
		u64 m_end  = (cvt_end < ext_end) ? cvt_end : ext_end;
		u32 m_len  = (u32)(m_end - m_key);

		ocsfs_btree_insert(&bt, m_key,
				   ext_encode(m_phys, m_len, OCSFS_EXT_WRITTEN));
		oi->i_extent_tree_root = bt.root_block;
	}

	if (ext_end > cvt_end) {
		u64 tail_key  = cvt_end;
		u64 tail_phys = phys + (tail_key - key);
		u32 tail_len  = (u32)(ext_end - tail_key);

		ocsfs_btree_insert(&bt, tail_key,
				   ext_encode(tail_phys, tail_len,
					      OCSFS_EXT_UNWRITTEN));
		oi->i_extent_tree_root = bt.root_block;
	}

	mark_inode_dirty(inode);
	return 0;
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

	ocsfs_btree_delete(&bt, orig->logical_block);
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
	u32 i;
	int ret;

	ret = ext_btree_open(inode, &bt, &ec);
	if (ret)
		return ret;

	do {
		tc.count = 0;
		ocsfs_btree_range_scan(&bt, 0, U64_MAX, ext_trunc_collect, &tc);
		for (i = 0; i < tc.count; i++) {
			ocsfs_btree_delete(&bt, tc.keys[i]);
			oi->i_extent_tree_root = bt.root_block;
		}
	} while (tc.count > 0);

	oi->i_extent_tree_root = 0;
	mark_inode_dirty(inode);
	return 0;
}

/* ── count total blocks ── */

static int ext_count_cb(u64 key, u64 val, void *ctx)
{
	u64 *total = ctx;

	(void)key;
	*total += ext_len(val);
	return 0;
}

int ocsfs_extent_btree_count(struct inode *inode, u64 *count)
{
	struct ocsfs_btree bt;
	struct ext_btree_ctx ec;
	int ret;

	ret = ext_btree_open(inode, &bt, &ec);
	if (ret)
		return ret;

	*count = 0;
	ocsfs_btree_range_scan(&bt, 0, U64_MAX, ext_count_cb, count);
	return 0;
}
