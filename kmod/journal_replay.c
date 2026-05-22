// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — journal_replay.c
 * Journal crash recovery: scan tail→head, roll back uncommitted transactions.
 * journal_write / journal_sync helpers and the transaction API are in journal.c.
 */

#include "ocsfs.h"

/* Read raw data from the journal at a given logical position */
static int journal_read(struct super_block *sb, struct ocsfs_journal *j,
			u64 pos, void *buf, size_t len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u8 *dst = buf;
	u64 journal_start    = sizeof(struct ocsfs_disk_journal_hdr);
	u64 journal_data_size = j->size - journal_start;

	while (len > 0) {
		struct buffer_head *bh;
		u64 wrapped = journal_start + (pos - journal_start) %
			      journal_data_size;
		u64 block = (j->disk_off + wrapped) / sbi->s_block_size;
		u32 boff  = (j->disk_off + wrapped) % sbi->s_block_size;
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

/* ═══════════════════════════════════════════════════════════════
 * JOURNAL REPLAY
 *
 * Scans tail→head. Committed txns: apply AFTER-images (redo).
 * Uncommitted/aborted txns: apply BEFORE-images (undo).
 * ═══════════════════════════════════════════════════════════════ */

static int journal_replay_j(struct super_block *sb, struct ocsfs_journal *j)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
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
		u32 type;
		u64 tid;
		bool committed = false;
		u64 ahead;

		ret = journal_read(sb, j, scan_pos, &jt, sizeof(jt));
		if (ret)
			return ret;

		type = le32_to_cpu(jt.jt_type);

		if (type != OCSFS_JTYPE_BEGIN) {
			/* Stray or corrupted record — skip one header */
			scan_pos += sizeof(jt);
			continue;
		}

		tid = le64_to_cpu(jt.jt_id);

		/*
		 * Forward-scan for the matching COMMIT.  Payload between BEGIN
		 * and COMMIT is a sequence of fixed-stride (bref + block) units,
		 * so we probe at each stride offset.  Stop if we hit the next
		 * BEGIN (this txn never committed) or fall off j->head.
		 */
		ahead = scan_pos + sizeof(jt);
		while (ahead + sizeof(jt) <= j->head) {
			struct ocsfs_disk_journal_txn jt2;

			ret = journal_read(sb, j, ahead, &jt2, sizeof(jt2));
			if (ret)
				break;

			if (le32_to_cpu(jt2.jt_type) == OCSFS_JTYPE_COMMIT &&
			    le64_to_cpu(jt2.jt_id) == tid) {
				u32 crc = ocsfs_crc32c(~0U, &jt2,
					sizeof(jt2) - sizeof(__le32));
				if (le32_to_cpu(jt2.jt_checksum) == crc) {
					committed = true;
					break;
				}
			}
			if (le32_to_cpu(jt2.jt_type) == OCSFS_JTYPE_ABORT &&
			    le64_to_cpu(jt2.jt_id) == tid) {
				/* Explicit abort — rollback same as uncommitted */
				break;
			}
			if (le32_to_cpu(jt2.jt_type) == OCSFS_JTYPE_BEGIN) {
				/* Next txn started — current never committed */
				break;
			}

			ahead += sizeof(struct ocsfs_disk_journal_bref) +
				 sbi->s_block_size;
		}

		if (!committed) {
			u64 replay_pos = scan_pos + sizeof(jt);
			int this_replayed = 0;
			u64 stride = sizeof(struct ocsfs_disk_journal_bref) +
				     sbi->s_block_size;

			while (replay_pos + stride <= j->head) {
				struct ocsfs_disk_journal_bref bref;
				u64 blk;
				struct buffer_head *bh;

				ret = journal_read(sb, j, replay_pos,
						   &bref, sizeof(bref));
				if (ret)
					break;
				if (!(le32_to_cpu(bref.jbr_flags) &
				      OCSFS_JBR_BEFORE))
					break;

				replay_pos += sizeof(bref);
				blk = le64_to_cpu(bref.jbr_block_num);
				bh  = sb_bread(sb, blk);
				if (bh) {
					if (!journal_read(sb, j, replay_pos,
							  bh->b_data,
							  bh->b_size)) {
						mark_buffer_dirty(bh);
						sync_dirty_buffer(bh);
						this_replayed++;
						replayed++;
					}
					brelse(bh);
				}
				replay_pos += sbi->s_block_size;
			}

			pr_info("ocsfs: rolled back txn %llu (%d blocks)\n",
				tid, this_replayed);
			scan_pos = replay_pos;
		} else {
			/* Redo: apply AFTER-images so committed data survives crash */
			u64 replay_pos = scan_pos + sizeof(jt);
			u64 stride = sizeof(struct ocsfs_disk_journal_bref) +
				     sbi->s_block_size;
			int this_replayed = 0;

			while (replay_pos + stride <= ahead) {
				struct ocsfs_disk_journal_bref bref;
				u32 flags;

				ret = journal_read(sb, j, replay_pos,
						   &bref, sizeof(bref));
				if (ret)
					break;

				flags = le32_to_cpu(bref.jbr_flags);
				replay_pos += sizeof(bref);

				if (flags & OCSFS_JBR_AFTER) {
					u64 blk = le64_to_cpu(bref.jbr_block_num);
					struct buffer_head *bh = sb_bread(sb, blk);

					if (bh) {
						if (!journal_read(sb, j, replay_pos,
								  bh->b_data,
								  bh->b_size)) {
							mark_buffer_dirty(bh);
							sync_dirty_buffer(bh);
							this_replayed++;
							replayed++;
						}
						brelse(bh);
					}
				}
				replay_pos += sbi->s_block_size;
			}

			if (this_replayed > 0)
				pr_info("ocsfs: redo txn %llu (%d blocks)\n",
					tid, this_replayed);
			scan_pos = ahead + sizeof(jt);
		}
	}

	j->tail = j->head;

	/* Persist the updated tail so replay is not re-run on next mount. */
	if (j->j_header_bh) {
		struct ocsfs_disk_journal_hdr *jh =
			(struct ocsfs_disk_journal_hdr *)j->j_header_bh->b_data;
		jh->jh_head     = cpu_to_le64(j->head);
		jh->jh_tail     = cpu_to_le64(j->tail);
		jh->jh_checksum = cpu_to_le32(
			ocsfs_crc32c(~0U, jh, sizeof(*jh) - sizeof(__le32)));
		mark_buffer_dirty(j->j_header_bh);
		sync_dirty_buffer(j->j_header_bh);
	}

	if (replayed > 0)
		pr_info("ocsfs: journal replay complete (%d blocks restored)\n",
			replayed);

	return 0;
}

int ocsfs_journal_replay(struct super_block *sb)
{
	return journal_replay_j(sb, &OCSFS_SB(sb)->s_journal);
}

/*
 * Replay a specific node's journal (used by crash recovery of a peer node).
 * Uses journal_replay_j directly — no sbi->s_journal swap needed.
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
	if ((u64)journal_size <= sizeof(struct ocsfs_disk_journal_hdr)) {
		pr_err("ocsfs: node %u journal region too small (%u bytes)\n",
		       node_slot, journal_size);
		return -EINVAL;
	}
	journal_off  = le64_to_cpu(sbi->s_ds->s_journal_off) +
		       (u64)node_slot * journal_size;

	memset(&tmp_j, 0, sizeof(tmp_j));
	tmp_j.disk_off = journal_off;
	tmp_j.size     = journal_size;
	mutex_init(&tmp_j.j_lock);

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

	tmp_j.head     = le64_to_cpu(jh->jh_head);
	tmp_j.tail     = le64_to_cpu(jh->jh_tail);
	tmp_j.sequence = le64_to_cpu(jh->jh_sequence);
	tmp_j.j_header_bh = bh;
	tmp_j.j_sb     = sb;

	if (tmp_j.tail == tmp_j.head) {
		pr_info("ocsfs: node %u journal is clean\n", node_slot);
		brelse(bh);
		return 0;
	}

	pr_info("ocsfs: replaying journal for node %u (tail=%llu head=%llu)\n",
		node_slot, tmp_j.tail, tmp_j.head);

	ret = journal_replay_j(sb, &tmp_j);

	jh->jh_head     = jh->jh_tail;
	jh->jh_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, jh, sizeof(*jh) - sizeof(__le32)));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return ret;
}
