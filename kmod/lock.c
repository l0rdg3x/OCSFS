// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — lock.c
 * Lock acquire, release, downgrade, and node recovery.
 * I/O helpers, bitmask helpers, and resource allocation are in lock_io.c.
 *
 * Lock compatibility matrix:
 *   NL + anything = compatible
 *   SH + SH       = compatible
 *   SH + EX       = conflict
 *   EX + anything = conflict (except NL)
 *   CW + CW       = compatible (registrants-only mode)
 */

#include "ocsfs.h"
#include "lock_internal.h"

/* ═══════════════════════════════════════════════════════════════
 * LOCK ACQUIRE
 *
 * Protocol:
 *   1. Read lock entry from disk
 *   2. Check compatibility with current holder
 *   3. If compatible: update entry via CAS (add self as holder)
 *   4. If conflict: set waiter bit, poll with exponential backoff
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_lock_acquire(struct super_block *sb, struct ocsfs_lock_res *lr,
		       u16 mode)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_lock dl;
	struct buffer_head *bh;
	int ret;
	int retries = 0;
	u32 delay_us = OCSFS_LOCK_RETRY_MIN_US;

	if (!sbi->s_clustered) {
		lr->lr_mode = mode;
		return 0;
	}

	mutex_lock(&lr->lr_mutex);

	/*
	 * NOTE: the cache fast-path was removed because it allowed a remote
	 * node that had preempted/recovered our holder slot to be ignored
	 * for up to OCSFS_LOCK_CACHE_NS, violating cross-node coherence.
	 * Every acquire now goes through the on-disk lock table.
	 *
	 * The correct optimization is an epoch counter bumped by
	 * ocsfs_lock_recover_node() and checked here — NOT a wall-clock TTL.
	 */
	lr->lr_cached        = false;
	lr->lr_cache_expires = 0;

	ret = lock_probe_slot(sb, lr);
	if (ret) {
		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

retry:
	ret = lock_read_entry(sb, lr->lr_slot, &dl, &bh);
	if (ret) {
		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

	if (le32_to_cpu(dl.le_magic) != OCSFS_LOCK_MAGIC) {
		memset(&dl, 0, sizeof(dl));
		dl.le_magic         = cpu_to_le32(OCSFS_LOCK_MAGIC);
		dl.le_resource_id   = cpu_to_le64(lr->lr_resource_id);
		dl.le_resource_type = cpu_to_le32(lr->lr_resource_type);
	}

	u16 cur_mode = le16_to_cpu(dl.le_mode);

	if (cur_mode == OCSFS_LOCK_NL || lock_modes_compatible(cur_mode, mode)) {
		if (mode == OCSFS_LOCK_EX) {
			dl.le_mode        = cpu_to_le16(OCSFS_LOCK_EX);
			dl.le_holder_slot = cpu_to_le16(sbi->s_node_slot);
			dl.le_holder_gen  = cpu_to_le32(sbi->s_mount_gen);
		} else if (mode == OCSFS_LOCK_SH) {
			dl.le_mode = cpu_to_le16(OCSFS_LOCK_SH);
			add_sh_holder(&dl, sbi->s_node_slot);
		} else if (mode == OCSFS_LOCK_CW) {
			dl.le_mode = cpu_to_le16(OCSFS_LOCK_CW);
			add_sh_holder(&dl, sbi->s_node_slot);
		}

		dl.le_grant_time = cpu_to_le64(ktime_get_real_ns());
		clear_waiter_bit(&dl, sbi->s_node_slot);

		ret = lock_write_entry(sb, lr->lr_slot, &dl, bh);
		brelse(bh);

		if (ret == -EAGAIN)
			goto retry;

		if (ret == 0) {
			lr->lr_mode          = mode;
			/* No caching — see comment at top of ocsfs_lock_acquire(). */
			lr->lr_cached        = false;
			lr->lr_cache_expires = 0;
		}

		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

	set_waiter_bit(&dl, sbi->s_node_slot);
	lock_write_entry(sb, lr->lr_slot, &dl, bh);
	brelse(bh);

	if (++retries > OCSFS_LOCK_MAX_RETRIES) {
		pr_warn("ocsfs: lock acquire timeout on resource 0x%llx "
			"(mode %u, held %u by slot %u)\n",
			lr->lr_resource_id, mode, cur_mode,
			le16_to_cpu(dl.le_holder_slot));
		mutex_unlock(&lr->lr_mutex);
		return -ETIMEDOUT;
	}

	usleep_range(delay_us, delay_us * 2);
	delay_us = min_t(u32, delay_us * 2, OCSFS_LOCK_RETRY_MAX_US);
	goto retry;
}

/* ═══════════════════════════════════════════════════════════════
 * LOCK RELEASE
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_lock_release(struct super_block *sb, struct ocsfs_lock_res *lr)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_lock dl;
	struct buffer_head *bh;
	int ret;

	if (!sbi->s_clustered) {
		lr->lr_mode = OCSFS_LOCK_NL;
		return 0;
	}

	mutex_lock(&lr->lr_mutex);

	ret = lock_read_entry(sb, lr->lr_slot, &dl, &bh);
	if (ret) {
		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

	if (lr->lr_mode == OCSFS_LOCK_EX) {
		dl.le_holder_slot = 0;
		dl.le_holder_gen  = 0;
		if (!has_sh_holders(&dl) && !has_waiters(&dl))
			dl.le_mode = cpu_to_le16(OCSFS_LOCK_NL);
	} else if (lr->lr_mode == OCSFS_LOCK_SH ||
		   lr->lr_mode == OCSFS_LOCK_CW) {
		remove_sh_holder(&dl, sbi->s_node_slot);
		if (!has_sh_holders(&dl) && !has_waiters(&dl))
			dl.le_mode = cpu_to_le16(OCSFS_LOCK_NL);
	}

	ret = lock_write_entry(sb, lr->lr_slot, &dl, bh);
	brelse(bh);

	lr->lr_mode          = OCSFS_LOCK_NL;
	lr->lr_cached        = false;
	lr->lr_cache_expires = 0;
	mutex_unlock(&lr->lr_mutex);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * LOCK DOWNGRADE
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_lock_downgrade(struct super_block *sb, struct ocsfs_lock_res *lr,
			 u16 new_mode)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_lock dl;
	struct buffer_head *bh;
	int ret;

	if (!sbi->s_clustered) {
		lr->lr_mode = new_mode;
		return 0;
	}

	if (new_mode >= lr->lr_mode)
		return -EINVAL;

	mutex_lock(&lr->lr_mutex);

	ret = lock_read_entry(sb, lr->lr_slot, &dl, &bh);
	if (ret) {
		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

	if (lr->lr_mode == OCSFS_LOCK_EX && new_mode == OCSFS_LOCK_SH) {
		dl.le_mode        = cpu_to_le16(OCSFS_LOCK_SH);
		dl.le_holder_slot = 0;
		dl.le_holder_gen  = 0;
		add_sh_holder(&dl, sbi->s_node_slot);
	} else if (new_mode == OCSFS_LOCK_NL) {
		/*
		 * Must release lr_mutex before calling ocsfs_lock_release(),
		 * which acquires the same mutex — would deadlock on non-recursive
		 * mutexes (BUG-002 fix).
		 */
		brelse(bh);
		mutex_unlock(&lr->lr_mutex);
		return ocsfs_lock_release(sb, lr);
	}

	ret = lock_write_entry(sb, lr->lr_slot, &dl, bh);
	brelse(bh);

	if (ret == 0) {
		lr->lr_mode          = new_mode;
		lr->lr_cached        = false;
		lr->lr_cache_expires = 0;
	}

	mutex_unlock(&lr->lr_mutex);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * LOCK RECOVERY — release all locks held by a failed node
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_lock_recover_node(struct super_block *sb, u16 node_slot,
			    u32 mount_gen)
{
	struct ocsfs_disk_lock dl;
	struct buffer_head *bh;
	u32 i;
	int recovered = 0;
	int ret;

	pr_info("ocsfs: recovering locks for node slot %u (gen=%u)\n",
		node_slot, mount_gen);

	for (i = 0; i < OCSFS_LOCK_ENTRY_COUNT; i++) {
		ret = lock_read_entry(sb, i, &dl, &bh);
		if (ret)
			continue;

		if (le32_to_cpu(dl.le_magic) != OCSFS_LOCK_MAGIC) {
			brelse(bh);
			continue;
		}

		bool modified = false;

		if (le16_to_cpu(dl.le_mode) == OCSFS_LOCK_EX &&
		    le16_to_cpu(dl.le_holder_slot) == node_slot &&
		    le32_to_cpu(dl.le_holder_gen) == mount_gen) {
			dl.le_holder_slot = 0;
			dl.le_holder_gen  = 0;
			if (!has_sh_holders(&dl))
				dl.le_mode = cpu_to_le16(OCSFS_LOCK_NL);
			modified = true;
			recovered++;
		}

		if (is_sh_holder(&dl, node_slot)) {
			remove_sh_holder(&dl, node_slot);
			if (!has_sh_holders(&dl) &&
			    le16_to_cpu(dl.le_mode) != OCSFS_LOCK_EX)
				dl.le_mode = cpu_to_le16(OCSFS_LOCK_NL);
			modified = true;
			recovered++;
		}

		clear_waiter_bit(&dl, node_slot);

		if (modified)
			lock_write_entry(sb, i, &dl, bh);

		brelse(bh);
	}

	pr_info("ocsfs: recovered %d locks from node slot %u\n",
		recovered, node_slot);
	return 0;
}
