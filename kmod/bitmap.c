// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — bitmap.c
 * Block bitmap allocator and inode number allocator for the kernel module.
 *
 * Phase 1: single-node, simple first-fit allocation within AGs.
 *
 * Each AG has a block bitmap stored on disk. Bits are 0 = free, 1 = used.
 * The bitmap is read into buffer_heads on demand and modified in place.
 */

#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * BLOCK ALLOCATION
 *
 * Allocates @count contiguous blocks from the specified AG (or any AG
 * if the preferred one is full). Returns the absolute block number
 * of the first allocated block in @block_out.
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_ag_alloc_blocks(struct super_block *sb, u32 ag_no,
				 u32 count, u64 *block_out,
				 struct ocsfs_txn *txn)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_ag_info *ag = &sbi->s_ags[ag_no];
	u64 bitmap_blocks;
	u64 b;
	u32 found = 0;
	u64 start_bit = 0;
	int ret;

	if (ag->free_blocks < count)
		return -ENOSPC;

	/* Cross-node: DLM EX on AG prevents two nodes allocating same blocks. */
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(sb, &ag->ag_lock_res, OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	mutex_lock(&ag->ag_lock);

	bitmap_blocks = (ag->bitmap_size + sbi->s_block_size - 1) /
			sbi->s_block_size;

	/* Scan bitmap for @count contiguous free bits */
	for (b = 0; b < bitmap_blocks; b++) {
		struct buffer_head *bh;
		u64 bm_block = (ag->bitmap_off / sbi->s_block_size) + b;
		u32 bits_in_block = sbi->s_block_size * 8;
		u32 bit;

		/* Clamp to actual block count in this AG */
		if (b * bits_in_block >= ag->block_count)
			break;
		if ((b + 1) * bits_in_block > ag->block_count)
			bits_in_block = (u32)(ag->block_count - b * bits_in_block);

		bh = sb_bread(sb, bm_block);
		if (!bh) {
			ret = -EIO;
			goto out_unlock;
		}

		for (bit = 0; bit < bits_in_block; bit++) {
			u32 byte_idx = bit / 8;
			u32 bit_idx = bit % 8;
			u8 *byte_ptr = (u8 *)bh->b_data + byte_idx;

			if (!(*byte_ptr & (1 << bit_idx))) {
				/* Free bit */
				if (found == 0)
					start_bit = b * (sbi->s_block_size * 8) + bit;
				found++;
				if (found == count) {
					/* Mark bits as allocated */
					u64 mark_bit;

					for (mark_bit = start_bit;
					     mark_bit < start_bit + count;
					     mark_bit++) {
						u64 mb = mark_bit / (sbi->s_block_size * 8);
						u32 mbit = (u32)(mark_bit % (sbi->s_block_size * 8));
						struct buffer_head *mbh;

						if (mb == b) {
							mbh = bh;
						} else {
							mbh = sb_bread(sb,
								(ag->bitmap_off / sbi->s_block_size) + mb);
							if (!mbh) {
								brelse(bh);
								ret = -EIO;
								goto out_unlock;
							}
						}

						if (txn) {
							ret = ocsfs_txn_add_bh(txn, mbh);
							if (ret) {
								if (mb != b)
									brelse(mbh);
								brelse(bh);
								goto out_unlock;
							}
						}

						((u8 *)mbh->b_data)[mbit / 8] |=
							(1 << (mbit % 8));
						if (!txn)
							mark_buffer_dirty(mbh);

						if (mb != b)
							brelse(mbh);
					}

					brelse(bh);

					ag->free_blocks -= count;
					spin_lock(&sbi->s_free_lock);
					sbi->s_free_blocks -= count;
					spin_unlock(&sbi->s_free_lock);

					*block_out = ag->block_start + start_bit;
					ret = 0;
					goto out_unlock;
				}
			} else {
				/* Used bit — reset counter */
				found = 0;
			}
		}

		brelse(bh);
	}

	ret = -ENOSPC;

out_unlock:
	mutex_unlock(&ag->ag_lock);
	if (sbi->s_clustered)
		ocsfs_lock_release(sb, &ag->ag_lock_res);
	return ret;
}

int ocsfs_alloc_blocks(struct super_block *sb, u32 ag_hint, u32 count,
		       u64 *block_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_txn *txn;
	u32 i;
	int ret;

	txn = ocsfs_txn_begin(sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);

	/* Try preferred AG first */
	if (ag_hint < sbi->s_ag_count) {
		ret = ocsfs_ag_alloc_blocks(sb, ag_hint, count, block_out, txn);
		if (ret == 0)
			goto commit;
	}

	/* Fall back to any AG with space */
	for (i = 0; i < sbi->s_ag_count; i++) {
		if (i == ag_hint)
			continue;
		ret = ocsfs_ag_alloc_blocks(sb, i, count, block_out, txn);
		if (ret == 0)
			goto commit;
	}

	ret = -ENOSPC;
	ocsfs_txn_abort(txn);
	return ret;

commit:
	return ocsfs_txn_commit(txn);
}

/*
 * Allocate blocks within an existing transaction (no txn_begin/commit).
 * Use when the caller already holds an open txn and cannot re-enter j_lock.
 */
int ocsfs_alloc_blocks_txn(struct ocsfs_txn *txn, struct super_block *sb,
			    u32 ag_hint, u32 count, u64 *block_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u32 i;
	int ret;

	if (ag_hint < sbi->s_ag_count) {
		ret = ocsfs_ag_alloc_blocks(sb, ag_hint, count, block_out, txn);
		if (ret == 0)
			return 0;
	}

	for (i = 0; i < sbi->s_ag_count; i++) {
		if (i == ag_hint)
			continue;
		ret = ocsfs_ag_alloc_blocks(sb, i, count, block_out, txn);
		if (ret == 0)
			return 0;
	}

	return -ENOSPC;
}

/* ═══════════════════════════════════════════════════════════════
 * BLOCK FREE
 * ═══════════════════════════════════════════════════════════════ */

/* Journalized free: journals BEFORE-images so crash recovery can restore them.
 * Caller must have an open txn; acquires AG DLM EX + ag_lock internally. */
int ocsfs_free_blocks_txn(struct ocsfs_txn *txn, u64 block, u32 count)
{
	struct ocsfs_journal *j = txn->t_journal;
	struct super_block *sb = j->j_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u32 ag_no;
	struct ocsfs_ag_info *ag;
	u64 local_block;
	u32 i;
	int ret = 0;

	for (ag_no = 0; ag_no < sbi->s_ag_count; ag_no++) {
		ag = &sbi->s_ags[ag_no];
		if (block >= ag->block_start &&
		    block < ag->block_start + ag->block_count)
			break;
	}

	if (ag_no >= sbi->s_ag_count) {
		pr_warn("ocsfs: free_blocks_txn: block %llu not in any AG\n", block);
		return -EINVAL;
	}

	local_block = block - ag->block_start;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(sb, &ag->ag_lock_res, OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	mutex_lock(&ag->ag_lock);

	for (i = 0; i < count; i++) {
		u64 bit = local_block + i;
		u64 bm_block_idx = bit / (sbi->s_block_size * 8);
		u32 bm_bit = (u32)(bit % (sbi->s_block_size * 8));
		struct buffer_head *bh;

		bh = sb_bread(sb,
			      (ag->bitmap_off / sbi->s_block_size) + bm_block_idx);
		if (!bh) {
			ret = -EIO;
			goto out_unlock;
		}

		ret = ocsfs_txn_add_bh(txn, bh);
		if (ret) {
			brelse(bh);
			goto out_unlock;
		}

		((u8 *)bh->b_data)[bm_bit / 8] &= ~(1 << (bm_bit % 8));
		brelse(bh);
	}

	ag->free_blocks += count;
	spin_lock(&sbi->s_free_lock);
	sbi->s_free_blocks += count;
	spin_unlock(&sbi->s_free_lock);

out_unlock:
	mutex_unlock(&ag->ag_lock);
	if (sbi->s_clustered)
		ocsfs_lock_release(sb, &ag->ag_lock_res);
	return ret;
}

void ocsfs_free_blocks(struct super_block *sb, u64 block, u32 count)
{
	struct ocsfs_txn *txn = ocsfs_txn_begin(sb);

	if (IS_ERR(txn)) {
		pr_err("ocsfs: free_blocks: failed to open txn (%ld)\n",
		       PTR_ERR(txn));
		return;
	}

	if (ocsfs_free_blocks_txn(txn, block, count))
		ocsfs_txn_abort(txn);
	else
		ocsfs_txn_commit(txn);
}

/* ═══════════════════════════════════════════════════════════════
 * INODE NUMBER ALLOCATION
 *
 * Inode numbers are allocated from the AG's inode table.
 * Each AG can hold ag_inode_count inodes. We scan the inode table
 * blocks looking for an inode slot with magic == 0 (free).
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_alloc_inode_num(struct super_block *sb, u32 ag_hint, u64 *ino_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_txn *txn;
	u32 ag_no;
	u32 try;

	/* Open txn before any AG lock (ordering: j_lock → ag_lock). */
	txn = ocsfs_txn_begin(sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);

	for (try = 0; try < sbi->s_ag_count; try++) {
		struct ocsfs_ag_info *ag;
		u64 i;

		ag_no = (ag_hint + try) % sbi->s_ag_count;
		ag = &sbi->s_ags[ag_no];

		if (ag->free_inodes == 0)
			continue;

		if (sbi->s_clustered) {
			int lret = ocsfs_lock_acquire(sb, &ag->ag_lock_res,
						      OCSFS_LOCK_EX);
			if (lret)
				continue; /* can't lock this AG — try next */
		}

		mutex_lock(&ag->ag_lock);

		for (i = (ag_no == 0 ? OCSFS_FIRST_USER_INO : 0);
		     i < ag->inode_count; i++) {
			struct buffer_head *bh;
			u64 off = ag->inode_table_off + i * OCSFS_INODE_SIZE;
			u64 block = off / sbi->s_block_size;
			u32 boff = off % sbi->s_block_size;
			struct ocsfs_disk_inode *di;

			bh = sb_bread(sb, block);
			if (!bh)
				continue;

			di = (struct ocsfs_disk_inode *)(bh->b_data + boff);

			if (le32_to_cpu(di->i_magic) != OCSFS_INODE_MAGIC) {
				int tr = ocsfs_txn_add_bh(txn, bh);

				if (!tr) {
					memset(di, 0, OCSFS_INODE_SIZE);
					di->i_magic = cpu_to_le32(OCSFS_INODE_MAGIC);
					di->i_ino = cpu_to_le64(
						ag_no * sbi->s_ag_size + i);
					brelse(bh);

					ag->free_inodes--;
					*ino_out = ag_no * sbi->s_ag_size + i;

					mutex_unlock(&ag->ag_lock);
					if (sbi->s_clustered)
						ocsfs_lock_release(sb, &ag->ag_lock_res);
					return ocsfs_txn_commit(txn);
				}

				brelse(bh);
			} else {
				brelse(bh);
			}
		}

		mutex_unlock(&ag->ag_lock);
		if (sbi->s_clustered)
			ocsfs_lock_release(sb, &ag->ag_lock_res);
	}

	ocsfs_txn_abort(txn);
	return -ENOSPC;
}

/* ═══════════════════════════════════════════════════════════════
 * INODE NUMBER FREE
 * ═══════════════════════════════════════════════════════════════ */

void ocsfs_free_inode_num(struct super_block *sb, u64 ino)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u32 ag_no = ocsfs_ino_to_ag(sbi, ino);
	struct ocsfs_ag_info *ag;
	u64 local = ino % sbi->s_ag_size;
	u64 off, block;
	u32 boff;
	struct buffer_head *bh;
	struct ocsfs_disk_inode *di;
	struct ocsfs_txn *txn;

	if (ag_no >= sbi->s_ag_count) {
		pr_warn("ocsfs: free_inode_num: ino %llu out of range\n", ino);
		return;
	}

	/* Open txn before AG lock (ordering: j_lock → ag_lock). */
	txn = ocsfs_txn_begin(sb);
	if (IS_ERR(txn)) {
		pr_err("ocsfs: free_inode_num: txn_begin failed\n");
		return;
	}

	ag = &sbi->s_ags[ag_no];

	if (sbi->s_clustered &&
	    ocsfs_lock_acquire(sb, &ag->ag_lock_res, OCSFS_LOCK_EX)) {
		pr_warn_ratelimited("ocsfs: free_inode_num %llu: DLM EX failed\n",
				    ino);
		ocsfs_txn_abort(txn);
		return;
	}

	mutex_lock(&ag->ag_lock);

	off = ag->inode_table_off + local * OCSFS_INODE_SIZE;
	block = off / sbi->s_block_size;
	boff = off % sbi->s_block_size;

	bh = sb_bread(sb, block);
	if (bh) {
		if (ocsfs_txn_add_bh(txn, bh) == 0) {
			di = (struct ocsfs_disk_inode *)(bh->b_data + boff);
			memset(di, 0, OCSFS_INODE_SIZE);
		}
		brelse(bh);
	}

	ag->free_inodes++;
	mutex_unlock(&ag->ag_lock);
	if (sbi->s_clustered)
		ocsfs_lock_release(sb, &ag->ag_lock_res);
	ocsfs_txn_commit(txn);
}

/* Scan all AGs for orphan inodes (OCSFS_IFLAG_ORPHAN) and log warnings. */
int ocsfs_orphan_scan(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	int total = 0;
	u32 ag_no;

	for (ag_no = 0; ag_no < sbi->s_ag_count; ag_no++) {
		struct ocsfs_ag_info *ag = &sbi->s_ags[ag_no];
		u64 i;

		mutex_lock(&ag->ag_lock);

		for (i = (ag_no == 0 ? OCSFS_FIRST_USER_INO : 0);
		     i < ag->inode_count; i++) {
			struct buffer_head *bh;
			u64 off   = ag->inode_table_off + i * OCSFS_INODE_SIZE;
			u64 block = off / sbi->s_block_size;
			u32 boff  = off % sbi->s_block_size;
			struct ocsfs_disk_inode *di;

			bh = sb_bread(sb, block);
			if (!bh)
				continue;

			di = (struct ocsfs_disk_inode *)(bh->b_data + boff);

			if (le32_to_cpu(di->i_magic) == OCSFS_INODE_MAGIC &&
			    (le32_to_cpu(di->i_flags) & OCSFS_IFLAG_ORPHAN)) {
				u64 ino = (u64)ag_no * sbi->s_ag_size + i;

				pr_warn("ocsfs: orphan inode %llu "
					"(mode=%04o size=%llu) — run fsck\n",
					ino,
					le16_to_cpu(di->i_mode),
					le64_to_cpu(di->i_size));
				total++;
			}

			brelse(bh);
		}

		mutex_unlock(&ag->ag_lock);
	}

	if (total)
		pr_warn("ocsfs: %d orphan inode(s) — filesystem needs fsck\n",
			total);

	return total;
}
