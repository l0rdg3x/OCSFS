// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — scsi_pr.c
 * SCSI-3 Persistent Reservations interface.
 *
 * Provides hardware-level fencing via SCSI PR commands:
 *   - REGISTER: announce this node's PR key to the LUN
 *   - RESERVE:  acquire a reservation (Write Exclusive – Registrants Only)
 *   - RELEASE:  release the reservation
 *   - PREEMPT AND ABORT: fence a failed node (revoke its registration,
 *     causing the SAN fabric to reject its I/O)
 *
 * PR keys are derived from node UUID + mount generation so that stale
 * registrations from a previous mount of the same node are distinguishable.
 */

#include "ocsfs.h"

/*
 * Build a PR key from UUID and mount generation.
 * Uses first 4 bytes of UUID XOR'd with mount_gen in upper 32 bits.
 */
u64 ocsfs_pr_make_key(const u8 *uuid, u32 mount_gen)
{
	u32 uuid_part;

	memcpy(&uuid_part, uuid, sizeof(uuid_part));
	return ((u64)uuid_part << 32) | mount_gen;
}

/* ═══════════════════════════════════════════════════════════════
 * LOW-LEVEL SCSI PR COMMAND ISSUING
 *
 * Uses the block device's SCSI interface to send PR OUT commands.
 * On real hardware this goes through sg_io. For testing/loopback
 * we provide a fallback that always succeeds (single-node mode).
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Build and send a PERSISTENT RESERVE OUT command.
 *
 * CDB format (10 bytes):
 *   [0] = 0x5F (PR OUT opcode)
 *   [1] = service_action (REGISTER, RESERVE, etc.)
 *   [5..8] = parameter list length (always 24 for basic)
 *
 * Parameter data (24 bytes):
 *   [0..7]   = reservation key (current key, or 0 for new register)
 *   [8..15]  = service action reservation key (new key)
 *   [20..23] = scope/type
 */
static int ocsfs_pr_out(struct super_block *sb, u8 service_action,
			u64 cur_key, u64 sa_key, u8 type)
{
	struct block_device *bdev = sb->s_bdev;
	struct scsi_device *sdev;
	u8 cdb[10];
	u8 param[24];
	struct scsi_sense_hdr sshdr;
	const struct scsi_exec_args exec_args = {
		.sshdr = &sshdr,
	};
	int ret;

	/*
	 * Check if the underlying device supports SCSI commands.
	 * Loop devices and files don't — fall back gracefully.
	 */
	if (!bdev->bd_disk || !bdev->bd_disk->queue) {
		pr_debug("ocsfs: PR command on non-SCSI device, skipping\n");
		return 0;
	}

	sdev = scsi_device_from_queue(bdev->bd_disk->queue);
	if (!sdev) {
		/* Not a SCSI device (e.g., loopback) — skip PR */
		pr_debug("ocsfs: not a SCSI device, PR commands skipped\n");
		return 0;
	}

	/* Build CDB */
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x5F;  /* PERSISTENT RESERVE OUT */
	cdb[1] = service_action & 0x1F;
	/* Parameter list length = 24 */
	cdb[7] = 0;
	cdb[8] = 24;

	/* Build parameter data */
	memset(param, 0, sizeof(param));
	/* Reservation key (bytes 0-7, big-endian) */
	put_unaligned_be64(cur_key, &param[0]);
	/* Service action reservation key (bytes 8-15, big-endian) */
	put_unaligned_be64(sa_key, &param[8]);
	/* Type (byte 20, bits 3:0) and scope (byte 20, bits 7:4) */
	param[20] = type & 0x0F;

	ret = scsi_execute_cmd(sdev, cdb, REQ_OP_DRV_OUT,
			       param, sizeof(param),
			       30 * HZ, /* 30 second timeout */
			       3,       /* 3 retries */
			       &exec_args);

	scsi_device_put(sdev);

	if (ret) {
		pr_err("ocsfs: PR OUT (sa=%u) failed: ret=%d "
		       "sense=%x/%x/%x\n",
		       service_action, ret,
		       sshdr.sense_key, sshdr.asc, sshdr.ascq);
		return -EIO;
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Register this node's PR key with the LUN.
 * Must be called during mount before any other PR operation.
 */
int ocsfs_pr_register(struct super_block *sb, u64 key)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	int ret;

	ret = ocsfs_pr_out(sb, OCSFS_PR_REGISTER, 0, key, 0);
	if (ret)
		return ret;

	sbi->s_pr.pr_key = key;
	sbi->s_pr.pr_registered = true;
	pr_info("ocsfs: PR registered key 0x%016llx\n", key);
	return 0;
}

/*
 * Unregister — clear our PR key (during unmount).
 */
int ocsfs_pr_unregister(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	int ret;

	if (!sbi->s_pr.pr_registered)
		return 0;

	ret = ocsfs_pr_out(sb, OCSFS_PR_REGISTER,
			   sbi->s_pr.pr_key, 0, 0);
	if (ret == 0) {
		sbi->s_pr.pr_registered = false;
		pr_info("ocsfs: PR unregistered\n");
	}
	return ret;
}

/*
 * Reserve — acquire a reservation of the given type.
 */
int ocsfs_pr_reserve(struct super_block *sb, u8 type)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	return ocsfs_pr_out(sb, OCSFS_PR_RESERVE,
			    sbi->s_pr.pr_key, 0, type);
}

/*
 * Release — release our reservation.
 */
int ocsfs_pr_release(struct super_block *sb, u8 type)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	return ocsfs_pr_out(sb, OCSFS_PR_RELEASE,
			    sbi->s_pr.pr_key, 0, type);
}

/*
 * Preempt — remove another node's registration (for fencing).
 */
int ocsfs_pr_preempt(struct super_block *sb, u64 victim_key, u8 type)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	pr_info("ocsfs: PR PREEMPT victim key 0x%016llx\n", victim_key);
	return ocsfs_pr_out(sb, OCSFS_PR_PREEMPT,
			    sbi->s_pr.pr_key, victim_key, type);
}

/*
 * Preempt and Abort — fence a node AND abort its in-flight I/O.
 * This is the strongest fencing mechanism.
 */
int ocsfs_pr_preempt_abort(struct super_block *sb, u64 victim_key, u8 type)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	pr_warn("ocsfs: PR PREEMPT AND ABORT — fencing node with "
		"key 0x%016llx\n", victim_key);
	return ocsfs_pr_out(sb, OCSFS_PR_PREEMPT_AND_ABORT,
			    sbi->s_pr.pr_key, victim_key, type);
}
