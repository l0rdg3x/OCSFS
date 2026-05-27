// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — heartbeat.c
 * Storage-path heartbeat for node liveness detection.
 *
 * Each active node writes its timestamp + monotonic sequence number
 * to its heartbeat sector on the shared LUN every HB_INTERVAL (5s).
 *
 * A background kernel thread periodically reads all heartbeat sectors
 * and flags nodes whose heartbeat is older than HB_TIMEOUT (15s) as
 * potentially dead, triggering the recovery protocol.
 *
 * This is fundamentally superior to network heartbeats because it
 * validates the actual I/O path to the shared storage device.
 */

#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * HEARTBEAT I/O
 * ═══════════════════════════════════════════════════════════════ */

static int heartbeat_read_timeout(struct buffer_head *bh);

/* Submit a heartbeat block write with bounded I/O timeout. */
static int heartbeat_write_timeout(struct buffer_head *bh)
{
	lock_buffer(bh);
	if (!test_clear_buffer_dirty(bh)) {
		unlock_buffer(bh);
		return 0;
	}
	get_bh(bh);
	bh->b_end_io = end_buffer_write_sync;
	submit_bh(REQ_OP_WRITE | REQ_SYNC, bh);
	if (wait_on_bit_timeout(&bh->b_state, BH_Lock, TASK_UNINTERRUPTIBLE,
				msecs_to_jiffies(OCSFS_HB_IO_TIMEOUT_MS))) {
		pr_warn_ratelimited(
			"ocsfs: heartbeat I/O timeout after %ums -- FC path hung\n",
			OCSFS_HB_IO_TIMEOUT_MS);
		return -ETIMEDOUT;
	}
	/* Check write I/O error, not read uptodate: a write can silently fail
	 * while leaving the buffer marked uptodate from the last read. */
	return buffer_write_io_error(bh) ? -EIO : 0;
}

/* NUOV-ARCH-3: best-effort RMW of own slot in the HB summary block. */
static void ocsfs_hb_summary_update(struct super_block *sb, u16 slot,
				     u64 sequence, u64 timestamp)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 block = OCSFS_HB_SUMMARY_OFF / sbi->s_block_size;
	u32 boff  = (u32)(slot * sizeof(struct ocsfs_disk_hb_summary_entry));
	struct buffer_head *bh;
	struct ocsfs_disk_hb_summary_entry *hse;

	if (boff + sizeof(*hse) > sbi->s_block_size)
		return;

	bh = sb_getblk(sb, block);
	if (!bh)
		return;

	if (heartbeat_read_timeout(bh) < 0) {
		brelse(bh);
		return;
	}

	hse = (struct ocsfs_disk_hb_summary_entry *)(bh->b_data + boff);
	hse->hse_sequence  = cpu_to_le64(sequence);
	hse->hse_timestamp = cpu_to_le64(timestamp);

	mark_buffer_dirty(bh);
	heartbeat_write_timeout(bh);
	brelse(bh);
}

/* Write this node's heartbeat entry to disk */
int ocsfs_heartbeat_write(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct buffer_head *bh;
	struct ocsfs_disk_heartbeat *dhb;
	int ret;
	u64 seq, ts;
	u64 off = OCSFS_HEARTBEAT_OFF +
		  (u64)sbi->s_node_slot * OCSFS_HEARTBEAT_ENTRY_SIZE;
	u64 block = off / sbi->s_block_size;
	u32 boff = off % sbi->s_block_size;

	/*
	 * Use a bounded read so a hung FC path cannot stall the heartbeat
	 * thread indefinitely during the initial block fetch.
	 */
	bh = sb_getblk(sb, block);
	if (!bh)
		return -EIO;
	ret = heartbeat_read_timeout(bh);
	if (ret) {
		brelse(bh);
		return ret;
	}

	dhb = (struct ocsfs_disk_heartbeat *)(bh->b_data + boff);

	ts  = ktime_get_real_ns();
	seq = atomic64_inc_return(&sbi->s_hb.hb_sequence);

	dhb->hb_magic     = cpu_to_le32(OCSFS_HEARTBEAT_MAGIC);
	dhb->hb_node_slot = cpu_to_le16(sbi->s_node_slot);
	dhb->hb_state     = cpu_to_le16(OCSFS_NODE_ACTIVE);
	dhb->hb_timestamp = cpu_to_le64(ts);
	dhb->hb_sequence  = cpu_to_le64(seq);
	dhb->hb_mount_gen = cpu_to_le32(sbi->s_mount_gen);
	dhb->hb_checksum  = cpu_to_le32(
		ocsfs_crc32c(~0U, dhb,
			     OCSFS_HEARTBEAT_ENTRY_SIZE - sizeof(__le32)));

	mark_buffer_dirty(bh);
	ret = heartbeat_write_timeout(bh);
	brelse(bh);

	/* NUOV-ARCH-3: update summary block so check_peers needs only 1 read */
	if (ret == 0 && (sbi->s_feature_ro_compat & OCSFS_FEATURE_RO_COMPAT_HB_SUMMARY))
		ocsfs_hb_summary_update(sb, sbi->s_node_slot, seq, ts);

	return ret;
}

/* Submit a heartbeat block read with bounded I/O timeout. */
static int heartbeat_read_timeout(struct buffer_head *bh)
{
	lock_buffer(bh);
	clear_buffer_uptodate(bh);
	get_bh(bh);
	bh->b_end_io = end_buffer_read_sync;
	submit_bh(REQ_OP_READ, bh);
	if (wait_on_bit_timeout(&bh->b_state, BH_Lock, TASK_UNINTERRUPTIBLE,
				msecs_to_jiffies(OCSFS_HB_IO_TIMEOUT_MS))) {
		pr_warn_ratelimited(
			"ocsfs: heartbeat read timeout after %ums -- FC path hung\n",
			OCSFS_HB_IO_TIMEOUT_MS);
		return -ETIMEDOUT;
	}
	return buffer_uptodate(bh) ? 0 : -EIO;
}


/* ═══════════════════════════════════════════════════════════════
 * PEER CHECKING
 *
 * Read all active peers' heartbeats and detect failures.
 * A node is "dead" if:
 *   - Its slot state is ACTIVE
 *   - Its heartbeat timestamp is older than HB_TIMEOUT
 *   - Its mount generation matches the slot table
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_heartbeat_check_peers(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 now = ktime_get_real_ns();
	u64 timeout_ns = (u64)OCSFS_HB_TIMEOUT_MS * 1000000ULL;
	struct buffer_head *cur_bh = NULL;
	u64 cur_block = (u64)-1;
	u16 i;

	/* NUOV-ARCH-3: O(1) fast path — one summary block instead of N/4 reads */
	if (sbi->s_feature_ro_compat & OCSFS_FEATURE_RO_COMPAT_HB_SUMMARY) {
		u64 sum_blk = OCSFS_HB_SUMMARY_OFF / sbi->s_block_size;
		struct buffer_head *sum_bh = sb_getblk(sb, sum_blk);

		if (sum_bh && heartbeat_read_timeout(sum_bh) == 0) {
			for (i = 0; i < sbi->s_max_nodes; i++) {
				struct ocsfs_node_info *ni;
				const struct ocsfs_disk_hb_summary_entry *hse;
				u64 hb_ts;
				u32 boff;

				if (i == sbi->s_node_slot)
					continue;

				spin_lock(&sbi->s_node_lock);
				ni = &sbi->s_nodes[i];
				if (ni->ni_state != OCSFS_NODE_ACTIVE &&
				    ni->ni_state != OCSFS_NODE_SUSPECTED) {
					spin_unlock(&sbi->s_node_lock);
					continue;
				}
				spin_unlock(&sbi->s_node_lock);

				boff = (u32)(i * sizeof(*hse));
				if (boff + sizeof(*hse) > sbi->s_block_size)
					continue;

				hse = (const struct ocsfs_disk_hb_summary_entry *)
				      (sum_bh->b_data + boff);
				hb_ts = le64_to_cpu(hse->hse_timestamp);

				spin_lock(&sbi->s_node_lock);
				ni->ni_last_hb     = hb_ts;
				ni->ni_hb_sequence = le64_to_cpu(hse->hse_sequence);
				spin_unlock(&sbi->s_node_lock);

				if (now - hb_ts > timeout_ns) {
					u64 confirm_ns = (u64)OCSFS_HB_CONFIRM_MS *
							 1000000ULL;

					spin_lock(&sbi->s_node_lock);
					if (ni->ni_state == OCSFS_NODE_ACTIVE) {
						ni->ni_state = OCSFS_NODE_SUSPECTED;
						ni->ni_suspect_time = now;
						spin_unlock(&sbi->s_node_lock);
						pr_warn("ocsfs: node slot %u heartbeat stale "
							"(%llums), marking suspected\n",
							i,
							(now - hb_ts) / 1000000ULL);
					} else if (ni->ni_state == OCSFS_NODE_SUSPECTED &&
						   (now - ni->ni_suspect_time) > confirm_ns) {
						spin_unlock(&sbi->s_node_lock);
						pr_warn("ocsfs: node slot %u confirmed dead "
							"(suspected %llums ago), triggering recovery\n",
							i,
							(now - ni->ni_suspect_time) / 1000000ULL);
						ocsfs_recovery_trigger(sb, i);
					} else {
						spin_unlock(&sbi->s_node_lock);
					}
				} else {
					spin_lock(&sbi->s_node_lock);
					if (ni->ni_state == OCSFS_NODE_SUSPECTED) {
						ni->ni_state = OCSFS_NODE_ACTIVE;
						ni->ni_suspect_time = 0;
						pr_info("ocsfs: node slot %u heartbeat recovered\n",
							i);
					}
					spin_unlock(&sbi->s_node_lock);
				}
			}
			brelse(sum_bh);
			goto check_recovery_leader;
		}
		if (sum_bh)
			brelse(sum_bh);
		/* summary read failed — fall through to slow path */
	}

	for (i = 0; i < sbi->s_max_nodes; i++) {
		struct ocsfs_node_info *ni;
		struct ocsfs_disk_heartbeat *dhb;
		u64 off, block, hb_ts;
		u32 boff;

		/* Skip ourselves */
		if (i == sbi->s_node_slot)
			continue;

		spin_lock(&sbi->s_node_lock);
		ni = &sbi->s_nodes[i];
		if (ni->ni_state != OCSFS_NODE_ACTIVE &&
		    ni->ni_state != OCSFS_NODE_EVICTING) {
			spin_unlock(&sbi->s_node_lock);
			continue;
		}
		spin_unlock(&sbi->s_node_lock);

		off   = OCSFS_HEARTBEAT_OFF + (u64)i * OCSFS_HEARTBEAT_ENTRY_SIZE;
		block = off / sbi->s_block_size;
		boff  = (u32)(off % sbi->s_block_size);

		/*
		 * Read the physical block only when it changes.  With 4 HB
		 * entries per 4 KiB block this cuts I/O by up to 4x for
		 * dense node tables.  heartbeat_read_timeout forces a fresh
		 * read past the page cache so all slots in the block are
		 * coherent with each other (same point-in-time snapshot).
		 */
		if (block != cur_block) {
			if (cur_bh) {
				brelse(cur_bh);
				cur_bh = NULL;
			}
			cur_bh = sb_getblk(sb, block);
			if (cur_bh && heartbeat_read_timeout(cur_bh) < 0) {
				brelse(cur_bh);
				cur_bh = NULL;
			}
			cur_block = block;
		}

		if (!cur_bh)
			continue;

		dhb = (struct ocsfs_disk_heartbeat *)(cur_bh->b_data + boff);

		if (le32_to_cpu(dhb->hb_magic) != OCSFS_HEARTBEAT_MAGIC)
			continue;

		{
			u32 crc = ocsfs_crc32c(~0U, dhb,
					       OCSFS_HEARTBEAT_ENTRY_SIZE -
					       sizeof(__le32));

			if (le32_to_cpu(dhb->hb_checksum) != crc)
				continue;
		}

		hb_ts = le64_to_cpu(dhb->hb_timestamp);

		/* Update cached heartbeat info */
		spin_lock(&sbi->s_node_lock);
		ni->ni_last_hb = hb_ts;
		ni->ni_hb_sequence = le64_to_cpu(dhb->hb_sequence);
		spin_unlock(&sbi->s_node_lock);

		/* Two-stage detection: SUSPECTED → confirmed dead */
		if (now - hb_ts > timeout_ns) {
			u64 confirm_ns = (u64)OCSFS_HB_CONFIRM_MS * 1000000ULL;

			spin_lock(&sbi->s_node_lock);
			if (ni->ni_state == OCSFS_NODE_ACTIVE) {
				/* First time we notice staleness: mark suspected */
				ni->ni_state = OCSFS_NODE_SUSPECTED;
				ni->ni_suspect_time = now;
				spin_unlock(&sbi->s_node_lock);
				pr_warn("ocsfs: node slot %u heartbeat stale "
					"(%llums), marking suspected\n",
					i, (now - hb_ts) / 1000000ULL);
			} else if (ni->ni_state == OCSFS_NODE_SUSPECTED &&
				   (now - ni->ni_suspect_time) > confirm_ns) {
				/* Still no heartbeat after confirm window — dead */
				spin_unlock(&sbi->s_node_lock);
				pr_warn("ocsfs: node slot %u confirmed dead "
					"(suspected %llums ago), triggering recovery\n",
					i,
					(now - ni->ni_suspect_time) / 1000000ULL);
				ocsfs_recovery_trigger(sb, i);
			} else {
				spin_unlock(&sbi->s_node_lock);
			}
		} else {
			/* Heartbeat is fresh — clear suspected state if set */
			spin_lock(&sbi->s_node_lock);
			if (ni->ni_state == OCSFS_NODE_SUSPECTED) {
				ni->ni_state = OCSFS_NODE_ACTIVE;
				ni->ni_suspect_time = 0;
				pr_info("ocsfs: node slot %u heartbeat recovered\n",
					i);
			}
			spin_unlock(&sbi->s_node_lock);
		}
	}

	if (cur_bh)
		brelse(cur_bh);

check_recovery_leader:
	/*
	 * CRIT-6: probe the recovery leader block for OCSFS_RL_REPLAY_ACTIVE.
	 * If a peer leader has set this flag, it is currently replaying a failed
	 * node's journal AFTER-images.  We set s_remote_recovery_barrier so that
	 * ocsfs_lock_acquire() defers EX acquisitions, preventing the race where
	 * a survivor writes a block that the replaying leader is about to restore.
	 * The staleness window is at most one HB_CHECK interval.
	 */
	if (sbi->s_clustered) {
		u64 rl_blk = OCSFS_RECOVERY_LEADER_OFF / sbi->s_block_size;
		struct buffer_head *rl_bh = sb_getblk(sb, rl_blk);
		bool remote_replay = false;

		if (rl_bh) {
			clear_buffer_uptodate(rl_bh);
			if (bh_read(rl_bh, 0) >= 0) {
				const struct ocsfs_disk_recovery_leader *rl =
					(const struct ocsfs_disk_recovery_leader *)
					rl_bh->b_data;

				if (le32_to_cpu(rl->rl_magic) ==
					    OCSFS_RECOVERY_LEADER_MAGIC &&
				    le16_to_cpu(rl->rl_leader_slot) !=
					    OCSFS_RL_SLOT_FREE &&
				    le16_to_cpu(rl->rl_leader_slot) !=
					    sbi->s_node_slot &&
				    (le32_to_cpu(rl->rl_epoch) &
					    OCSFS_RL_REPLAY_ACTIVE))
					remote_replay = true;
			}
			brelse(rl_bh);
		}
		atomic_set(&sbi->s_remote_recovery_barrier, remote_replay ? 1 : 0);
	}

	return 0;
}

/* Is a node considered alive? */
bool ocsfs_node_is_alive(struct super_block *sb, u16 slot)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_node_info *ni;
	u64 now = ktime_get_real_ns();
	u64 timeout_ns = (u64)OCSFS_HB_TIMEOUT_MS * 1000000ULL;
	bool alive;

	if (slot >= sbi->s_max_nodes)
		return false;
	if (slot == sbi->s_node_slot)
		return true;

	spin_lock(&sbi->s_node_lock);
	ni = &sbi->s_nodes[slot];
	/* SUSPECTED is still considered alive until confirmed dead */
	alive = ((ni->ni_state == OCSFS_NODE_ACTIVE ||
		  ni->ni_state == OCSFS_NODE_SUSPECTED) &&
		 (now - ni->ni_last_hb) < timeout_ns * OCSFS_HB_SUSPECTED_MULT);
	spin_unlock(&sbi->s_node_lock);

	return alive;
}

/* ═══════════════════════════════════════════════════════════════
 * HEARTBEAT THREAD
 *
 * Background kthread that:
 *   1. Writes our heartbeat every HB_INTERVAL
 *   2. Checks peers every HB_CHECK interval
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_heartbeat_thread(void *data)
{
	struct super_block *sb = data;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	unsigned long write_jiffies = msecs_to_jiffies(OCSFS_HB_INTERVAL_MS);
	unsigned long check_jiffies = msecs_to_jiffies(OCSFS_HB_CHECK_MS);
	unsigned long next_write = jiffies;
	unsigned long next_check = jiffies + check_jiffies;

	pr_info("ocsfs: heartbeat thread started (slot %u, interval %ums)\n",
		sbi->s_node_slot, OCSFS_HB_INTERVAL_MS);

	while (!kthread_should_stop()) {
		unsigned long now = jiffies;
		long sleep_jiffies;

		/* Write our heartbeat */
		if (time_after_eq(now, next_write)) {
			ocsfs_heartbeat_write(sb);
			next_write = jiffies + write_jiffies;
			/*
			 * Recheck stop after potentially long I/O (up to
			 * OCSFS_HB_IO_TIMEOUT_MS ms) so umount does not hang
			 * indefinitely when the FC path is hung.
			 */
			if (kthread_should_stop())
				break;
		}

		/* Check peers */
		if (time_after_eq(now, next_check)) {
			ocsfs_heartbeat_check_peers(sb);
			next_check = jiffies + check_jiffies;
			if (kthread_should_stop())
				break;
		}

		/* Sleep until next event, but not longer than needed */
		sleep_jiffies = min_t(long,
				      (long)(next_write - jiffies),
				      (long)(next_check - jiffies));
		if (sleep_jiffies > 0)
			wait_event_interruptible_timeout(sbi->s_hb.hb_waitq,
							 kthread_should_stop(),
							 sleep_jiffies);
	}

	pr_info("ocsfs: heartbeat thread stopped\n");
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * START / STOP
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_heartbeat_start(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	int ret;

	atomic64_set(&sbi->s_hb.hb_sequence, 0);
	init_waitqueue_head(&sbi->s_hb.hb_waitq);

	/* Write initial heartbeat */
	ret = ocsfs_heartbeat_write(sb);
	if (ret)
		return ret;

	/* Start background thread */
	sbi->s_hb.hb_thread = kthread_run(ocsfs_heartbeat_thread, sb,
					   "ocsfs-hb/%u", sbi->s_node_slot);
	if (IS_ERR(sbi->s_hb.hb_thread)) {
		ret = PTR_ERR(sbi->s_hb.hb_thread);
		sbi->s_hb.hb_thread = NULL;
		return ret;
	}

	sbi->s_hb.hb_running = true;
	return 0;
}

void ocsfs_heartbeat_stop(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	if (sbi->s_hb.hb_thread) {
		wake_up(&sbi->s_hb.hb_waitq);
		kthread_stop(sbi->s_hb.hb_thread);
		sbi->s_hb.hb_thread = NULL;
		sbi->s_hb.hb_running = false;
		pr_info("ocsfs: heartbeat stopped\n");
	}
}
