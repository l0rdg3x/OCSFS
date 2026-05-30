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
 * HELPERS
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Read a metadata (block bitmap / inode-table) block — a plain cached read.
 *
 * Cross-node coherence is NOT done here per-read (that loses uncommitted local
 * marks and double-allocates).  Instead the allocators, on a *real* cross-node
 * DLM acquire of the AG lock, invalidate the AG's cached bitmap / inode table
 * once (ocsfs_ag_invalidate_bitmap / the @fresh path of ocsfs_inode_getblk),
 * and hold the AG lock until the txn commits.  Within the held window reads are
 * cached so read-your-own-writes (and same-node concurrent allocations, which
 * piggyback as cache hits and must see each other's in-memory marks) work.
 */
static struct buffer_head *ocsfs_meta_getblk(struct super_block *sb, u64 blkno)
{
	return sb_bread(sb, blkno);
}

/*
 * Inode-table block read for the allocator.  @fresh is set when the AG lock was
 * just acquired cross-node (a peer may have allocated inodes since we last
 * looked) AND no concurrent same-node allocation is in flight (we are the real
 * acquirer, not a cache hit) — only then is it safe to drop the cached copy and
 * re-read from disk without clobbering another local txn's uncommitted inode.
 */
static struct buffer_head *ocsfs_inode_getblk(struct super_block *sb, u64 blkno,
					      bool fresh)
{
	struct buffer_head *bh;

	if (!fresh)
		return sb_bread(sb, blkno);

	bh = sb_getblk(sb, blkno);
	if (!bh)
		return NULL;
	clear_buffer_uptodate(bh);
	if (bh_read(bh, 0) < 0) {
		brelse(bh);
		return NULL;
	}
	return bh;
}

/*
 * Invalidate the AG's cached block-bitmap blocks so the next (cached) read
 * picks up a peer's committed allocations.  Called ONCE, right after a real
 * cross-node DLM acquire of the AG lock, while no same-node allocation is in
 * flight in this AG — so it cannot clobber another local txn's uncommitted
 * marks (those acquirers get a cache hit and skip this).
 */
static void ocsfs_ag_invalidate_bitmap(struct super_block *sb,
				       struct ocsfs_ag_info *ag)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 first = ag->bitmap_off / sbi->s_block_size;
	u64 nblk  = (ag->bitmap_size + sbi->s_block_size - 1) / sbi->s_block_size;
	u64 b;

	for (b = 0; b < nblk; b++) {
		struct buffer_head *bh = sb_getblk(sb, first + b);

		if (!bh)
			continue;
		if (!buffer_dirty(bh))   /* never drop uncommitted marks */
			clear_buffer_uptodate(bh);
		brelse(bh);
	}
}

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
	bool fresh = false;   /* TEMP DEBUG */

	if (ag->free_blocks < count)
		return -ENOSPC;

	/* Cross-node: DLM EX on AG prevents two nodes allocating same blocks. */
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire_fresh(sb, &ag->ag_lock_res,
					       OCSFS_LOCK_EX, &fresh);
		if (ret)
			return ret;
		/* Real cross-node acquire: a peer may have allocated since we
		 * last cached this AG's bitmap — drop the stale cache. */
		if (fresh)
			ocsfs_ag_invalidate_bitmap(sb, ag);
	}

	mutex_lock(&ag->ag_lock);

	bitmap_blocks = (ag->bitmap_size + sbi->s_block_size - 1) /
			sbi->s_block_size;

	/* Scan bitmap for @count contiguous free bits */
	for (b = 0; b < bitmap_blocks; b++) {
		struct buffer_head *bh;
		u64 bm_block = (ag->bitmap_off / sbi->s_block_size) + b;
		u32 bits_in_block = sbi->s_block_size * 8;
		u32 scan_pos;

		/* Clamp to actual block count in this AG */
		if (b * bits_in_block >= ag->block_count)
			break;
		if ((b + 1) * bits_in_block > ag->block_count)
			bits_in_block = (u32)(ag->block_count - b * bits_in_block);

		bh = ocsfs_meta_getblk(sb, bm_block);
		if (!bh) {
			ret = -EIO;
			goto out_unlock;
		}

		/*
		 * Use LE bitmap primitives to skip used bits in bulk rather
		 * than testing each bit individually.
		 */
		if (found > 0) {
			/*
			 * Continuation: check whether leading free bits in
			 * this block extend the cross-block run.
			 */
			u32 first_used = find_next_bit_le(bh->b_data,
							   bits_in_block, 0);

			if (first_used > 0) {
				found += first_used;
				if (found >= count)
					goto do_mark;
			}
			if (first_used == bits_in_block) {
				/* Whole block free — run continues */
				brelse(bh);
				continue;
			}
			/* Run broken; rescan from bit 0 of this block.
			 * MEDIO-N5: resetting scan_pos to first_used skips
			 * bits 0..first_used-1 as potential new run starts.
			 * Restarting from 0 lets the while loop rediscover them;
			 * since bit first_used is used they can't extend beyond
			 * first_used bits, but the full rescan avoids missing
			 * valid runs that straddle this block and the next. */
			found = 0;
			scan_pos = 0;
		} else {
			scan_pos = 0;
		}

		while (scan_pos < bits_in_block) {
			u32 zbit = find_next_zero_bit_le(bh->b_data,
							  bits_in_block,
							  scan_pos);
			u32 next_set;

			if (zbit >= bits_in_block)
				break;

			next_set = find_next_bit_le(bh->b_data,
						     bits_in_block, zbit);
			found = next_set - zbit;
			start_bit = b * (sbi->s_block_size * 8) + zbit;

			if (found >= count)
				goto do_mark;

			if (next_set >= bits_in_block)
				break; /* run extends into next block */

			/* Run ended — try next free region */
			scan_pos = next_set;
			found = 0;
		}

		brelse(bh);
		continue;

do_mark:
		{
			u64 mark_bit;

			for (mark_bit = start_bit;
			     mark_bit < start_bit + count;
			     mark_bit++) {
				u64 mb = mark_bit / (sbi->s_block_size * 8);
				u32 mbit = (u32)(mark_bit %
						 (sbi->s_block_size * 8));
				struct buffer_head *mbh;

				if (mb == b) {
					mbh = bh;
				} else {
					mbh = ocsfs_meta_getblk(sb,
						(ag->bitmap_off /
						 sbi->s_block_size) + mb);
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
	}

	ret = -ENOSPC;

out_unlock:
	mutex_unlock(&ag->ag_lock);
	if (sbi->s_clustered) {
		/*
		 * Cross-node: on a successful allocation in a journalled txn, keep
		 * the AG DLM lock held until the caller commits — the bitmap marks
		 * are not yet durable, so releasing now would let a peer allocate
		 * the same blocks (double allocation / corruption).  ocsfs_txn_-
		 * commit/abort releases it.  On failure (or no txn) release now.
		 */
		if (ret == 0 && txn)
			ocsfs_txn_defer_unlock(txn, &ag->ag_lock_res);
		else
			ocsfs_lock_release(sb, &ag->ag_lock_res);
	}
	return ret;
}

int ocsfs_alloc_blocks(struct super_block *sb, u32 ag_hint, u32 count,
		       u64 *block_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_txn *txn;
	u32 i, chosen;
	int ret;

	/*
	 * Phase 1 (lockless): read free_blocks counters to pick a candidate AG.
	 * The read may be slightly stale — phase 2 confirms under DLM-EX.
	 * This avoids holding j_lock across a full multi-AG bitmap scan.
	 */
	chosen = sbi->s_ag_count; /* sentinel: none chosen yet */
	if (ag_hint < sbi->s_ag_count &&
	    READ_ONCE(sbi->s_ags[ag_hint].free_blocks) >= count) {
		chosen = ag_hint;
	} else {
		for (i = 0; i < sbi->s_ag_count; i++) {
			if (i == ag_hint)
				continue;
			if (READ_ONCE(sbi->s_ags[i].free_blocks) >= count) {
				chosen = i;
				break;
			}
		}
	}

	if (chosen == sbi->s_ag_count)
		return -ENOSPC;

	/* Phase 2: open txn only for the chosen AG. */
	txn = ocsfs_txn_begin(sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);

	ret = ocsfs_ag_alloc_blocks(sb, chosen, count, block_out, txn);
	if (ret == 0)
		return ocsfs_txn_commit(txn);

	ocsfs_txn_abort(txn);
	if (ret != -ENOSPC)
		return ret; /* hard I/O or DLM error — do not retry */

	/* Stale lockless read — fall back to remaining AGs. */
	for (i = 0; i < sbi->s_ag_count; i++) {
		if (i == chosen)
			continue;
		if (READ_ONCE(sbi->s_ags[i].free_blocks) < count)
			continue;
		txn = ocsfs_txn_begin(sb);
		if (IS_ERR(txn))
			return PTR_ERR(txn);
		ret = ocsfs_ag_alloc_blocks(sb, i, count, block_out, txn);
		if (ret == 0)
			return ocsfs_txn_commit(txn);
		ocsfs_txn_abort(txn);
		if (ret != -ENOSPC)
			return ret;
	}

	return -ENOSPC;
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
		pr_warn("ocsfs: free_blocks_txn: block %llu not found in any of %u AGs — possible double-free or fs corruption\n",
			block, sbi->s_ag_count);
		return -EINVAL;
	}

	local_block = block - ag->block_start;

	if (sbi->s_clustered) {
		bool fresh = false;

		ret = ocsfs_lock_acquire_fresh(sb, &ag->ag_lock_res,
					       OCSFS_LOCK_EX, &fresh);
		if (ret)
			return ret;
		if (fresh)
			ocsfs_ag_invalidate_bitmap(sb, ag);
	}

	mutex_lock(&ag->ag_lock);

	for (i = 0; i < count; i++) {
		u64 bit = local_block + i;
		u64 bm_block_idx = bit / (sbi->s_block_size * 8);
		u32 bm_bit = (u32)(bit % (sbi->s_block_size * 8));
		struct buffer_head *bh;

		bh = ocsfs_meta_getblk(sb,
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

	if (unlikely(ag->free_blocks + count > ag->block_count))
		pr_warn_ratelimited("ocsfs: AG %u free_blocks overflow: cur=%llu count=%u cap=%llu\n",
				    ag->ag_no, ag->free_blocks, count, ag->block_count);
	ag->free_blocks += count;
	spin_lock(&sbi->s_free_lock);
	sbi->s_free_blocks += count;
	spin_unlock(&sbi->s_free_lock);

out_unlock:
	mutex_unlock(&ag->ag_lock);
	if (sbi->s_clustered) {
		/* Hold the AG lock until commit so the freed bits are durable
		 * before a peer can reallocate them (mirrors the alloc path). */
		if (ret == 0 && txn)
			ocsfs_txn_defer_unlock(txn, &ag->ag_lock_res);
		else
			ocsfs_lock_release(sb, &ag->ag_lock_res);
	}
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
		bool fresh = false;
		u64 i;

		ag_no = (ag_hint + try) % sbi->s_ag_count;
		ag = &sbi->s_ags[ag_no];

		if (ag->free_inodes == 0)
			continue;

		if (sbi->s_clustered) {
			int lret = ocsfs_lock_acquire_fresh(sb,
						&ag->ag_lock_res,
						OCSFS_LOCK_EX, &fresh);
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

			/* Fresh disk read only on a real cross-node acquire
			 * (peer may have allocated inodes); a same-node cache
			 * hit reads cached to see the concurrent local marks. */
			bh = ocsfs_inode_getblk(sb, block, fresh);
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
					{
						/*
						 * Cross-node: commit the inode allocation
						 * BEFORE releasing the AG DLM lock, so the
						 * new inode is durable on disk when a peer
						 * acquires the AG and reads the (fresh) inode
						 * table — otherwise both nodes hand out the
						 * same inode number in the uncommitted window.
						 */
						int cr = ocsfs_txn_commit(txn);

						if (cr) {
							/* Undo the counter decrement (BASSO-10). */
							mutex_lock(&ag->ag_lock);
							ag->free_inodes++;
							mutex_unlock(&ag->ag_lock);
						}
						if (sbi->s_clustered)
							ocsfs_lock_release(
								sb, &ag->ag_lock_res);
						return cr;
					}
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

	bool fresh = false;

	if (sbi->s_clustered &&
	    ocsfs_lock_acquire_fresh(sb, &ag->ag_lock_res, OCSFS_LOCK_EX,
				     &fresh)) {
		pr_warn_ratelimited("ocsfs: free_inode_num %llu: DLM EX failed\n",
				    ino);
		ocsfs_txn_abort(txn);
		return;
	}

	mutex_lock(&ag->ag_lock);

	off = ag->inode_table_off + local * OCSFS_INODE_SIZE;
	block = off / sbi->s_block_size;
	boff = off % sbi->s_block_size;

	bh = ocsfs_inode_getblk(sb, block, fresh);
	if (bh) {
		if (ocsfs_txn_add_bh(txn, bh) == 0) {
			di = (struct ocsfs_disk_inode *)(bh->b_data + boff);
			memset(di, 0, OCSFS_INODE_SIZE);
		}
		brelse(bh);
	}

	ag->free_inodes++;
	mutex_unlock(&ag->ag_lock);
	/*
	 * Commit before releasing DLM EX. If we release EX first and crash
	 * before commit, the inode slot remains ACTIVE on disk but is logically
	 * freed — permanently leaked. Holding EX through commit ensures that
	 * any node acquiring EX after us sees the committed (zeroed) slot.
	 */
	ocsfs_txn_commit(txn);
	if (sbi->s_clustered)
		ocsfs_lock_release(sb, &ag->ag_lock_res);
}

/* Scan all AGs for orphan inodes (OCSFS_IFLAG_ORPHAN) and log warnings. */
int ocsfs_orphan_scan(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 *inos = NULL;
	u32 total = 0, cap = 0;
	u32 ag_no, j;

	/*
	 * Pass 1 — collect orphan inode numbers while holding ag_lock.
	 * We cannot call ocsfs_iget (which blocks) under ag_lock, so we
	 * stash the numbers and clean up in a second pass.
	 */
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

			/* Cached read.  The old clustered forced re-read
			 * (clear_buffer_uptodate + bh_read) issued one disk read
			 * per inode slot — 8× per block, defeating the page cache —
			 * which made mounting a large multi-AG volume on a SAN/iSCSI
			 * target take minutes in __bh_read.  Cross-node coherence of
			 * the inode tables belongs at DLM-acquire time (TODO), like
			 * the other metadata reads. */
			bh = sb_bread(sb, block);
			if (!bh)
				continue;

			di = (struct ocsfs_disk_inode *)(bh->b_data + boff);

			if (le32_to_cpu(di->i_magic) == OCSFS_INODE_MAGIC &&
			    (le32_to_cpu(di->i_flags) & OCSFS_IFLAG_ORPHAN)) {
				u64 ino = (u64)ag_no * sbi->s_ag_size + i;

				pr_warn("ocsfs: orphan inode %llu "
					"(mode=%04o size=%llu)\n",
					ino,
					le16_to_cpu(di->i_mode),
					le64_to_cpu(di->i_size));

				if (total == cap) {
					u32 new_cap = max(cap * 2u, 16u);
					u64 *tmp = krealloc(inos,
						    new_cap * sizeof(u64),
						    GFP_NOFS);
					if (tmp) {
						inos = tmp;
						cap  = new_cap;
					}
				}
				if (total < cap)
					inos[total] = ino;
				total++;
			}
			brelse(bh);
		}

		mutex_unlock(&ag->ag_lock);
	}

	if (!total)
		return 0;

	/*
	 * Pass 2 — reclaim (single-node only).
	 *
	 * In cluster mode, a peer may legitimately hold the orphan inode open
	 * (e.g. tmpfile across a mount cycle, or an open file descriptor on
	 * another node that has not yet triggered evict_inode).  Querying each
	 * peer via DLM SH would add O(N·orphans) round-trips at every mount;
	 * the safer approach is to leave cleanup to offline fsck, which can
	 * compare the inode refcount on disk (should be 0 for a true orphan)
	 * after all nodes have unmounted.
	 *
	 * In single-node mode at mount time no process has the inode open, so
	 * clear_nlink + iput safely triggers evict_inode → frees blocks.
	 */
	if (sbi->s_clustered) {
		pr_warn("ocsfs: %u orphan inode(s) — run fsck to reclaim\n",
			total);
		goto out;
	}

	for (j = 0; j < min(total, cap); j++) {
		struct inode *inode = ocsfs_iget(sb, inos[j]);

		if (IS_ERR(inode)) {
			pr_warn_ratelimited(
				"ocsfs: orphan_scan: iget %llu failed (%ld)\n",
				inos[j], PTR_ERR(inode));
			continue;
		}
		if (OCSFS_I(inode)->i_flags & OCSFS_IFLAG_ORPHAN) {
			clear_nlink(inode);
			mark_inode_dirty(inode);
		}
		iput(inode);
	}
	pr_info("ocsfs: recovered %u orphan inode(s)\n", min(total, cap));
	if (total > cap)
		pr_warn("ocsfs: %u orphan(s) not recovered (list truncated) — run fsck\n",
			total - cap);
out:
	kfree(inos);
	return (int)total;
}
