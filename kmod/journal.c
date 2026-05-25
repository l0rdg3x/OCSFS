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

#include "ocsfs.h"

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
	return 0;
}

void ocsfs_journal_exit(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_journal *j = &sbi->s_journal;

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
		    (u64)sizeof(struct ocsfs_disk_journal_bref) * 2 +
		    (u64)sb->s_blocksize * 2;
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

int ocsfs_txn_add_bh(struct ocsfs_txn *txn, struct buffer_head *bh)
{
	struct ocsfs_txn_buf *tb;
	struct ocsfs_journal *j = txn->t_journal;
	struct ocsfs_disk_journal_bref bref;
	struct super_block *sb = j->j_sb;
	int ret;

	/* Idempotent: skip if this block is already journaled in this txn */
	list_for_each_entry(tb, &txn->t_buffers, list) {
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
	txn->t_nr_blocks++;

	memset(&bref, 0, sizeof(bref));
	bref.jbr_block_num = cpu_to_le64(bh->b_blocknr);
	bref.jbr_flags     = cpu_to_le32(OCSFS_JBR_BEFORE);
	bref.jbr_checksum  = cpu_to_le32(
		ocsfs_crc32c(~0U, bh->b_data, bh->b_size));

	ret = journal_write(sb, j, &bref, sizeof(bref));
	if (ret)
		return ret;

	return journal_write(sb, j, bh->b_data, bh->b_size);
}

int ocsfs_txn_commit(struct ocsfs_txn *txn)
{
	struct ocsfs_journal *j = txn->t_journal;
	struct ocsfs_disk_journal_txn jt;
	struct ocsfs_txn_buf *tb, *tmp;
	struct super_block *sb;
	u32 data_len;
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
			goto out;
		ret = journal_write(sb, j, tb->after_buf, tb->bh->b_size);
		if (ret)
			goto out;
	}

	/*
	 * data_len: BEFORE-images (from add_bh) + AFTER-images (written above).
	 * Each bh contributes 2 * (bref + block) to the payload.
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
		goto out;

	/* Synchronous write of the COMMIT record itself. */
	ret = journal_write_sync(sb, j, &jt, sizeof(jt));
	if (ret)
		goto out;

	ret = journal_sync(sb, j);
	if (ret)
		goto out;

	/*
	 * COMMIT durable.  Now write all modified blocks to their final disk
	 * locations synchronously before advancing j->tail.
	 *
	 * Why this ordering matters: j->tail is persisted in journal_sync()
	 * above.  If we advance j->tail = j->head and then crash before the
	 * data blocks reach disk, recovery will see tail == head and replay
	 * nothing — the committed AFTER-images in the journal are effectively
	 * lost.  Syncing the data blocks first means the checkpoint is only
	 * recorded once the data is guaranteed durable; if sync fails, tail
	 * stays where it was and the next recovery can re-apply the AFTER-images.
	 */
	{
		int ckpt_ret = 0;

		/*
		 * Checkpoint: write all modified blocks to their final disk
		 * locations.  Submit all writes in parallel (one round-trip on
		 * SAN regardless of transaction size), then wait for completion.
		 * This replaces the previous serial sync_dirty_buffer() loop which
		 * incurred one SAN round-trip per block.
		 */
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

		if (!ckpt_ret) {
			j->tail = j->head;
		} else {
			pr_warn_ratelimited(
				"ocsfs: journal checkpoint failed (%d) — "
				"tail not advanced, recovery will replay\n",
				ckpt_ret);
		}
	}

out:
	list_for_each_entry_safe(tb, tmp, &txn->t_buffers, list) {
		list_del(&tb->list);
		brelse(tb->bh);
		kfree(tb->before_buf);
		kfree(tb->after_buf);
		kfree(tb);
	}

	txn->t_started = false;
	mutex_unlock(&j->j_lock);
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

	kfree(txn);
}
