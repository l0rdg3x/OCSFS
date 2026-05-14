// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — scsi_pr.c
 * SCSI-3 Persistent Reservations via block-layer pr_ops.
 *
 * Uses bdev->bd_disk->fops->pr_ops instead of raw SCSI CDB commands so
 * it works with any PR-capable block device (SCSI, NVMe, etc.) and does
 * not depend on scsi_device_from_queue, which is not exported to out-of-
 * tree modules. Devices that don't support PR (loop, files) are silently
 * skipped — single-node mode operates correctly without PR.
 */

#include "ocsfs.h"
#include <linux/pr.h>

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
