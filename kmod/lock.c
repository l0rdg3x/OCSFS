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
 * ARCH-2: per-lock-res dispatch helpers
 * Overflow entries live in separately-allocated blocks (lr_overflow_addr != 0);
 * primary entries live in the lock table (lr_overflow_addr == 0).
 * ═══════════════════════════════════════════════════════════════ */

static int lr_read_entry(struct super_block *sb, struct ocsfs_lock_res *lr,
			 struct ocsfs_disk_lock *dl, struct buffer_head **bh_out)
{
	if (lr->lr_overflow_addr)
		return lock_read_entry_at_addr(sb, lr->lr_overflow_addr, dl, bh_out);
	return lock_read_entry(sb, lr->lr_slot, dl, bh_out);
}

static int lr_write_entry(struct super_block *sb, struct ocsfs_lock_res *lr,
			  struct ocsfs_disk_lock *entry, struct buffer_head *bh)
{
	if (lr->lr_overflow_addr)
		return lock_write_entry_at_addr(sb, lr->lr_overflow_addr, entry, bh);
	return lock_write_entry(sb, lr->lr_slot, entry, bh);
}

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
	u32 delay_us = OCSFS_LOCK_RETRY_MIN_US;
	ktime_t deadline = ktime_add_ms(ktime_get(),
					OCSFS_LOCK_ACQUIRE_TIMEOUT_MS);

	if (!sbi->s_clustered) {
		lr->lr_mode = mode;
		return 0;
	}

	/*
	 * Recovery barrier: while the leader is replaying journal AFTER-images
	 * (Phase 3), block any local EX acquisition to avoid overwriting data
	 * that the replay is about to restore.  Return -EAGAIN so callers retry
	 * after recovery completes.
	 */
	if (mode == OCSFS_LOCK_EX &&
	    (atomic_read(&sbi->s_recovery_barrier) ||
	     atomic_read(&sbi->s_remote_recovery_barrier)))
		return -EAGAIN;

	mutex_lock(&lr->lr_mutex);

	/*
	 * Epoch-based lock cache (MEDIO-V3-1, ARCH-V3-7).
	 *
	 * Cache hit: we already hold a compatible mode AND s_lock_epoch hasn't
	 * changed since our last disk validation (no recovery since then).
	 * Skip both lock_probe_slot and the disk CAS round-trip.
	 *
	 * Safety: while we hold SH no peer can hold EX, so the disk state
	 * cannot have changed in a way that would invalidate our SH.  While
	 * we hold EX no peer can hold anything, so EX re-acquires are safe.
	 * Recovery bumps s_lock_epoch, invalidating all cached entries.
	 *
	 * CW is excluded from caching: it is never requested in the current
	 * code paths, so the numeric ordering (CW=3 > EX=2) is not a proxy
	 * for lock strength.
	 */
	{
		u32 cur_epoch = (u32)atomic_read(&sbi->s_lock_epoch);

		if (lr->lr_mode >= mode &&
		    lr->lr_mode != OCSFS_LOCK_CW &&
		    lr->lr_lock_epoch == cur_epoch) {
			mutex_unlock(&lr->lr_mutex);
			return 0;
		}
	}

	ret = lock_probe_slot(sb, lr);
	if (ret) {
		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

retry:
	ret = lr_read_entry(sb, lr, &dl, &bh);
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
			/* ARCH-V3-5: write lease so waiters know when to retry */
			dl.le_lease_ns   = cpu_to_le64(ktime_get_real_ns() +
						       OCSFS_LOCK_LEASE_NS);
			dl.le_lease_slot = cpu_to_le16(sbi->s_node_slot);
		} else if (mode == OCSFS_LOCK_SH) {
			dl.le_mode = cpu_to_le16(OCSFS_LOCK_SH);
			add_sh_holder(&dl, sbi->s_node_slot);
		} else if (mode == OCSFS_LOCK_CW) {
			dl.le_mode = cpu_to_le16(OCSFS_LOCK_CW);
			add_sh_holder(&dl, sbi->s_node_slot);
		}

		dl.le_grant_time = cpu_to_le64(ktime_get_real_ns());
		clear_waiter_bit(&dl, sbi->s_node_slot);

		ret = lr_write_entry(sb, lr, &dl, bh);
		brelse(bh);

		if (ret == -EAGAIN)
			goto retry;

		if (ret == 0) {
			lr->lr_mode = mode;
			/* ARCH-7: snapshot the previous EX holder's dirty range/epoch
			 * for both SH and EX mode.
			 * SH: read path uses it for selective page cache invalidation.
			 * EX: write path uses it before starting the write so it can
			 *     invalidate only the stale pages (not the full mapping). */
			lr->lr_inv_lo     = le64_to_cpu(dl.le_inv_lo);
			lr->lr_inv_hi     = le64_to_cpu(dl.le_inv_hi);
			lr->lr_inv_epoch  = le32_to_cpu(dl.le_inv_epoch);
			/* Record epoch for cache: next acquire at same-or-lower mode
			 * skips the disk round-trip (MEDIO-V3-1). */
			lr->lr_lock_epoch = (u32)atomic_read(&sbi->s_lock_epoch);
		}

		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

	set_waiter_bit(&dl, sbi->s_node_slot);
	lr_write_entry(sb, lr, &dl, bh);
	brelse(bh);

	if (ktime_after(ktime_get(), deadline)) {
		pr_warn("ocsfs: lock acquire timeout (%ums) on resource 0x%llx "
			"(mode %u, held %u by slot %u)\n",
			OCSFS_LOCK_ACQUIRE_TIMEOUT_MS,
			lr->lr_resource_id, mode, cur_mode,
			le16_to_cpu(dl.le_holder_slot));
		mutex_unlock(&lr->lr_mutex);
		return -ETIMEDOUT;
	}

	/* ARCH-V3-5: if the current holder has a valid lease, sleep until it
	 * expires rather than waking every delay_us — reduces CAS contention. */
	{
		u64 lease = le64_to_cpu(dl.le_lease_ns);
		u64 now   = ktime_get_real_ns();

		if (lease && now < lease)
			delay_us = min_t(u32, (u32)((lease - now) / 1000),
					 OCSFS_LOCK_RETRY_MAX_US);
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
	int retries = 0;

	if (!sbi->s_clustered) {
		lr->lr_mode = OCSFS_LOCK_NL;
		return 0;
	}

	mutex_lock(&lr->lr_mutex);

retry_release:
	ret = lr_read_entry(sb, lr, &dl, &bh);
	if (ret) {
		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

	if (lr->lr_mode == OCSFS_LOCK_EX) {
		/* ARCH-7: record dirty range and bump epoch so the next SH
		 * acquirer can do selective page cache invalidation instead of
		 * always calling invalidate_inode_pages2 on the full file. */
		dl.le_inv_lo    = cpu_to_le64(lr->lr_inv_lo);
		dl.le_inv_hi    = cpu_to_le64(lr->lr_inv_hi);
		dl.le_inv_epoch = cpu_to_le32(le32_to_cpu(dl.le_inv_epoch) + 1);
		lr->lr_inv_lo   = 0;
		lr->lr_inv_hi   = 0;
		dl.le_holder_slot = 0;
		dl.le_holder_gen  = 0;
		/* ARCH-V3-5: clear lease so waiting nodes can proceed */
		dl.le_lease_ns   = 0;
		dl.le_lease_slot = cpu_to_le16(OCSFS_LOCK_NO_LEASE);
		/*
		 * Always reset to NL after EX release. Waiter bits indicate
		 * demand but do not hold the lock — leaving mode=EX with no
		 * holder would cause all waiters to livelock: each retry sees
		 * mode=EX (no holder) and treats it as conflicted.
		 */
		dl.le_mode = cpu_to_le16(OCSFS_LOCK_NL);
	} else if (lr->lr_mode == OCSFS_LOCK_SH ||
		   lr->lr_mode == OCSFS_LOCK_CW) {
		remove_sh_holder(&dl, sbi->s_node_slot);
		if (!has_sh_holders(&dl))
			dl.le_mode = cpu_to_le16(OCSFS_LOCK_NL);
	}

	ret = lr_write_entry(sb, lr, &dl, bh);
	brelse(bh);

	if (ret == -EAGAIN && ++retries < OCSFS_LOCK_MAX_RETRIES)
		goto retry_release;

	if (ret) {
		pr_warn_ratelimited("ocsfs: lock_release failed for resource "
				    "0x%llx (%d) — lock may be stranded on disk\n",
				    lr->lr_resource_id, ret);
		/* Do NOT clear lr_mode on failure: the on-disk entry was not
		 * updated, so keeping lr_mode == EX lets the heartbeat timeout
		 * trigger proper recovery instead of leaving an orphaned lock. */
	} else {
		lr->lr_mode = OCSFS_LOCK_NL;
		lr->lr_lock_epoch = 0;  /* invalidate cache; next acquire goes to disk */
	}
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

	ret = lr_read_entry(sb, lr, &dl, &bh);
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
	} else {
		/* Unsupported mode combination — indicates a caller bug */
		WARN_ON(1);
		brelse(bh);
		mutex_unlock(&lr->lr_mutex);
		return -EINVAL;
	}

	ret = lr_write_entry(sb, lr, &dl, bh);
	brelse(bh);

	if (ret == 0) {
		lr->lr_mode = new_mode;
		/* Invalidate cache: after EX→SH downgrade a peer can take EX
		 * and modify data; the next SH acquire must go to disk. */
		lr->lr_lock_epoch = 0;
	}

	mutex_unlock(&lr->lr_mutex);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * LOCK RECOVERY — release all locks held by a failed node
 * ═══════════════════════════════════════════════════════════════ */

/* Apply recovery to one lock entry.  Clears EX/SH/waiter state for
 * node_slot/mount_gen.  Returns true if the entry was modified. */
static bool ocsfs_lock_recover_entry(struct ocsfs_disk_lock *dl,
				     u16 node_slot, u32 mount_gen,
				     int *recovered)
{
	bool modified = false;
	u32 wbyte, mbyte, mshift;
	u8  wbit;

	if (le16_to_cpu(dl->le_mode) == OCSFS_LOCK_EX &&
	    le16_to_cpu(dl->le_holder_slot) == node_slot &&
	    /* le_holder_gen is 32-bit: wraps after 2^32 mounts — accepted
	     * risk; upgrading to 64-bit requires an on-disk format change. */
	    le32_to_cpu(dl->le_holder_gen) == mount_gen) {
		dl->le_holder_slot = 0;
		dl->le_holder_gen  = 0;
		if (!has_sh_holders(dl))
			dl->le_mode = cpu_to_le16(OCSFS_LOCK_NL);
		modified = true;
		(*recovered)++;
	}

	if (is_sh_holder(dl, node_slot)) {
		remove_sh_holder(dl, node_slot);
		if (!has_sh_holders(dl) &&
		    le16_to_cpu(dl->le_mode) != OCSFS_LOCK_EX)
			dl->le_mode = cpu_to_le16(OCSFS_LOCK_NL);
		modified = true;
		(*recovered)++;
	}

	wbyte = node_slot / 8;
	wbit  = 1u << (node_slot % 8);
	if (!modified && wbyte < sizeof(dl->le_waiters) &&
	    (dl->le_waiters[wbyte] & wbit))
		modified = true;
	clear_waiter_bit(dl, node_slot);

	/* 2-bit mode entry: 2 bits per slot, packed LSB-first in each byte. */
	mbyte  = (node_slot * 2) / 8;
	mshift = (node_slot * 2) % 8;
	if (mbyte < sizeof(dl->le_waiter_modes) &&
	    (dl->le_waiter_modes[mbyte] & (3u << mshift))) {
		dl->le_waiter_modes[mbyte] &= ~(3u << mshift);
		modified = true;
	}

	return modified;
}

int ocsfs_lock_recover_node(struct super_block *sb, u16 node_slot,
			    u32 mount_gen)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_lock dl;
	struct buffer_head *bh;
	u32 i;
	int recovered = 0;
	int ret;

	pr_info("ocsfs: recovering locks for node slot %u (gen=%u)\n",
		node_slot, mount_gen);

	for (i = 0; i < ocsfs_lock_primary_count(sbi); i++) {
		u64 chain_block;

		ret = lock_read_entry(sb, i, &dl, &bh);
		if (ret)
			continue;

		if (le32_to_cpu(dl.le_magic) != OCSFS_LOCK_MAGIC) {
			brelse(bh);
			continue;
		}

		chain_block = le64_to_cpu(dl.le_overflow_block);

		if (ocsfs_lock_recover_entry(&dl, node_slot, mount_gen,
					     &recovered)) {
			ret = lock_write_entry(sb, i, &dl, bh);
			if (ret)
				pr_warn("ocsfs: lock recovery write failed for "
					"entry %u (%d)\n", i, ret);
		}

		brelse(bh);

		/* Follow overflow chain for this primary slot (MEDIO-V3-3).
		 * Each overflow block stores up to (block_size / entry_size)
		 * entries; slot-0 of each block holds the next chain link in
		 * le_overflow_block. */
		while (chain_block) {
			u64 addr = chain_block * sbi->s_block_size;
			u32 entries_per_block =
				sbi->s_block_size / OCSFS_LOCK_ENTRY_SIZE;
			u64 next_chain_block = 0;
			u32 j;

			for (j = 0; j < entries_per_block; j++) {
				u64 entry_addr =
					addr + (u64)j * OCSFS_LOCK_ENTRY_SIZE;
				struct ocsfs_disk_lock odl;
				struct buffer_head *obh;

				ret = lock_read_entry_at_addr(sb, entry_addr,
							      &odl, &obh);
				if (ret)
					break;

				if (j == 0)
					next_chain_block =
						le64_to_cpu(odl.le_overflow_block);

				if (le32_to_cpu(odl.le_magic) == OCSFS_LOCK_MAGIC &&
				    ocsfs_lock_recover_entry(&odl, node_slot,
							     mount_gen,
							     &recovered)) {
					ret = lock_write_entry_at_addr(
						sb, entry_addr, &odl, obh);
					if (ret)
						pr_warn("ocsfs: lock recovery "
							"write failed for "
							"overflow addr %llu "
							"(%d)\n",
							(unsigned long long)
							entry_addr, ret);
				}

				brelse(obh);
			}

			chain_block = next_chain_block;
		}
	}

	/*
	 * Bump s_lock_epoch to invalidate all cached lock entries across
	 * every lock_res on this node. Next acquire will hit the slow path
	 * and re-validate from disk.
	 */
	atomic_inc(&sbi->s_lock_epoch);

	pr_info("ocsfs: recovered %d locks from node slot %u\n",
		recovered, node_slot);
	return 0;
}

/*
 * ocsfs_lock_renew_lease — extend the EX lease of the current holder.
 * Called by long-running write operations to avoid lease expiry while
 * still holding the lock (ARCH-V3-5).
 */
int ocsfs_lock_renew_lease(struct super_block *sb, struct ocsfs_lock_res *lr)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_lock dl;
	struct buffer_head *bh;
	int ret;

	if (!sbi->s_clustered || lr->lr_mode != OCSFS_LOCK_EX)
		return 0;
	mutex_lock(&lr->lr_mutex);
	ret = lr_read_entry(sb, lr, &dl, &bh);
	if (ret == 0 &&
	    le16_to_cpu(dl.le_holder_slot) == sbi->s_node_slot) {
		dl.le_lease_ns   = cpu_to_le64(ktime_get_real_ns() +
					       OCSFS_LOCK_LEASE_NS);
		dl.le_lease_slot = cpu_to_le16(sbi->s_node_slot);
		ret = lr_write_entry(sb, lr, &dl, bh);
	}
	brelse(bh);
	mutex_unlock(&lr->lr_mutex);
	return ret;
}
