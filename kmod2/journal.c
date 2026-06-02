// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — journal.c
 * Write-ahead redo log + crash recovery (single-node, slot-0 WAL).
 *
 * A transaction enrols the metadata buffers it changes (snapshotting a
 * before-image for abort). Commit writes, to the journal ring:
 *     DESC(seq, [home,crc]*) | after-image block * nr | COMMIT(seq)
 * then flushes, publishes the journal header (tail->this txn), checkpoints the
 * home blocks, flushes, and empties the journal. At most one transaction lives
 * in the journal at a time (synchronous checkpoint) — simple and correct.
 *
 * Replay at mount re-applies a committed txn found in [tail, head); a torn or
 * absent record stops replay cleanly (the op is undone, since home blocks are
 * written only after COMMIT is durable).
 */
#include "ocsfs.h"
#include <linux/blkdev.h>
#include <linux/moduleparam.h>

/* DEBUG/TEST: when set, txn_commit publishes the journal then returns WITHOUT
 * checkpointing (journal left dirty). A hard crash here is then recovered by
 * replay on the next mount — the deterministic way to exercise J2. Never set
 * in production. */
static bool ocsfs2_crash_after_commit;
module_param_named(crash_after_commit, ocsfs2_crash_after_commit, bool, 0644);
MODULE_PARM_DESC(crash_after_commit,
		 "TEST: skip checkpoint after commit to exercise replay");

static inline u64 jblk_phys(struct ocsfs2_journal *j, u64 rec)
{
	return j->j_first_blk + 1 + (rec % j->j_ring_len);
}

/* Overwrite a whole journal/home block with @src (4096 bytes), durably. */
static int write_block(struct super_block *sb, u64 phys, const void *src)
{
	struct buffer_head *bh = sb_getblk(sb, phys);

	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memcpy(bh->b_data, src, sb->s_blocksize);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	mark_buffer_dirty(bh);
	if (sync_dirty_buffer(bh)) {
		brelse(bh);
		return -EIO;
	}
	brelse(bh);
	return 0;
}

static int persist_header(struct ocsfs2_journal *j)
{
	struct ocsfs2_disk_journal_hdr *jh =
		(struct ocsfs2_disk_journal_hdr *)j->j_hdr_bh->b_data;

	jh->jh_head = cpu_to_le64(j->j_head);
	jh->jh_tail = cpu_to_le64(j->j_tail);
	jh->jh_sequence = cpu_to_le64(j->j_seq);
	jh->jh_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, jh,
		offsetof(struct ocsfs2_disk_journal_hdr, jh_checksum)));
	mark_buffer_dirty(j->j_hdr_bh);
	if (sync_dirty_buffer(j->j_hdr_bh))
		return -EIO;
	blkdev_issue_flush(j->j_sb->s_bdev);
	return 0;
}

int ocsfs2_journal_init(struct super_block *sb)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_journal *j = &sbi->s_journal;
	struct ocsfs2_disk_journal_hdr *jh;
	struct buffer_head *bh;
	u32 crc;

	/* each node uses its own per-slot journal region (slot 0 single-node) */
	u16 slot = sbi->s_cluster ? sbi->s_cluster->self_slot : 0;

	j->j_sb = sb;
	j->j_off = sbi->s_journal_off + (u64)slot * sbi->s_journal_size;
	j->j_first_blk = j->j_off / sb->s_blocksize;
	j->j_blocks = sbi->s_journal_size / sb->s_blocksize;
	if (j->j_blocks < 4)
		return -EINVAL;
	j->j_ring_len = j->j_blocks - 1;
	mutex_init(&j->j_lock);

	bh = sb_bread(sb, j->j_first_blk);
	if (!bh)
		return -EIO;
	jh = (struct ocsfs2_disk_journal_hdr *)bh->b_data;
	if (le32_to_cpu(jh->jh_magic) != OCSFS2_JOURNAL_MAGIC) {
		pr_err("ocsfs2: journal header bad magic\n");
		brelse(bh);
		return -EINVAL;
	}
	crc = ocsfs2_crc32c(~0U, jh,
		offsetof(struct ocsfs2_disk_journal_hdr, jh_checksum));
	if (crc != le32_to_cpu(jh->jh_checksum)) {
		pr_err("ocsfs2: journal header crc mismatch\n");
		brelse(bh);
		return -EUCLEAN;
	}
	j->j_head = le64_to_cpu(jh->jh_head);
	j->j_tail = le64_to_cpu(jh->jh_tail);
	j->j_seq  = le64_to_cpu(jh->jh_sequence);
	j->j_hdr_bh = bh;
	j->j_active = true;
	return 0;
}

void ocsfs2_journal_exit(struct super_block *sb)
{
	struct ocsfs2_journal *j = &OCSFS2_SB(sb)->s_journal;

	if (!j->j_active)
		return;
	if (j->j_hdr_bh) {
		brelse(j->j_hdr_bh);
		j->j_hdr_bh = NULL;
	}
	j->j_active = false;
}

/* ── transactions ── */

struct ocsfs2_txn *ocsfs2_txn_begin(struct super_block *sb)
{
	struct ocsfs2_txn *txn = kzalloc(sizeof(*txn), GFP_NOFS);

	if (!txn)
		return NULL;
	txn->t_sb = sb;
	INIT_LIST_HEAD(&txn->t_bufs);
	/* jbd2-style: metadata write helpers find this via current->journal_info */
	current->journal_info = txn;
	return txn;
}

int ocsfs2_txn_get(struct ocsfs2_txn *txn, struct buffer_head *bh)
{
	struct ocsfs2_txn_buf *tb;

	if (txn->t_failed)
		return -EIO;
	list_for_each_entry(tb, &txn->t_bufs, link)
		if (tb->bh == bh)
			return 0;   /* already enrolled */
	if (txn->t_nr >= OCSFS2_JTXN_MAX_BLOCKS) {
		txn->t_failed = true;
		return -ENOSPC;
	}
	tb = kzalloc(sizeof(*tb), GFP_NOFS);
	if (!tb) {
		txn->t_failed = true;
		return -ENOMEM;
	}
	tb->before = kmemdup(bh->b_data, txn->t_sb->s_blocksize, GFP_NOFS);
	if (!tb->before) {
		kfree(tb);
		txn->t_failed = true;
		return -ENOMEM;
	}
	get_bh(bh);
	tb->bh = bh;
	tb->home_block = bh->b_blocknr;
	list_add_tail(&tb->link, &txn->t_bufs);
	txn->t_nr++;
	return 0;
}

static void txn_free(struct ocsfs2_txn *txn, bool restore)
{
	struct ocsfs2_txn_buf *tb, *n;

	list_for_each_entry_safe(tb, n, &txn->t_bufs, link) {
		if (restore)
			memcpy(tb->bh->b_data, tb->before, txn->t_sb->s_blocksize);
		list_del(&tb->link);
		put_bh(tb->bh);
		kfree(tb->before);
		kfree(tb);
	}
	kfree(txn);
}

void ocsfs2_txn_abort(struct ocsfs2_txn *txn)
{
	current->journal_info = NULL;
	txn_free(txn, true);   /* revert in-memory buffers to their before-image */
}

int ocsfs2_txn_commit(struct ocsfs2_txn *txn)
{
	struct super_block *sb = txn->t_sb;
	struct ocsfs2_journal *j = &OCSFS2_SB(sb)->s_journal;
	struct ocsfs2_txn_buf *tb;
	struct ocsfs2_disk_jdesc *desc;
	struct ocsfs2_disk_jcommit *commit;
	u64 start, seq, len;
	u32 nr, i;
	int ret = 0;

	current->journal_info = NULL;   /* this task is no longer building a txn */

	if (txn->t_failed) {
		ocsfs2_txn_abort(txn);
		return -EIO;
	}
	if (txn->t_nr == 0) {
		txn_free(txn, false);
		return 0;
	}

	nr = txn->t_nr;
	len = 1 + nr + 1;

	desc = kzalloc(sb->s_blocksize, GFP_NOFS);
	commit = kzalloc(sb->s_blocksize, GFP_NOFS);
	if (!desc || !commit) {
		kfree(desc);
		kfree(commit);
		ocsfs2_txn_abort(txn);
		return -ENOMEM;
	}

	mutex_lock(&j->j_lock);
	if (len > j->j_ring_len) {
		ret = -E2BIG;
		goto out_unlock;
	}
	start = j->j_head;
	seq = j->j_seq;

	/* descriptor */
	desc->jd_magic = cpu_to_le32(OCSFS2_JOURNAL_MAGIC);
	desc->jd_type = cpu_to_le32(OCSFS2_JREC_DESC);
	desc->jd_seq = cpu_to_le64(seq);
	desc->jd_nr = cpu_to_le32(nr);
	i = 0;
	list_for_each_entry(tb, &txn->t_bufs, link) {
		desc->jd_ent[i].je_home = cpu_to_le64(tb->home_block);
		desc->jd_ent[i].je_crc =
			cpu_to_le32(ocsfs2_crc32c(~0U, tb->bh->b_data, sb->s_blocksize));
		i++;
	}
	desc->jd_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, desc,
		offsetof(struct ocsfs2_disk_jdesc, jd_checksum)));
	ret = write_block(sb, jblk_phys(j, start), desc);
	if (ret)
		goto out_unlock;

	/* after-images */
	i = 0;
	list_for_each_entry(tb, &txn->t_bufs, link) {
		ret = write_block(sb, jblk_phys(j, start + 1 + i), tb->bh->b_data);
		if (ret)
			goto out_unlock;
		i++;
	}

	/* commit */
	commit->jc_magic = cpu_to_le32(OCSFS2_JOURNAL_MAGIC);
	commit->jc_type = cpu_to_le32(OCSFS2_JREC_COMMIT);
	commit->jc_seq = cpu_to_le64(seq);
	commit->jc_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, commit,
		offsetof(struct ocsfs2_disk_jcommit, jc_checksum)));
	ret = write_block(sb, jblk_phys(j, start + 1 + nr), commit);
	if (ret)
		goto out_unlock;

	blkdev_issue_flush(sb->s_bdev);

	/* publish: the journal now holds a committed txn in [start, start+len) */
	j->j_head = start + len;
	j->j_tail = start;
	ret = persist_header(j);
	if (ret)
		goto out_unlock;

	if (unlikely(ocsfs2_crash_after_commit)) {
		/* leave the journal dirty on purpose; a crash now is replayed */
		mutex_unlock(&j->j_lock);
		kfree(desc);
		kfree(commit);
		txn_free(txn, false);
		return 0;
	}

	/* checkpoint: copy the after-images to their home locations */
	list_for_each_entry(tb, &txn->t_bufs, link) {
		mark_buffer_dirty(tb->bh);
		if (sync_dirty_buffer(tb->bh)) {
			ret = -EIO;
			goto out_unlock;   /* header still points at the txn -> replay redoes it */
		}
	}
	blkdev_issue_flush(sb->s_bdev);

	/* empty the journal */
	j->j_tail = j->j_head;
	j->j_seq = seq + 1;
	ret = persist_header(j);

out_unlock:
	mutex_unlock(&j->j_lock);
	kfree(desc);
	kfree(commit);
	if (ret)
		txn_free(txn, true);    /* failed before/at publish: revert */
	else
		txn_free(txn, false);   /* committed: keep the changes */
	return ret;
}

/* ── replay ── */

int ocsfs2_journal_replay(struct super_block *sb)
{
	struct ocsfs2_journal *j = &OCSFS2_SB(sb)->s_journal;
	struct buffer_head *dbh = NULL, *cbh = NULL;
	struct ocsfs2_disk_jdesc *desc;
	struct ocsfs2_disk_jcommit *commit;
	u64 tail, head, seq;
	u32 nr, i, crc;
	int ret = 0;

	if (!j->j_active)
		return 0;

	mutex_lock(&j->j_lock);
	tail = j->j_tail;
	head = j->j_head;
	if (tail == head)
		goto clean;   /* journal empty: clean unmount */

	dbh = sb_bread(sb, jblk_phys(j, tail));
	if (!dbh) { ret = -EIO; goto out; }
	desc = (struct ocsfs2_disk_jdesc *)dbh->b_data;
	crc = ocsfs2_crc32c(~0U, desc,
			    offsetof(struct ocsfs2_disk_jdesc, jd_checksum));
	if (le32_to_cpu(desc->jd_magic) != OCSFS2_JOURNAL_MAGIC ||
	    le32_to_cpu(desc->jd_type) != OCSFS2_JREC_DESC ||
	    crc != le32_to_cpu(desc->jd_checksum)) {
		pr_warn("ocsfs2: replay: torn descriptor at %llu — journal recovered\n", tail);
		goto reset;
	}
	nr = le32_to_cpu(desc->jd_nr);
	seq = le64_to_cpu(desc->jd_seq);
	if (nr > OCSFS2_JTXN_MAX_BLOCKS || 1 + nr + 1 > head - tail) {
		pr_warn("ocsfs2: replay: bad nr %u — journal recovered\n", nr);
		goto reset;
	}

	cbh = sb_bread(sb, jblk_phys(j, tail + 1 + nr));
	if (!cbh) { ret = -EIO; goto out; }
	commit = (struct ocsfs2_disk_jcommit *)cbh->b_data;
	crc = ocsfs2_crc32c(~0U, commit,
			    offsetof(struct ocsfs2_disk_jcommit, jc_checksum));
	if (le32_to_cpu(commit->jc_magic) != OCSFS2_JOURNAL_MAGIC ||
	    le32_to_cpu(commit->jc_type) != OCSFS2_JREC_COMMIT ||
	    le64_to_cpu(commit->jc_seq) != seq ||
	    crc != le32_to_cpu(commit->jc_checksum)) {
		pr_warn("ocsfs2: replay: no valid COMMIT for seq %llu — undone\n", seq);
		goto reset;
	}

	/* committed: re-apply after-images to their home blocks */
	for (i = 0; i < nr; i++) {
		struct buffer_head *aibh = sb_bread(sb, jblk_phys(j, tail + 1 + i));
		u64 home = le64_to_cpu(desc->jd_ent[i].je_home);
		u32 ecrc = le32_to_cpu(desc->jd_ent[i].je_crc);

		if (!aibh) { ret = -EIO; goto out; }
		if (ocsfs2_crc32c(~0U, aibh->b_data, sb->s_blocksize) != ecrc) {
			pr_err("ocsfs2: replay: after-image %u crc mismatch (home %llu) — skipping\n",
			       i, home);
			brelse(aibh);
			continue;
		}
		ret = write_block(sb, home, aibh->b_data);
		brelse(aibh);
		if (ret)
			goto out;
	}
	blkdev_issue_flush(sb->s_bdev);
	pr_info("ocsfs2: replayed transaction seq %llu (%u blocks)\n", seq, nr);

reset:
	j->j_tail = j->j_head;
	ret = persist_header(j);
clean:
out:
	if (dbh)
		brelse(dbh);
	if (cbh)
		brelse(cbh);
	mutex_unlock(&j->j_lock);
	return ret;
}
