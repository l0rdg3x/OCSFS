// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — refcount.c
 * Per-AG reflink/snapshot reference-count store (Plan 4).
 *
 * A record {phys, len, refcount} covers a contiguous SHARED physical range;
 * refcount is always >= 2 when stored. A block with no record has an implicit
 * refcount of 1 (sole owner) and is freed normally. The tree root block number
 * lives in the AG header (ag_rc_btree_root, 0 = empty tree).
 *
 * Plan 4 uses a single leaf node (level 0): all records sit in one 4 KiB block,
 * sorted by phys, non-overlapping. That holds up to 254 distinct shared ranges
 * per AG — ample for the VM-disk reflink/snapshot workload. Growth to an
 * internal B+tree level (when a leaf overflows) is a bounded future extension,
 * mirroring the inline-extent → B+tree spill deferred to Plan 2b; an overflow
 * returns -ENOSPC rather than corrupting.
 *
 * Locking: each AG has rc_lock, taken before ag_lock (the bitmap lock that
 * alloc/free use), so the consistent order is rc_lock -> ag_lock. Updates are
 * journaled when a transaction is active (reflink, CoW), else synced directly
 * (truncate/evict), matching the bitmap path.
 */
#include "ocsfs.h"
#include <linux/minmax.h>

/* An in-core refcount segment (record). */
struct rc_seg {
	u64 phys;
	u32 len;
	u32 rc;
};

/* sized to hold a full leaf plus the few extra records a split can introduce */
#define RC_WORK_RECS  (OCSFS2_RC_MAX_RECS + 4)

static struct ocsfs2_ag_info *ag_of_block(struct ocsfs2_sb_info *sbi, u64 phys)
{
	u32 ag;

	for (ag = 0; ag < sbi->s_ag_count; ag++) {
		struct ocsfs2_ag_info *ai = &sbi->s_ags[ag];

		if (phys >= ai->block_start &&
		    phys < ai->block_start + ai->block_count)
			return ai;
	}
	return NULL;
}

/* Read + validate the refcount leaf at @blk. Returns a held bh or NULL. */
static struct buffer_head *rc_read_node(struct super_block *sb, u64 blk,
					struct ocsfs2_disk_rc_node **node_out)
{
	struct buffer_head *bh = sb_bread(sb, blk);
	struct ocsfs2_disk_rc_node *node;
	u32 crc;

	if (!bh)
		return NULL;
	node = (struct ocsfs2_disk_rc_node *)bh->b_data;
	crc = ocsfs2_crc32c(~0U, node,
			    offsetof(struct ocsfs2_disk_rc_node, rn_checksum));
	if (le32_to_cpu(node->rn_magic) != OCSFS2_RC_NODE_MAGIC ||
	    crc != le32_to_cpu(node->rn_checksum) ||
	    le16_to_cpu(node->rn_nr) > OCSFS2_RC_MAX_RECS) {
		pr_err_ratelimited("ocsfs2: refcount node %llu invalid\n",
				   (unsigned long long)blk);
		brelse(bh);
		return NULL;
	}
	*node_out = node;
	return bh;
}

/* ── refcount lookup ── */

u32 ocsfs2_refcount_get(struct super_block *sb, u64 phys)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_ag_info *ai = ag_of_block(sbi, phys);
	struct ocsfs2_disk_rc_node *node;
	struct buffer_head *bh;
	u32 rc = 1;
	u16 i, n;

	if (!ai)
		return 1;
	mutex_lock(&ai->rc_lock);
	if (!ai->rc_btree_root)
		goto out;
	bh = rc_read_node(sb, ai->rc_btree_root, &node);
	if (!bh)
		goto out;          /* treat a damaged node as unshared (no CoW) */
	n = le16_to_cpu(node->rn_nr);
	for (i = 0; i < n; i++) {
		u64 p = le64_to_cpu(node->rn_recs[i].rr_phys);
		u32 l = le32_to_cpu(node->rn_recs[i].rr_len);

		if (phys >= p && phys < p + l) {
			rc = le32_to_cpu(node->rn_recs[i].rr_refcount);
			break;
		}
		if (p > phys)
			break;     /* sorted: records past phys cannot match */
	}
	brelse(bh);
out:
	mutex_unlock(&ai->rc_lock);
	return rc;
}

/* ── leaf load / store ── */

static int rc_load(struct super_block *sb, struct ocsfs2_ag_info *ai,
		   struct rc_seg *recs, int *nr)
{
	struct ocsfs2_disk_rc_node *node;
	struct buffer_head *bh;
	u16 i, n;

	*nr = 0;
	if (!ai->rc_btree_root)
		return 0;
	bh = rc_read_node(sb, ai->rc_btree_root, &node);
	if (!bh)
		return -EIO;
	n = le16_to_cpu(node->rn_nr);
	for (i = 0; i < n; i++) {
		recs[i].phys = le64_to_cpu(node->rn_recs[i].rr_phys);
		recs[i].len  = le32_to_cpu(node->rn_recs[i].rr_len);
		recs[i].rc   = le32_to_cpu(node->rn_recs[i].rr_refcount);
	}
	*nr = n;
	brelse(bh);
	return 0;
}

/* Point the AG header's refcount-tree root at @root (journaled-or-synced). */
static int rc_set_root(struct super_block *sb, struct ocsfs2_ag_info *ai, u64 root)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u64 ag_blk = sbi->s_ag_desc_off / sb->s_blocksize +
		     (u64)ai->ag_no * sbi->s_ag_blocks;
	struct buffer_head *bh = sb_bread(sb, ag_blk);
	struct ocsfs2_disk_ag *dag;
	bool in_txn = ocsfs2_current_txn() != NULL;
	int ret = 0;

	if (!bh)
		return -EIO;
	if (in_txn) {
		ret = ocsfs2_jbuf(bh);
		if (ret) {
			brelse(bh);
			return ret;
		}
	}
	/* serialise against write_back_meta, which also rewrites this header */
	mutex_lock(&ai->ag_lock);
	dag = (struct ocsfs2_disk_ag *)bh->b_data;
	dag->ag_rc_btree_root = cpu_to_le64(root);
	dag->ag_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, dag,
			   offsetof(struct ocsfs2_disk_ag, ag_checksum)));
	mutex_unlock(&ai->ag_lock);
	mark_buffer_dirty(bh);
	if (!in_txn)
		sync_dirty_buffer(bh);
	brelse(bh);
	ai->rc_btree_root = root;
	return 0;
}

/* Write the record set back to the leaf, allocating/freeing the node block as
 * the tree becomes non-empty/empty. Caller holds rc_lock. */
static int rc_store(struct super_block *sb, struct ocsfs2_ag_info *ai,
		    const struct rc_seg *recs, int nr)
{
	struct ocsfs2_disk_rc_node *node;
	struct buffer_head *bh;
	bool in_txn = ocsfs2_current_txn() != NULL;
	int i, ret;

	if (nr == 0) {
		u64 old = ai->rc_btree_root;

		if (!old)
			return 0;
		ret = rc_set_root(sb, ai, 0);
		if (ret)
			return ret;
		ocsfs2_free_blocks(sb, old, 1);   /* metadata block, raw free */
		return 0;
	}
	if (nr > OCSFS2_RC_MAX_RECS)
		return -ENOSPC;                   /* leaf full: multi-level spill deferred */

	if (!ai->rc_btree_root) {
		u64 blk;

		ret = ocsfs2_alloc_blocks(sb, ai->ag_no, 1, &blk);
		if (ret)
			return ret;
		ret = rc_set_root(sb, ai, blk);
		if (ret) {
			ocsfs2_free_blocks(sb, blk, 1);
			return ret;
		}
	}

	bh = sb_bread(sb, ai->rc_btree_root);
	if (!bh)
		return -EIO;
	if (in_txn) {
		ret = ocsfs2_jbuf(bh);
		if (ret) {
			brelse(bh);
			return ret;
		}
	}
	node = (struct ocsfs2_disk_rc_node *)bh->b_data;
	memset(node, 0, sb->s_blocksize);
	node->rn_magic = cpu_to_le32(OCSFS2_RC_NODE_MAGIC);
	node->rn_level = cpu_to_le16(0);
	node->rn_nr = cpu_to_le16((u16)nr);
	node->rn_ag = cpu_to_le32(ai->ag_no);
	for (i = 0; i < nr; i++) {
		node->rn_recs[i].rr_phys = cpu_to_le64(recs[i].phys);
		node->rn_recs[i].rr_len = cpu_to_le32(recs[i].len);
		node->rn_recs[i].rr_refcount = cpu_to_le32(recs[i].rc);
	}
	node->rn_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, node,
			    offsetof(struct ocsfs2_disk_rc_node, rn_checksum)));
	mark_buffer_dirty(bh);
	if (!in_txn)
		sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

/* ── range arithmetic ── */

/* Append [p, e) at resulting refcount @rc to the right output list, coalescing
 * with the previous segment. rc==0 -> the blocks are released to @freed;
 * rc==1 -> implicit (no stored record); rc>=2 -> a stored record. */
static void seg_emit(struct rc_seg *out, int *no, u64 p, u64 e, u32 rc,
		     struct rc_seg *freed, int *nf)
{
	if (e <= p)
		return;
	if (rc == 0) {
		if (*nf > 0 && (u64)freed[*nf - 1].phys + freed[*nf - 1].len == p)
			freed[*nf - 1].len += (u32)(e - p);
		else {
			freed[*nf].phys = p;
			freed[*nf].len = (u32)(e - p);
			freed[*nf].rc = 0;
			(*nf)++;
		}
		return;
	}
	if (rc == 1)
		return;
	if (*no > 0 && (u64)out[*no - 1].phys + out[*no - 1].len == p &&
	    out[*no - 1].rc == rc)
		out[*no - 1].len += (u32)(e - p);
	else {
		out[*no].phys = p;
		out[*no].len = (u32)(e - p);
		out[*no].rc = rc;
		(*no)++;
	}
}

/* Apply @delta (+1 / -1) to every block of [a, b) across the sorted record set
 * @in, producing the new record set in @out and (for delta < 0) the released
 * ranges in @freed. Returns the new record count, or -1 on leaf overflow. */
static int rc_apply(const struct rc_seg *in, int nin, u64 a, u64 b, int delta,
		    struct rc_seg *out, struct rc_seg *freed, int *nfreed)
{
	int i = 0, no = 0, nf = 0;
	u64 p;

	/* records ending at or before a: kept verbatim */
	while (i < nin && (u64)in[i].phys + in[i].len <= a) {
		seg_emit(out, &no, in[i].phys, (u64)in[i].phys + in[i].len,
			 in[i].rc, freed, &nf);
		i++;
	}
	/* a record straddling a: keep its [phys, a) head (do not advance i) */
	if (i < nin && in[i].phys < a && (u64)in[i].phys + in[i].len > a)
		seg_emit(out, &no, in[i].phys, a, in[i].rc, freed, &nf);

	p = a;
	while (p < b) {
		if (i < nin && in[i].phys <= p &&
		    p < (u64)in[i].phys + in[i].len) {
			u64 rend = (u64)in[i].phys + in[i].len;
			u64 seg_end = min(rend, b);

			seg_emit(out, &no, p, seg_end, in[i].rc + delta, freed, &nf);
			p = seg_end;
			if (rend <= b)
				i++;          /* this record fully inside [.., b) */
		} else {
			u64 gap_end = (i < nin && in[i].phys < b) ? in[i].phys : b;

			seg_emit(out, &no, p, gap_end, (u32)(1 + delta), freed, &nf);
			p = gap_end;
		}
	}
	/* a record straddling b: keep its [b, end) tail */
	if (i < nin && in[i].phys < b && (u64)in[i].phys + in[i].len > b) {
		seg_emit(out, &no, b, (u64)in[i].phys + in[i].len, in[i].rc,
			 freed, &nf);
		i++;
	}
	/* records starting at or after b: kept verbatim */
	while (i < nin) {
		seg_emit(out, &no, in[i].phys, (u64)in[i].phys + in[i].len,
			 in[i].rc, freed, &nf);
		i++;
	}

	*nfreed = nf;
	return no > OCSFS2_RC_MAX_RECS ? -1 : no;
}

/* ── public mutators ── */

int ocsfs2_refcount_inc(struct super_block *sb, u64 phys, u32 len)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_ag_info *ai = ag_of_block(sbi, phys);
	struct rc_seg *in, *out, *freed;
	int nin, no, nf = 0, ret;

	if (!ai || len == 0)
		return -EINVAL;
	in = kmalloc_array(RC_WORK_RECS, sizeof(*in), GFP_NOFS);
	out = kmalloc_array(RC_WORK_RECS, sizeof(*out), GFP_NOFS);
	freed = kmalloc_array(RC_WORK_RECS, sizeof(*freed), GFP_NOFS);
	if (!in || !out || !freed) {
		ret = -ENOMEM;
		goto out_free;
	}

	mutex_lock(&ai->rc_lock);
	ret = rc_load(sb, ai, in, &nin);
	if (ret)
		goto unlock;
	no = rc_apply(in, nin, phys, phys + len, +1, out, freed, &nf);
	if (no < 0) {
		pr_warn_ratelimited("ocsfs2: AG%u refcount leaf full (spill deferred)\n",
				    ai->ag_no);
		ret = -ENOSPC;
		goto unlock;
	}
	ret = rc_store(sb, ai, out, no);
unlock:
	mutex_unlock(&ai->rc_lock);
out_free:
	kfree(in);
	kfree(out);
	kfree(freed);
	return ret;
}

void ocsfs2_free_blocks_rc(struct super_block *sb, u64 phys, u32 len)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_ag_info *ai = ag_of_block(sbi, phys);
	struct rc_seg *in, *out, *freed;
	int nin, no, nf = 0, i;

	if (!ai || len == 0)
		return;
	in = kmalloc_array(RC_WORK_RECS, sizeof(*in), GFP_NOFS);
	out = kmalloc_array(RC_WORK_RECS, sizeof(*out), GFP_NOFS);
	freed = kmalloc_array(RC_WORK_RECS, sizeof(*freed), GFP_NOFS);
	if (!in || !out || !freed) {
		/* No room to consult the tree. If the AG has no shared ranges at
		 * all, a plain free is correct; otherwise we must not risk freeing
		 * a still-shared block, so leak rather than corrupt. */
		if (!ai->rc_btree_root)
			ocsfs2_free_blocks(sb, phys, len);
		else
			pr_err_ratelimited("ocsfs2: free_blocks_rc OOM, leaking %u blocks at %llu\n",
					   len, (unsigned long long)phys);
		goto out_free;
	}

	mutex_lock(&ai->rc_lock);
	if (rc_load(sb, ai, in, &nin))
		goto unlock;
	no = rc_apply(in, nin, phys, phys + len, -1, out, freed, &nf);
	if (no < 0) {
		pr_err_ratelimited("ocsfs2: AG%u refcount dec overflow at %llu (leaked)\n",
				   ai->ag_no, (unsigned long long)phys);
		goto unlock;
	}
	if (rc_store(sb, ai, out, no))
		goto unlock;
	for (i = 0; i < nf; i++)
		ocsfs2_free_blocks(sb, freed[i].phys, freed[i].len);
unlock:
	mutex_unlock(&ai->rc_lock);
out_free:
	kfree(in);
	kfree(out);
	kfree(freed);
}
