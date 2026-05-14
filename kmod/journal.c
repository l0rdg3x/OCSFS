// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — journal.c
 * Write-ahead logging (WAL) journal for crash recovery.
 *
 * Each node has its own journal region at:
 *   s_journal_off + (node_slot * s_journal_size)
 *
 * Journal init must run AFTER cluster_init so that s_node_slot is known.
 * Recovery of a failed node uses ocsfs_journal_replay_node() which opens
 * the target node's journal at its offset directly.
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

	/*
	 * Each node has its own journal at:
	 *   s_journal_off + (s_node_slot * s_journal_size)
	 * cluster_init() must have run before us so s_node_slot is set.
	 */
	journal_size = le32_to_cpu(sbi->s_ds->s_journal_size);
	journal_off = le64_to_cpu(sbi->s_ds->s_journal_off) +
		      (u64)sbi->s_node_slot * journal_size;

	j->disk_off = journal_off;
	j->size = journal_size;
	j->j_node_slot = sbi->s_node_slot;

	mutex_init(&j->j_lock);

	/* Read journal header */
	bh = sb_bread(sb, journal_off / sbi->s_block_size);
	if (!bh) {
		pr_err("ocsfs: failed to read journal header\n");
		return -EIO;
	}

	jh = (struct ocsfs_disk_journal_hdr *)bh->b_data;

	if (le32_to_cpu(jh->jh_magic) == OCSFS_JOURNAL_MAGIC) {
		j->head = le64_to_cpu(jh->jh_head);
		j->tail = le64_to_cpu(jh->jh_tail);
		j->sequence = le64_to_cpu(jh->jh_sequence);
	} else {
		/* Virgin journal — initialize */
		j->head = sizeof(struct ocsfs_disk_journal_hdr);
		j->tail = j->head;
		j->sequence = 1;
	}

	j->j_header_bh = bh;
	j->j_sb = sb;
	return 0;
}

void ocsfs_journal_exit(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_journal *j = &sbi->s_journal;

	/* Flush journal header */
	if (j->j_header_bh) {
		struct ocsfs_disk_journal_hdr *jh =
			(struct ocsfs_disk_journal_hdr *)j->j_header_bh->b_data;

		jh->jh_magic = cpu_to_le32(OCSFS_JOURNAL_MAGIC);
		jh->jh_node_slot = cpu_to_le16(j->j_node_slot);
		jh->jh_head = cpu_to_le64(j->head);
		jh->jh_tail = cpu_to_le64(j->tail);
		jh->jh_sequence = cpu_to_le64(j->sequence);
		jh->jh_size = cpu_to_le64(j->size);
		jh->jh_checksum = cpu_to_le32(
			ocsfs_crc32c(~0U, jh,
				     sizeof(*jh) - sizeof(__le32)));

		mark_buffer_dirty(j->j_header_bh);
		sync_dirty_buffer(j->j_header_bh);
		brelse(j->j_header_bh);
		j->j_header_bh = NULL;
	}
}

/* ═══════════════════════════════════════════════════════════════
 * JOURNAL WRITE HELPERS
 * ═══════════════════════════════════════════════════════════════ */

/* Write raw data into the journal at current head position.
 * Wraps around if necessary. Advances j->head. */
static int journal_write(struct super_block *sb, struct ocsfs_journal *j,
			 const void *data, size_t len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	const u8 *src = data;
	u64 journal_start = sizeof(struct ocsfs_disk_journal_hdr);
	u64 journal_data_size = j->size - journal_start;

	while (len > 0) {
		struct buffer_head *bh;
		u64 pos = journal_start + (j->head - journal_start) %
			  journal_data_size;
		u64 block = (j->disk_off + pos) / sbi->s_block_size;
		u32 boff = (j->disk_off + pos) % sbi->s_block_size;
		u32 chunk = min_t(u32, len, sbi->s_block_size - boff);

		bh = sb_bread(sb, block);
		if (!bh)
			return -EIO;

		memcpy(bh->b_data + boff, src, chunk);
		mark_buffer_dirty(bh);
		brelse(bh);

		src += chunk;
		len -= chunk;
		j->head += chunk;
	}

	return 0;
}

/* Sync all dirty journal blocks to disk */
static int journal_sync(struct super_block *sb, struct ocsfs_journal *j)
{
	/* sync_filesystem would be too broad; we rely on buffer_head
	 * sync semantics. Mark and sync the header to persist head/tail. */
	struct ocsfs_disk_journal_hdr *jh;

	if (!j->j_header_bh)
		return -EIO;

	jh = (struct ocsfs_disk_journal_hdr *)j->j_header_bh->b_data;
	jh->jh_magic = cpu_to_le32(OCSFS_JOURNAL_MAGIC);
	jh->jh_node_slot = cpu_to_le16(j->j_node_slot);
	jh->jh_head = cpu_to_le64(j->head);
	jh->jh_tail = cpu_to_le64(j->tail);
	jh->jh_sequence = cpu_to_le64(j->sequence);
	jh->jh_size = cpu_to_le64(j->size);
	jh->jh_checksum = cpu_to_le32(
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
	int ret;

	txn = kzalloc(sizeof(*txn), GFP_KERNEL);
	if (!txn)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&j->j_lock);

	txn->t_journal = j;
	txn->t_id = j->sequence++;
	txn->t_nr_blocks = 0;
	txn->t_started = true;
	INIT_LIST_HEAD(&txn->t_buffers);

	/* Write BEGIN record */
	memset(&jt, 0, sizeof(jt));
	jt.jt_type = cpu_to_le32(OCSFS_JTYPE_BEGIN);
	jt.jt_id = cpu_to_le64(txn->t_id);
	jt.jt_timestamp = cpu_to_le64(ktime_get_real_ns());
	jt.jt_node_slot = cpu_to_le16(j->j_node_slot);
	jt.jt_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, &jt, sizeof(jt) - sizeof(__le32)));

	ret = journal_write(sb, j, &jt, sizeof(jt));
	if (ret) {
		mutex_unlock(&j->j_lock);
		kfree(txn);
		return ERR_PTR(ret);
	}

	/* Keep lock held until commit/abort */
	return txn;
}

int ocsfs_txn_add_bh(struct ocsfs_txn *txn, struct buffer_head *bh)
{
	struct ocsfs_txn_buf *tb;
	struct ocsfs_journal *j = txn->t_journal;
	struct ocsfs_disk_journal_bref bref;
	struct super_block *sb = txn->t_journal->j_sb;
	int ret;

	tb = kzalloc(sizeof(*tb), GFP_KERNEL);
	if (!tb)
		return -ENOMEM;

	tb->bh = bh;
	tb->block_num = bh->b_blocknr;
	get_bh(bh);
	list_add_tail(&tb->list, &txn->t_buffers);
	txn->t_nr_blocks++;

	/* Write block reference + before-image to journal */
	memset(&bref, 0, sizeof(bref));
	bref.jbr_block_num = cpu_to_le64(bh->b_blocknr);
	bref.jbr_flags = cpu_to_le32(OCSFS_JBR_BEFORE);
	bref.jbr_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, bh->b_data, bh->b_size));

	ret = journal_write(sb, j, &bref, sizeof(bref));
	if (ret)
		return ret;

	/* Write the actual block data (before-image) */
	ret = journal_write(sb, j, bh->b_data, bh->b_size);
	return ret;
}

int ocsfs_txn_commit(struct ocsfs_txn *txn)
{
	struct ocsfs_journal *j = txn->t_journal;
	struct ocsfs_disk_journal_txn jt;
	struct ocsfs_txn_buf *tb, *tmp;
	struct super_block *sb;
	int ret;

	if (list_empty(&txn->t_buffers)) {
		/* Empty transaction — just clean up */
		mutex_unlock(&j->j_lock);
		kfree(txn);
		return 0;
	}

	sb = j->j_sb;

	/* Write COMMIT record */
	memset(&jt, 0, sizeof(jt));
	jt.jt_type = cpu_to_le32(OCSFS_JTYPE_COMMIT);
	jt.jt_id = cpu_to_le64(txn->t_id);
	jt.jt_timestamp = cpu_to_le64(ktime_get_real_ns());
	jt.jt_node_slot = cpu_to_le16(j->j_node_slot);
	jt.jt_block_count = cpu_to_le16(txn->t_nr_blocks);
	jt.jt_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, &jt, sizeof(jt) - sizeof(__le32)));

	ret = journal_write(sb, j, &jt, sizeof(jt));
	if (ret)
		goto out;

	/* Sync journal to ensure commit record is on disk */
	ret = journal_sync(sb, j);
	if (ret)
		goto out;

	/* Advance tail — committed transactions can be reclaimed */
	j->tail = j->head;

out:
	/* Free transaction buffers */
	list_for_each_entry_safe(tb, tmp, &txn->t_buffers, list) {
		list_del(&tb->list);
		brelse(tb->bh);
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

	/* Write ABORT record (best-effort) */
	/* On abort, we just free resources and unlock */

	list_for_each_entry_safe(tb, tmp, &txn->t_buffers, list) {
		list_del(&tb->list);
		brelse(tb->bh);
		kfree(tb);
	}

	if (txn->t_started)
		mutex_unlock(&j->j_lock);

	kfree(txn);
}

/* ═══════════════════════════════════════════════════════════════
 * JOURNAL REPLAY — crash recovery
 *
 * Scans the journal from tail to head. For each BEGIN that has no
 * matching COMMIT, replays the before-images to restore blocks
 * to their pre-transaction state.
 * ═══════════════════════════════════════════════════════════════ */

static int journal_read(struct super_block *sb, struct ocsfs_journal *j,
			u64 pos, void *buf, size_t len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u8 *dst = buf;
	u64 journal_start = sizeof(struct ocsfs_disk_journal_hdr);
	u64 journal_data_size = j->size - journal_start;

	while (len > 0) {
		struct buffer_head *bh;
		u64 wrapped = journal_start + (pos - journal_start) %
			      journal_data_size;
		u64 block = (j->disk_off + wrapped) / sbi->s_block_size;
		u32 boff = (j->disk_off + wrapped) % sbi->s_block_size;
		u32 chunk = min_t(u32, len, sbi->s_block_size - boff);

		bh = sb_bread(sb, block);
		if (!bh)
			return -EIO;

		memcpy(dst, bh->b_data + boff, chunk);
		brelse(bh);

		dst += chunk;
		len -= chunk;
		pos += chunk;
	}

	return 0;
}

int ocsfs_journal_replay(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_journal *j = &sbi->s_journal;
	struct ocsfs_disk_journal_txn jt;
	u64 scan_pos;
	int replayed = 0;
	int ret;

	if (j->tail == j->head) {
		pr_info("ocsfs: journal is clean\n");
		return 0;
	}

	pr_info("ocsfs: replaying journal (tail=%llu head=%llu)\n",
		j->tail, j->head);

	scan_pos = j->tail;

	while (scan_pos < j->head) {
		ret = journal_read(sb, j, scan_pos, &jt, sizeof(jt));
		if (ret)
			return ret;

		u32 type = le32_to_cpu(jt.jt_type);

		if (type == OCSFS_JTYPE_BEGIN) {
			/* Scan ahead to see if there's a matching COMMIT */
			u64 ahead = scan_pos + sizeof(jt);
			bool committed = false;
			u64 tid = le64_to_cpu(jt.jt_id);

			while (ahead < j->head) {
				struct ocsfs_disk_journal_txn jt2;

				ret = journal_read(sb, j, ahead, &jt2,
						   sizeof(jt2));
				if (ret)
					break;

				if (le32_to_cpu(jt2.jt_type) == OCSFS_JTYPE_COMMIT &&
				    le64_to_cpu(jt2.jt_id) == tid) {
					committed = true;
					break;
				}

				/* Skip over block references + data */
				if (le32_to_cpu(jt2.jt_type) == OCSFS_JTYPE_METADATA ||
				    le32_to_cpu(jt2.jt_type) == OCSFS_JTYPE_BEGIN ||
				    le32_to_cpu(jt2.jt_type) == OCSFS_JTYPE_COMMIT) {
					ahead += sizeof(jt2);
					if (le16_to_cpu(jt2.jt_block_count) > 0)
						ahead += le32_to_cpu(jt2.jt_data_len);
				} else {
					ahead += sizeof(jt2);
				}
			}

			if (!committed) {
				/* Uncommitted transaction — replay before-images */
				u64 replay_pos = scan_pos + sizeof(jt);

				while (replay_pos < j->head) {
					struct ocsfs_disk_journal_bref bref;

					ret = journal_read(sb, j, replay_pos,
							   &bref, sizeof(bref));
					if (ret)
						break;

					if (le32_to_cpu(bref.jbr_flags) &
					    OCSFS_JBR_BEFORE) {
						struct buffer_head *bh;
						u64 blk = le64_to_cpu(
							bref.jbr_block_num);

						replay_pos += sizeof(bref);

						bh = sb_bread(sb, blk);
						if (bh) {
							ret = journal_read(
								sb, j,
								replay_pos,
								bh->b_data,
								bh->b_size);
							if (ret == 0) {
								mark_buffer_dirty(bh);
								sync_dirty_buffer(bh);
								replayed++;
							}
							brelse(bh);
						}
						replay_pos += sbi->s_block_size;
					} else {
						break;
					}
				}

				pr_info("ocsfs: rolled back txn %llu "
					"(%d blocks)\n", tid, replayed);
			}

			scan_pos += sizeof(jt);
		} else if (type == OCSFS_JTYPE_COMMIT) {
			scan_pos += sizeof(jt);
		} else {
			/* Skip unknown record */
			scan_pos += sizeof(jt);
		}
	}

	/* Reset journal to clean state */
	j->tail = j->head;

	if (replayed > 0)
		pr_info("ocsfs: journal replay complete (%d blocks restored)\n",
			replayed);

	return 0;
}

/*
 * Replay a specific node's journal (Phase 2: recovery of failed node).
 * Sets up a temporary ocsfs_journal for the target node's journal
 * region and replays it.
 */
int ocsfs_journal_replay_node(struct super_block *sb, u16 node_slot)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_journal tmp_j;
	struct ocsfs_disk_journal_hdr *jh;
	struct buffer_head *bh;
	u64 journal_off;
	u32 journal_size;
	int ret;

	journal_size = le32_to_cpu(sbi->s_ds->s_journal_size);
	journal_off = le64_to_cpu(sbi->s_ds->s_journal_off) +
		      (u64)node_slot * journal_size;

	memset(&tmp_j, 0, sizeof(tmp_j));
	tmp_j.disk_off = journal_off;
	tmp_j.size = journal_size;
	mutex_init(&tmp_j.j_lock);

	/* Read the target node's journal header */
	bh = sb_bread(sb, journal_off / sbi->s_block_size);
	if (!bh) {
		pr_err("ocsfs: failed to read journal header for node %u\n",
		       node_slot);
		return -EIO;
	}

	jh = (struct ocsfs_disk_journal_hdr *)bh->b_data;

	if (le32_to_cpu(jh->jh_magic) != OCSFS_JOURNAL_MAGIC) {
		pr_info("ocsfs: node %u journal not initialized, skipping\n",
			node_slot);
		brelse(bh);
		return 0;
	}

	tmp_j.head = le64_to_cpu(jh->jh_head);
	tmp_j.tail = le64_to_cpu(jh->jh_tail);
	tmp_j.sequence = le64_to_cpu(jh->jh_sequence);
	tmp_j.j_header_bh = bh;

	if (tmp_j.tail == tmp_j.head) {
		pr_info("ocsfs: node %u journal is clean\n", node_slot);
		brelse(bh);
		return 0;
	}

	pr_info("ocsfs: replaying journal for node %u "
		"(tail=%llu head=%llu)\n",
		node_slot, tmp_j.tail, tmp_j.head);

	/*
	 * Temporarily swap in the target journal and replay it.
	 * This reuses the same replay logic from ocsfs_journal_replay.
	 */
	{
		struct ocsfs_journal saved = sbi->s_journal;
		sbi->s_journal = tmp_j;
		ret = ocsfs_journal_replay(sb);
		sbi->s_journal = saved;
	}

	/* Reset the replayed journal to clean state */
	jh->jh_head = jh->jh_tail;
	jh->jh_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, jh, sizeof(*jh) - sizeof(__le32)));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return ret;
}
