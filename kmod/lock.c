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

/*
 * @was_fresh (optional): set true when this call performed a real on-disk
 * (cross-node) acquire, false on a local cache hit / non-clustered.  The
 * allocators use it to know when to invalidate the AG's cached bitmap / inode
 * table (a peer may have changed it) WITHOUT clobbering a concurrent same-node
 * allocation's uncommitted marks (those come through as cache hits).
 */
static int lock_acquire_impl(struct super_block *sb, struct ocsfs_lock_res *lr,
			     u16 mode, bool *was_fresh)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_lock dl;
	struct buffer_head *bh;
	int ret;
	u32 delay_us = OCSFS_LOCK_RETRY_MIN_US;
	ktime_t deadline = ktime_add_ms(ktime_get(),
					OCSFS_LOCK_ACQUIRE_TIMEOUT_MS);

	if (was_fresh)
		*was_fresh = false;

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
	 * Writer priority (fixes EX starvation under a continuous reader
	 * stream).  A shared acquire defers while a local EX waiter is pending
	 * so the existing SH holders' lr_hold can drain to 0 — clearing the
	 * on-disk SH holder bit — and the writer is granted instead of being
	 * starved by back-to-back readers.
	 *
	 * This MUST run before the cache fast-path: under continuous reads
	 * lr_mode stays SH, so every fresh reader would otherwise hit the
	 * cache and bump lr_hold without ever deferring, and lr_hold would
	 * never reach 0.  No i_lock_res / ag_rc_lock_res / keystore path
	 * acquires SH recursively or SH-under-EX on the same resource, so
	 * blocking a fresh shared acquirer here cannot deadlock a thread that
	 * already holds this lock.
	 *
	 * Bounded by the acquire deadline: the writer decrements lr_ex_wait
	 * and wakes us when it is granted or times out; past our own deadline
	 * we fall through and try the disk so the reader is not starved either.
	 */
	if (mode == OCSFS_LOCK_SH || mode == OCSFS_LOCK_CW) {
		while (lr->lr_ex_wait > 0 &&
		       !ktime_after(ktime_get(), deadline)) {
			mutex_unlock(&lr->lr_mutex);
			wait_event_timeout(lr->lr_wq, lr->lr_ex_wait == 0,
					   msecs_to_jiffies(1000));
			mutex_lock(&lr->lr_mutex);
		}
	}

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
			lr->lr_hold++;   /* count this nested/compatible hold */
			lr->lr_lazy = false;  /* actively held again */
			mutex_unlock(&lr->lr_mutex);
			return 0;
		}
	}

	ret = lock_probe_slot(sb, lr);
	if (ret) {
		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

	/* Register as a pending writer so concurrent shared acquirers defer
	 * (writer priority).  Decremented + waiters woken on every exit path
	 * below: read error, grant, and deadline timeout. */
	if (mode == OCSFS_LOCK_EX)
		lr->lr_ex_wait++;

retry:
	ret = lr_read_entry(sb, lr, &dl, &bh);
	if (ret) {
		if (mode == OCSFS_LOCK_EX && --lr->lr_ex_wait == 0)
			wake_up_all(&lr->lr_wq);
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

	/*
	 * Self-held re-adoption.  The on-disk lock is still recorded as held EX by
	 * THIS node (our slot+gen) but our in-memory cache missed — typically
	 * because a peer's recovery bumped s_lock_epoch (ocsfs_lock_recover_node),
	 * invalidating lr_lock_epoch while we genuinely still hold the grant.
	 * Without this, the compatibility test below treats our OWN exclusive grant
	 * as a conflict and we block on ourselves until the 30s acquire deadline —
	 * an observed self-deadlock under heavy churn that then strands every AG /
	 * block lock we hold and starves the other nodes (cascading 30s timeouts).
	 * Re-adopt the grant (it is ours; a real revoke would have rewritten
	 * le_holder_slot) and refresh the cache instead of conflicting.  Only valid
	 * for a request that our held EX already covers (mode <= EX).
	 */
	if (cur_mode == OCSFS_LOCK_EX && mode <= OCSFS_LOCK_EX &&
	    le16_to_cpu(dl.le_holder_slot) == sbi->s_node_slot &&
	    le32_to_cpu(dl.le_holder_gen) == sbi->s_mount_gen) {
		brelse(bh);
		lr->lr_mode = OCSFS_LOCK_EX;
		lr->lr_hold++;
		lr->lr_lock_epoch = (u32)atomic_read(&sbi->s_lock_epoch);
		if (mode == OCSFS_LOCK_EX && --lr->lr_ex_wait == 0)
			wake_up_all(&lr->lr_wq);
		mutex_unlock(&lr->lr_mutex);
		return 0;
	}

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
			lr->lr_hold++;   /* count this hold (first acquire or upgrade) */
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

		if (mode == OCSFS_LOCK_EX && --lr->lr_ex_wait == 0)
			wake_up_all(&lr->lr_wq);
		if (was_fresh && ret == 0)
			*was_fresh = true;   /* real cross-node disk acquire */
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
		if (mode == OCSFS_LOCK_EX && --lr->lr_ex_wait == 0)
			wake_up_all(&lr->lr_wq);
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
	/*
	 * Drop lr_mutex while we back off.  Critical for a same-node EX
	 * waiter: otherwise readers calling ocsfs_lock_release() would block
	 * on lr_mutex, lr_hold could never reach 0, the on-disk SH holder bit
	 * would never clear, and this EX acquire would deadlock against its
	 * own node's readers until the 30s deadline.  lr_ex_wait stays
	 * incremented across the retry so new shared acquirers keep deferring.
	 */
	mutex_unlock(&lr->lr_mutex);
	usleep_range(delay_us, delay_us * 2);
	delay_us = min_t(u32, delay_us * 2, OCSFS_LOCK_RETRY_MAX_US);
	mutex_lock(&lr->lr_mutex);
	goto retry;
}

int ocsfs_lock_acquire(struct super_block *sb, struct ocsfs_lock_res *lr,
		       u16 mode)
{
	return lock_acquire_impl(sb, lr, mode, NULL);
}

/* As ocsfs_lock_acquire, but reports whether a real cross-node disk acquire
 * happened (*was_fresh) — see lock_acquire_impl. */
int ocsfs_lock_acquire_fresh(struct super_block *sb, struct ocsfs_lock_res *lr,
			     u16 mode, bool *was_fresh)
{
	return lock_acquire_impl(sb, lr, mode, was_fresh);
}

/* ═══════════════════════════════════════════════════════════════
 * LOCK RELEASE
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Perform the real on-disk release.  Caller holds lr->lr_mutex (NOT released
 * here) and has already confirmed this is the last local holder (lr_hold <= 1).
 * Shared by ocsfs_lock_release() and the lazy-revoke sweep.
 */
static int lock_release_ondisk_locked(struct super_block *sb,
				      struct ocsfs_lock_res *lr)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_lock dl;
	struct buffer_head *bh;
	int ret;
	int retries = 0;

retry_release:
	ret = lr_read_entry(sb, lr, &dl, &bh);
	if (ret)
		return ret;

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
		lr->lr_hold = 0;        /* last holder released */
		lr->lr_lazy = false;    /* really released now */
	}
	return ret;
}

int ocsfs_lock_release(struct super_block *sb, struct ocsfs_lock_res *lr)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	int ret;

	if (!sbi->s_clustered) {
		lr->lr_mode = OCSFS_LOCK_NL;
		return 0;
	}

	mutex_lock(&lr->lr_mutex);

	/* Reference-counted: while any other local holder remains, keep the
	 * on-disk lock and lr_mode untouched.  This is what stops a lockless
	 * read's SH acquire+release from releasing the EX a concurrent write/
	 * dedup/reflink/truncate holds (which would clobber lr_mode and, on a
	 * peer cluster, hand the lock to another node mid-update). */
	if (lr->lr_hold > 1) {
		lr->lr_hold--;
		mutex_unlock(&lr->lr_mutex);
		return 0;
	}

	ret = lock_release_ondisk_locked(sb, lr);
	mutex_unlock(&lr->lr_mutex);
	return ret;
}

/*
 * Lazy release (PERF): keep the on-disk lock held when this is the last local
 * holder, so the next acquire by this node is a cache hit with no disk
 * round-trip.  Used by the write_iter inode-EX path — the sustained VM-disk
 * write workload re-takes the same inode EX thousands of times.  A peer that
 * starts waiting is served by the lazy-revoke sweep within one interval;
 * evict / unmount / crash-recovery all really-release, so the lock is never
 * stranded.
 */
int ocsfs_lock_release_lazy(struct super_block *sb, struct ocsfs_lock_res *lr)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	if (!sbi->s_clustered) {
		lr->lr_mode = OCSFS_LOCK_NL;
		return 0;
	}

	mutex_lock(&lr->lr_mutex);
	if (lr->lr_hold > 1) {
		lr->lr_hold--;
		mutex_unlock(&lr->lr_mutex);
		return 0;
	}
	lr->lr_hold = 0;
	lr->lr_lazy = true;
	mutex_unlock(&lr->lr_mutex);
	return 0;
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

/* ═══════════════════════════════════════════════════════════════
 * LAZY-LOCK REVOCATION SWEEP (PERF)
 *
 * write_iter releases the inode EX lazily (ocsfs_lock_release_lazy): the
 * on-disk lock stays held so the next write is a cache hit with no disk
 * round-trip.  This sweep is what lets a *peer* eventually get the lock: every
 * interval it scans in-core regular-file inodes whose lock is lazily held and,
 * if a peer has set a waiter bit on disk, performs the real release.  Bounds a
 * peer's wait (e.g. VM live-migration) to one sweep interval.  A crashed
 * holder's lazy locks are reclaimed by the normal recovery lock-cleanup; evict
 * and unmount really-release via ocsfs_lock_release.
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_LAZY_REVOKE_INTERVAL_JIFFIES  msecs_to_jiffies(2000)
#define OCSFS_LAZY_REVOKE_BATCH             64

static void ocsfs_lazy_revoke_fn(struct work_struct *work)
{
	struct ocsfs_sb_info *sbi = container_of(to_delayed_work(work),
						 struct ocsfs_sb_info,
						 s_lazy_revoke_work);
	struct super_block *sb = sbi->s_sb;
	struct inode *batch[OCSFS_LAZY_REVOKE_BATCH];
	struct inode *inode;
	int n = 0, i;

	if (!sbi->s_clustered || !(sb->s_flags & SB_ACTIVE))
		goto reschedule;

	/* Collect a batch of inodes holding their lock lazily (racy peek;
	 * re-checked under lr_mutex below). */
	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		if (n >= OCSFS_LAZY_REVOKE_BATCH)
			break;
		if (!S_ISREG(inode->i_mode))
			continue;
		if (!OCSFS_I(inode)->i_lock_res.lr_lazy)
			continue;
		if (igrab(inode))
			batch[n++] = inode;
	}
	spin_unlock(&sb->s_inode_list_lock);

	for (i = 0; i < n; i++) {
		struct ocsfs_lock_res *lr = &OCSFS_I(batch[i])->i_lock_res;
		struct ocsfs_disk_lock dl;
		struct buffer_head *bh;

		mutex_lock(&lr->lr_mutex);
		/* Still lazily held with no active local holder, and a peer is
		 * waiting on disk?  Then really release — under lr_mutex so no
		 * local acquire can race the handoff. */
		if (lr->lr_lazy && lr->lr_hold == 0 &&
		    lr_read_entry(sb, lr, &dl, &bh) == 0) {
			bool waited = has_waiters(&dl);

			brelse(bh);
			if (waited)
				lock_release_ondisk_locked(sb, lr);
		}
		mutex_unlock(&lr->lr_mutex);
		iput(batch[i]);
	}

reschedule:
	if (sbi->s_clustered)
		queue_delayed_work(system_wq, &sbi->s_lazy_revoke_work,
				   OCSFS_LAZY_REVOKE_INTERVAL_JIFFIES);
}

void ocsfs_lazy_revoke_start(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	INIT_DELAYED_WORK(&sbi->s_lazy_revoke_work, ocsfs_lazy_revoke_fn);
	if (sbi->s_clustered)
		queue_delayed_work(system_wq, &sbi->s_lazy_revoke_work,
				   OCSFS_LAZY_REVOKE_INTERVAL_JIFFIES);
}

void ocsfs_lazy_revoke_stop(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	cancel_delayed_work_sync(&sbi->s_lazy_revoke_work);
}
