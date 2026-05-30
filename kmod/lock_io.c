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
	u64 off   = ocsfs_lock_table_base(sbi) + (u64)slot * OCSFS_LOCK_ENTRY_SIZE;
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
	u64 off   = ocsfs_lock_table_base(sbi) + (u64)slot * OCSFS_LOCK_ENTRY_SIZE;
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

/* ARCH-2: read/write a lock entry at an absolute byte address on disk.
 * Used for overflow block entries that live outside the primary lock table. */
int lock_read_entry_at_addr(struct super_block *sb, u64 byte_addr,
			    struct ocsfs_disk_lock *out,
			    struct buffer_head **bh_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 block = byte_addr / sbi->s_block_size;
	u32 boff  = (u32)(byte_addr % sbi->s_block_size);
	struct buffer_head *bh;

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

int lock_write_entry_at_addr(struct super_block *sb, u64 byte_addr,
			     struct ocsfs_disk_lock *entry,
			     struct buffer_head *bh)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 block = byte_addr / sbi->s_block_size;
	u32 boff  = (u32)(byte_addr % sbi->s_block_size);
	u8 expected_buf[sizeof(struct ocsfs_disk_lock)];
	u8 new_buf[sizeof(struct ocsfs_disk_lock)];
	struct ocsfs_disk_lock *new_entry = (struct ocsfs_disk_lock *)new_buf;
	u32 expected_version;

	memcpy(expected_buf, bh->b_data + boff, sizeof(*entry));
	expected_version = le32_to_cpu(
		((struct ocsfs_disk_lock *)expected_buf)->le_version);
	memcpy(new_buf, entry, sizeof(*entry));
	new_entry->le_version  = cpu_to_le32(expected_version + 1);
	new_entry->le_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, new_entry,
			     OCSFS_LOCK_ENTRY_SIZE - sizeof(__le32)));

	return ocsfs_atomic_cas(sb, block, boff,
				sizeof(struct ocsfs_disk_lock),
				expected_buf, new_buf);
}

/*
 * Find / claim the on-disk slot for a resource using open-addressing probe.
 * ARCH-2: uses runtime primary count (from sbi); on exhaustion follows the
 * le_overflow_block chain from the base slot, allocating a new overflow block
 * if the chain is empty.
 * Returns 0 with lr->lr_slot / lr->lr_overflow_addr set, or -ENOSPC / error.
 * Called with lr->lr_mutex held.
 */
int lock_probe_slot(struct super_block *sb, struct ocsfs_lock_res *lr)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u32 count = ocsfs_lock_primary_count(sbi);
	u32 base  = ocsfs_lock_table_slot(lr->lr_resource_id, count);
	u32 last_slot = base; /* last primary slot in probe (chain anchor) */
	int i;

	/* ── Phase 1: quadratic probe through primary table ── */
	for (i = 0; i < OCSFS_LOCK_PROBE_MAX; i++) {
		u32 slot = (base + (u32)i * (u32)i) % count;
		struct ocsfs_disk_lock dl;
		struct buffer_head *bh;
		int ret;

		ret = lock_read_entry(sb, slot, &dl, &bh);
		if (ret)
			return ret;
		brelse(bh);

		last_slot = slot;

		if (le32_to_cpu(dl.le_magic) != OCSFS_LOCK_MAGIC) {
			lr->lr_slot           = slot;
			lr->lr_overflow_addr  = 0;
			return 0;
		}
		if (le64_to_cpu(dl.le_resource_id) == lr->lr_resource_id) {
			lr->lr_slot           = slot;
			lr->lr_overflow_addr  = 0;
			return 0;
		}
	}

	/* ── Phase 2: follow / build overflow chain from last_slot ── */
	{
		struct ocsfs_disk_lock anchor_dl;
		struct buffer_head *anchor_bh;
		u64 chain_block;
		int ret;

		ret = lock_read_entry(sb, last_slot, &anchor_dl, &anchor_bh);
		if (ret)
			return ret;

		chain_block = le64_to_cpu(anchor_dl.le_overflow_block);

		/* Walk the existing chain looking for a free or matching slot. */
		while (chain_block) {
			u64 addr = chain_block * sbi->s_block_size;
			/* Up to OCSFS_BLOCK_SIZE/OCSFS_LOCK_ENTRY_SIZE entries per block */
			u32 entries_per_block = sbi->s_block_size / OCSFS_LOCK_ENTRY_SIZE;
			u32 j;

			for (j = 0; j < entries_per_block; j++) {
				u64 entry_addr = addr + (u64)j * OCSFS_LOCK_ENTRY_SIZE;
				struct ocsfs_disk_lock dl;
				struct buffer_head *bh;

				ret = lock_read_entry_at_addr(sb, entry_addr, &dl, &bh);
				if (ret) {
					brelse(anchor_bh);
					return ret;
				}
				brelse(bh);

				if (le32_to_cpu(dl.le_magic) != OCSFS_LOCK_MAGIC) {
					brelse(anchor_bh);
					lr->lr_slot          = last_slot;
					lr->lr_overflow_addr = entry_addr;
					return 0;
				}
				if (le64_to_cpu(dl.le_resource_id) == lr->lr_resource_id) {
					brelse(anchor_bh);
					lr->lr_slot          = last_slot;
					lr->lr_overflow_addr = entry_addr;
					return 0;
				}
			}

			/* Block full — follow its overflow chain link (stored at slot 0). */
			{
				u64 hdr_addr = addr;
				struct ocsfs_disk_lock hdr_dl;
				struct buffer_head *hdr_bh;

				ret = lock_read_entry_at_addr(sb, hdr_addr, &hdr_dl, &hdr_bh);
				if (ret) {
					brelse(anchor_bh);
					return ret;
				}
				brelse(hdr_bh);
				chain_block = le64_to_cpu(hdr_dl.le_overflow_block);
			}
		}

		/* Chain ends: allocate a new overflow block and CAS-link it. */
		{
			u64 new_block = 0;
			struct ocsfs_disk_lock updated_anchor;

			ret = ocsfs_alloc_blocks(sb, 0, 1, &new_block);
			if (ret) {
				brelse(anchor_bh);
				pr_warn("ocsfs: cannot allocate overflow block for lock table\n");
				return -ENOSPC;
			}

			/* Zero the new block. */
			{
				struct buffer_head *nbh = sb_getblk(sb, new_block);

				if (!nbh) {
					ocsfs_free_blocks(sb, new_block, 1);
					brelse(anchor_bh);
					return -EIO;
				}
				lock_buffer(nbh);
				memset(nbh->b_data, 0, sbi->s_block_size);
				set_buffer_uptodate(nbh);
				mark_buffer_dirty(nbh);
				unlock_buffer(nbh);
				sync_dirty_buffer(nbh);
				brelse(nbh);
			}

			/* CAS-set le_overflow_block in anchor to new_block. */
			memcpy(&updated_anchor, &anchor_dl, sizeof(updated_anchor));
			updated_anchor.le_overflow_block = cpu_to_le64(new_block);

			ret = lock_write_entry(sb, last_slot, &updated_anchor, anchor_bh);
			brelse(anchor_bh);
			if (ret) {
				ocsfs_free_blocks(sb, new_block, 1);
				return ret; /* -EAGAIN: caller will retry from probe start */
			}

			/* The first entry in the new block is our slot. */
			lr->lr_slot          = last_slot;
			lr->lr_overflow_addr = new_block * sbi->s_block_size;
			return 0;
		}
	}
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
	/* lr_slot is overwritten by lock_probe_slot; use legacy default here */
	lr->lr_slot          = ocsfs_lock_table_slot(resource_id, OCSFS_LOCK_ENTRY_COUNT);
	mutex_init(&lr->lr_mutex);
	init_waitqueue_head(&lr->lr_wq);
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
	lr->lr_slot          = ocsfs_lock_table_slot(resource_id,
					ocsfs_lock_primary_count(sbi));
	lr->lr_dynamic       = true;
	mutex_init(&lr->lr_mutex);
	init_waitqueue_head(&lr->lr_wq);

	spin_lock(&sbi->s_lock_list_lock);
	list_add(&lr->lr_list, &sbi->s_lock_list);
	spin_unlock(&sbi->s_lock_list_lock);

	return lr;
}

void ocsfs_lock_free(struct ocsfs_lock_res *lr)
{
	kfree(lr);
}
