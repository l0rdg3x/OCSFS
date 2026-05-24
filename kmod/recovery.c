// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — recovery.c
 * Multi-phase crash recovery protocol.
 *
 * When a node is detected as failed (heartbeat timeout), the surviving
 * nodes execute a 5-phase recovery:
 *
 *   Phase 1 — Leader Election:   lowest-slot surviving node wins
 *   Phase 2 — SCSI PR Fencing:   preempt-and-abort the failed node's key
 *   Phase 3 — Journal Replay:    replay the failed node's journal
 *   Phase 4 — Lock Recovery:     release all locks held by the failed node
 *   Phase 5 — Slot Cleanup:      mark the slot as DEAD
 *
 * Only the elected leader performs recovery. Other nodes wait.
 * The SCSI PR fencing ensures the failed node cannot write even if
 * it is still running (zombie/partitioned).
 *
 * Multiple concurrent failures are handled via s_recovery_pending bitmask:
 * each failed slot sets its bit; the work function drains them in order.
 */

#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * LEADER ELECTION
 *
 * The active node with the lowest slot number becomes recovery leader.
 * We use a special lock in the Lock Table (LOCKRES_RECOVERY) to
 * ensure only one node performs recovery at a time.
 * ═══════════════════════════════════════════════════════════════ */

static bool ocsfs_is_recovery_leader(struct super_block *sb, u16 failed_slot)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	bool is_leader = true;
	u16 i;

	/*
	 * Hold the lock for the entire scan so no node changes state between
	 * iterations.  Dropping and re-acquiring per iteration creates a TOCTOU
	 * window where a lower-numbered slot could become ACTIVE after we
	 * already skipped it, causing two nodes to both believe they are leader.
	 */
	spin_lock(&sbi->s_node_lock);
	for (i = 0; i < sbi->s_max_nodes; i++) {
		if (i == failed_slot)
			continue;
		if (sbi->s_nodes[i].ni_state == OCSFS_NODE_ACTIVE) {
			is_leader = (i == sbi->s_node_slot);
			goto out;
		}
	}
out:
	spin_unlock(&sbi->s_node_lock);
	return is_leader;
}

/* ═══════════════════════════════════════════════════════════════
 * RECOVERY EXECUTION — 5 phases
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_recovery_run(struct super_block *sb, u16 failed_slot)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_node_info *ni;
	u32 failed_gen;
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
	 * Phase 1 — Leader Election
	 */
	pr_info("ocsfs: recovery phase 1: leader election\n");

	if (!ocsfs_is_recovery_leader(sb, failed_slot)) {
		pr_info("ocsfs: not the recovery leader, deferring\n");
		sbi->s_recovery_in_progress = false;
		mutex_unlock(&sbi->s_recovery_lock);
		return 0;
	}

	/*
	 * In cluster mode, acquire the distributed recovery lock before
	 * proceeding.  This ensures only one node performs recovery even
	 * if in-memory node-state diverges during a partial partition.
	 * Re-check leadership after acquiring to handle the TOCTOU window.
	 */
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(sb, &sbi->s_recovery_lock_res,
					 OCSFS_LOCK_EX);
		if (ret) {
			pr_info("ocsfs: recovery DLM lock failed (%d), deferring\n",
				ret);
			/* Re-arm so the work queue retries this slot */
			set_bit(failed_slot, sbi->s_recovery_pending);
			sbi->s_recovery_in_progress = false;
			mutex_unlock(&sbi->s_recovery_lock);
			return ret;
		}
		if (!ocsfs_is_recovery_leader(sb, failed_slot)) {
			pr_info("ocsfs: leadership lost after DLM acquire, deferring\n");
			ocsfs_lock_release(sb, &sbi->s_recovery_lock_res);
			sbi->s_recovery_in_progress = false;
			mutex_unlock(&sbi->s_recovery_lock);
			return 0;
		}
	}

	pr_info("ocsfs: this node (slot %u) is the recovery leader\n",
		sbi->s_node_slot);

	/*
	 * Phase 2 — SCSI PR Fencing
	 *
	 * Issue PREEMPT AND ABORT to revoke the failed node's PR key.
	 * After this, the SAN fabric will reject any I/O from the
	 * failed node's HBA, providing hardware-level fencing.
	 */
	pr_info("ocsfs: recovery phase 2: SCSI PR fencing\n");

	spin_lock(&sbi->s_node_lock);
	ni->ni_state = OCSFS_NODE_EVICTING;
	spin_unlock(&sbi->s_node_lock);

	ret = ocsfs_pr_preempt_abort(sb, failed_pr_key,
				     OCSFS_PR_TYPE_WRITE_EXCL_REG);
	if (ret) {
		pr_warn("ocsfs: PR fencing failed (ret=%d), "
			"continuing with recovery\n", ret);
		/* Continue anyway — the node may be on a non-SCSI device */
	}

	/*
	 * Phase 3 — Journal Replay
	 *
	 * Read the failed node's journal and replay committed but
	 * uncheckpointed transactions. Uncommitted transactions are
	 * rolled back (before-images restored).
	 */
	pr_info("ocsfs: recovery phase 3: journal replay for node %u\n",
		failed_slot);

	ret = ocsfs_journal_replay_node(sb, failed_slot);
	if (ret) {
		/*
		 * Journal replay failure means we cannot guarantee the
		 * consistency of shared data.  Continuing would allow
		 * other nodes to write on top of potentially dirty blocks.
		 * Force this node read-only and abort recovery so the
		 * administrator can intervene.
		 */
		pr_err("ocsfs: journal replay for node %u failed: %d — "
		       "forcing read-only to prevent data corruption\n",
		       failed_slot, ret);
		sb->s_flags |= SB_RDONLY;
		sbi->s_recovery_in_progress = false;
		if (sbi->s_clustered)
			ocsfs_lock_release(sb, &sbi->s_recovery_lock_res);
		mutex_unlock(&sbi->s_recovery_lock);
		return ret;
	}

	/*
	 * Phase 4 — Lock Recovery
	 *
	 * Scan the entire Lock Table and release/clear any locks held
	 * by the failed node (matching slot + mount generation).
	 */
	pr_info("ocsfs: recovery phase 4: lock recovery\n");

	ret = ocsfs_lock_recover_node(sb, failed_slot, failed_gen);
	if (ret) {
		pr_err("ocsfs: lock recovery for node %u failed: %d\n",
		       failed_slot, ret);
	}

	/*
	 * Phase 5 — Slot Cleanup
	 *
	 * Mark the failed node's slot as DEAD in the Node Slot Table.
	 * The slot becomes reusable when the node re-mounts.
	 */
	pr_info("ocsfs: recovery phase 5: slot cleanup\n");

	ret = ocsfs_node_mark_dead(sb, failed_slot);
	if (ret) {
		pr_err("ocsfs: failed to mark node %u as dead: %d\n",
		       failed_slot, ret);
	}

	sbi->s_recovery_in_progress = false;

	pr_warn("ocsfs: ═══ RECOVERY COMPLETE for node slot %u ═══\n",
		failed_slot);

	if (sbi->s_clustered)
		ocsfs_lock_release(sb, &sbi->s_recovery_lock_res);
	mutex_unlock(&sbi->s_recovery_lock);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * RECOVERY WORK — drains s_recovery_pending bitmask
 *
 * Processes all pending failed slots in slot-number order.
 * New failures arriving while the work runs are picked up by
 * the while loop on the next iteration without losing any event.
 * ═══════════════════════════════════════════════════════════════ */

static void ocsfs_recovery_work_fn(struct work_struct *work)
{
	struct ocsfs_sb_info *sbi =
		container_of(work, struct ocsfs_sb_info, s_recovery_work);
	struct super_block *sb = sbi->s_sb;
	unsigned int slot;

	while ((slot = find_first_bit(sbi->s_recovery_pending,
				      OCSFS_MAX_NODES)) < OCSFS_MAX_NODES) {
		/*
		 * Clear the bit before running recovery so that a new
		 * failure on the same slot arriving during recovery is
		 * not silently dropped — it will set the bit again and
		 * the loop will process it.
		 */
		clear_bit(slot, sbi->s_recovery_pending);
		ocsfs_recovery_run(sb, (u16)slot);
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
	INIT_WORK(&sbi->s_recovery_work, ocsfs_recovery_work_fn);
	ocsfs_lock_init(&sbi->s_recovery_lock_res,
			ocsfs_lock_hash_recovery(),
			OCSFS_LOCKRES_RECOVERY);

	return 0;
}

void ocsfs_recovery_exit(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	cancel_work_sync(&sbi->s_recovery_work);
}
