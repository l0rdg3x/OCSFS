// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — scsi_pr.c
 * SCSI-3 Persistent Reservations and Compare-And-Write.
 *
 * PR operations use bdev->bd_disk->fops->pr_ops (block-layer abstraction)
 * so they work with any PR-capable device without depending on non-exported
 * SCSI symbols.
 *
 * CAW (Compare-And-Write, opcode 0x89, SBC-4 §5.3) — two execution paths:
 *
 *   BSG-DIRECT (primary):
 *     Reads the scsi_device pointer directly from request_queue->queuedata.
 *     The SCSI midlayer stores it there when it creates the blk_mq queue for
 *     each logical unit; accessing a public struct field requires no unexported
 *     symbols and works on hardened kernels where CONFIG_KPROBES is disabled.
 *     Validated by checking sdev->host != NULL.
 *
 *   KPROBE-SHIM (fallback):
 *     Resolves scsi_device_from_queue() via kprobe at module init; the probe
 *     is immediately unregistered — only the function pointer is kept.  Used
 *     on kernels where queuedata validation fails (non-SCSI dm-mpath stacking)
 *     but CONFIG_KPROBES is available.
 *
 * ocsfs_bsg_execute_cdb() is the single entry point for both paths.
 * On non-SCSI devices (virtio-blk, loop, NVMe) both probes fail gracefully
 * and ocsfs_atomic_cas() falls back to the PR-lease software path.
 */

#include "ocsfs.h"
#include <linux/pr.h>
#include <linux/unaligned.h>
#include <linux/kprobes.h>
#include <linux/mempool.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
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

bool ocsfs_pr_probe(struct super_block *sb)
{
	const struct pr_ops *ops = ocsfs_pr_ops(sb);

	return ops && ops->pr_register && ops->pr_preempt;
}

/* ═══════════════════════════════════════════════════════════════
 * SCSI COMPARE-AND-WRITE — BSG implementation
 *
 * CAW (opcode 0x89, SBC-4 §5.3): if disk[LBA] == expected, write new_data.
 * Eliminates the TOCTOU window in lock_write_entry() on real SCSI storage.
 *
 * Two-path architecture:
 *   1. BSG-DIRECT (no kprobe, works on hardened kernels)
 *   2. KPROBE-SHIM (fallback for dm-mpath and similar stacking drivers)
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

/* ── BSG-DIRECT path ── */

/*
 * ocsfs_bsg_get_sdev — obtain scsi_device from block device queue.
 *
 * The SCSI midlayer stores the scsi_device pointer in q->queuedata when it
 * creates the blk_mq queue for each SCSI logical unit (scsi_alloc_sdev +
 * scsi_mq_setup_tags).  Accessing this public struct field requires no
 * unexported symbols.
 *
 * Validation: sdev->host is set in scsi_alloc_sdev() before the queue is
 * linked.  A NULL host means queuedata does NOT point to a scsi_device
 * (virtio-blk, NVMe, loop, dm targets without SCSI queuedata all have either
 * queuedata==NULL or a different opaque type whose first field is not a host).
 *
 * scsi_execute_cmd() provides a second safety net: it validates the device
 * state before submitting the request and returns -ENODEV on mismatch.
 */
static struct scsi_device *ocsfs_bsg_get_sdev(struct request_queue *q)
{
	struct scsi_device *sdev;

	if (!q || !q->queuedata)
		return NULL;
	sdev = (struct scsi_device *)q->queuedata;
	if (!sdev->host)
		return NULL;
	return sdev;
}

/* ── KPROBE-SHIM fallback ── */

typedef struct scsi_device *(*ocsfs_sdfq_fn_t)(struct request_queue *q);
static ocsfs_sdfq_fn_t ocsfs_sdfq;

/*
 * Resolve scsi_device_from_queue() via kprobe and cache the pointer.
 * The kprobe is registered just to get the kernel VA, then immediately
 * unregistered — we never intercept any real call.
 *
 * Falls back gracefully when CONFIG_KPROBES is disabled (kprobe stubs return
 * -ENOSYS) so probe failures are not fatal.
 */
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

/* ── Unified probe ── */

/*
 * ocsfs_scsi_caw_probe — check whether CAW is available on this device.
 *
 * Returns true if either the BSG-DIRECT or KPROBE-SHIM path can supply a
 * valid scsi_device for scsi_execute_cmd().
 *
 * Called once at mount time; result cached in sbi->s_caw_supported.
 */
bool ocsfs_scsi_caw_probe(struct super_block *sb)
{
	struct request_queue *q = bdev_get_queue(sb->s_bdev);

	/* BSG-direct path — preferred; works on hardened kernels */
	if (ocsfs_bsg_get_sdev(q)) {
		pr_info("ocsfs: CAW probe: BSG-direct path available\n");
		return true;
	}

	/* Kprobe fallback — for dm-mpath and similar stacking drivers */
	if (ocsfs_caw_resolve_sdfq() && ocsfs_sdfq(q)) {
		pr_info("ocsfs: CAW probe: kprobe-shim fallback available\n");
		return true;
	}

	return false;
}

/* ── ocsfs_bsg_execute_cdb — unified SCSI CDB execution ── */

/*
 * ocsfs_bsg_execute_cdb — execute an arbitrary SCSI CDB via scsi_execute_cmd.
 *
 * Obtains the scsi_device via BSG-direct (q->queuedata) or kprobe fallback.
 * Fills a sense buffer; interprets MISCOMPARE (sense key 0x0e) → -EAGAIN so
 * the CAS retry loop can handle it.
 *
 * @sb:        mounted filesystem superblock
 * @cdb:       SCSI Command Descriptor Block (16 bytes for CAW)
 * @buf:       data buffer (expected ++ new_data for CAW)
 * @buf_len:   total buffer length in bytes
 * @data_dir:  DMA_TO_DEVICE or DMA_FROM_DEVICE
 *
 * Returns 0 on success, -EAGAIN on MISCOMPARE, negative errno otherwise.
 */
int ocsfs_bsg_execute_cdb(struct super_block *sb,
			   const u8 cdb[16], void *buf, unsigned int buf_len,
			   enum dma_data_direction data_dir)
{
	u8 sense[SCSI_SENSE_BUFFERSIZE];
	struct scsi_exec_args args = {
		.sense     = sense,
		.sense_len = sizeof(sense),
	};
	struct request_queue *q = bdev_get_queue(sb->s_bdev);
	struct scsi_device *sdev;
	int ret;

	/* BSG-direct path */
	sdev = ocsfs_bsg_get_sdev(q);

	/* Kprobe fallback */
	if (!sdev && ocsfs_sdfq)
		sdev = ocsfs_sdfq(q);

	if (!sdev)
		return -EOPNOTSUPP;

	ret = scsi_execute_cmd(sdev, cdb, data_dir == DMA_TO_DEVICE ?
			       REQ_OP_DRV_OUT : REQ_OP_DRV_IN,
			       buf, buf_len, HZ * 30, 3, &args);

	if (ret < 0)
		return ret;
	if (ret > 0) {
		/* MISCOMPARE (sense key 0x0e) → CAS lost the race, retry */
		if ((sense[2] & 0x0f) == MISCOMPARE)
			return -EAGAIN;
		pr_debug_ratelimited("ocsfs: BSG CDB 0x%02x failed: "
				     "host_byte=%d, status=%d, sense_key=0x%02x\n",
				     cdb[0], host_byte(ret), status_byte(ret),
				     sense[2] & 0x0f);
		return -EIO;
	}

	return 0;
}

/* ── ocsfs_scsi_caw — public CAW entry point ── */

/*
 * Issue a SCSI Compare-And-Write (opcode 0x89, SBC-4 §5.3) for one FS block.
 * Buffer layout expected by the device: expected (lbs bytes) || new_data (lbs bytes).
 * lba is a filesystem block number; converted to SCSI LBA accounting for
 * the device's logical block size.
 */
int ocsfs_scsi_caw(struct super_block *sb, u64 lba,
		   const void *expected, const void *new_data,
		   unsigned int lbs)
{
	unsigned int lbs_dev;
	u32 num_blocks;
	u8 cdb[16];
	u8 *buf;
	int ret;

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

	ret = ocsfs_bsg_execute_cdb(sb, cdb, buf, 2 * lbs, DMA_TO_DEVICE);

	if (2 * lbs <= OCSFS_CAW_BUF_SIZE && ocsfs_caw_pool)
		mempool_free(buf, ocsfs_caw_pool);
	else
		kfree(buf);

	return ret;
}
