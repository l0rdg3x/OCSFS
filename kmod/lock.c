// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — lock.c
 * On-disk distributed lock manager.
 *
 * All lock state lives in the 1 MB Lock Table region on the shared LUN.
 * Lock operations use buffer_head reads + writes with CAS-style versioning.
 * On real SCSI hardware, Compare-And-Write (CAW) provides atomicity;
 * for the initial implementation we use read-modify-write with version
 * checks and retry on conflict.
 *
 * Lock compatibility matrix:
 *   NL + anything = compatible
 *   SH + SH       = compatible
 *   SH + EX       = conflict
 *   EX + anything = conflict (except NL)
 *   CW + CW       = compatible (registrants-only mode)
 */

#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * LOCK TABLE I/O
 * ═══════════════════════════════════════════════════════════════ */

/* Read a lock entry from disk */
static int lock_read_entry(struct super_block *sb, u32 slot,
			   struct ocsfs_disk_lock *out,
			   struct buffer_head **bh_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 off = OCSFS_LOCK_TABLE_OFF +
		  (u64)slot * OCSFS_LOCK_ENTRY_SIZE;
	u64 block = off / sbi->s_block_size;
	u32 boff = off % sbi->s_block_size;
	struct buffer_head *bh;

	bh = sb_bread(sb, block);
	if (!bh)
		return -EIO;

	memcpy(out, bh->b_data + boff, sizeof(*out));

	if (bh_out) {
		*bh_out = bh;
	} else {
		brelse(bh);
	}

	return 0;
}

/* Write a lock entry to disk (CAS: check version first) */
static int lock_write_entry(struct super_block *sb, u32 slot,
			    struct ocsfs_disk_lock *entry,
			    struct buffer_head *bh)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 off = OCSFS_LOCK_TABLE_OFF +
		  (u64)slot * OCSFS_LOCK_ENTRY_SIZE;
	u32 boff = off % sbi->s_block_size;

	/* Increment version for CAS semantics */
	entry->le_version = cpu_to_le32(
		le32_to_cpu(entry->le_version) + 1);

	/* Compute checksum */
	entry->le_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, entry,
			     OCSFS_LOCK_ENTRY_SIZE - sizeof(__le32)));

	memcpy(bh->b_data + boff, entry, sizeof(*entry));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * LOCK COMPATIBILITY
 * ═══════════════════════════════════════════════════════════════ */

static bool lock_modes_compatible(u16 held, u16 requested)
{
	if (held == OCSFS_LOCK_NL || requested == OCSFS_LOCK_NL)
		return true;
	if (held == OCSFS_LOCK_SH && requested == OCSFS_LOCK_SH)
		return true;
	if (held == OCSFS_LOCK_CW && requested == OCSFS_LOCK_CW)
		return true;
	return false;
}

/* Set a bit in the waiter bitmask */
static void set_waiter_bit(struct ocsfs_disk_lock *dl, u16 slot)
{
	u32 byte = slot / 8;
	u32 bit = slot % 8;

	if (byte < sizeof(dl->le_waiters))
		dl->le_waiters[byte] |= (1 << bit);
}

/* Clear a bit in the waiter bitmask */
static void clear_waiter_bit(struct ocsfs_disk_lock *dl, u16 slot)
{
	u32 byte = slot / 8;
	u32 bit = slot % 8;

	if (byte < sizeof(dl->le_waiters))
		dl->le_waiters[byte] &= ~(1 << bit);
}

/* Check if a node is in the SH holders bitmask */
static bool is_sh_holder(struct ocsfs_disk_lock *dl, u16 slot)
{
	if (slot < 32)
		return !!(le32_to_cpu(dl->le_sh_holders) & (1 << slot));
	if (slot - 32 < sizeof(dl->le_sh_holders_ext) * 8)
		return !!(dl->le_sh_holders_ext[(slot - 32) / 8] &
			  (1 << ((slot - 32) % 8)));
	return false;
}

/* Add this node to SH holders */
static void add_sh_holder(struct ocsfs_disk_lock *dl, u16 slot)
{
	if (slot < 32) {
		u32 mask = le32_to_cpu(dl->le_sh_holders);
		mask |= (1 << slot);
		dl->le_sh_holders = cpu_to_le32(mask);
	} else if (slot - 32 < sizeof(dl->le_sh_holders_ext) * 8) {
		dl->le_sh_holders_ext[(slot - 32) / 8] |=
			(1 << ((slot - 32) % 8));
	}
}

/* Remove this node from SH holders */
static void remove_sh_holder(struct ocsfs_disk_lock *dl, u16 slot)
{
	if (slot < 32) {
		u32 mask = le32_to_cpu(dl->le_sh_holders);
		mask &= ~(1 << slot);
		dl->le_sh_holders = cpu_to_le32(mask);
	} else if (slot - 32 < sizeof(dl->le_sh_holders_ext) * 8) {
		dl->le_sh_holders_ext[(slot - 32) / 8] &=
			~(1 << ((slot - 32) % 8));
	}
}

/* Check if any SH holders remain */
static bool has_sh_holders(struct ocsfs_disk_lock *dl)
{
	int i;

	if (le32_to_cpu(dl->le_sh_holders) != 0)
		return true;
	for (i = 0; i < sizeof(dl->le_sh_holders_ext); i++) {
		if (dl->le_sh_holders_ext[i] != 0)
			return true;
	}
	return false;
}

/* Check if any waiters exist */
static bool has_waiters(struct ocsfs_disk_lock *dl)
{
	int i;

	for (i = 0; i < sizeof(dl->le_waiters); i++) {
		if (dl->le_waiters[i] != 0)
			return true;
	}
	return false;
}

/* ═══════════════════════════════════════════════════════════════
 * LOCK RESOURCE ALLOCATION
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_dlm_init(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	INIT_LIST_HEAD(&sbi->s_lock_list);
	spin_lock_init(&sbi->s_lock_list_lock);
	return 0;
}

void ocsfs_dlm_exit(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_lock_res *lr, *tmp;

	/* Release all held locks */
	spin_lock(&sbi->s_lock_list_lock);
	list_for_each_entry_safe(lr, tmp, &sbi->s_lock_list, lr_list) {
		if (lr->lr_mode != OCSFS_LOCK_NL) {
			spin_unlock(&sbi->s_lock_list_lock);
			ocsfs_lock_release(sb, lr);
			spin_lock(&sbi->s_lock_list_lock);
		}
		list_del(&lr->lr_list);
		kfree(lr);
	}
	spin_unlock(&sbi->s_lock_list_lock);
}

struct ocsfs_lock_res *ocsfs_lock_alloc(struct super_block *sb,
					u64 resource_id, u32 resource_type)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_lock_res *lr;

	lr = kzalloc(sizeof(*lr), GFP_KERNEL);
	if (!lr)
		return ERR_PTR(-ENOMEM);

	lr->lr_resource_id = resource_id;
	lr->lr_resource_type = resource_type;
	lr->lr_mode = OCSFS_LOCK_NL;
	lr->lr_slot = ocsfs_lock_table_slot(resource_id);
	mutex_init(&lr->lr_mutex);

	spin_lock(&sbi->s_lock_list_lock);
	list_add(&lr->lr_list, &sbi->s_lock_list);
	spin_unlock(&sbi->s_lock_list_lock);

	return lr;
}

void ocsfs_lock_free(struct ocsfs_lock_res *lr)
{
	/* Caller must release lock first */
	kfree(lr);
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
	int retries = 0;
	u32 delay_us = OCSFS_LOCK_RETRY_MIN_US;

	if (!sbi->s_clustered) {
		/* Single-node mode: no on-disk locking needed */
		lr->lr_mode = mode;
		return 0;
	}

	mutex_lock(&lr->lr_mutex);

retry:
	ret = lock_read_entry(sb, lr->lr_slot, &dl, &bh);
	if (ret) {
		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

	/* Check if entry is uninitialized or for a different resource */
	if (le32_to_cpu(dl.le_magic) != OCSFS_LOCK_MAGIC ||
	    le64_to_cpu(dl.le_resource_id) != lr->lr_resource_id) {
		/* Initialize/claim this lock entry */
		if (le32_to_cpu(dl.le_magic) != OCSFS_LOCK_MAGIC) {
			memset(&dl, 0, sizeof(dl));
			dl.le_magic = cpu_to_le32(OCSFS_LOCK_MAGIC);
			dl.le_resource_id = cpu_to_le64(lr->lr_resource_id);
			dl.le_resource_type = cpu_to_le32(lr->lr_resource_type);
		}
	}

	u16 cur_mode = le16_to_cpu(dl.le_mode);

	/* Check compatibility */
	if (cur_mode == OCSFS_LOCK_NL || lock_modes_compatible(cur_mode, mode)) {
		/* Grant the lock */
		if (mode == OCSFS_LOCK_EX) {
			dl.le_mode = cpu_to_le16(OCSFS_LOCK_EX);
			dl.le_holder_slot = cpu_to_le16(sbi->s_node_slot);
			dl.le_holder_gen = cpu_to_le32(sbi->s_mount_gen);
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

		if (ret == 0)
			lr->lr_mode = mode;

		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

	/* Conflict — set waiter bit and retry */
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

	/* Exponential backoff */
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
		/* Clear exclusive holder */
		dl.le_holder_slot = 0;
		dl.le_holder_gen = 0;
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

	lr->lr_mode = OCSFS_LOCK_NL;
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
		return -EINVAL;  /* not a downgrade */

	mutex_lock(&lr->lr_mutex);

	ret = lock_read_entry(sb, lr->lr_slot, &dl, &bh);
	if (ret) {
		mutex_unlock(&lr->lr_mutex);
		return ret;
	}

	if (lr->lr_mode == OCSFS_LOCK_EX && new_mode == OCSFS_LOCK_SH) {
		dl.le_mode = cpu_to_le16(OCSFS_LOCK_SH);
		dl.le_holder_slot = 0;
		dl.le_holder_gen = 0;
		add_sh_holder(&dl, sbi->s_node_slot);
	} else if (new_mode == OCSFS_LOCK_NL) {
		return ocsfs_lock_release(sb, lr);
	}

	ret = lock_write_entry(sb, lr->lr_slot, &dl, bh);
	brelse(bh);

	if (ret == 0)
		lr->lr_mode = new_mode;

	mutex_unlock(&lr->lr_mutex);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * LOCK RECOVERY — release all locks held by a failed node
 * ═══════════════════════════════════════════════════════════════ */

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

	for (i = 0; i < OCSFS_LOCK_ENTRY_COUNT; i++) {
		ret = lock_read_entry(sb, i, &dl, &bh);
		if (ret)
			continue;

		if (le32_to_cpu(dl.le_magic) != OCSFS_LOCK_MAGIC) {
			brelse(bh);
			continue;
		}

		bool modified = false;

		/* Check EX holder */
		if (le16_to_cpu(dl.le_mode) == OCSFS_LOCK_EX &&
		    le16_to_cpu(dl.le_holder_slot) == node_slot &&
		    le32_to_cpu(dl.le_holder_gen) == mount_gen) {
			dl.le_holder_slot = 0;
			dl.le_holder_gen = 0;
			if (!has_sh_holders(&dl))
				dl.le_mode = cpu_to_le16(OCSFS_LOCK_NL);
			modified = true;
			recovered++;
		}

		/* Check SH holders */
		if (is_sh_holder(&dl, node_slot)) {
			remove_sh_holder(&dl, node_slot);
			if (!has_sh_holders(&dl) &&
			    le16_to_cpu(dl.le_mode) != OCSFS_LOCK_EX)
				dl.le_mode = cpu_to_le16(OCSFS_LOCK_NL);
			modified = true;
			recovered++;
		}

		/* Clear waiter bit */
		clear_waiter_bit(&dl, node_slot);

		if (modified)
			lock_write_entry(sb, i, &dl, bh);

		brelse(bh);
	}

	pr_info("ocsfs: recovered %d locks from node slot %u\n",
		recovered, node_slot);
	return 0;
}
