// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — journal.c
 * Write-ahead logging (WAL) journal for crash recovery.
 *
 * Each node has its own journal region at:
 *   s_journal_off + (node_slot * s_journal_size)
 *
 * Journal init must run AFTER cluster_init so that s_node_slot is known.
 * Recovery of a failed node uses ocsfs_journal_replay_node() in journal_replay.c.
 */

#include <linux/xxhash.h>
#include <linux/crypto.h>
#include <crypto/hash.h>
#include "ocsfs.h"

/*
 * ocsfs_journal_hmac_commit — compute HMAC-SHA256/128 of a COMMIT record.
 * Covers the first 28 bytes (excluding jt_checksum) to authenticate the
 * txn identity, block count, and data length.  ALTO-V3-10.
 */
int ocsfs_journal_hmac_commit(struct super_block *sb,
			      const struct ocsfs_disk_journal_txn *jt,
			      u8 *out16)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	u8 tmp[32];
	int ret;

	tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_NOFS);
	if (!desc) { crypto_free_shash(tfm); return -ENOMEM; }
	desc->tfm = tfm;
	ret = crypto_shash_setkey(tfm, sbi->s_cluster_secret, 32);
	if (!ret)
		ret = crypto_shash_digest(desc, (const u8 *)jt,
					  sizeof(*jt) - sizeof(__le32), tmp);
	kfree(desc);
	crypto_free_shash(tfm);
	if (!ret)
		memcpy(out16, tmp, 16);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * JOURNAL INIT / EXIT
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_journal_init(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_journal *j = &sbi->s_journal;
	struct ocsfs_disk_journal_hdr *jh;
	struct buffer_head *bh;
	u64 journal_off;
	u32 journal_size;

	journal_size = le32_to_cpu(sbi->s_ds->s_journal_size);
	if ((u64)journal_size <= sizeof(struct ocsfs_disk_journal_hdr)) {
		pr_err("ocsfs: journal region too small (%u bytes)\n", journal_size);
		return -EINVAL;
	}
	journal_off = le64_to_cpu(sbi->s_ds->s_journal_off) +
		      (u64)sbi->s_node_slot * journal_size;

	j->disk_off    = journal_off;
	j->size        = journal_size;
	j->j_node_slot = sbi->s_node_slot;

	mutex_init(&j->j_lock);

	bh = sb_bread(sb, journal_off / sbi->s_block_size);
	if (!bh) {
		pr_err("ocsfs: failed to read journal header\n");
		return -EIO;
	}

	jh = (struct ocsfs_disk_journal_hdr *)bh->b_data;

	if (le32_to_cpu(jh->jh_magic) == OCSFS_JOURNAL_MAGIC) {
		u32 crc = ocsfs_crc32c(~0U, jh, sizeof(*jh) - sizeof(__le32));

		if (crc != le32_to_cpu(jh->jh_checksum)) {
			pr_warn("ocsfs: journal header checksum mismatch — starting fresh\n");
			j->head     = sizeof(struct ocsfs_disk_journal_hdr);
			j->tail     = j->head;
			j->sequence = 1;
		} else {
			j->head     = le64_to_cpu(jh->jh_head);
			j->tail     = le64_to_cpu(jh->jh_tail);
			j->sequence = le64_to_cpu(jh->jh_sequence);
		}
	} else {
		j->head     = sizeof(struct ocsfs_disk_journal_hdr);
		j->tail     = j->head;
		j->sequence = 1;
	}

	j->j_header_bh = bh;
	j->j_sb        = sb;

	atomic64_set(&j->j_ckpt_ticket, 0);
	atomic64_set(&j->j_ckpt_now,    0);
	init_waitqueue_head(&j->j_ckpt_waitq);

	return 0;
}

void ocsfs_journal_exit(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_journal *j = &sbi->s_journal;

	/*
	 * Wait for all in-flight checkpoints to complete before writing the
	 * final journal header.  j_ckpt_ticket is the next ticket to hand
	 * out; j_ckpt_now is the one currently running.  Equal means idle.
	 */
	wait_event(j->j_ckpt_waitq,
		   atomic64_read(&j->j_ckpt_now) ==
		   atomic64_read(&j->j_ckpt_ticket));

	if (j->j_header_bh) {
		struct ocsfs_disk_journal_hdr *jh =
			(struct ocsfs_disk_journal_hdr *)j->j_header_bh->b_data;

		jh->jh_magic     = cpu_to_le32(OCSFS_JOURNAL_MAGIC);
		jh->jh_node_slot = cpu_to_le16(j->j_node_slot);
		jh->jh_head      = cpu_to_le64(j->head);
		jh->jh_tail      = cpu_to_le64(j->tail);
		jh->jh_sequence  = cpu_to_le64(j->sequence);
		jh->jh_size      = cpu_to_le64(j->size);
		jh->jh_checksum  = cpu_to_le32(
			ocsfs_crc32c(~0U, jh, sizeof(*jh) - sizeof(__le32)));

		mark_buffer_dirty(j->j_header_bh);
		sync_dirty_buffer(j->j_header_bh);
		brelse(j->j_header_bh);
		j->j_header_bh = NULL;
		blkdev_issue_flush(sb->s_bdev);
	}
}

/* ═══════════════════════════════════════════════════════════════
 * JOURNAL WRITE HELPERS (private — used only by transaction API)
 * ═══════════════════════════════════════════════════════════════ */

static int journal_write(struct super_block *sb, struct ocsfs_journal *j,
			 const void *data, size_t len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	const u8 *src = data;
	u64 journal_start    = sizeof(struct ocsfs_disk_journal_hdr);
	u64 journal_data_size = j->size - journal_start;
	u64 used;
	u64 available;

	/*
	 * Journal overflow protection: refuse the write if it would wrap
	 * over the uncommitted tail.  head/tail are monotonically increasing
	 * logical offsets, so (head - tail) is the in-flight payload size.
	 */
	used      = j->head - j->tail;
	available = (used > journal_data_size) ? 0 : journal_data_size - used;
	if (len > available) {
		pr_err("ocsfs: journal full (head=%llu tail=%llu len=%zu avail=%llu)\n",
		       j->head, j->tail, len, available);
		return -ENOSPC;
	}

	while (len > 0) {
		struct buffer_head *bh;
		u64 pos   = journal_start + (j->head - journal_start) %
			    journal_data_size;
		u64 block = (j->disk_off + pos) / sbi->s_block_size;
		u32 boff  = (j->disk_off + pos) % sbi->s_block_size;
		u32 chunk = min_t(u32, len, sbi->s_block_size - boff);

		bh = sb_bread(sb, block);
		if (!bh)
			return -EIO;

		memcpy(bh->b_data + boff, src, chunk);
		mark_buffer_dirty(bh);
		/*
		 * Do NOT sync_dirty_buffer here: a single commit can produce
		 * dozens of journal_write calls.  Sync is amortized in
		 * ocsfs_txn_commit() via blkdev_issue_flush() before COMMIT.
		 */
		brelse(bh);

		src      += chunk;
		len      -= chunk;
		j->head  += chunk;
	}

	return 0;
}

/*
 * Variant of journal_write that synchronously flushes the underlying bh.
 * Used ONLY for the COMMIT record: every preceding journal_write is async,
 * blkdev_issue_flush() guarantees those reach the platter, then this
 * synchronous write makes the COMMIT durable atomically.
 */
static int journal_write_sync(struct super_block *sb, struct ocsfs_journal *j,
			      const void *data, size_t len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	const u8 *src = data;
	u64 journal_start    = sizeof(struct ocsfs_disk_journal_hdr);
	u64 journal_data_size = j->size - journal_start;
	u64 used;
	u64 available;
	int ret = 0;

	used      = j->head - j->tail;
	available = (used > journal_data_size) ? 0 : journal_data_size - used;
	if (len > available) {
		pr_err("ocsfs: journal full on commit (used=%llu len=%zu)\n",
		       used, len);
		return -ENOSPC;
	}

	while (len > 0) {
		struct buffer_head *bh;
		u64 pos   = journal_start + (j->head - journal_start) %
			    journal_data_size;
		u64 block = (j->disk_off + pos) / sbi->s_block_size;
		u32 boff  = (j->disk_off + pos) % sbi->s_block_size;
		u32 chunk = min_t(u32, len, sbi->s_block_size - boff);

		bh = sb_bread(sb, block);
		if (!bh)
			return -EIO;

		memcpy(bh->b_data + boff, src, chunk);
		mark_buffer_dirty(bh);
		ret = sync_dirty_buffer(bh);
		brelse(bh);
		if (ret)
			return ret;

		src      += chunk;
		len      -= chunk;
		j->head  += chunk;
	}

	return 0;
}

static int journal_sync(struct super_block *sb, struct ocsfs_journal *j)
{
	struct ocsfs_disk_journal_hdr *jh;

	if (!j->j_header_bh)
		return -EIO;

	jh = (struct ocsfs_disk_journal_hdr *)j->j_header_bh->b_data;
	jh->jh_magic     = cpu_to_le32(OCSFS_JOURNAL_MAGIC);
	jh->jh_node_slot = cpu_to_le16(j->j_node_slot);
	jh->jh_head      = cpu_to_le64(j->head);
	jh->jh_tail      = cpu_to_le64(j->tail);
	jh->jh_sequence  = cpu_to_le64(j->sequence);
	jh->jh_size      = cpu_to_le64(j->size);
	jh->jh_checksum  = cpu_to_le32(
		ocsfs_crc32c(~0U, jh, sizeof(*jh) - sizeof(__le32)));

	mark_buffer_dirty(j->j_header_bh);
	return sync_dirty_buffer(j->j_header_bh);
}

/* ═══════════════════════════════════════════════════════════════
 * TRANSACTION API
 * ═══════════════════════════════════════════════════════════════ */

/* Minimum number of blocks a txn must be able to accommodate.
 * A typical OCSFS operation touches inode + dir block + bitmap + AG desc
 * (4–8 blocks); 16 gives comfortable headroom for BEFORE+AFTER images plus
 * the BEGIN/COMMIT records, without wasting excessive journal space.
 */
#define OCSFS_TXN_MIN_BLOCKS  16

struct ocsfs_txn *ocsfs_txn_begin(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_journal *j = &sbi->s_journal;
	struct ocsfs_txn *txn;
	struct ocsfs_disk_journal_txn jt;
	u64 journal_start;
	u64 journal_data_size;
	u64 used;
	u64 min_space;
	int ret;

	txn = kzalloc(sizeof(*txn), GFP_KERNEL);
	if (!txn)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&j->j_lock);

	/*
	 * Refuse to start a txn that cannot possibly fit at least BEGIN +
	 * one bref + one block + COMMIT.  Prevents partial writes from
	 * tripping ENOSPC inside add_bh/commit and leaving torn records.
	 */
	journal_start     = sizeof(struct ocsfs_disk_journal_hdr);
	journal_data_size = j->size - journal_start;
	used              = j->head - j->tail;
	min_space = (u64)sizeof(struct ocsfs_disk_journal_txn) * 2 +
		    (u64)sizeof(struct ocsfs_disk_journal_bref) * OCSFS_TXN_MIN_BLOCKS +
		    (u64)sb->s_blocksize * OCSFS_TXN_MIN_BLOCKS;
	if (used + min_space > journal_data_size) {
		mutex_unlock(&j->j_lock);
		kfree(txn);
		pr_err("ocsfs: txn_begin refused (journal near-full, used=%llu)\n",
		       used);
		return ERR_PTR(-ENOSPC);
	}

	txn->t_journal  = j;
	txn->t_nr_blocks = 0;
	txn->t_started  = true;
	INIT_LIST_HEAD(&txn->t_buffers);
	INIT_LIST_HEAD(&txn->t_locks);

	/*
	 * Save journal state before modifying it so we can roll back cleanly
	 * if journal_write() fails (e.g. journal full or I/O error).
	 */
	{
		u64 saved_head = j->head;
		u64 saved_seq  = j->sequence;

		txn->t_id = j->sequence++;

		memset(&jt, 0, sizeof(jt));
		jt.jt_type        = cpu_to_le32(OCSFS_JTYPE_BEGIN);
		jt.jt_id          = cpu_to_le64(txn->t_id);
		jt.jt_timestamp   = cpu_to_le64(ktime_get_real_ns());
		jt.jt_node_slot   = cpu_to_le16(j->j_node_slot);
		jt.jt_block_count = 0;
		jt.jt_data_len    = 0;
		jt.jt_checksum    = cpu_to_le32(
			ocsfs_crc32c(~0U, &jt, sizeof(jt) - sizeof(__le32)));

		ret = journal_write(sb, j, &jt, sizeof(jt));
		if (ret) {
			j->sequence = saved_seq;
			j->head     = saved_head;
			mutex_unlock(&j->j_lock);
			kfree(txn);
			return ERR_PTR(ret);
		}
	}

	return txn;
}

/* A DLM lock whose release is deferred until this txn commits/aborts. */
struct ocsfs_txn_lock {
	struct list_head        list;
	struct ocsfs_lock_res  *lr;
};

/*
 * Defer releasing @lr until this txn commits or aborts.  Used by the block
 * allocator for the AG lock: holding it across the commit makes the allocation
 * durable on disk before a peer can acquire the AG and read the bitmap, so two
 * nodes never hand out the same block.  On OOM we release immediately (correct,
 * just loses the cross-node protection for that rare case).
 */
void ocsfs_txn_defer_unlock(struct ocsfs_txn *txn, struct ocsfs_lock_res *lr)
{
	struct ocsfs_txn_lock *tl = kzalloc(sizeof(*tl), GFP_NOFS);

	if (!tl) {
		ocsfs_lock_release(txn->t_journal->j_sb, lr);
		return;
	}
	tl->lr = lr;
	list_add_tail(&tl->list, &txn->t_locks);
}

/* Release all locks deferred via ocsfs_txn_defer_unlock(). */
static void ocsfs_txn_release_locks(struct ocsfs_txn *txn)
{
	struct ocsfs_txn_lock *tl, *tmp;
	struct super_block *sb = txn->t_journal->j_sb;

	list_for_each_entry_safe(tl, tmp, &txn->t_locks, list) {
		ocsfs_lock_release(sb, tl->lr);
		list_del(&tl->list);
		kfree(tl);
	}
}

int ocsfs_txn_add_bh(struct ocsfs_txn *txn, struct buffer_head *bh)
{
	struct ocsfs_txn_buf *tb;
	struct ocsfs_journal *j = txn->t_journal;
	struct ocsfs_disk_journal_bref bref;
	struct super_block *sb = j->j_sb;
	u32 hslot = (u32)(bh->b_blocknr & 63U);
	int ret;

	/* O(1) idempotency check via hash-set (replaces O(n) list scan) */
	hlist_for_each_entry(tb, &txn->t_block_hash[hslot], hash_node) {
		if (tb->block_num == bh->b_blocknr)
			return 0;
	}

	tb = kzalloc(sizeof(*tb), GFP_KERNEL);
	if (!tb)
		return -ENOMEM;

	tb->before_buf = kmalloc(bh->b_size, GFP_KERNEL);
	if (!tb->before_buf) {
		kfree(tb);
		return -ENOMEM;
	}
	tb->after_buf = kmalloc(bh->b_size, GFP_KERNEL);
	if (!tb->after_buf) {
		kfree(tb->before_buf);
		kfree(tb);
		return -ENOMEM;
	}

	memcpy(tb->before_buf, bh->b_data, bh->b_size);
	tb->bh        = bh;
	tb->block_num = bh->b_blocknr;
	get_bh(bh);
	list_add_tail(&tb->list, &txn->t_buffers);
	hlist_add_head(&tb->hash_node, &txn->t_block_hash[hslot]);
	txn->t_nr_blocks++;

	memset(&bref, 0, sizeof(bref));
	bref.jbr_block_num = cpu_to_le64(bh->b_blocknr);
	/* 62-bit BEFORE-image hash: primary CRC in jbr_checksum, secondary CRC
	 * (different seed) packed into jbr_flags[31:2] (CRIT-V3-3). */
	bref.jbr_checksum  = cpu_to_le32(ocsfs_crc32c(~0U, bh->b_data, bh->b_size));
	/* ALTO-N4: use xxh64 for hash2 — independent from the CRC32C primary;
	 * seed ~1U CRC32C was linearly correlated with seed ~0U (same collisions). */
	bref.jbr_flags     = cpu_to_le32(OCSFS_JBR_BEFORE |
		((u32)(xxh64(bh->b_data, bh->b_size, 0) & OCSFS_JBR_HASH2_MASK)));

	{
		u64 saved_head = j->head;

		ret = journal_write(sb, j, &bref, sizeof(bref));
		if (ret)
			goto undo_tb;

		ret = journal_write(sb, j, bh->b_data, bh->b_size);
		if (ret)
			goto undo_tb;

		return 0;

undo_tb:
		j->head = saved_head;
		list_del(&tb->list);
		hlist_del(&tb->hash_node);
		txn->t_nr_blocks--;
		kfree(tb->before_buf);
		kfree(tb->after_buf);
		put_bh(bh);
		kfree(tb);
		return ret;
	}
}

int ocsfs_txn_commit(struct ocsfs_txn *txn)
{
	struct ocsfs_journal *j = txn->t_journal;
	struct ocsfs_disk_journal_txn jt;
	struct ocsfs_txn_buf *tb, *tmp;
	struct super_block *sb;
	u32 data_len;
	s64 my_ticket;
	u64 my_head;
	int ret;

	if (list_empty(&txn->t_buffers)) {
		mutex_unlock(&j->j_lock);
		kfree(txn);
		return 0;
	}

	sb = j->j_sb;

	/*
	 * Write AFTER-images before COMMIT so redo-replay can recover
	 * committed data that was lost before kernel writeback.
	 */
	list_for_each_entry(tb, &txn->t_buffers, list) {
		struct ocsfs_disk_journal_bref abref;

		/*
		 * Snapshot the block under lock_buffer to avoid a torn read if
		 * kernel writeback is running concurrently. The shadow copy is
		 * what we store in the journal as the AFTER-image.
		 */
		lock_buffer(tb->bh);
		memcpy(tb->after_buf, tb->bh->b_data, tb->bh->b_size);
		unlock_buffer(tb->bh);

		memset(&abref, 0, sizeof(abref));
		abref.jbr_block_num = cpu_to_le64(tb->bh->b_blocknr);
		abref.jbr_flags     = cpu_to_le32(OCSFS_JBR_AFTER);
		abref.jbr_checksum  = cpu_to_le32(
			ocsfs_crc32c(~0U, tb->after_buf, tb->bh->b_size));

		ret = journal_write(sb, j, &abref, sizeof(abref));
		if (ret)
			goto out_locked;
		ret = journal_write(sb, j, tb->after_buf, tb->bh->b_size);
		if (ret)
			goto out_locked;
	}

	/* jt_block_count is __le16 — truncation to U16_MAX would corrupt replay */
	if (txn->t_nr_blocks > U16_MAX) {
		pr_err("ocsfs: txn %llu: t_nr_blocks %u exceeds u16 max, aborting\n",
		       txn->t_id, txn->t_nr_blocks);
		ret = -EINVAL;
		goto out_locked;
	}

	/*
	 * data_len: BEFORE-images (from add_bh) + AFTER-images (written above).
	 * Each bh contributes 2 * (bref + block) to the payload.
	 * Max: 65535 * 2 * ~4112 ≈ 539 MiB, well within u32.
	 */
	data_len = (u32)txn->t_nr_blocks * 2 *
		   (u32)(sizeof(struct ocsfs_disk_journal_bref) +
			 sb->s_blocksize);

	memset(&jt, 0, sizeof(jt));
	jt.jt_type        = cpu_to_le32(OCSFS_JTYPE_COMMIT);
	jt.jt_id          = cpu_to_le64(txn->t_id);
	jt.jt_timestamp   = cpu_to_le64(ktime_get_real_ns());
	jt.jt_node_slot   = cpu_to_le16(j->j_node_slot);
	jt.jt_block_count = cpu_to_le16(txn->t_nr_blocks);
	jt.jt_data_len    = cpu_to_le32(data_len);
	jt.jt_checksum    = cpu_to_le32(
		ocsfs_crc32c(~0U, &jt, sizeof(jt) - sizeof(__le32)));

	/*
	 * Barrier: ensure all BEFORE+AFTER payload (written async above)
	 * reaches the platter BEFORE the COMMIT record becomes durable.
	 * One flush amortizes dozens of journal_write calls.
	 */
	ret = blkdev_issue_flush(sb->s_bdev);
	if (ret)
		goto out_locked;

	/* Synchronous write of the COMMIT record itself. */
	ret = journal_write_sync(sb, j, &jt, sizeof(jt));
	if (ret)
		goto out_locked;

	ret = journal_sync(sb, j);
	if (ret)
		goto out_locked;

	/* ALTO-V3-10: write HMAC record immediately after COMMIT when feature
	 * is active, so journal_replay can verify authenticity of AFTER-images. */
	if (OCSFS_SB(sb)->s_feature_incompat & OCSFS_FEATURE_INCOMPAT_JOURNAL_HMAC) {
		struct ocsfs_disk_journal_hmac_rec hr;
		u8 hmac[16];

		if (ocsfs_journal_hmac_commit(sb, &jt, hmac) == 0) {
			memset(&hr, 0, sizeof(hr));
			hr.jhr_type     = cpu_to_le32(OCSFS_JTYPE_HMAC);
			hr.jhr_txn_id   = jt.jt_id;
			memcpy(hr.jhr_hmac, hmac, 16);
			hr.jhr_checksum = cpu_to_le32(
				ocsfs_crc32c(~0U, &hr,
					     sizeof(hr) - sizeof(__le32)));
			journal_write_sync(sb, j, &hr, sizeof(hr));
		}
	}

	/*
	 * COMMIT is durable.  Claim a checkpoint ticket (in commit order, under
	 * j_lock) then release j_lock so other txns can begin journaling while
	 * this txn checkpoints its blocks to their final disk locations.
	 *
	 * The ticket ensures checkpoints run in FIFO order, which guarantees
	 * j->tail only advances monotonically and recovery remains correct:
	 * if txn A committed before B, A checkpoints before B, so j->tail
	 * never skips over A's AFTER-images before they reach final locations.
	 */
	my_ticket = atomic64_add_return(1, &j->j_ckpt_ticket) - 1;
	my_head   = j->head;
	mutex_unlock(&j->j_lock);

	/*
	 * Wait for our turn.  Earlier txns may still be checkpointing;
	 * sleep until j_ckpt_now equals our ticket.
	 */
	wait_event(j->j_ckpt_waitq,
		   atomic64_read(&j->j_ckpt_now) == my_ticket);

	/*
	 * Checkpoint: write all modified blocks to their final disk locations.
	 * Submit all writes in parallel (one SAN round-trip per txn regardless
	 * of block count), then wait for completion.
	 *
	 * j_lock is NOT held here, so new txns can commit their journal writes
	 * concurrently.  The ticket ensures only one checkpoint runs at a time,
	 * preserving tail ordering.
	 *
	 * Why syncing blocks before advancing tail matters: if we move tail
	 * before blocks reach disk and then crash, recovery sees an empty
	 * journal window and skips replay — the AFTER-images are gone and the
	 * data blocks are not at their final locations.  Syncing first ensures
	 * the checkpoint is only "recorded" once durable.
	 */
	{
		int ckpt_ret = 0;

		list_for_each_entry(tb, &txn->t_buffers, list) {
			mark_buffer_dirty(tb->bh);
			write_dirty_buffer(tb->bh, REQ_SYNC);
		}
		list_for_each_entry(tb, &txn->t_buffers, list) {
			wait_on_buffer(tb->bh);
			if (!buffer_uptodate(tb->bh) && !ckpt_ret)
				ckpt_ret = -EIO;
		}
		if (!ckpt_ret)
			ckpt_ret = blkdev_issue_flush(sb->s_bdev);

		/* Advance tail under j_lock for consistency with journal_write readers. */
		mutex_lock(&j->j_lock);
		if (!ckpt_ret) {
			j->tail = my_head;
		} else {
			pr_warn_ratelimited(
				"ocsfs: journal checkpoint failed (%d) — "
				"tail not advanced, recovery will replay\n",
				ckpt_ret);
		}
		mutex_unlock(&j->j_lock);
	}

	/* Signal the next checkpoint it may proceed. */
	atomic64_inc(&j->j_ckpt_now);
	wake_up_all(&j->j_ckpt_waitq);

	goto cleanup;

out_locked:
	mutex_unlock(&j->j_lock);

cleanup:
	list_for_each_entry_safe(tb, tmp, &txn->t_buffers, list) {
		list_del(&tb->list);
		brelse(tb->bh);
		kfree(tb->before_buf);
		kfree(tb->after_buf);
		kfree(tb);
	}

	/* Release AG (and other) locks held across this txn — now that the
	 * allocation is durable, a peer may take them and see our changes. */
	ocsfs_txn_release_locks(txn);

	txn->t_started = false;
	kfree(txn);
	return ret;
}

void ocsfs_txn_abort(struct ocsfs_txn *txn)
{
	struct ocsfs_journal *j = txn->t_journal;
	struct ocsfs_txn_buf *tb, *tmp;

	/*
	 * Restore each buffer to its pre-txn state so that in-memory bitmap
	 * bits (and other metadata) are not left in a partially-modified state
	 * when the txn is abandoned without a COMMIT record.
	 */
	list_for_each_entry_safe(tb, tmp, &txn->t_buffers, list) {
		lock_buffer(tb->bh);
		memcpy(tb->bh->b_data, tb->before_buf, tb->bh->b_size);
		clear_buffer_dirty(tb->bh);
		set_buffer_uptodate(tb->bh);
		unlock_buffer(tb->bh);
		list_del(&tb->list);
		brelse(tb->bh);
		kfree(tb->before_buf);
		kfree(tb->after_buf);
		kfree(tb);
	}

	if (txn->t_started) {
		struct ocsfs_disk_journal_txn jt;

		memset(&jt, 0, sizeof(jt));
		jt.jt_type      = cpu_to_le32(OCSFS_JTYPE_ABORT);
		jt.jt_id        = cpu_to_le64(txn->t_id);
		jt.jt_timestamp = cpu_to_le64(ktime_get_real_ns());
		jt.jt_node_slot = cpu_to_le16(j->j_node_slot);
		jt.jt_checksum  = cpu_to_le32(
			ocsfs_crc32c(~0U, &jt, sizeof(jt) - sizeof(__le32)));
		journal_write(j->j_sb, j, &jt, sizeof(jt));

		mutex_unlock(&j->j_lock);
	}

	/* Release locks held across the txn (the allocation was rolled back to
	 * its pre-txn state above, so a peer taking the AG sees it as free). */
	ocsfs_txn_release_locks(txn);

	kfree(txn);
}
