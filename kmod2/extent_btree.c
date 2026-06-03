// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — extent_btree.c
 * Per-inode B+tree extent map for large/fragmented files (Plan 2b). When the
 * 16 inline extents overflow, the whole map spills here (i_extent_tree_root).
 * Leaf nodes (level 0) hold sorted, non-overlapping extent records and are
 * chained left-to-right (en_next) for O(1) successor lookup; internal nodes
 * hold {min_logical, child} pointers. Insert uses proactive top-down splitting
 * (CLRS B-tree style) so it never has to back-track. All node mutations are
 * journaled (own transaction if the caller has none); on failure the txn is
 * aborted and the in-core inode reloaded. Single-node.
 */
#include "ocsfs.h"

static inline struct ocsfs2_disk_ext_rec *RECS(struct ocsfs2_disk_ext_node *n)
{
	return (struct ocsfs2_disk_ext_rec *)n->en_body;
}
static inline struct ocsfs2_disk_ext_ptr *PTRS(struct ocsfs2_disk_ext_node *n)
{
	return (struct ocsfs2_disk_ext_ptr *)n->en_body;
}

static void rec_load(const struct ocsfs2_disk_ext_rec *r, struct ocsfs2_extent *e)
{
	e->logical  = le64_to_cpu(r->er_logical);
	e->physical = le64_to_cpu(r->er_physical);
	e->length   = le32_to_cpu(r->er_length);
	e->flags    = le16_to_cpu(r->er_flags);
}
static void rec_store(struct ocsfs2_disk_ext_rec *r, u64 l, u64 p, u32 len, u16 fl)
{
	r->er_logical  = cpu_to_le64(l);
	r->er_physical = cpu_to_le64(p);
	r->er_length   = cpu_to_le32(len);
	r->er_flags    = cpu_to_le16(fl);
	r->er_pad      = 0;
}

static u32 node_crc(const struct ocsfs2_disk_ext_node *n)
{
	return ocsfs2_crc32c(~0U, n,
			     offsetof(struct ocsfs2_disk_ext_node, en_checksum));
}

static struct buffer_head *node_read(struct super_block *sb, u64 blk)
{
	struct buffer_head *bh = sb_bread(sb, blk);
	struct ocsfs2_disk_ext_node *n;

	if (!bh)
		return NULL;
	n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	if (le32_to_cpu(n->en_magic) != OCSFS2_EXT_NODE_MAGIC ||
	    node_crc(n) != le32_to_cpu(n->en_checksum) ||
	    le16_to_cpu(n->en_nr) > OCSFS2_EXT_LEAF_MAX) {
		pr_err_ratelimited("ocsfs2: extent node %llu invalid\n",
				   (unsigned long long)blk);
		brelse(bh);
		return NULL;
	}
	return bh;
}

/* Finalise a modified node: recompute crc, dirty, sync if outside a txn. */
static void node_finish(struct super_block *sb, struct buffer_head *bh)
{
	struct ocsfs2_disk_ext_node *n = (struct ocsfs2_disk_ext_node *)bh->b_data;

	n->en_checksum = cpu_to_le32(node_crc(n));
	if (!ocsfs2_current_txn()) {       /* in a txn the journal owns writeback */
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
	}
}

/* Allocate a fresh node block; returns a held, enrolled bh (caller fills). */
static int node_alloc(struct inode *inode, u16 level, struct buffer_head **out)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct ocsfs2_disk_ext_node *n;
	u64 blk;
	int ret;

	ret = ocsfs2_alloc_blocks(sb, OCSFS2_I(inode)->i_ag, 1, &blk);
	if (ret)
		return ret;
	bh = sb_getblk(sb, blk);
	if (!bh) {
		ocsfs2_free_blocks(sb, blk, 1);
		return -EIO;
	}
	lock_buffer(bh);
	memset(bh->b_data, 0, sb->s_blocksize);
	n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	n->en_magic = cpu_to_le32(OCSFS2_EXT_NODE_MAGIC);
	n->en_level = cpu_to_le16(level);
	n->en_nr = 0;
	n->en_next = 0;
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	ocsfs2_jbuf(bh);   /* enrol so the after-image is journaled */
	*out = bh;
	return 0;
}

static inline u64 node_blk(struct buffer_head *bh) { return bh->b_blocknr; }

/* Child index whose subtree covers @lblk: last ptr with ep_logical <= lblk,
 * clamped to 0 (keys below ptr[0] belong to the leftmost child). */
static u16 child_index(struct ocsfs2_disk_ext_node *n, u64 lblk)
{
	struct ocsfs2_disk_ext_ptr *p = PTRS(n);
	u16 nr = le16_to_cpu(n->en_nr), i;

	for (i = 1; i < nr; i++)
		if (le64_to_cpu(p[i].ep_logical) > lblk)
			break;
	return i - 1;
}

/* ── find ── */

int ocsfs2_ext_tree_find(struct inode *inode, u64 lblk,
			 struct ocsfs2_extent *cover, u64 *next_logical)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh = node_read(sb, OCSFS2_I(inode)->i_extent_tree_root);
	struct ocsfs2_disk_ext_node *n;
	u64 nextlog = U64_MAX;
	int ret = -ENOENT;
	u16 nr, j;

	if (!bh)
		return -EIO;
	n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	while (le16_to_cpu(n->en_level) > 0) {
		u64 child = le64_to_cpu(PTRS(n)[child_index(n, lblk)].ep_child);
		struct buffer_head *nb = node_read(sb, child);

		brelse(bh);
		if (!nb)
			return -EIO;
		bh = nb;
		n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	}

	nr = le16_to_cpu(n->en_nr);
	for (j = 0; j < nr; j++) {
		struct ocsfs2_extent e;

		rec_load(&RECS(n)[j], &e);
		if (lblk >= e.logical && lblk < e.logical + e.length) {
			if (cover)
				*cover = e;
			ret = 0;
			goto out;
		}
		if (e.logical > lblk) {
			nextlog = e.logical;
			goto out;
		}
	}
	/* successor not in this leaf: first record of the next leaf */
	if (n->en_next) {
		struct buffer_head *nb = node_read(sb, le64_to_cpu(n->en_next));

		if (nb) {
			struct ocsfs2_disk_ext_node *nn =
				(struct ocsfs2_disk_ext_node *)nb->b_data;
			if (le16_to_cpu(nn->en_nr) > 0)
				nextlog = le64_to_cpu(RECS(nn)[0].er_logical);
			brelse(nb);
		}
	}
out:
	brelse(bh);
	if (next_logical)
		*next_logical = nextlog;
	return ret;
}

/* ── leaf insert (sorted, merging contiguous neighbours) ── */
static void leaf_insert_merge(struct ocsfs2_disk_ext_node *n, u64 logical,
			      u64 phys, u32 len, u16 flags)
{
	struct ocsfs2_disk_ext_rec *r = RECS(n);
	u16 nr = le16_to_cpu(n->en_nr), pos, i;

	for (pos = 0; pos < nr; pos++)
		if (le64_to_cpu(r[pos].er_logical) > logical)
			break;

	/* merge with previous */
	if (pos > 0) {
		struct ocsfs2_extent p;

		rec_load(&r[pos - 1], &p);
		if (p.flags == flags && p.logical + p.length == logical &&
		    p.physical + p.length == phys) {
			rec_store(&r[pos - 1], p.logical, p.physical,
				  p.length + len, flags);
			return;
		}
	}
	/* merge with next */
	if (pos < nr) {
		struct ocsfs2_extent nx;

		rec_load(&r[pos], &nx);
		if (nx.flags == flags && logical + len == nx.logical &&
		    phys + len == nx.physical) {
			rec_store(&r[pos], logical, phys, nx.length + len, flags);
			return;
		}
	}
	for (i = nr; i > pos; i--)
		r[i] = r[i - 1];
	rec_store(&r[pos], logical, phys, len, flags);
	n->en_nr = cpu_to_le16(nr + 1);
}

/* Split parent's full child @i into two; parent gains one pointer (it has
 * room — proactive splitting guarantees it). */
static int split_child(struct inode *inode, struct buffer_head *pbh, u16 i)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs2_disk_ext_node *p = (struct ocsfs2_disk_ext_node *)pbh->b_data;
	struct ocsfs2_disk_ext_ptr *pp = PTRS(p);
	struct buffer_head *cbh, *sbh;
	struct ocsfs2_disk_ext_node *c, *s;
	u16 pnr = le16_to_cpu(p->en_nr), level, cnr, half, k;
	u64 sep;
	int ret;

	cbh = node_read(sb, le64_to_cpu(pp[i].ep_child));
	if (!cbh)
		return -EIO;
	c = (struct ocsfs2_disk_ext_node *)cbh->b_data;
	level = le16_to_cpu(c->en_level);
	cnr = le16_to_cpu(c->en_nr);
	half = cnr / 2;

	ret = node_alloc(inode, level, &sbh);
	if (ret) {
		brelse(cbh);
		return ret;
	}
	s = (struct ocsfs2_disk_ext_node *)sbh->b_data;
	ocsfs2_jbuf(cbh);
	ocsfs2_jbuf(pbh);

	if (level == 0) {
		for (k = half; k < cnr; k++)
			RECS(s)[k - half] = RECS(c)[k];
		s->en_nr = cpu_to_le16(cnr - half);
		c->en_nr = cpu_to_le16(half);
		s->en_next = c->en_next;
		c->en_next = cpu_to_le64(node_blk(sbh));
		sep = le64_to_cpu(RECS(s)[0].er_logical);
	} else {
		for (k = half; k < cnr; k++)
			PTRS(s)[k - half] = PTRS(c)[k];
		s->en_nr = cpu_to_le16(cnr - half);
		c->en_nr = cpu_to_le16(half);
		sep = le64_to_cpu(PTRS(s)[0].ep_logical);
	}
	node_finish(sb, cbh);
	node_finish(sb, sbh);

	for (k = pnr; k > i + 1; k--)
		pp[k] = pp[k - 1];
	pp[i + 1].ep_logical = cpu_to_le64(sep);
	pp[i + 1].ep_child = cpu_to_le64(node_blk(sbh));
	p->en_nr = cpu_to_le16(pnr + 1);
	node_finish(sb, pbh);

	brelse(cbh);
	brelse(sbh);
	return 0;
}

static bool node_full(struct ocsfs2_disk_ext_node *n)
{
	u16 max = le16_to_cpu(n->en_level) == 0 ? OCSFS2_EXT_LEAF_MAX
						: OCSFS2_EXT_INT_MAX;
	return le16_to_cpu(n->en_nr) >= max;
}

static int insert_nonfull(struct inode *inode, struct buffer_head *bh,
			  u64 logical, u64 phys, u32 len, u16 flags)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs2_disk_ext_node *n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	struct buffer_head *cbh;
	u16 i;
	int ret;

	if (le16_to_cpu(n->en_level) == 0) {
		ocsfs2_jbuf(bh);
		leaf_insert_merge(n, logical, phys, len, flags);
		node_finish(sb, bh);
		return 0;
	}

	i = child_index(n, logical);
	cbh = node_read(sb, le64_to_cpu(PTRS(n)[i].ep_child));
	if (!cbh)
		return -EIO;
	if (node_full((struct ocsfs2_disk_ext_node *)cbh->b_data)) {
		ret = split_child(inode, bh, i);
		if (ret) {
			brelse(cbh);
			return ret;
		}
		/* re-evaluate: the key may now belong to the new right sibling */
		if (le64_to_cpu(PTRS(n)[i + 1].ep_logical) <= logical) {
			brelse(cbh);
			i++;
			cbh = node_read(sb, le64_to_cpu(PTRS(n)[i].ep_child));
			if (!cbh)
				return -EIO;
		}
	}
	ret = insert_nonfull(inode, cbh, logical, phys, len, flags);
	brelse(cbh);
	return ret;
}

/* core insert assuming a tree exists and caller manages the txn */
static int tree_insert_locked(struct inode *inode, u64 logical, u64 phys,
			      u32 len, u16 flags)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	struct buffer_head *rbh = node_read(sb, oi->i_extent_tree_root);
	struct ocsfs2_disk_ext_node *r;
	int ret;

	if (!rbh)
		return -EIO;
	r = (struct ocsfs2_disk_ext_node *)rbh->b_data;
	if (node_full(r)) {
		struct buffer_head *nr_bh;
		struct ocsfs2_disk_ext_node *nr;
		u64 minlog = le16_to_cpu(r->en_level) == 0 ?
			le64_to_cpu(RECS(r)[0].er_logical) :
			le64_to_cpu(PTRS(r)[0].ep_logical);

		ret = node_alloc(inode, le16_to_cpu(r->en_level) + 1, &nr_bh);
		if (ret) { brelse(rbh); return ret; }
		nr = (struct ocsfs2_disk_ext_node *)nr_bh->b_data;
		PTRS(nr)[0].ep_logical = cpu_to_le64(minlog);
		PTRS(nr)[0].ep_child = cpu_to_le64(node_blk(rbh));
		nr->en_nr = cpu_to_le16(1);
		node_finish(sb, nr_bh);
		oi->i_extent_tree_root = node_blk(nr_bh);
		ret = ocsfs2_write_inode_block(inode);   /* persist new root */
		if (!ret)
			ret = split_child(inode, nr_bh, 0);
		brelse(rbh);
		rbh = nr_bh;
		if (ret) { brelse(rbh); return ret; }
	}
	ret = insert_nonfull(inode, rbh, logical, phys, len, flags);
	brelse(rbh);
	return ret;
}

/* ── txn wrapper for the public mutators ── */
#define TREE_TXN_BEGIN(sb) \
	bool _own = !ocsfs2_current_txn(); \
	struct ocsfs2_txn *_txn = NULL; \
	if (_own) { _txn = ocsfs2_txn_begin(sb); if (!_txn) return -ENOMEM; }
#define TREE_TXN_END(inode, ret) \
	do { if (_own) { \
		if (ret) { ocsfs2_txn_abort(_txn); ocsfs2_reload_extents(inode); } \
		else ret = ocsfs2_txn_commit(_txn); \
	} } while (0)

int ocsfs2_ext_tree_insert(struct inode *inode, u64 logical, u64 phys,
			   u32 len, u16 flags)
{
	struct super_block *sb = inode->i_sb;
	int ret;
	TREE_TXN_BEGIN(sb);
	ret = tree_insert_locked(inode, logical, phys, len, flags);
	TREE_TXN_END(inode, ret);
	return ret;
}

/* migrate the 16 inline extents into a new leaf, then insert the overflow rec */
int ocsfs2_extent_spill(struct inode *inode, u64 logical, u64 phys, u32 len,
			u16 flags)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	struct buffer_head *bh;
	struct ocsfs2_disk_ext_node *n;
	int ret;
	u16 i;
	TREE_TXN_BEGIN(sb);

	ret = node_alloc(inode, 0, &bh);
	if (ret)
		goto done;
	n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	for (i = 0; i < oi->i_extent_count; i++)
		rec_store(&RECS(n)[i], oi->i_extents[i].logical,
			  oi->i_extents[i].physical, oi->i_extents[i].length,
			  oi->i_extents[i].flags);
	n->en_nr = cpu_to_le16(oi->i_extent_count);
	node_finish(sb, bh);
	brelse(bh);

	oi->i_extent_tree_root = node_blk(bh);
	oi->i_extent_count = 0;             /* inline now empty; tree is authoritative */
	ret = ocsfs2_write_inode_block(inode);
	if (ret)
		goto done;
	ret = tree_insert_locked(inode, logical, phys, len, flags);
done:
	TREE_TXN_END(inode, ret);
	return ret;
}

/* Migrate the 16 inline extents into a fresh leaf and switch the inode to tree
 * mode WITHOUT inserting a record. Lets the punch/remap split paths grow past
 * the inline slot limit (they would otherwise fail with a spurious -ENOSPC on a
 * heavily fragmented file even with the volume nearly empty). After this the
 * extent count is effectively unbounded — same as the write/insert path which
 * already spills via ocsfs2_extent_spill. */
int ocsfs2_extent_spill_only(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	struct buffer_head *bh;
	struct ocsfs2_disk_ext_node *n;
	u64 root;
	int ret;
	u16 i;
	TREE_TXN_BEGIN(sb);

	ret = node_alloc(inode, 0, &bh);
	if (ret)
		goto done;
	n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	for (i = 0; i < oi->i_extent_count; i++)
		rec_store(&RECS(n)[i], oi->i_extents[i].logical,
			  oi->i_extents[i].physical, oi->i_extents[i].length,
			  oi->i_extents[i].flags);
	n->en_nr = cpu_to_le16(oi->i_extent_count);
	root = node_blk(bh);
	node_finish(sb, bh);
	brelse(bh);

	oi->i_extent_tree_root = root;
	oi->i_extent_count = 0;             /* inline now empty; tree authoritative */
	ret = ocsfs2_write_inode_block(inode);
done:
	TREE_TXN_END(inode, ret);
	return ret;
}

/* ── descend to the leaf covering @lblk (no split); returns held bh ── */
static struct buffer_head *leaf_for(struct inode *inode, u64 lblk)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh = node_read(sb, OCSFS2_I(inode)->i_extent_tree_root);
	struct ocsfs2_disk_ext_node *n;

	while (bh) {
		n = (struct ocsfs2_disk_ext_node *)bh->b_data;
		if (le16_to_cpu(n->en_level) == 0)
			return bh;
		{
			u64 child = le64_to_cpu(PTRS(n)[child_index(n, lblk)].ep_child);

			brelse(bh);
			bh = node_read(sb, child);
		}
	}
	return NULL;
}

int ocsfs2_ext_tree_update_phys(struct inode *inode, u64 logical, u32 len,
				u64 new_phys, u16 flags)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct ocsfs2_disk_ext_node *n;
	u16 nr, j;
	int ret = -ENOENT;
	TREE_TXN_BEGIN(sb);

	bh = leaf_for(inode, logical);
	if (!bh) { ret = -EIO; goto done; }
	n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	nr = le16_to_cpu(n->en_nr);
	for (j = 0; j < nr; j++) {
		if (le64_to_cpu(RECS(n)[j].er_logical) == logical &&
		    le32_to_cpu(RECS(n)[j].er_length) == len) {
			ocsfs2_jbuf(bh);
			rec_store(&RECS(n)[j], logical, new_phys, len, flags);
			node_finish(sb, bh);
			ret = 0;
			break;
		}
	}
	brelse(bh);
done:
	TREE_TXN_END(inode, ret);
	return ret;
}

int ocsfs2_ext_tree_remap_range(struct inode *inode, u64 logical, u32 len,
				u64 new_phys, u16 new_flags)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct ocsfs2_disk_ext_node *n;
	struct ocsfs2_extent c;
	u64 end = logical + len, rstart, rend;
	u32 head;
	u16 nr, j;
	int ret = -ENOENT;
	bool have = false;
	TREE_TXN_BEGIN(sb);

	bh = leaf_for(inode, logical);
	if (!bh) { ret = -EIO; goto done; }
	n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	nr = le16_to_cpu(n->en_nr);
	for (j = 0; j < nr; j++) {
		rec_load(&RECS(n)[j], &c);
		if (logical >= c.logical && end <= c.logical + c.length) {
			have = true;
			break;
		}
	}
	if (!have) { brelse(bh); ret = -ENOENT; goto done; }

	rstart = c.logical;
	rend = c.logical + c.length;
	head = (u32)(logical - rstart);
	ocsfs2_jbuf(bh);
	if (head) {
		rec_store(&RECS(n)[j], rstart, c.physical, head, c.flags);
	} else {
		u16 k;

		for (k = j; k < nr - 1; k++)
			RECS(n)[k] = RECS(n)[k + 1];
		n->en_nr = cpu_to_le16(nr - 1);
	}
	node_finish(sb, bh);
	brelse(bh);

	ret = tree_insert_locked(inode, logical, new_phys, len, new_flags);
	if (!ret && rend > end)
		ret = tree_insert_locked(inode, end,
					 c.physical + (end - rstart),
					 (u32)(rend - end), c.flags);
done:
	TREE_TXN_END(inode, ret);
	return ret;
}

/* Free only the node blocks of the tree rooted at @blk (NOT the data extents) —
 * used when rebuilding the tree structure after a delete. */
static void free_tree_nodes(struct inode *inode, u64 blk)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh = node_read(sb, blk);
	struct ocsfs2_disk_ext_node *n;
	u16 nr, j;

	if (!bh)
		return;
	n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	nr = le16_to_cpu(n->en_nr);
	if (le16_to_cpu(n->en_level) > 0)
		for (j = 0; j < nr; j++)
			free_tree_nodes(inode, le64_to_cpu(PTRS(n)[j].ep_child));
	brelse(bh);
	ocsfs2_free_blocks(sb, blk, 1);
}

/* A2: a delete (punch/truncate) that empties a leaf leaves the internal routing
 * keys stale — a later wide insert into that key-range would split-brain (reads
 * past the first leaf see a hole). The B+tree has no delete-time rebalance, so
 * rebuild it from its leaf chain, which stays reliable (en_next is intact and
 * the leftmost-child descent doesn't depend on the stale keys): collect every
 * surviving record, free the node blocks (not the data), then re-insert via the
 * normal path (clean inline map or freshly-built tree with correct routing).
 * Caller holds i_meta_lock and an open txn. */
static int ext_tree_collapse(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	struct ocsfs2_extent *recs;
	struct buffer_head *bh;
	int cap = 512, nrec = 0, i, ret = 0;
	u64 root = oi->i_extent_tree_root;

	if (!root)
		return 0;
	recs = kmalloc_array(cap, sizeof(*recs), GFP_NOFS);
	if (!recs)
		return -ENOMEM;

	/* descend to the leftmost leaf (child 0 at every level) */
	bh = node_read(sb, root);
	while (bh && le16_to_cpu(((struct ocsfs2_disk_ext_node *)bh->b_data)->en_level) > 0) {
		struct ocsfs2_disk_ext_node *n =
			(struct ocsfs2_disk_ext_node *)bh->b_data;
		u64 child = le64_to_cpu(PTRS(n)[0].ep_child);

		brelse(bh);
		bh = node_read(sb, child);
	}
	/* walk the leaf chain, collecting every record in order */
	while (bh) {
		struct ocsfs2_disk_ext_node *n =
			(struct ocsfs2_disk_ext_node *)bh->b_data;
		u64 next = le64_to_cpu(n->en_next);
		u16 j, nr = le16_to_cpu(n->en_nr);

		for (j = 0; j < nr; j++) {
			if (nrec == cap) {
				struct ocsfs2_extent *t =
					krealloc(recs, (size_t)cap * 2 * sizeof(*recs),
						 GFP_NOFS);
				if (!t) { brelse(bh); ret = -ENOMEM; goto out; }
				recs = t; cap *= 2;
			}
			rec_load(&RECS(n)[j], &recs[nrec++]);
		}
		brelse(bh);
		bh = next ? node_read(sb, next) : NULL;
	}

	/* tear down the node blocks (data untouched) and rebuild from scratch */
	free_tree_nodes(inode, root);
	oi->i_extent_tree_root = 0;
	oi->i_extent_count = 0;
	for (i = 0; i < nrec; i++) {
		ret = ocsfs2_extent_insert(inode, recs[i].logical,
					   recs[i].physical, recs[i].length,
					   recs[i].flags);
		if (ret)
			goto out;
	}
	ret = ocsfs2_write_inode_block(inode);
out:
	kfree(recs);
	return ret;
}

int ocsfs2_ext_tree_punch_range(struct inode *inode, u64 lblk, u64 end)
{
	struct super_block *sb = inode->i_sb;
	u32 spb = sb->s_blocksize / 512;
	struct buffer_head *bh;
	bool tail_valid = false, stop = false, emptied = false;
	u64 tail_log = 0, tail_phys = 0;
	u32 tail_len = 0;
	u16 tail_flags = 0;
	int ret = 0;
	TREE_TXN_BEGIN(sb);

	bh = leaf_for(inode, lblk);
	while (bh && !stop) {
		struct ocsfs2_disk_ext_node *n =
			(struct ocsfs2_disk_ext_node *)bh->b_data;
		u64 next = le64_to_cpu(n->en_next);
		u16 j = 0;

		ocsfs2_jbuf(bh);
		while (j < le16_to_cpu(n->en_nr)) {
			struct ocsfs2_extent e;
			u64 rs, re;

			rec_load(&RECS(n)[j], &e);
			rs = e.logical;
			re = e.logical + e.length;
			if (re <= lblk) { j++; continue; }
			if (rs >= end) { stop = true; break; }

			if (rs >= lblk && re <= end) {              /* full */
				u16 k;

				ocsfs2_free_blocks_rc(sb, e.physical, e.length);
				inode->i_blocks -= (u64)e.length * spb;
				for (k = j; k < le16_to_cpu(n->en_nr) - 1; k++)
					RECS(n)[k] = RECS(n)[k + 1];
				n->en_nr = cpu_to_le16(le16_to_cpu(n->en_nr) - 1);
				continue;
			}
			if (rs < lblk && re > end) {                /* straddles both */
				ocsfs2_free_blocks_rc(sb, e.physical + (lblk - rs),
						      (u32)(end - lblk));
				inode->i_blocks -= (u64)(end - lblk) * spb;
				tail_valid = true;
				tail_log = end;
				tail_phys = e.physical + (end - rs);
				tail_len = (u32)(re - end);
				tail_flags = e.flags;
				rec_store(&RECS(n)[j], rs, e.physical,
					  (u32)(lblk - rs), e.flags);
				stop = true;
				break;
			}
			if (rs < lblk) {                            /* overlaps start */
				ocsfs2_free_blocks_rc(sb, e.physical + (lblk - rs),
						      (u32)(re - lblk));
				inode->i_blocks -= (u64)(re - lblk) * spb;
				rec_store(&RECS(n)[j], rs, e.physical,
					  (u32)(lblk - rs), e.flags);
				j++;
				continue;
			}
			/* overlaps end */
			ocsfs2_free_blocks_rc(sb, e.physical, (u32)(end - rs));
			inode->i_blocks -= (u64)(end - rs) * spb;
			rec_store(&RECS(n)[j], end, e.physical + (end - rs),
				  (u32)(re - end), e.flags);
			j++;
		}
		if (le16_to_cpu(n->en_nr) == 0)
			emptied = true;        /* A2: stale routing -> rebuild */
		node_finish(sb, bh);
		brelse(bh);
		bh = (next && !stop) ? node_read(sb, next) : NULL;
	}
	if (bh)
		brelse(bh);
	if (tail_valid)
		ret = tree_insert_locked(inode, tail_log, tail_phys, tail_len,
					 tail_flags);
	if (!ret && emptied)
		ret = ext_tree_collapse(inode);
	TREE_TXN_END(inode, ret);
	return ret;
}

int ocsfs2_ext_tree_truncate_from(struct inode *inode, u64 from)
{
	struct super_block *sb = inode->i_sb;
	u32 spb = sb->s_blocksize / 512;
	struct buffer_head *bh;
	bool emptied = false;
	int ret = 0;
	TREE_TXN_BEGIN(sb);

	bh = leaf_for(inode, from);
	while (bh) {
		struct ocsfs2_disk_ext_node *n =
			(struct ocsfs2_disk_ext_node *)bh->b_data;
		u64 next = le64_to_cpu(n->en_next);
		u16 j = 0;

		ocsfs2_jbuf(bh);
		while (j < le16_to_cpu(n->en_nr)) {
			struct ocsfs2_extent e;
			u64 re;

			rec_load(&RECS(n)[j], &e);
			re = e.logical + e.length;
			if (re <= from) { j++; continue; }
			if (e.logical >= from) {                  /* whole record gone */
				u16 k;

				ocsfs2_free_blocks_rc(sb, e.physical, e.length);
				inode->i_blocks -= (u64)e.length * spb;
				for (k = j; k < le16_to_cpu(n->en_nr) - 1; k++)
					RECS(n)[k] = RECS(n)[k + 1];
				n->en_nr = cpu_to_le16(le16_to_cpu(n->en_nr) - 1);
				continue;
			}
			/* straddles `from`: trim the tail */
			ocsfs2_free_blocks_rc(sb, e.physical + (from - e.logical),
					      (u32)(re - from));
			inode->i_blocks -= (u64)(re - from) * spb;
			rec_store(&RECS(n)[j], e.logical, e.physical,
				  (u32)(from - e.logical), e.flags);
			j++;
		}
		if (le16_to_cpu(n->en_nr) == 0)
			emptied = true;
		node_finish(sb, bh);
		brelse(bh);
		bh = next ? node_read(sb, next) : NULL;
	}

	if (from == 0) {
		/* whole tree empty: drop every node and revert to inline */
		ocsfs2_ext_tree_free_all(inode);
		ret = ocsfs2_write_inode_block(inode);
	} else if (emptied) {
		ret = ext_tree_collapse(inode);   /* A2: rebuild, stale routing */
	}
	TREE_TXN_END(inode, ret);
	return ret;
}

/* recursively free a subtree's data extents (refcount-aware) + node blocks */
static void free_subtree(struct inode *inode, u64 blk)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh = node_read(sb, blk);
	struct ocsfs2_disk_ext_node *n;
	u16 nr, j;

	if (!bh)
		return;
	n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	nr = le16_to_cpu(n->en_nr);
	if (le16_to_cpu(n->en_level) == 0) {
		for (j = 0; j < nr; j++) {
			struct ocsfs2_extent e;

			rec_load(&RECS(n)[j], &e);
			ocsfs2_free_blocks_rc(sb, e.physical, e.length);
		}
	} else {
		for (j = 0; j < nr; j++)
			free_subtree(inode, le64_to_cpu(PTRS(n)[j].ep_child));
	}
	brelse(bh);
	ocsfs2_free_blocks(sb, blk, 1);   /* the node block itself */
}

void ocsfs2_ext_tree_free_all(struct inode *inode)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);

	if (oi->i_extent_tree_root) {
		free_subtree(inode, oi->i_extent_tree_root);
		oi->i_extent_tree_root = 0;
	}
}
