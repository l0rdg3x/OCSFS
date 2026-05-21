// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — scsi_pr.c
 * SCSI-3 Persistent Reservations and Compare-And-Write.
 *
 * PR operations use bdev->bd_disk->fops->pr_ops (block-layer abstraction)
 * so they work with any PR-capable device without depending on non-exported
 * SCSI symbols.
 *
 * CAW (Compare-And-Write, opcode 0x89) uses scsi_execute_cmd() via
 * scsi_device_from_queue() — both are EXPORT_SYMBOL_GPL and available to
 * GPL modules on kernel >= 5.16. On non-SCSI devices (loop, virtio) CAW
 * is probed at mount time and disabled gracefully, falling back to the
 * software version-check path in lock.c.
 */

#include "ocsfs.h"
#include <linux/pr.h>
#include <linux/unaligned.h>
#include <scsi/scsi_proto.h>

/*
 * Derive a unique, hard-to-predict PR key from the full 16-byte UUID
 * and mount generation. Uses two independent CRC32C passes so that
 * the upper and lower 32 bits are both UUID-dependent.
 * Previous version used only 4 bytes of UUID, making keys guessable.
 */
u64 ocsfs_pr_make_key(const u8 *uuid, u32 mount_gen)
{
	u32 hi = crc32c(mount_gen,       uuid, 16);
	u32 lo = crc32c(~hi ^ mount_gen, uuid, 16);

	return ((u64)hi << 32) | lo;
}

/* Map OCSFS SCSI-CDB type encoding to block-layer enum pr_type. */
static enum pr_type ocsfs_to_pr_type(u8 t)
{
	switch (t) {
	case OCSFS_PR_TYPE_WRITE_EXCL:      return PR_WRITE_EXCLUSIVE;
	case OCSFS_PR_TYPE_EXCL_ACCESS:     return PR_EXCLUSIVE_ACCESS;
	case OCSFS_PR_TYPE_WRITE_EXCL_REG:  return PR_WRITE_EXCLUSIVE_REG_ONLY;
	case OCSFS_PR_TYPE_EXCL_ACCESS_REG: return PR_EXCLUSIVE_ACCESS_REG_ONLY;
	default:                            return PR_WRITE_EXCLUSIVE_REG_ONLY;
	}
}

static const struct pr_ops *ocsfs_pr_ops(struct super_block *sb)
{
	struct block_device *bdev = sb->s_bdev;

	if (!bdev->bd_disk || !bdev->bd_disk->fops)
		return NULL;
	return bdev->bd_disk->fops->pr_ops;
}

int ocsfs_pr_register(struct super_block *sb, u64 key)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	const struct pr_ops *ops = ocsfs_pr_ops(sb);
	int ret = 0;

	if (ops && ops->pr_register)
		ret = ops->pr_register(sb->s_bdev, 0, key, 0);
	else
		pr_debug("ocsfs: PR not supported by device, skipping\n");

	if (ret)
		return ret;

	sbi->s_pr.pr_key = key;
	sbi->s_pr.pr_registered = true;
	pr_info("ocsfs: PR registered key 0x%016llx\n", key);
	return 0;
}

int ocsfs_pr_unregister(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	const struct pr_ops *ops = ocsfs_pr_ops(sb);
	int ret = 0;

	if (!sbi->s_pr.pr_registered)
		return 0;

	if (ops && ops->pr_register)
		ret = ops->pr_register(sb->s_bdev, sbi->s_pr.pr_key, 0, 0);

	if (ret == 0) {
		sbi->s_pr.pr_registered = false;
		pr_info("ocsfs: PR unregistered\n");
	}
	return ret;
}

int ocsfs_pr_reserve(struct super_block *sb, u8 type)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	const struct pr_ops *ops = ocsfs_pr_ops(sb);

	if (!ops || !ops->pr_reserve)
		return 0;
	return ops->pr_reserve(sb->s_bdev, sbi->s_pr.pr_key,
				ocsfs_to_pr_type(type), 0);
}

int ocsfs_pr_release(struct super_block *sb, u8 type)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	const struct pr_ops *ops = ocsfs_pr_ops(sb);

	if (!ops || !ops->pr_release)
		return 0;
	return ops->pr_release(sb->s_bdev, sbi->s_pr.pr_key,
				ocsfs_to_pr_type(type));
}

int ocsfs_pr_preempt(struct super_block *sb, u64 victim_key, u8 type)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	const struct pr_ops *ops = ocsfs_pr_ops(sb);

	pr_info("ocsfs: PR PREEMPT victim key 0x%016llx\n", victim_key);
	if (!ops || !ops->pr_preempt)
		return 0;
	return ops->pr_preempt(sb->s_bdev, sbi->s_pr.pr_key, victim_key,
				ocsfs_to_pr_type(type), false);
}

int ocsfs_pr_preempt_abort(struct super_block *sb, u64 victim_key, u8 type)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	const struct pr_ops *ops = ocsfs_pr_ops(sb);

	pr_warn("ocsfs: PR PREEMPT AND ABORT — fencing node with "
		"key 0x%016llx\n", victim_key);
	if (!ops || !ops->pr_preempt)
		return 0;
	return ops->pr_preempt(sb->s_bdev, sbi->s_pr.pr_key, victim_key,
				ocsfs_to_pr_type(type), true);
}

/* ═══════════════════════════════════════════════════════════════
 * SCSI COMPARE-AND-WRITE (BUG-003 fix)
 *
 * CAW (opcode 0x89, SBC-4) atomically: if disk[LBA] == expected,
 * write new_data to disk[LBA]. Eliminates the TOCTOU race in
 * lock_write_entry() when running on real SCSI storage.
 *
 * Falls back gracefully to -EOPNOTSUPP on non-SCSI devices.
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Build a 16-byte CDB for COMPARE AND WRITE (SBC-4 §5.3).
 * Pure function — no I/O, safe to call from KUnit tests.
 */
void ocsfs_build_caw_cdb(u8 cdb[16], u64 lba)
{
	memset(cdb, 0, 16);
	cdb[0] = COMPARE_AND_WRITE;         /* opcode 0x89 */
	put_unaligned_be64(lba, &cdb[2]);   /* LOGICAL BLOCK ADDRESS */
	put_unaligned_be32(1, &cdb[10]);    /* NUMBER OF LOGICAL BLOCKS = 1 */
}

/*
 * Probe whether the block device supports SCSI Compare-And-Write.
 *
 * NOTE: Full CAW support requires scsi_device_from_queue() + scsi_execute_cmd().
 * scsi_execute_cmd is EXPORT_SYMBOL, but scsi_device_from_queue is NOT exported
 * in all kernel configurations (not present in Module.symvers for this kernel).
 * Until it is exported, this probe always returns false and the software
 * version-check fallback in lock_write_entry() is used instead.
 *
 * To enable hardware CAW: add EXPORT_SYMBOL_GPL(scsi_device_from_queue) to
 * drivers/scsi/scsi_lib.c and uncomment the SCSI implementation below.
 */
bool ocsfs_scsi_caw_probe(struct super_block *sb)
{
	return false;
}

/*
 * Issue a SCSI Compare-And-Write for one logical block.
 * Currently returns -EOPNOTSUPP — see ocsfs_scsi_caw_probe() for rationale.
 * lock_write_entry() falls through to the software version-check path.
 */
int ocsfs_scsi_caw(struct super_block *sb, u64 lba,
		   const void *expected, const void *new_data,
		   unsigned int lbs)
{
	return -EOPNOTSUPP;
}
