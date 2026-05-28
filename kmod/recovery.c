// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — recovery.c
 * Multi-phase crash recovery protocol.
 *
 * When a node is detected as failed (heartbeat timeout), the surviving
 * nodes execute a 5-phase recovery:
 *
 *   Phase 1 — Leader Election:   CAS on-disk recovery leader block
 *   Phase 2 — SCSI PR Fencing:   preempt-and-abort the failed node's key
 *   Phase 3 — Journal Replay:    replay the failed node's journal
 *   Phase 4 — Lock Recovery:     release all locks held by the failed node
 *   Phase 5 — Slot Cleanup:      mark the slot as DEAD
 *
 * Leader election uses ocsfs_atomic_cas() on an on-disk block at
 * OCSFS_RECOVERY_LEADER_OFF.  Only the node that wins the CAS proceeds.
 * This eliminates the TOCTOU window between in-memory is_leader check
 * and DLM acquire that could cause two nodes to run recovery in parallel.
 */

#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * RECOVERY LEADER BLOCK — CAS-based distributed election
 * ═══════════════════════════════════════════════════════════════ */

static u64 ocsfs_rl_block(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	return OCSFS_RECOVERY_LEADER_OFF / sbi->s_block_size;
}

static u32 ocsfs_rl_crc(const struct ocsfs_disk_recovery_leader *rl)
{
	return ocsfs_crc32c(~0U, rl, offsetof(struct ocsfs_disk_recovery_leader,
					      rl_checksum));
}

/*
 * ocsfs_recovery_leader_acquire — attempt to become recovery leader via CAS.
 *
 * Returns 0 on success (this node is now leader).
 *         -EAGAIN if another node already holds leadership.
 *         -errno on I/O or other error.
 *
 * epoch_out receives the epoch to pass to ocsfs_recovery_leader_release().
 */
static int ocsfs_recovery_leader_acquire(struct super_block *sb,
					 u16 failed_slot, u32 *epoch_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_recovery_leader cur, new;
	struct buffer_head *bh;
	u64 block = ocsfs_rl_block(sb);
	u64 now;
	int attempt, ret;

	for (attempt = 0; attempt < CAS_MAX_ATTEMPTS; attempt++) {
		bh = sb_getblk(sb, block);
		if (!bh)
			return -EIO;
		clear_buffer_uptodate(bh);
		ret = bh_read(bh, 0);
		if (ret < 0) {
			brelse(bh);
			return -EIO;
		}
		memcpy(&cur, bh->b_data, sizeof(cur));
		brelse(bh);

		now = ktime_get_real_ns();

		/* If the block is uninitialised or leadership has expired, we can claim. */
		if (le32_to_cpu(cur.rl_magic) == OCSFS_RECOVERY_LEADER_MAGIC &&
		    le16_to_cpu(cur.rl_leader_slot) != OCSFS_RL_SLOT_FREE &&
		    le64_to_cpu(cur.rl_deadline_ns) > now) {
			/* Valid leader exists and hasn't expired */
			if (le16_to_cpu(cur.rl_leader_slot) == sbi->s_node_slot) {
				/* We already hold it (retry after own timeout?) */
				if (epoch_out)
					*epoch_out = le32_to_cpu(cur.rl_epoch);
				return 0;
			}
			return -EAGAIN;
		}

		/* Build new leader block claiming leadership for this node */
		memset(&new, 0, sizeof(new));
		new.rl_magic       = cpu_to_le32(OCSFS_RECOVERY_LEADER_MAGIC);
		new.rl_leader_slot = cpu_to_le16(sbi->s_node_slot);
		new.rl_target_slot = cpu_to_le16(failed_slot);
		new.rl_leader_gen  = cpu_to_le32(sbi->s_mount_gen);
		new.rl_epoch       = cpu_to_le32(le32_to_cpu(cur.rl_epoch) + 1);
		new.rl_deadline_ns = cpu_to_le64(now + RECOVERY_LEADER_TIMEOUT_NS);
		new.rl_checksum    = cpu_to_le32(ocsfs_rl_crc(&new));

		ret = ocsfs_atomic_cas(sb, block, 0, sizeof(cur), &cur, &new);
		if (ret == 0) {
			if (epoch_out)
				*epoch_out = le32_to_cpu(new.rl_epoch);
			return 0;
		}
		if (ret != -EAGAIN)
			return ret;

		{
			u32 delay_us = min_t(u32, 1U << min(attempt, 15U), 50000U);
			usleep_range(delay_us, delay_us + delay_us / 4);
		}
	}
	return -EBUSY;
}

/*
 * ocsfs_recovery_leader_release — relinquish the recovery leader role.
 *
 * Only releases if we still hold leadership (epoch matches).
 */
static void ocsfs_recovery_leader_release(struct super_block *sb,
					  u16 failed_slot, u32 epoch)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_recovery_leader cur, free;
	struct buffer_head *bh;
	u64 block = ocsfs_rl_block(sb);
	int attempt, ret;

	memset(&free, 0, sizeof(free));
	free.rl_magic       = cpu_to_le32(OCSFS_RECOVERY_LEADER_MAGIC);
	free.rl_leader_slot = cpu_to_le16(OCSFS_RL_SLOT_FREE);
	free.rl_target_slot = cpu_to_le16(OCSFS_RL_SLOT_FREE);

	/*
	 * Re-read the leader block on every attempt: after a -EAGAIN the
	 * block content has changed (another node did a CAS), so the old
	 * expected value is stale and the next attempt would fail again
	 * without a re-read.  Up to 15 attempts × 10-20ms ≈ 150-300ms,
	 * vs. the 60s deadline-expiry freeze of the old 3-attempt loop
	 * (NUOV-MEDIO-11).
	 */
	for (attempt = 0; attempt < 15; attempt++) {
		bh = sb_getblk(sb, block);
		if (!bh)
			return;
		clear_buffer_uptodate(bh);
		if (bh_read(bh, 0) < 0) {
			brelse(bh);
			return;
		}
		memcpy(&cur, bh->b_data, sizeof(cur));
		brelse(bh);

		/* Stop if we no longer own the slot (another node took over) */
		if (le16_to_cpu(cur.rl_leader_slot) != sbi->s_node_slot ||
		    le32_to_cpu(cur.rl_epoch) != epoch)
			return;

		free.rl_epoch    = cur.rl_epoch;
		free.rl_checksum = cpu_to_le32(ocsfs_rl_crc(&free));

		ret = ocsfs_atomic_cas(sb, block, 0, sizeof(cur), &cur, &free);
		if (ret != -EAGAIN)
			return;

		usleep_range(10000, 20000);
	}
	pr_warn_ratelimited("ocsfs: recovery leader release: CAS failed "
			    "after %d attempts — slot expires at deadline\n",
			    15);
}

/* ═══════════════════════════════════════════════════════════════
 * REPLAY ACTIVE FLAG — cross-node quiescence for AFTER-image replay
 *
 * Sets or clears OCSFS_RL_REPLAY_ACTIVE in the on-disk recovery leader
 * block.  Survivor nodes read this flag during heartbeat_check_peers and
 * set s_remote_recovery_barrier, causing their EX lock acquisitions to
 * return -EAGAIN until replay is complete.  This prevents the race where
 * a survivor writes a block that the replaying leader is about to restore
 * (NUOV-CRIT-6).  The staleness window is ≤ one HB_CHECK interval.
 * ═══════════════════════════════════════════════════════════════ */
static void ocsfs_recovery_set_replay_active(struct super_block *sb, bool active)
{
	struct ocsfs_disk_recovery_leader cur, new;
	struct buffer_head *bh;
	u64 block = ocsfs_rl_block(sb);

	bh = sb_getblk(sb, block);
	if (!bh)
		return;
	clear_buffer_uptodate(bh);
	if (bh_read(bh, 0) < 0) {
		brelse(bh);
		return;
	}
	memcpy(&cur, bh->b_data, sizeof(cur));
	memcpy(&new, &cur, sizeof(new));

	if (active)
		new.rl_epoch |= cpu_to_le32(OCSFS_RL_REPLAY_ACTIVE);
	else
		new.rl_epoch &= cpu_to_le32(~OCSFS_RL_REPLAY_ACTIVE);
	new.rl_checksum = cpu_to_le32(ocsfs_rl_crc(&new));

	lock_buffer(bh);
	memcpy(bh->b_data, &new, sizeof(new));
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
}

/* ═══════════════════════════════════════════════════════════════
 * RECOVERY EXECUTION — 5 phases
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_recovery_run(struct super_block *sb, u16 failed_slot)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_node_info *ni;
	u32 failed_gen, leader_epoch;
	u64 failed_pr_key;
	int ret;

	mutex_lock(&sbi->s_recovery_lock);
	sbi->s_recovery_in_progress = true;

	pr_warn("ocsfs: ═══ RECOVERY START for node slot %u ═══\n",
		failed_slot);

	/* Cache failed node info */
	spin_lock(&sbi->s_node_lock);
	ni = &sbi->s_nodes[failed_slot];
	failed_gen = ni->ni_mount_gen;
	failed_pr_key = ni->ni_pr_key;
	spin_unlock(&sbi->s_node_lock);

	/*
	 * Phase 1 — Leader Election via CAS on-disk.
	 *
	 * ocsfs_recovery_leader_acquire() atomically writes our node slot
	 * into the on-disk recovery leader block.  Only the node that wins
	 * the CAS will proceed; others get -EAGAIN and defer gracefully.
	 * This replaces both the in-memory scan and the DLM re-check that
	 * had a TOCTOU window between them.
	 */
	pr_info("ocsfs: recovery phase 1: CAS leader election\n");

	ret = ocsfs_recovery_leader_acquire(sb, failed_slot, &leader_epoch);
	if (ret) {
		if (ret == -EAGAIN)
			pr_info("ocsfs: not the recovery leader for slot %u, deferring\n",
				failed_slot);
		else
			pr_warn("ocsfs: leader election for slot %u failed (%d), deferring\n",
				failed_slot, ret);
		sbi->s_recovery_in_progress = false;
		mutex_unlock(&sbi->s_recovery_lock);
		return ret;
	}

	pr_info("ocsfs: this node (slot %u) is the recovery leader\n",
		sbi->s_node_slot);

	/*
	 * Phase 2 — SCSI PR Fencing
	 */
	pr_info("ocsfs: recovery phase 2: SCSI PR fencing\n");

	spin_lock(&sbi->s_node_lock);
	ni->ni_state = OCSFS_NODE_EVICTING;
	spin_unlock(&sbi->s_node_lock);

	ret = ocsfs_pr_preempt_abort(sb, failed_pr_key,
				     OCSFS_PR_TYPE_WRITE_EXCL_REG);
	if (ret && ret != -EOPNOTSUPP && sbi->s_pr_capable) {
		/*
		 * We registered PR successfully at mount (s_pr_capable) but
		 * fencing of the dead node failed now.  The node may still be
		 * alive and writing — proceeding with journal replay would risk
		 * split-brain corruption.  Force read-only and bail.
		 */
		pr_err("ocsfs: PR fencing failed (ret=%d) on PR-capable device — "
		       "forcing read-only to prevent split-brain\n", ret);
		sb->s_flags |= SB_RDONLY;
		ocsfs_node_mark_dead(sb, failed_slot);
		ocsfs_recovery_leader_release(sb, failed_slot, leader_epoch);
		sbi->s_recovery_in_progress = false;
		mutex_unlock(&sbi->s_recovery_lock);
		return ret;
	}
	if (ret)
		pr_warn("ocsfs: PR fencing unavailable (ret=%d), "
			"continuing without hardware isolation\n", ret);

	/*
	 * Degraded safety check: if we have no hardware fence, re-read the
	 * superblock and verify s_last_mount_time has not changed since we
	 * started recovery. A changed timestamp means another node (possibly
	 * the "dead" one) has written to the device — abort to prevent
	 * split-brain.
	 */
	if (!sbi->s_pr_capable) {
		struct buffer_head *sb_bh;
		struct ocsfs_disk_super *sb_ds;
		u64 current_mount_time;

		sb_bh = sb_getblk(sb, 0);
		if (sb_bh) {
			clear_buffer_uptodate(sb_bh);
			if (bh_read(sb_bh, 0) == 0) {
				sb_ds = (struct ocsfs_disk_super *)sb_bh->b_data;
				current_mount_time = le64_to_cpu(sb_ds->s_last_mount_time);
				if (current_mount_time != le64_to_cpu(sbi->s_ds->s_last_mount_time)) {
					pr_err("ocsfs: degraded recovery: superblock mount time "
					       "changed during recovery (another node may be alive) — "
					       "aborting to prevent split-brain\n");
					brelse(sb_bh);
					sb->s_flags |= SB_RDONLY;
					ocsfs_node_mark_dead(sb, failed_slot);
					ocsfs_recovery_leader_release(sb, failed_slot, leader_epoch);
					sbi->s_recovery_in_progress = false;
					mutex_unlock(&sbi->s_recovery_lock);
					return -EPERM;
				}
			}
			brelse(sb_bh);
		} else {
			pr_warn("ocsfs: degraded recovery: cannot re-read superblock, "
				"proceeding without mount-time check\n");
		}
	}

	/*
	 * Phase 3 — Journal Replay
	 *
	 * Write OCSFS_RL_REPLAY_ACTIVE to the shared leader block so survivor
	 * nodes defer EX acquisitions during replay (cross-node quiescence,
	 * NUOV-CRIT-6).  The local s_recovery_barrier is set inside
	 * ocsfs_journal_replay_node so it is always paired correctly even if
	 * the function is called directly (NUOV-MEDIO-1).
	 */
	pr_info("ocsfs: recovery phase 3: journal replay for node %u\n",
		failed_slot);

	ocsfs_recovery_set_replay_active(sb, true);
	ret = ocsfs_journal_replay_node(sb, failed_slot);
	ocsfs_recovery_set_replay_active(sb, false);

	if (ret) {
		pr_err("ocsfs: journal replay for node %u failed: %d — "
		       "forcing read-only\n", failed_slot, ret);
		sb->s_flags |= SB_RDONLY;
		ocsfs_recovery_leader_release(sb, failed_slot, leader_epoch);
		sbi->s_recovery_in_progress = false;
		mutex_unlock(&sbi->s_recovery_lock);
		return ret;
	}

	/*
	 * Phase 4 — Lock Recovery
	 */
	pr_info("ocsfs: recovery phase 4: lock recovery\n");

	ret = ocsfs_lock_recover_node(sb, failed_slot, failed_gen);
	if (ret)
		pr_err("ocsfs: lock recovery for node %u failed: %d\n",
		       failed_slot, ret);

	/*
	 * Phase 5 — Slot Cleanup
	 */
	pr_info("ocsfs: recovery phase 5: slot cleanup\n");

	ret = ocsfs_node_mark_dead(sb, failed_slot);
	if (ret)
		pr_err("ocsfs: failed to mark node %u as dead: %d\n",
		       failed_slot, ret);

	ocsfs_recovery_leader_release(sb, failed_slot, leader_epoch);
	sbi->s_recovery_in_progress = false;

	pr_warn("ocsfs: ═══ RECOVERY COMPLETE for node slot %u ═══\n",
		failed_slot);

	mutex_unlock(&sbi->s_recovery_lock);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * RECOVERY WORK — drains s_recovery_pending bitmask
 * ═══════════════════════════════════════════════════════════════ */

static void ocsfs_recovery_work_fn(struct work_struct *work)
{
	struct ocsfs_sb_info *sbi =
		container_of(work, struct ocsfs_sb_info, s_recovery_work);
	struct super_block *sb = sbi->s_sb;
	unsigned int slot;
	/* Per-slot exponential backoff for -EAGAIN (ALTO-V3-4).
	 * Doubles on each contended round, resets when we win or move
	 * to a different slot. */
	unsigned int eagain_ms = OCSFS_RECOVERY_YIELD_MS;

	while ((slot = find_first_bit(sbi->s_recovery_pending,
				      OCSFS_MAX_NODES)) < OCSFS_MAX_NODES) {
		int ret = ocsfs_recovery_run(sb, (u16)slot);
		/*
		 * Clear the bit AFTER the run so that a crash of this node
		 * mid-recovery does not silently drop the pending slot.
		 * If another node won leader election (-EAGAIN), re-arm the bit:
		 * if that leader dies before finishing, the next work invocation
		 * (triggered by heartbeat detecting the dead leader) will win
		 * the CAS and complete recovery for this slot.
		 */
		clear_bit(slot, sbi->s_recovery_pending);
		if (ret == -EAGAIN) {
			set_bit(slot, sbi->s_recovery_pending);
			/* Exponential backoff: another node is the leader now.
			 * Ramp 50ms → 5s to avoid busy-looping when a slow
			 * leader holds the CAS for an extended period. */
			msleep(eagain_ms);
			eagain_ms = min_t(unsigned int,
					  eagain_ms * 2,
					  OCSFS_RECOVERY_EAGAIN_MAX_MS);
		} else if (ret && ret != -EPERM && ret != -EUCLEAN) {
			/* Transient error (I/O, ENOMEM, …): -EPERM means the
			 * node is still alive (degraded cross-check), -EUCLEAN
			 * means journal corruption → SB_RDONLY already set.
			 * For everything else, re-arm and backoff 60s. */
			set_bit(slot, sbi->s_recovery_pending);
			pr_warn("ocsfs: recovery for slot %u failed (%d), "
				"retrying in 60s\n", slot, ret);
			msleep(OCSFS_RECOVERY_BACKOFF_MS);
			eagain_ms = OCSFS_RECOVERY_YIELD_MS;
		} else {
			/* Success or permanent error: reset backoff for next slot. */
			eagain_ms = OCSFS_RECOVERY_YIELD_MS;
		}
	}
}

void ocsfs_recovery_trigger(struct super_block *sb, u16 failed_slot)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	set_bit(failed_slot, sbi->s_recovery_pending);
	schedule_work(&sbi->s_recovery_work);
}

/* ═══════════════════════════════════════════════════════════════
 * INIT / EXIT
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_recovery_init(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	mutex_init(&sbi->s_recovery_lock);
	sbi->s_recovery_in_progress = false;
	bitmap_zero(sbi->s_recovery_pending, OCSFS_MAX_NODES);
	atomic_set(&sbi->s_recovery_barrier, 0);
	atomic_set(&sbi->s_remote_recovery_barrier, 0);
	INIT_WORK(&sbi->s_recovery_work, ocsfs_recovery_work_fn);

	return 0;
}

void ocsfs_recovery_exit(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	/* Drain any in-progress recovery run before cancelling (ALTO-V3-5).
	 * flush_work waits for the current execution to complete; cancel_work_sync
	 * then prevents any re-queued execution from starting. */
	flush_work(&sbi->s_recovery_work);
	cancel_work_sync(&sbi->s_recovery_work);
	if (unlikely(!bitmap_empty(sbi->s_recovery_pending, OCSFS_MAX_NODES)))
		pr_warn("ocsfs: umount with pending recovery slots — "
			"these nodes were not fully recovered\n");
}
