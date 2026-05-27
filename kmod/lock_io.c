// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — lock_io.c
 * Lock table I/O, compatibility matrix, bitmask helpers, and resource allocation.
 * acquire / release / downgrade / recover are in lock.c.
 */

#include "ocsfs.h"
#include "lock_internal.h"

/* ═══════════════════════════════════════════════════════════════
 * LOCK TABLE I/O
 * ═══════════════════════════════════════════════════════════════ */

int lock_read_entry(struct super_block *sb, u32 slot,
		    struct ocsfs_disk_lock *out, struct buffer_head **bh_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 off   = OCSFS_LOCK_TABLE_OFF + (u64)slot * OCSFS_LOCK_ENTRY_SIZE;
	u64 block = off / sbi->s_block_size;
	u32 boff  = off % sbi->s_block_size;
	struct buffer_head *bh;

	/*
	 * Force a fresh read from the block device — the page cache can hold
	 * stale data written by a remote cluster node.  This matches the
	 * pattern used by the software CAS path in lock_write_entry().
	 */
	bh = sb_getblk(sb, block);
	if (!bh)
		return -EIO;
	clear_buffer_uptodate(bh);
	if (bh_read(bh, 0) < 0) {
		brelse(bh);
		return -EIO;
	}

	memcpy(out, bh->b_data + boff, sizeof(*out));

	if (bh_out)
		*bh_out = bh;
	else
		brelse(bh);

	return 0;
}

/*
 * Write a lock entry atomically via CAW (when supported) or software fallback.
 * CAW: SCSI opcode 0x89 — MISCOMPARE returns -EAGAIN; caller retries.
 * Fallback: re-reads from disk + version check before write.
 */
int lock_write_entry(struct super_block *sb, u32 slot,
		     struct ocsfs_disk_lock *entry, struct buffer_head *bh)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 off   = OCSFS_LOCK_TABLE_OFF + (u64)slot * OCSFS_LOCK_ENTRY_SIZE;
	u64 block = off / sbi->s_block_size;
	u32 boff  = off % sbi->s_block_size;
	u8 expected_buf[sizeof(struct ocsfs_disk_lock)];
	u8 new_buf[sizeof(struct ocsfs_disk_lock)];
	struct ocsfs_disk_lock *new_entry = (struct ocsfs_disk_lock *)new_buf;
	u32 expected_version;

	/*
	 * expected_buf must capture the BEFORE state (what is currently on
	 * disk), not the after state.  All callers mutate their local 'dl'
	 * before calling here, so 'entry' already holds the desired new value.
	 * bh->b_data + boff still contains the pre-mutation disk content
	 * because lock_read_entry copies into the caller's local variable and
	 * returns the buffer_head unmodified.
	 *
	 * The SCSI CAW path below also uses bh->b_data for the expected LBS;
	 * align the software CAS path to the same semantics.
	 */
	memcpy(expected_buf, bh->b_data + boff, sizeof(*entry));
	expected_version = le32_to_cpu(
		((struct ocsfs_disk_lock *)expected_buf)->le_version);
	memcpy(new_buf, entry, sizeof(*entry));
	new_entry->le_version  = cpu_to_le32(expected_version + 1);
	new_entry->le_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, new_entry,
			     OCSFS_LOCK_ENTRY_SIZE - sizeof(__le32)));

	/* SCSI CAW fast-path (hardware atomicità, opzionale) */
	if (sbi->s_caw_supported) {
		unsigned int lbs = bdev_logical_block_size(sb->s_bdev);

		if (lbs > 0 && lbs <= sbi->s_block_size && is_power_of_2(lbs)) {
			u32 lbs_start = boff & ~(lbs - 1u);
			u8 *exp_lbs   = kmalloc(lbs, GFP_KERNEL);
			u8 *new_lbs   = kmalloc(lbs, GFP_KERNEL);
			int ret       = -ENOMEM;

			if (exp_lbs && new_lbs) {
				u64 scsi_lba = off / lbs;

				memcpy(exp_lbs, bh->b_data + lbs_start, lbs);
				memcpy(new_lbs, exp_lbs, lbs);
				memcpy(new_lbs + (boff & (lbs - 1u)),
				       new_buf, sizeof(*entry));

				ret = ocsfs_scsi_caw(sb, scsi_lba,
						     exp_lbs, new_lbs, lbs);
				if (ret == -EAGAIN)
					clear_buffer_uptodate(bh);
			}
			kfree(exp_lbs);
			kfree(new_lbs);

			if (ret != -EOPNOTSUPP)
				return ret;
		}
	}

	/*
	 * CAS atomica cross-node via PR-lease (CRIT-1 fix).
	 * Sostituisce il vecchio software fallback read→version-check→write
	 * che aveva una race window TOCTOU tra i due nodi.
	 * Single-node: ocsfs_atomic_cas usa write diretto (nessun lease).
	 */
	return ocsfs_atomic_cas(sb, block, boff,
				sizeof(struct ocsfs_disk_lock),
				expected_buf, new_buf);
}

/*
 * Find / claim the on-disk slot for a resource using open-addressing linear probe.
 * Returns 0 with lr->lr_slot set, or -ENOSPC if all probe slots are occupied.
 * Called with lr->lr_mutex held.
 */
int lock_probe_slot(struct super_block *sb, struct ocsfs_lock_res *lr)
{
	u32 base = ocsfs_lock_table_slot(lr->lr_resource_id);
	int i;

	for (i = 0; i < OCSFS_LOCK_PROBE_MAX; i++) {
		u32 slot = (base + i) % OCSFS_LOCK_ENTRY_COUNT;
		struct ocsfs_disk_lock dl;
		struct buffer_head *bh;
		int ret;

		ret = lock_read_entry(sb, slot, &dl, &bh);
		if (ret)
			return ret;
		brelse(bh);

		if (le32_to_cpu(dl.le_magic) != OCSFS_LOCK_MAGIC) {
			lr->lr_slot = slot;
			return 0;
		}
		if (le64_to_cpu(dl.le_resource_id) == lr->lr_resource_id) {
			lr->lr_slot = slot;
			return 0;
		}
	}

	pr_warn("ocsfs: lock table full (probe_max=%d), resource 0x%llx\n",
		OCSFS_LOCK_PROBE_MAX, lr->lr_resource_id);
	return -ENOSPC;
}

/* ═══════════════════════════════════════════════════════════════
 * LOCK COMPATIBILITY AND BITMASK HELPERS
 * ═══════════════════════════════════════════════════════════════ */

bool lock_modes_compatible(u16 held, u16 requested)
{
	if (held == OCSFS_LOCK_NL || requested == OCSFS_LOCK_NL)
		return true;
	if (held == OCSFS_LOCK_SH && requested == OCSFS_LOCK_SH)
		return true;
	if (held == OCSFS_LOCK_CW && requested == OCSFS_LOCK_CW)
		return true;
	return false;
}

void set_waiter_bit(struct ocsfs_disk_lock *dl, u16 slot)
{
	u32 byte = slot / 8, bit = slot % 8;

	if (byte < sizeof(dl->le_waiters))
		dl->le_waiters[byte] |= (1 << bit);
}

void clear_waiter_bit(struct ocsfs_disk_lock *dl, u16 slot)
{
	u32 byte = slot / 8, bit = slot % 8;

	if (byte < sizeof(dl->le_waiters))
		dl->le_waiters[byte] &= ~(1 << bit);
}

bool is_sh_holder(struct ocsfs_disk_lock *dl, u16 slot)
{
	if (slot < 32)
		return !!(le32_to_cpu(dl->le_sh_holders) & (1U << slot));
	if (slot - 32 < sizeof(dl->le_sh_holders_ext) * 8)
		return !!(dl->le_sh_holders_ext[(slot - 32) / 8] &
			  (1 << ((slot - 32) % 8)));
	return false;
}

void add_sh_holder(struct ocsfs_disk_lock *dl, u16 slot)
{
	if (slot < 32) {
		u32 mask = le32_to_cpu(dl->le_sh_holders);

		mask |= (1U << slot);
		dl->le_sh_holders = cpu_to_le32(mask);
	} else if (slot - 32 < sizeof(dl->le_sh_holders_ext) * 8) {
		dl->le_sh_holders_ext[(slot - 32) / 8] |=
			(1 << ((slot - 32) % 8));
	}
}

void remove_sh_holder(struct ocsfs_disk_lock *dl, u16 slot)
{
	if (slot < 32) {
		u32 mask = le32_to_cpu(dl->le_sh_holders);

		mask &= ~(1U << slot);
		dl->le_sh_holders = cpu_to_le32(mask);
	} else if (slot - 32 < sizeof(dl->le_sh_holders_ext) * 8) {
		dl->le_sh_holders_ext[(slot - 32) / 8] &=
			~(1 << ((slot - 32) % 8));
	}
}

bool has_sh_holders(struct ocsfs_disk_lock *dl)
{
	int i;

	if (le32_to_cpu(dl->le_sh_holders) != 0)
		return true;
	for (i = 0; i < (int)sizeof(dl->le_sh_holders_ext); i++) {
		if (dl->le_sh_holders_ext[i] != 0)
			return true;
	}
	return false;
}

bool has_waiters(struct ocsfs_disk_lock *dl)
{
	int i;

	for (i = 0; i < (int)sizeof(dl->le_waiters); i++) {
		if (dl->le_waiters[i] != 0)
			return true;
	}
	return false;
}

/* ═══════════════════════════════════════════════════════════════
 * LOCK RESOURCE ALLOCATION
 * ═══════════════════════════════════════════════════════════════ */

void ocsfs_lock_init(struct ocsfs_lock_res *lr, u64 resource_id,
		     u32 resource_type)
{
	memset(lr, 0, sizeof(*lr));
	lr->lr_resource_id   = resource_id;
	lr->lr_resource_type = resource_type;
	lr->lr_mode          = OCSFS_LOCK_NL;
	lr->lr_slot          = ocsfs_lock_table_slot(resource_id);
	mutex_init(&lr->lr_mutex);
	INIT_LIST_HEAD(&lr->lr_list);
}

int ocsfs_dlm_init(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	INIT_LIST_HEAD(&sbi->s_lock_list);
	spin_lock_init(&sbi->s_lock_list_lock);
	atomic_set(&sbi->s_lock_epoch, 0);
	return 0;
}

void ocsfs_dlm_exit(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_lock_res *lr, *tmp;
	u32 i;

	/* Release per-AG static locks (not in s_lock_list, lr_dynamic=false). */
	if (sbi->s_ags) {
		for (i = 0; i < sbi->s_ag_count; i++) {
			if (sbi->s_ags[i].ag_lock_res.lr_mode != OCSFS_LOCK_NL)
				ocsfs_lock_release(sb, &sbi->s_ags[i].ag_lock_res);
			if (sbi->s_ags[i].ag_rc_lock_res.lr_mode != OCSFS_LOCK_NL)
				ocsfs_lock_release(sb, &sbi->s_ags[i].ag_rc_lock_res);
		}
	}

	spin_lock(&sbi->s_lock_list_lock);
	list_for_each_entry_safe(lr, tmp, &sbi->s_lock_list, lr_list) {
		if (lr->lr_mode != OCSFS_LOCK_NL) {
			spin_unlock(&sbi->s_lock_list_lock);
			ocsfs_lock_release(sb, lr);
			spin_lock(&sbi->s_lock_list_lock);
		}
		list_del(&lr->lr_list);
		if (lr->lr_dynamic)
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

	lr->lr_resource_id   = resource_id;
	lr->lr_resource_type = resource_type;
	lr->lr_mode          = OCSFS_LOCK_NL;
	lr->lr_slot          = ocsfs_lock_table_slot(resource_id);
	lr->lr_dynamic       = true;
	mutex_init(&lr->lr_mutex);

	spin_lock(&sbi->s_lock_list_lock);
	list_add(&lr->lr_list, &sbi->s_lock_list);
	spin_unlock(&sbi->s_lock_list_lock);

	return lr;
}

void ocsfs_lock_free(struct ocsfs_lock_res *lr)
{
	kfree(lr);
}
