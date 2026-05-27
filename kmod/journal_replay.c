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

	if (pos < journal_start)
		pos = journal_start;

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

	/* Wrap-around sanity: tail must not exceed head */
	if (j->tail > j->head || j->head > j->size) {
		pr_err("ocsfs: journal corrupt (tail=%llu head=%llu size=%llu) — reset\n",
		       j->tail, j->head, j->size);
		j->tail = j->head = sizeof(struct ocsfs_disk_journal_hdr);
		return -EUCLEAN;
	}

	pr_info("ocsfs: replaying journal (tail=%llu head=%llu)\n",
		j->tail, j->head);

	scan_pos = j->tail;

	while (scan_pos < j->head) {
		u32 type, crc_got, crc_exp;
		u64 tid;
		bool committed = false;
		u64 ahead;

		ret = journal_read(sb, j, scan_pos, &jt, sizeof(jt));
		if (ret)
			return ret;

		type = le32_to_cpu(jt.jt_type);

		if (type != OCSFS_JTYPE_BEGIN) {
			pr_err("ocsfs: journal replay: unexpected record type %u "
			       "at pos %llu — aborting, filesystem requires fsck\n",
			       type, scan_pos);
			return -EUCLEAN;
		}

		/* Verify BEGIN record integrity before trusting its fields */
		crc_exp = le32_to_cpu(jt.jt_checksum);
		crc_got = ocsfs_crc32c(~0U, &jt, sizeof(jt) - sizeof(__le32));
		if (crc_got != crc_exp) {
			pr_err("ocsfs: journal replay: BEGIN CRC mismatch at pos %llu "
			       "(exp=%08x got=%08x) — aborting\n",
			       scan_pos, crc_exp, crc_got);
			return -EUCLEAN;
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
				u32 acrc = ocsfs_crc32c(~0U, &jt2,
					sizeof(jt2) - sizeof(__le32));
				if (le32_to_cpu(jt2.jt_checksum) == acrc)
					break; /* confirmed ABORT */
				/* CRC mismatch: random data bytes, not a real ABORT */
			}
			if (le32_to_cpu(jt2.jt_type) == OCSFS_JTYPE_BEGIN) {
				u32 bcrc = ocsfs_crc32c(~0U, &jt2,
					sizeof(jt2) - sizeof(__le32));
				if (le32_to_cpu(jt2.jt_checksum) == bcrc)
					break; /* confirmed next txn BEGIN */
				/* CRC mismatch: data bytes, not a real BEGIN */
			}

			ahead += sizeof(struct ocsfs_disk_journal_bref) +
				 sbi->s_block_size;
		}

		if (!committed) {
			u64 replay_pos = scan_pos + sizeof(jt);
			int this_replayed = 0;
			u64 stride = sizeof(struct ocsfs_disk_journal_bref) +
				     sbi->s_block_size;

			/*
			 * Consume all bref+block pairs (BEFORE and AFTER).
			 * A crash between AFTER-write and COMMIT leaves AFTER
			 * images in the journal without a COMMIT record.  We
			 * must skip those images to stay aligned; only BEFORE
			 * images are applied (rollback).
			 */
			while (replay_pos + stride <= j->head) {
				struct ocsfs_disk_journal_bref bref;
				u32 flags;

				ret = journal_read(sb, j, replay_pos,
						   &bref, sizeof(bref));
				if (ret)
					break;

				flags = le32_to_cpu(bref.jbr_flags);

				/* Stop at anything that is not a bref */
				if (!(flags & (OCSFS_JBR_BEFORE | OCSFS_JBR_AFTER)))
					break;

				replay_pos += sizeof(bref);

				if (flags & OCSFS_JBR_BEFORE) {
					u64 blk = le64_to_cpu(bref.jbr_block_num);
					u32 expected_crc = le32_to_cpu(bref.jbr_checksum);
					struct buffer_head *bh = sb_bread(sb, blk);

					if (bh) {
						if (!journal_read(sb, j, replay_pos,
								  bh->b_data,
								  bh->b_size)) {
							u32 actual = ocsfs_crc32c(~0U,
								bh->b_data,
								bh->b_size);
							if (actual == expected_crc) {
								mark_buffer_dirty(bh);
								sync_dirty_buffer(bh);
								clear_buffer_uptodate(bh);
								this_replayed++;
								replayed++;
							} else {
								pr_warn("ocsfs: BEFORE-image CRC mismatch for block %llu in txn %llu, skipping\n",
									blk, tid);
								clear_buffer_uptodate(bh);
							}
						}
						brelse(bh);
					}
				}
				/* AFTER images in an uncommitted txn are skipped */

				replay_pos += sbi->s_block_size;
			}

			pr_info("ocsfs: rolled back txn %llu (%d blocks)\n",
				tid, this_replayed);
			scan_pos = replay_pos;
		} else {
			/*
			 * Redo: apply AFTER-images so committed data survives
			 * crash.
			 *
			 * Coherence guard (CRIT-3): before applying each
			 * AFTER-image, verify that the current disk content
			 * matches the stored BEFORE-image CRC for the same
			 * block.  A mismatch means a live peer node wrote to
			 * the block after the dead node committed and released
			 * EX — that peer's data is newer, so we skip the
			 * stale AFTER-image.
			 *
			 * Implementation: single allocation split into two
			 * parallel arrays (before_blks, before_crcs).  Pass 1
			 * fills them from BEFORE entries; Pass 2 applies AFTER
			 * entries using the map as a gate.
			 */
			u64 payload_start = scan_pos + sizeof(jt);
			u64 stride = sizeof(struct ocsfs_disk_journal_bref) +
				     sbi->s_block_size;
			u32 max_ent = (ahead > payload_start + stride) ?
				(u32)((ahead - payload_start) / stride) : 0;
			int this_replayed = 0;
			u64 *before_blks = NULL;
			u32 *before_crcs = NULL;
			u32 before_count = 0;

			/* Single kvmalloc: [max_ent u64s][max_ent u32s] */
			if (max_ent) {
				before_blks = kvmalloc(
					max_ent * (sizeof(u64) + sizeof(u32)),
					GFP_KERNEL);
				if (before_blks)
					before_crcs = (u32 *)(before_blks +
							      max_ent);
			}

			/* Pass 1: collect BEFORE-image CRCs */
			if (before_blks) {
				u64 rpos = payload_start;

				while (rpos + stride <= ahead) {
					struct ocsfs_disk_journal_bref b2;

					if (journal_read(sb, j, rpos,
							 &b2, sizeof(b2)))
						break;
					if ((le32_to_cpu(b2.jbr_flags) &
					     OCSFS_JBR_BEFORE) &&
					    before_count < max_ent) {
						before_blks[before_count] =
						    le64_to_cpu(b2.jbr_block_num);
						before_crcs[before_count] =
						    le32_to_cpu(b2.jbr_checksum);
						before_count++;
					}
					rpos += stride;
				}
			}

			/* Pass 2: apply AFTER-images, gated by BEFORE check */
			{
				u64 replay_pos = payload_start;

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
						u64 blk = le64_to_cpu(
							bref.jbr_block_num);
						u32 exp_crc = le32_to_cpu(
							bref.jbr_checksum);
						struct buffer_head *bh =
							sb_bread(sb, blk);

						if (bh) {
							bool skip = false;

							if (before_blks) {
								u32 bi;

								for (bi = 0; bi < before_count; bi++) {
									if (before_blks[bi] != blk)
										continue;
									/* Block modified by peer if CRC changed */
									if (ocsfs_crc32c(~0U, bh->b_data, bh->b_size) != before_crcs[bi]) {
										pr_info("ocsfs: skip AFTER blk %llu txn %llu (peer-modified)\n",
											blk, tid);
										skip = true;
									}
									break;
								}
							}

							if (!skip && !journal_read(
								    sb, j,
								    replay_pos,
								    bh->b_data,
								    bh->b_size)) {
								u32 actual = ocsfs_crc32c(
									~0U,
									bh->b_data,
									bh->b_size);
								if (actual == exp_crc) {
									mark_buffer_dirty(bh);
									sync_dirty_buffer(bh);
									clear_buffer_uptodate(bh);
									this_replayed++;
									replayed++;
								} else {
									pr_warn("ocsfs: AFTER CRC mismatch blk %llu txn %llu\n",
										blk, tid);
									clear_buffer_uptodate(bh);
								}
							}
							brelse(bh);
						}
					}
					replay_pos += sbi->s_block_size;
				}
			}

			kvfree(before_blks);

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

	{
		u32 crc = ocsfs_crc32c(~0U, jh, sizeof(*jh) - sizeof(__le32));

		if (crc != le32_to_cpu(jh->jh_checksum)) {
			pr_warn("ocsfs: node %u journal header checksum mismatch, skipping\n",
				node_slot);
			brelse(bh);
			return 0;
		}
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

	/* Set local barrier so EX lock acquisitions on *this* node also wait
	 * during replay, even when ocsfs_journal_replay_node is called directly
	 * without going through the full recovery path (NUOV-MEDIO-1). */
	atomic_set(&sbi->s_recovery_barrier, 1);
	ret = journal_replay_j(sb, &tmp_j);
	atomic_set(&sbi->s_recovery_barrier, 0);

	if (!ret) {
		jh->jh_tail     = jh->jh_head;
		jh->jh_checksum = cpu_to_le32(
			ocsfs_crc32c(~0U, jh, sizeof(*jh) - sizeof(__le32)));
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
	}
	brelse(bh);

	return ret;
}
