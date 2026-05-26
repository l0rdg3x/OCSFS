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
 * scsi_device_from_queue(), resolved at probe time via kprobe (present in
 * vmlinux but not exported — no kernel patch required). On non-SCSI devices
 * (loop, virtio) CAW probe fails gracefully; ocsfs_atomic_cas() falls back
 * to the PR-lease software path.
 */

#include "ocsfs.h"
#include <linux/pr.h>
#include <linux/unaligned.h>
#include <linux/kprobes.h>
#include <linux/mempool.h>
#include <linux/slab.h>
#include <crypto/sha2.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_cmnd.h>

/* Pre-allocated CAW buffer pool — avoids GFP_NOIO kmalloc in the hot CAS path.
 * Sized for 2 × max sector (4096): expected block || new-data block. */
#define OCSFS_CAW_BUF_SIZE  8192U   /* 2 × 4096-byte max sector */
#define OCSFS_CAW_POOL_MIN  4       /* guaranteed elements under memory pressure */

static struct kmem_cache *ocsfs_caw_cache;
static mempool_t         *ocsfs_caw_pool;

int ocsfs_scsi_pool_init(void)
{
	ocsfs_caw_cache = kmem_cache_create("ocsfs_caw_buf", OCSFS_CAW_BUF_SIZE,
					    0, SLAB_HWCACHE_ALIGN, NULL);
	if (!ocsfs_caw_cache)
		return -ENOMEM;

	ocsfs_caw_pool = mempool_create_slab_pool(OCSFS_CAW_POOL_MIN,
						   ocsfs_caw_cache);
	if (!ocsfs_caw_pool) {
		kmem_cache_destroy(ocsfs_caw_cache);
		ocsfs_caw_cache = NULL;
		return -ENOMEM;
	}
	return 0;
}

void ocsfs_scsi_pool_destroy(void)
{
	mempool_destroy(ocsfs_caw_pool);
	ocsfs_caw_pool = NULL;
	kmem_cache_destroy(ocsfs_caw_cache);
	ocsfs_caw_cache = NULL;
}

/*
 * Derive a unique PR key from uuid (16 B) + mount_gen (4 B) via SHA-256.
 * Takes the first 8 bytes of the digest as a little-endian u64.
 * SHA-256 provides full avalanche: any single-bit change in uuid or
 * mount_gen produces an unpredictable key, preventing reservation hijack.
 */
u64 ocsfs_pr_make_key(const u8 *uuid, u32 mount_gen)
{
	u8 input[20];
	u8 digest[SHA256_DIGEST_SIZE];

	memcpy(input, uuid, 16);
	put_unaligned_le32(mount_gen, input + 16);
	sha256(input, sizeof(input), digest);
	return get_unaligned_le64(digest);
}

/* Map OCSFS SCSI-CDB type encoding to block-layer enum pr_type. */
static enum pr_type ocsfs_to_pr_type(u8 t)
{
	switch (t) {
	case OCSFS_PR_TYPE_WRITE_EXCL:      return PR_WRITE_EXCLUSIVE;
	case OCSFS_PR_TYPE_EXCL_ACCESS:     return PR_EXCLUSIVE_ACCESS;
	case OCSFS_PR_TYPE_WRITE_EXCL_REG:  return PR_WRITE_EXCLUSIVE_REG_ONLY;
	case OCSFS_PR_TYPE_EXCL_ACCESS_REG: return PR_EXCLUSIVE_ACCESS_REG_ONLY;
	default:
		WARN_ON(1);
		return PR_WRITE_EXCLUSIVE_REG_ONLY;
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
		return -EOPNOTSUPP;
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
void ocsfs_build_caw_cdb(u8 cdb[16], u64 lba, u32 num_blocks)
{
	memset(cdb, 0, 16);
	cdb[0] = COMPARE_AND_WRITE;               /* opcode 0x89 */
	put_unaligned_be64(lba, &cdb[2]);         /* LOGICAL BLOCK ADDRESS */
	put_unaligned_be32(num_blocks, &cdb[10]); /* NUMBER OF LOGICAL BLOCKS */
}

/*
 * Probe whether the block device supports SCSI Persistent Reservations.
 * Returns true if both pr_register and pr_preempt are present and usable.
 * A device without PR cannot fence zombie nodes (see s_degraded for override).
 */
bool ocsfs_pr_probe(struct super_block *sb)
{
	const struct pr_ops *ops = ocsfs_pr_ops(sb);

	return ops && ops->pr_register && ops->pr_preempt;
}

/*
 * scsi_device_from_queue() is present in vmlinux but not exported to modules.
 * Resolve it once at probe time via kprobe so we can call it without a
 * kernel patch. The kprobe is registered and immediately unregistered —
 * we keep only the resolved function pointer.
 */
typedef struct scsi_device *(*ocsfs_sdfq_fn_t)(struct request_queue *q);
static ocsfs_sdfq_fn_t ocsfs_sdfq;

static bool ocsfs_caw_resolve_sdfq(void)
{
	struct kprobe kp = { .symbol_name = "scsi_device_from_queue" };

	if (ocsfs_sdfq)
		return true;

	if (register_kprobe(&kp) < 0)
		return false;

	ocsfs_sdfq = (ocsfs_sdfq_fn_t)kp.addr;
	unregister_kprobe(&kp);
	return ocsfs_sdfq != NULL;
}

/*
 * Probe whether the block device supports SCSI Compare-And-Write.
 * Resolves scsi_device_from_queue via kprobe; returns false on non-SCSI
 * devices (virtio, loop) so the CAS engine falls through to PR-lease.
 */
bool ocsfs_scsi_caw_probe(struct super_block *sb)
{
	struct scsi_device *sdev;

	if (!ocsfs_caw_resolve_sdfq())
		return false;

	sdev = ocsfs_sdfq(bdev_get_queue(sb->s_bdev));
	return sdev != NULL;
}

/*
 * Issue a SCSI Compare-And-Write (opcode 0x89, SBC-4 §5.3) for one LBA.
 * Buffer layout: expected (lbs bytes) || new_data (lbs bytes).
 * MISCOMPARE (sense key 0x0e) → -EAGAIN so the CAS loop retries.
 */
int ocsfs_scsi_caw(struct super_block *sb, u64 lba,
		   const void *expected, const void *new_data,
		   unsigned int lbs)
{
	u8 sense[SCSI_SENSE_BUFFERSIZE];
	struct scsi_exec_args args = {
		.sense     = sense,
		.sense_len = sizeof(sense),
	};
	unsigned int lbs_dev;
	u32 num_blocks;
	struct scsi_device *sdev;
	u8 cdb[16];
	u8 *buf;
	int ret;

	if (!ocsfs_sdfq)
		return -EOPNOTSUPP;

	sdev = ocsfs_sdfq(bdev_get_queue(sb->s_bdev));
	if (!sdev)
		return -EOPNOTSUPP;

	/*
	 * Convert filesystem block number to SCSI LBA and block count.
	 * bdev_logical_block_size() gives the device sector size (512 or 4096).
	 * One FS block covers (lbs / lbs_dev) SCSI logical blocks.
	 */
	lbs_dev = bdev_logical_block_size(sb->s_bdev);
	if (lbs_dev == 0 || lbs % lbs_dev != 0)
		return -EINVAL;
	num_blocks = lbs / lbs_dev;

	if (2 * lbs <= OCSFS_CAW_BUF_SIZE && ocsfs_caw_pool) {
		buf = mempool_alloc(ocsfs_caw_pool, GFP_NOIO);
	} else {
		buf = kmalloc(2 * lbs, GFP_NOIO);
	}
	if (!buf)
		return -ENOMEM;

	memcpy(buf,       expected, lbs);
	memcpy(buf + lbs, new_data, lbs);

	ocsfs_build_caw_cdb(cdb, lba * num_blocks, num_blocks);

	ret = scsi_execute_cmd(sdev, cdb, REQ_OP_DRV_OUT, buf, 2 * lbs,
			       HZ * 5, 3, &args);

	if (2 * lbs <= OCSFS_CAW_BUF_SIZE && ocsfs_caw_pool)
		mempool_free(buf, ocsfs_caw_pool);
	else
		kfree(buf);

	if (ret < 0)
		return ret;
	if (ret > 0) {
		if ((sense[2] & 0x0f) == MISCOMPARE)
			return -EAGAIN;
		return -EIO;
	}

	return 0;
}
