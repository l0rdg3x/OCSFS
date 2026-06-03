// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — journal.c
 * Write-ahead redo log + crash recovery (single-node, slot-0 WAL).
 *
 * A transaction enrols the metadata buffers it changes (snapshotting a
 * before-image for abort). Commit writes, to the journal ring:
 *     DESC(seq, [home,crc]*) | after-image block * nr | COMMIT(seq)
 * flushes, and publishes the journal header (head -> past this txn).
 *
 * Checkpoint (copying after-images to their home blocks) then empties the ring:
 *   - Cluster mode: synchronous, inside every commit — a peer reading a home
 *     block coherently must always see current metadata, so home can't lag.
 *   - Single-node (Plan 5): DEFERRED. The ring holds many committed txns
 *     [tail, head); committed txns stay on j->j_ckpt, pinning their metadata
 *     buffers so readers see the new content while the home block still lags.
 *     Checkpoint runs on ring pressure, ->sync_fs, ->fsync and unmount, writing
 *     each home once (coalesced) from the immutable ring after-image via a
 *     private bio (never through the cached buffer, so a concurrent open txn
 *     modifying the same block is never clobbered). This removes the per-op
 *     synchronous home write and collapses the repeated rewrites of a growing
 *     btree leaf / inode block.
 *
 * Replay at mount re-applies every committed txn found in [tail, head) in order;
 * a torn or absent record stops replay cleanly (the op is undone, since home
 * blocks lag COMMIT durability). A metadata block freed for reuse forces a
 * checkpoint (ocsfs2_txn_forget) so no stale ring after-image is ever replayed
 * onto a block that has become file data.
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

/* J5-C: write @n journal records (srcs[i] -> ring slot start+i) as ONE batch —
 * submit every block asynchronously, then wait for all. Contiguous ring slots
 * coalesce into a few large I/Os and pipeline instead of serialising one
 * sync-per-block round-trip. The caller issues a single blkdev_issue_flush
 * afterward as the commit barrier; per-after-image je_crc in the descriptor lets
 * replay reject a torn batch (any block not durable -> the whole txn is undone),
 * so no intra-batch ordering is needed. */
static int write_records_batched(struct ocsfs2_journal *j, u64 start,
				 void **srcs, u32 n)
{
	struct super_block *sb = j->j_sb;
	struct buffer_head **bhs;
	u32 i;
	int ret = 0;

	bhs = kmalloc_array(n, sizeof(*bhs), GFP_NOFS);
	if (!bhs) {                               /* fallback: serial, still correct */
		for (i = 0; i < n; i++) {
			ret = write_block(sb, jblk_phys(j, start + i), srcs[i]);
			if (ret)
				return ret;
		}
		return 0;
	}
	for (i = 0; i < n; i++) {
		struct buffer_head *bh = sb_getblk(sb, jblk_phys(j, start + i));

		if (!bh) {
			ret = -EIO;
			n = i;
			break;
		}
		lock_buffer(bh);
		memcpy(bh->b_data, srcs[i], sb->s_blocksize);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty(bh);
		write_dirty_buffer(bh, REQ_SYNC);   /* submit async */
		bhs[i] = bh;
	}
	for (i = 0; i < n; i++) {
		wait_on_buffer(bhs[i]);
		if (!buffer_uptodate(bhs[i]))
			ret = ret ? ret : -EIO;
		brelse(bhs[i]);
	}
	kfree(bhs);
	return ret;
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

/* Plan 5 single-node deferred checkpoint (defined below; declared here for
 * ocsfs2_txn_forget, which forces a checkpoint before a metadata block is
 * reused as data). Caller holds j_lock. */
static int checkpoint_locked(struct ocsfs2_journal *j);
static void txn_free(struct ocsfs2_txn *txn, bool restore);

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
	INIT_LIST_HEAD(&j->j_ckpt);
	j->j_running = NULL;
	j->j_run_handles = 0;
	init_waitqueue_head(&j->j_run_wait);

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
	struct ocsfs2_txn *txn, *tn;

	if (!j->j_active)
		return;
	/* Drop an uncommitted running batch (put_super flushes first; defensive). */
	if (j->j_running) {
		txn_free(j->j_running, false);
		j->j_running = NULL;
	}
	/* Drop any deferred txns still pinning buffers (put_super checkpoints first,
	 * so this is normally empty; on an error teardown the on-disk ring still
	 * holds them and the next mount replays them). */
	list_for_each_entry_safe(txn, tn, &j->j_ckpt, t_ckpt) {
		list_del(&txn->t_ckpt);
		txn_free(txn, false);
	}
	if (j->j_hdr_bh) {
		brelse(j->j_hdr_bh);
		j->j_hdr_bh = NULL;
	}
	j->j_active = false;
}

/* ── transactions ── */

struct ocsfs2_txn *ocsfs2_txn_begin(struct super_block *sb)
{
	struct ocsfs2_txn *txn;

	/* An explicit (non-data) op gets its own txn; first flush the running data
	 * batch so the two never enrol the same block in different transactions. */
	if (!ocsfs2_current_txn())
		ocsfs2_run_flush(sb);

	txn = kzalloc(sizeof(*txn), GFP_NOFS);

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

/* True if @blk is already enrolled in the calling task's current transaction.
 * Used by ocsfs2_meta_bread to avoid re-reading a block from disk when this
 * txn has modified it but not yet committed/checkpointed: the cached buffer is
 * authoritative (e.g. rename's del_dirent must see add_dirent's new entry in
 * the same dir block; a coherent re-read would clobber it). The current txn is
 * task-local (txn_begin) so the t_bufs walk needs no extra locking. */
bool ocsfs2_txn_has_block(struct super_block *sb, u64 blk)
{
	struct ocsfs2_txn *txn = current->journal_info;
	struct ocsfs2_txn_buf *tb;

	if (!txn || txn->t_sb != sb)
		return false;
	list_for_each_entry(tb, &txn->t_bufs, link)
		if (tb->home_block == blk)
			return true;
	return false;
}

/* Revoke blocks being freed: a just-freed metadata block (B+tree / refcount
 * node) may still be enrolled in the live transaction (modified before it was
 * freed) and/or carry a dirty buffer-cache buffer. If that physical block is
 * then reused as file DATA, the transaction's checkpoint or a stray buffer
 * writeback would stamp the stale metadata over the new data (found via
 * bpftrace: an ext-tree node checkpoint clobbering a reallocated data block).
 * So on free: drop the block from the current txn (don't journal/checkpoint it)
 * and clean any buffer-cache alias so nothing writes it back. */
void ocsfs2_txn_forget(struct super_block *sb, u64 start, u32 count)
{
	struct ocsfs2_journal *j = &OCSFS2_SB(sb)->s_journal;
	struct ocsfs2_txn *txn = ocsfs2_current_txn();

	if (txn) {
		struct ocsfs2_txn_buf *tb, *n;

		list_for_each_entry_safe(tb, n, &txn->t_bufs, link) {
			if (tb->home_block < start || tb->home_block >= start + count)
				continue;
			clear_buffer_dirty(tb->bh);   /* don't write the stale node back */
			list_del(&tb->link);
			brelse(tb->bh);
			kfree(tb->before);
			kfree(tb);
			txn->t_nr--;
		}
	}

	/* Deferred checkpoint (single-node): a committed-but-not-checkpointed txn may
	 * still hold this block's after-image in the ring. If the block is now reused
	 * as file DATA, a later replay would stamp that stale metadata over the data.
	 * If the freed range intersects any queued txn, force a checkpoint so the ring
	 * no longer references it (tail advances past it) before it is reused. */
	if (j->j_active) {
		struct ocsfs2_txn *ct;
		struct ocsfs2_txn_buf *tb;
		bool in_ring = false;

		mutex_lock(&j->j_lock);
		list_for_each_entry(ct, &j->j_ckpt, t_ckpt) {
			list_for_each_entry(tb, &ct->t_bufs, link)
				if (tb->home_block >= start &&
				    tb->home_block < start + count) {
					in_ring = true;
					break;
				}
			if (in_ring)
				break;
		}
		if (in_ring)
			checkpoint_locked(j);   /* empties ring: tail = head */
		mutex_unlock(&j->j_lock);
	}

	/* drop clean/dirty buffer-cache aliases for the freed range so a later data
	 * reuse of these blocks is never overwritten by stale metadata writeback */
	clean_bdev_aliases(sb->s_bdev, start, count);
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

/* ── deferred checkpoint (single-node) ── */

/* one home block that a committed txn wrote, and where its after-image lives */
struct ckpt_ent { u64 home; u64 ring_phys; };

/* Flush every committed txn on j_ckpt to its home blocks, coalesced (each home
 * written once, latest after-image wins), then empty the ring and release the
 * pinned buffers. Home content is read back from the immutable ring after-image
 * and written via a private bio — never through the home block's cached buffer —
 * so a concurrent open txn modifying that same buffer in cache is never
 * clobbered. j_lock held. */
static int checkpoint_locked(struct ocsfs2_journal *j)
{
	struct super_block *sb = j->j_sb;
	u32 bs = sb->s_blocksize;
	struct ocsfs2_txn *txn, *tn;
	struct ocsfs2_txn_buf *tb;
	struct ckpt_ent *ents = NULL;
	void *blkbuf = NULL;
	u32 n = 0, cap = 0, i, k;
	int ret = 0;

	if (list_empty(&j->j_ckpt))
		return 0;

	list_for_each_entry(txn, &j->j_ckpt, t_ckpt)
		cap += txn->t_nr;

	ents = kvmalloc_array(cap, sizeof(*ents), GFP_NOFS);
	blkbuf = kmalloc(bs, GFP_NOFS);
	if (!ents || !blkbuf) {
		ret = -ENOMEM;
		goto out;
	}

	/* gather (home, ring slot of its after-image) in commit order */
	list_for_each_entry(txn, &j->j_ckpt, t_ckpt) {
		i = 0;
		list_for_each_entry(tb, &txn->t_bufs, link) {
			ents[n].home = tb->home_block;
			ents[n].ring_phys = jblk_phys(j, txn->t_ring_start + 1 + i);
			n++;
			i++;
		}
	}

	/* Make the journal records durable before overwriting ANY home block: the
	 * commit path no longer flushes per-txn, so a crash midway through writing
	 * home blocks must be recoverable by re-applying the records on replay. */
	blkdev_issue_flush(sb->s_bdev);

	/* write each home once with its latest after-image (last writer wins) */
	for (i = 0; i < n; i++) {
		struct buffer_head *aibh;
		bool superseded = false;

		for (k = i + 1; k < n; k++)
			if (ents[k].home == ents[i].home) {
				superseded = true;
				break;
			}
		if (superseded)
			continue;

		aibh = sb_bread(sb, ents[i].ring_phys);
		if (!aibh) {
			ret = -EIO;
			goto out;
		}
		memcpy(blkbuf, aibh->b_data, bs);
		brelse(aibh);
		ret = ocsfs2_cl_bio(sb, ents[i].home * (u64)bs, blkbuf, bs,
				    REQ_OP_WRITE);
		if (ret)
			goto out;
	}
	blkdev_issue_flush(sb->s_bdev);

	/* home blocks durable: empty the ring and release the pinned buffers */
	j->j_tail = j->j_head;
	ret = persist_header(j);

	list_for_each_entry_safe(txn, tn, &j->j_ckpt, t_ckpt) {
		list_del(&txn->t_ckpt);
		txn_free(txn, false);   /* before-images already freed; drops bh pins */
	}
out:
	kvfree(ents);
	kfree(blkbuf);
	return ret;
}

/* Public checkpoint trigger (->sync_fs, ->fsync, unmount). No-op in cluster
 * mode (checkpoint is synchronous per commit) and when the ring is empty. */
int ocsfs2_journal_checkpoint(struct super_block *sb)
{
	struct ocsfs2_journal *j = &OCSFS2_SB(sb)->s_journal;
	int ret;

	if (!j->j_active)
		return 0;
	ocsfs2_run_flush(sb);   /* commit the running batch into the ring first */
	mutex_lock(&j->j_lock);
	ret = checkpoint_locked(j);
	mutex_unlock(&j->j_lock);
	return ret;
}

/* Deferred commit (single-node): write @txn's records (desc + after-images +
 * commit) to the ring as one batch, advance head, and queue the txn on j_ckpt
 * (pinning its buffers) for later coalesced checkpoint. j_lock held. On success
 * returns 0 and the txn is owned by j_ckpt (NOT freed); on error returns -errno
 * and the caller frees the txn. No per-commit flush/header (J5-C). */
static int commit_deferred_locked(struct ocsfs2_journal *j, struct ocsfs2_txn *txn)
{
	struct super_block *sb = j->j_sb;
	struct ocsfs2_txn_buf *tb;
	struct ocsfs2_disk_jdesc *desc;
	struct ocsfs2_disk_jcommit *commit;
	u64 start, seq, len;
	u32 nr, i;
	int ret;

	nr = txn->t_nr;
	len = 1 + nr + 1;
	if (len > j->j_ring_len)
		return -E2BIG;

	desc = kzalloc(sb->s_blocksize, GFP_NOFS);
	commit = kzalloc(sb->s_blocksize, GFP_NOFS);
	if (!desc || !commit) {
		kfree(desc);
		kfree(commit);
		return -ENOMEM;
	}

	/* the ring may already hold many committed txns — make room */
	while (j->j_head - j->j_tail + len > j->j_ring_len) {
		ret = checkpoint_locked(j);
		if (ret)
			goto out;
		if (j->j_head - j->j_tail + len > j->j_ring_len) {
			ret = -E2BIG;
			goto out;
		}
	}

	start = j->j_head;
	seq = j->j_seq;
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
	commit->jc_magic = cpu_to_le32(OCSFS2_JOURNAL_MAGIC);
	commit->jc_type = cpu_to_le32(OCSFS2_JREC_COMMIT);
	commit->jc_seq = cpu_to_le64(seq);
	commit->jc_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, commit,
		offsetof(struct ocsfs2_disk_jcommit, jc_checksum)));
	{
		void **srcs = kmalloc_array(len, sizeof(*srcs), GFP_NOFS);

		if (!srcs) {
			ret = -ENOMEM;
			goto out;
		}
		srcs[0] = desc;
		i = 1;
		list_for_each_entry(tb, &txn->t_bufs, link)
			srcs[i++] = tb->bh->b_data;
		srcs[i] = commit;
		ret = write_records_batched(j, start, srcs, (u32)len);
		kfree(srcs);
	}
	if (ret)
		goto out;

	j->j_head = start + len;
	j->j_seq = seq + 1;
	txn->t_ring_start = start;
	list_for_each_entry(tb, &txn->t_bufs, link) {
		kfree(tb->before);
		tb->before = NULL;
	}
	list_add_tail(&txn->t_ckpt, &j->j_ckpt);
	ret = 0;
out:
	kfree(desc);
	kfree(commit);
	return ret;
}

/* ── J5-D running transaction ── */

/* Commit the running batch (j_running) if any. j_lock held; caller must ensure
 * no op is mid-modify (j_run_handles == 0) so the after-images are consistent. */
static int run_commit_locked(struct ocsfs2_journal *j)
{
	struct ocsfs2_txn *txn = j->j_running;
	int ret;

	if (!txn)
		return 0;
	j->j_running = NULL;
	if (txn->t_failed) {
		/* a txn_get failed in the batch (ENOMEM / >254 buffers): the enrolled
		 * set may be incomplete — revert to before-images (consistent) rather
		 * than commit a partial batch. The in-flight (non-fsync'd) ops are lost. */
		txn_free(txn, true);
		return -EIO;
	}
	if (txn->t_nr == 0) {
		txn_free(txn, false);
		return 0;
	}
	ret = commit_deferred_locked(j, txn);
	if (ret)
		txn_free(txn, false);   /* commit I/O error: drop (FS will error out) */
	return ret;
}

/* Join the running batch: data-path ops (alloc+extent in iomap_begin, inode
 * writeback) enrol their buffers here instead of opening a private txn, so many
 * ops share one commit. Single-node only (caller gates on s_max_nodes <= 1). */
int ocsfs2_run_begin(struct super_block *sb)
{
	struct ocsfs2_journal *j = &OCSFS2_SB(sb)->s_journal;

	mutex_lock(&j->j_lock);
	if (!j->j_running) {
		struct ocsfs2_txn *t = kzalloc(sizeof(*t), GFP_NOFS);

		if (!t) {
			mutex_unlock(&j->j_lock);
			return -ENOMEM;
		}
		t->t_sb = sb;
		INIT_LIST_HEAD(&t->t_bufs);
		j->j_running = t;
	}
	current->journal_info = j->j_running;
	j->j_run_handles++;
	mutex_unlock(&j->j_lock);
	return 0;
}

/* Leave the running batch. Commit it now if it has grown past the buffer
 * threshold and this was the last in-flight op (so its content is consistent). */
void ocsfs2_run_end(struct super_block *sb)
{
	struct ocsfs2_journal *j = &OCSFS2_SB(sb)->s_journal;

	mutex_lock(&j->j_lock);
	current->journal_info = NULL;
	if (j->j_run_handles > 0)
		j->j_run_handles--;
	if (j->j_run_handles == 0) {
		if (j->j_running && (j->j_running->t_nr >= OCSFS2_RUN_MAX_BUFS ||
				     j->j_running->t_failed))
			run_commit_locked(j);
		wake_up(&j->j_run_wait);
	}
	mutex_unlock(&j->j_lock);
}

/* Force the running batch out (durability/serialisation barrier: ->fsync, sync,
 * an explicit non-data op, checkpoint, unmount). Waits for any in-flight op to
 * leave so the committed after-images are consistent. */
int ocsfs2_run_flush(struct super_block *sb)
{
	struct ocsfs2_journal *j = &OCSFS2_SB(sb)->s_journal;
	int ret = 0;

	if (!j->j_active)
		return 0;
	mutex_lock(&j->j_lock);
	while (j->j_running) {
		if (j->j_run_handles == 0) {
			ret = run_commit_locked(j);
			break;
		}
		mutex_unlock(&j->j_lock);
		wait_event(j->j_run_wait,
			   j->j_run_handles == 0 || !j->j_running);
		mutex_lock(&j->j_lock);
	}
	mutex_unlock(&j->j_lock);
	return ret;
}

int ocsfs2_txn_commit(struct ocsfs2_txn *txn)
{
	struct super_block *sb = txn->t_sb;
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_journal *j = &sbi->s_journal;
	/* A9: defer the checkpoint on BOTH single-node and cluster volumes. On a
	 * cluster volume the deferred home blocks are made current before any peer
	 * can read them by checkpointing on every metadata-lease release/downgrade
	 * (ocsfs2_inode_close_lease EX / ocsfs2_meta_unlock); a crashed peer's many
	 * uncheckpointed txns are recovered by the looping log-scan replay_slot. The
	 * coherent per-block metadata (bitmap/inode/refcount/csum) stays on the CAW
	 * path (immediate); only the journaled, lease-protected metadata (extent
	 * btree, dir, xattr) is deferred. */
	bool deferred = true;
	(void)sbi;
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

	mutex_lock(&j->j_lock);

	if (deferred) {
		/* single-node: defer checkpoint + no per-commit flush/header (J5-C) */
		ret = commit_deferred_locked(j, txn);
		mutex_unlock(&j->j_lock);
		if (ret)
			txn_free(txn, false);   /* not queued: drop (data path self-cleans) */
		return ret;                     /* 0: txn now owned by j_ckpt */
	}

	/* ── cluster: synchronous commit + immediate checkpoint ── */
	nr = txn->t_nr;
	len = 1 + nr + 1;
	desc = kzalloc(sb->s_blocksize, GFP_NOFS);
	commit = kzalloc(sb->s_blocksize, GFP_NOFS);
	if (!desc || !commit) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	if (len > j->j_ring_len) {
		ret = -E2BIG;
		goto out_unlock;
	}
	start = j->j_head;
	seq = j->j_seq;
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
	commit->jc_magic = cpu_to_le32(OCSFS2_JOURNAL_MAGIC);
	commit->jc_type = cpu_to_le32(OCSFS2_JREC_COMMIT);
	commit->jc_seq = cpu_to_le64(seq);
	commit->jc_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, commit,
		offsetof(struct ocsfs2_disk_jcommit, jc_checksum)));
	{
		void **srcs = kmalloc_array(len, sizeof(*srcs), GFP_NOFS);

		if (!srcs) {
			ret = -ENOMEM;
			goto out_unlock;
		}
		srcs[0] = desc;
		i = 1;
		list_for_each_entry(tb, &txn->t_bufs, link)
			srcs[i++] = tb->bh->b_data;
		srcs[i] = commit;
		ret = write_records_batched(j, start, srcs, (u32)len);
		kfree(srcs);
	}
	if (ret)
		goto out_unlock;
	j->j_head = start + len;
	j->j_seq = seq + 1;

	/* cluster keeps the per-commit header: dead-peer recovery (replay_slot) and a
	 * synchronous-checkpoint volume rely on the on-disk head being current. */
	blkdev_issue_flush(sb->s_bdev);   /* records durable before the header publishes them */
	ret = persist_header(j);   /* publish head (tail still marks this txn) */
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

	/* cluster: checkpoint synchronously (a peer reading a home block coherently
	 * must always see current metadata), then empty the ring */
	list_for_each_entry(tb, &txn->t_bufs, link) {
		mark_buffer_dirty(tb->bh);
		if (sync_dirty_buffer(tb->bh)) {
			ret = -EIO;
			goto out_unlock;   /* header still points at the txn -> replay redoes it */
		}
	}
	blkdev_issue_flush(sb->s_bdev);
	j->j_tail = j->j_head;
	ret = persist_header(j);

out_unlock:
	mutex_unlock(&j->j_lock);
	kfree(desc);
	kfree(commit);
	if (ret)
		txn_free(txn, true);    /* failed before/at publish: revert */
	else
		txn_free(txn, false);   /* committed (cluster): keep the changes */
	return ret;
}

/* ── replay ── */

/* Validate the txn whose descriptor sits at ring index @pos and, if fully
 * durable, apply its after-images to their home blocks. With the J5-C batched
 * commit a txn's blocks are written without intra-batch ordering, so a torn
 * batch (ANY block not durable) must be undone whole — never applied partially.
 * @want_seq != U64_MAX (log-scan) additionally requires desc.jd_seq == want_seq,
 * so a stale wrapped slot ends the scan. Returns 0 = applied (*outlen set to the
 * record count), 1 = torn/invalid/stop, -errno = I/O error. */
static int replay_one(struct super_block *sb, struct ocsfs2_journal *j, u64 pos,
		      u64 want_seq, u64 *outlen)
{
	struct buffer_head *dbh, *cbh, **aibhs;
	struct ocsfs2_disk_jdesc *desc;
	struct ocsfs2_disk_jcommit *commit;
	u64 seq;
	u32 nr, i, crc;
	int ret;
	bool ok;

	dbh = sb_bread(sb, jblk_phys(j, pos));
	if (!dbh)
		return -EIO;
	desc = (struct ocsfs2_disk_jdesc *)dbh->b_data;
	crc = ocsfs2_crc32c(~0U, desc, offsetof(struct ocsfs2_disk_jdesc, jd_checksum));
	if (le32_to_cpu(desc->jd_magic) != OCSFS2_JOURNAL_MAGIC ||
	    le32_to_cpu(desc->jd_type) != OCSFS2_JREC_DESC ||
	    crc != le32_to_cpu(desc->jd_checksum)) {
		brelse(dbh);
		return 1;                                   /* torn descriptor */
	}
	nr = le32_to_cpu(desc->jd_nr);
	seq = le64_to_cpu(desc->jd_seq);
	if (nr > OCSFS2_JTXN_MAX_BLOCKS || 1 + nr + 1 > j->j_ring_len ||
	    (want_seq != U64_MAX && seq != want_seq)) {
		brelse(dbh);
		return 1;                                   /* bad nr / seq discontinuity */
	}

	cbh = sb_bread(sb, jblk_phys(j, pos + 1 + nr));
	if (!cbh) { brelse(dbh); return -EIO; }
	commit = (struct ocsfs2_disk_jcommit *)cbh->b_data;
	crc = ocsfs2_crc32c(~0U, commit, offsetof(struct ocsfs2_disk_jcommit, jc_checksum));
	ok = le32_to_cpu(commit->jc_magic) == OCSFS2_JOURNAL_MAGIC &&
	     le32_to_cpu(commit->jc_type) == OCSFS2_JREC_COMMIT &&
	     le64_to_cpu(commit->jc_seq) == seq &&
	     crc == le32_to_cpu(commit->jc_checksum);
	brelse(cbh);
	if (!ok) { brelse(dbh); return 1; }              /* torn/absent commit */

	/* validate EVERY after-image before applying any (atomic redo of the batch) */
	aibhs = kcalloc(nr ? nr : 1, sizeof(*aibhs), GFP_NOFS);
	if (!aibhs) { brelse(dbh); return -ENOMEM; }
	ok = true;
	for (i = 0; i < nr && ok; i++) {
		aibhs[i] = sb_bread(sb, jblk_phys(j, pos + 1 + i));
		if (!aibhs[i] ||
		    ocsfs2_crc32c(~0U, aibhs[i]->b_data, sb->s_blocksize) !=
		    le32_to_cpu(desc->jd_ent[i].je_crc))
			ok = false;                          /* torn after-image */
	}
	ret = 1;
	if (ok) {
		ret = 0;
		for (i = 0; i < nr; i++) {
			ret = write_block(sb, le64_to_cpu(desc->jd_ent[i].je_home),
					  aibhs[i]->b_data);
			if (ret)
				break;                       /* I/O error */
		}
		if (!ret)
			*outlen = 1 + nr + 1;
	}
	for (i = 0; i < nr; i++)
		if (aibhs[i])
			brelse(aibhs[i]);
	kfree(aibhs);
	brelse(dbh);
	return ret;
}

int ocsfs2_journal_replay(struct super_block *sb)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_journal *j = &sbi->s_journal;
	bool deferred = true;   /* A9: deferred journal on all volumes -> log-scan */
	u64 pos, applied, len = 0, want_seq;

	(void)sbi;
	u32 ntxn = 0;
	int ret = 0;

	if (!j->j_active)
		return 0;

	mutex_lock(&j->j_lock);
	pos = j->j_tail;
	applied = pos;
	want_seq = j->j_seq;   /* header seq == seq of the txn at tail */

	if (!deferred && pos == j->j_head) {
		mutex_unlock(&j->j_lock);
		return 0;          /* cluster clean unmount: head == tail */
	}

	/* Deferred: the on-disk head lags (no per-commit header) — SCAN forward from
	 * tail, bounded by the ring, stopping at the first torn record or seq break.
	 * Cluster: the on-disk head is current — replay [tail, head). Either way the
	 * first invalid record ends replay (the ring is sequential). */
	while (deferred ? (pos - j->j_tail < j->j_ring_len) : (pos < j->j_head)) {
		int r = replay_one(sb, j, pos, deferred ? want_seq : U64_MAX, &len);

		if (r < 0) { ret = r; goto out; }
		if (r == 1)
			break;          /* torn / end of log */
		pos += len;
		applied = pos;
		want_seq++;
		ntxn++;
	}
	blkdev_issue_flush(sb->s_bdev);
	if (ntxn)
		pr_info("ocsfs2: replayed %u transaction(s) up to %llu\n", ntxn, applied);

	/* empty the ring: applied txns are durable on their home blocks */
	j->j_head = applied;
	j->j_tail = applied;
	if (deferred)
		j->j_seq = want_seq;
	ret = persist_header(j);
out:
	mutex_unlock(&j->j_lock);
	return ret;
}

/* L5: replay a DEAD peer's per-slot journal during recovery. Reads the dead
 * node's journal coherently (bio) and re-applies its committed-but-not-
 * checkpointed transaction to the home blocks, then empties that journal. The
 * caller holds the metadata lease (serialising vs live nodes) and has fenced
 * the dead node. */
int ocsfs2_journal_replay_slot(struct super_block *sb, u16 slot)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 bs = sb->s_blocksize, crc;
	u64 joff = sbi->s_journal_off + (u64)slot * sbi->s_journal_size;
	u64 first_blk = joff / bs;
	u64 ring_len = sbi->s_journal_size / bs - 1;
	struct buffer_head *hbh;
	struct ocsfs2_disk_journal_hdr *jh;
	u64 tail0, pos, applied, expected_seq;
	u32 ntxn = 0;
	int ret = 0;

	if (sbi->s_journal_size / bs < 4)
		return 0;
	hbh = ocsfs2_meta_bread(sb, first_blk);
	if (!hbh)
		return -EIO;
	jh = (struct ocsfs2_disk_journal_hdr *)hbh->b_data;
	crc = ocsfs2_crc32c(~0U, jh,
			    offsetof(struct ocsfs2_disk_journal_hdr, jh_checksum));
	if (le32_to_cpu(jh->jh_magic) != OCSFS2_JOURNAL_MAGIC ||
	    crc != le32_to_cpu(jh->jh_checksum)) {
		brelse(hbh);
		return -EUCLEAN;
	}

	/* A9 log-scan: a deferred-journal peer leaves the on-disk header at its last
	 * checkpoint and MANY committed txns after it. Re-apply each (validating seq
	 * continuity + every after-image crc) until a torn record or seq break. */
	tail0 = le64_to_cpu(jh->jh_tail);
	pos = tail0;
	applied = tail0;
	expected_seq = le64_to_cpu(jh->jh_sequence);

	while (pos - tail0 < ring_len) {
		struct buffer_head *d, *c, **ab;
		struct ocsfs2_disk_jdesc *de;
		struct ocsfs2_disk_jcommit *co;
		u64 sq;
		u32 n, k;
		bool ok;

		d = ocsfs2_meta_bread(sb, first_blk + 1 + (pos % ring_len));
		if (!d) { ret = -EIO; goto out; }
		de = (struct ocsfs2_disk_jdesc *)d->b_data;
		crc = ocsfs2_crc32c(~0U, de,
				    offsetof(struct ocsfs2_disk_jdesc, jd_checksum));
		sq = le64_to_cpu(de->jd_seq);
		if (le32_to_cpu(de->jd_magic) != OCSFS2_JOURNAL_MAGIC ||
		    le32_to_cpu(de->jd_type) != OCSFS2_JREC_DESC ||
		    crc != le32_to_cpu(de->jd_checksum) || sq != expected_seq) {
			brelse(d);
			break;
		}
		n = le32_to_cpu(de->jd_nr);
		if (n > OCSFS2_JTXN_MAX_BLOCKS || 1 + n + 1 > ring_len) {
			brelse(d);
			break;
		}

		c = ocsfs2_meta_bread(sb, first_blk + 1 + ((pos + 1 + n) % ring_len));
		if (!c) { brelse(d); ret = -EIO; goto out; }
		co = (struct ocsfs2_disk_jcommit *)c->b_data;
		crc = ocsfs2_crc32c(~0U, co,
				    offsetof(struct ocsfs2_disk_jcommit, jc_checksum));
		ok = le32_to_cpu(co->jc_magic) == OCSFS2_JOURNAL_MAGIC &&
		     le32_to_cpu(co->jc_type) == OCSFS2_JREC_COMMIT &&
		     le64_to_cpu(co->jc_seq) == sq &&
		     crc == le32_to_cpu(co->jc_checksum);
		brelse(c);
		if (!ok) { brelse(d); break; }

		/* validate ALL after-images, then apply (atomic redo of the batch) */
		ab = kcalloc(n ? n : 1, sizeof(*ab), GFP_NOFS);
		if (!ab) { brelse(d); ret = -ENOMEM; goto out; }
		for (k = 0; k < n && ok; k++) {
			ab[k] = ocsfs2_meta_bread(sb,
				first_blk + 1 + ((pos + 1 + k) % ring_len));
			if (!ab[k] || ocsfs2_crc32c(~0U, ab[k]->b_data, bs) !=
			    le32_to_cpu(de->jd_ent[k].je_crc))
				ok = false;
		}
		if (ok)
			for (k = 0; k < n && !ret; k++)
				ret = write_block(sb,
					le64_to_cpu(de->jd_ent[k].je_home),
					ab[k]->b_data);
		for (k = 0; k < n; k++)
			if (ab[k])
				brelse(ab[k]);
		kfree(ab);
		brelse(d);
		if (ret)
			goto out;
		if (!ok)
			break;          /* torn after-image -> stop */
		pos += 1 + n + 1;
		expected_seq++;
		applied = pos;
		ntxn++;
	}
	if (ntxn) {
		blkdev_issue_flush(sb->s_bdev);
		pr_info("ocsfs2: recovery: replayed dead slot %u, %u txn(s) up to %llu\n",
			slot, ntxn, applied);
	}

	/* empty the dead node's journal (tail=head=applied) so it isn't replayed
	 * again. jh aliases hbh->b_data, so flush hbh in place. */
	jh->jh_tail = cpu_to_le64(applied);
	jh->jh_head = cpu_to_le64(applied);
	jh->jh_sequence = cpu_to_le64(expected_seq);
	jh->jh_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, jh,
			  offsetof(struct ocsfs2_disk_journal_hdr, jh_checksum)));
	mark_buffer_dirty(hbh);
	if (sync_dirty_buffer(hbh))
		ret = ret ? ret : -EIO;
	blkdev_issue_flush(sb->s_bdev);
out:
	brelse(hbh);
	return ret;
}
