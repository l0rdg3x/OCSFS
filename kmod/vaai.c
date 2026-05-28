// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — vaai.c
 * VMware vStorage APIs for Array Integration (VAAI) offload commands.
 *
 * Implements three SCSI offload operations via BSG-direct path:
 *   WRITE SAME (0x93)  — zero or pattern fill a range of blocks
 *   UNMAP       (0x42) — TRIM/discard a range of blocks
 *   EXTENDED COPY (0x83) — server-side copy between two ranges on same LUN
 *
 * All three are best-effort: if the device returns CHECK CONDITION the
 * kernel falls back to ordinary host-side I/O. This matches what VMFS does.
 *
 * Userspace interface: OCSFS_IOC_VAAI_WRITE_SAME / _UNMAP / _XCOPY ioctls
 * defined in ocsfs.h, dispatched from ocsfs_ioctl() in file.c.
 */

#include <linux/blkdev.h>
#include <linux/unaligned.h>
#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════ */

/* Convert a byte offset + length into a (LBA, block_count) pair.
 * Returns -EINVAL if not block-aligned. */
static int bytes_to_lba(struct super_block *sb, u64 byte_off, u64 byte_len,
			 u64 *lba_out, u32 *nblocks_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u32 bs = sbi->s_block_size;

	if ((byte_off & (bs - 1)) || (byte_len & (bs - 1)))
		return -EINVAL;
	if (!byte_len)
		return -EINVAL;

	*lba_out     = byte_off / bs;
	*nblocks_out = (u32)(byte_len / bs);
	return 0;
}

/*
 * SEC-N2: verify that [lba, lba+nblocks) falls within the inode's allocated
 * physical extents.  For btree inodes we trust CAP_SYS_ADMIN (already
 * checked in file.c); for inline inodes we do a full range check.
 * Returns true if the range is owned by the inode (or btree — trusted).
 */
static bool ocsfs_vaai_owns_range(struct inode *inode, u64 lba, u32 nblocks)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 end_lba = lba + nblocks;
	u16 i;
	bool found = false;

	if (oi->i_extent_tree_root)
		return true; /* btree inodes: CAP_SYS_ADMIN already verified */

	mutex_lock(&oi->i_extent_lock);
	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];

		if (lba >= e->physical_block &&
		    end_lba <= e->physical_block + e->length) {
			found = true;
			break;
		}
	}
	mutex_unlock(&oi->i_extent_lock);
	return found;
}

/* ═══════════════════════════════════════════════════════════════
 * WRITE SAME (0x93) — 16-byte CDB
 * Zero-fill or pattern-fill a range.  We use NDOB (No Data-Out Block)
 * to request a device-side zero without transferring data.
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_vaai_write_same(struct inode *inode,
			   const struct ocsfs_vaai_arg __user *uarg)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs_vaai_arg arg;
	u64  lba;
	u32  nblocks;
	u8   cdb[16];
	int  ret;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	ret = bytes_to_lba(sb, arg.offset, arg.length, &lba, &nblocks);
	if (ret)
		return ret;

	/* SEC-V3-4: protect filesystem metadata area. */
	if (arg.offset < OCSFS_SB(sb)->s_data_off)
		return -EPERM;

	/* SEC-N2: for inline inodes, verify the range belongs to this file. */
	if (!ocsfs_vaai_owns_range(inode, lba, nblocks))
		return -EPERM;

	memset(cdb, 0, sizeof(cdb));
	cdb[0]  = 0x93;          /* WRITE SAME(16) */
	cdb[1]  = (1 << 0);      /* NDOB bit: no data-out buffer needed */
	put_unaligned_be64(lba,      &cdb[2]);
	put_unaligned_be32(nblocks,  &cdb[10]);

	/* DMA_NONE: NDOB means no data transfer */
	ret = ocsfs_bsg_execute_cdb(sb, cdb, NULL, 0, DMA_NONE);
	if (ret) {
		pr_debug("ocsfs: WRITE SAME failed for LBA %llu n=%u (%d)\n",
			 lba, nblocks, ret);
		return ret;
	}
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * UNMAP (0x42) — 10-byte CDB + parameter list
 * Tell the storage array that a range of blocks is no longer needed.
 * VMFS uses this on VM delete / disk trim.
 * ═══════════════════════════════════════════════════════════════ */

/* UNMAP parameter list layout (SBC-3):
 *   Bytes 0-1: UNMAP data length (= total_len - 2)
 *   Bytes 2-3: UNMAP block descriptor data length (= total_len - 8)
 *   Bytes 4-7: reserved
 *   [Per descriptor, 16 bytes]:
 *     Bytes 0-7:  UNMAP LBA
 *     Bytes 8-11: number of logical blocks
 *     Bytes 12-15: reserved
 */
#define UNMAP_HDR_SIZE       8
#define UNMAP_DESC_SIZE      16
#define UNMAP_PARAM_SIZE     (UNMAP_HDR_SIZE + UNMAP_DESC_SIZE)

int ocsfs_vaai_unmap(struct inode *inode,
		      const struct ocsfs_vaai_arg __user *uarg)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs_vaai_arg arg;
	u64  lba;
	u32  nblocks;
	u8  *param;
	u8   cdb[16];
	int  ret;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	ret = bytes_to_lba(sb, arg.offset, arg.length, &lba, &nblocks);
	if (ret)
		return ret;

	/* SEC-N2: protect metadata area + verify range belongs to this inode. */
	if (arg.offset < OCSFS_SB(sb)->s_data_off)
		return -EPERM;
	if (!ocsfs_vaai_owns_range(inode, lba, nblocks))
		return -EPERM;

	param = kzalloc(UNMAP_PARAM_SIZE, GFP_KERNEL);
	if (!param)
		return -ENOMEM;

	put_unaligned_be16(UNMAP_PARAM_SIZE - 2,   param + 0); /* data length */
	put_unaligned_be16(UNMAP_DESC_SIZE,         param + 2); /* descriptor length */
	put_unaligned_be64(lba,                     param + 8);
	put_unaligned_be32(nblocks,                 param + 16);

	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x42;                    /* UNMAP (10-byte CDB) */
	put_unaligned_be16(UNMAP_PARAM_SIZE, &cdb[7]);

	ret = ocsfs_bsg_execute_cdb(sb, cdb, param, UNMAP_PARAM_SIZE,
				    DMA_TO_DEVICE);
	if (ret)
		pr_debug("ocsfs: UNMAP failed for LBA %llu n=%u (%d)\n",
			 lba, nblocks, ret);

	kfree(param);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * EXTENDED COPY (0x83) — server-side copy within the same LUN
 * Used by VMFS for fast VM clone / storage vMotion offload.
 *
 * We support only the simplest case: copy N blocks from src_offset
 * to dst_offset on the same device (segment type 0x02 — block device
 * to block device, same I_T nexus).
 * ═══════════════════════════════════════════════════════════════ */

/* XCOPY parameter list for a single B2B segment (minimal subset):
 *  Header (16 bytes):
 *    [0]   list identifier
 *    [1]   priority (0)
 *    [2-3] target descriptor list length
 *    [4-7] reserved
 *    [8-9] segment descriptor list length
 *    [10-15] reserved
 *  Target descriptor — type 0xE4 (identification + identification w/ no
 *    device type): we skip target descriptors because we're using the
 *    initiator as both source and destination (inline same LUN copy).
 *    Instead we rely on the device's XCOPY implementation accepting
 *    LID4 (List IDentifier 4) where both segments reference the same
 *    I_T nexus.  Linux md/dm-multipath implementations support this.
 *  Segment descriptor — type 0x02 (block device → block device):
 *    [0]   0x02
 *    [1]   reserved
 *    [2-3] descriptor length (28)
 *    [4-5] source target descriptor index (0)
 *    [6-7] destination target descriptor index (0)
 *    [8-11] reserved
 *    [12-15] number of blocks
 *    [16-23] source LBA
 *    [24-31] destination LBA
 */

#define XCOPY_HDR_SIZE   16
#define XCOPY_SEG_SIZE   32

int ocsfs_vaai_xcopy(struct super_block *sb,
		      const struct ocsfs_vaai_xcopy_arg __user *uarg)
{
	struct ocsfs_vaai_xcopy_arg arg;
	u64  src_lba, dst_lba;
	u32  nblocks;
	u8  *param;
	u8   cdb[16];
	u16  seg_len;
	u32  param_len;
	int  ret;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	{
		struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
		u32 bs = sbi->s_block_size;

		if ((arg.src_offset & (bs - 1)) ||
		    (arg.dst_offset & (bs - 1)) ||
		    (arg.length & (bs - 1)) || !arg.length)
			return -EINVAL;

		src_lba  = arg.src_offset / bs;
		dst_lba  = arg.dst_offset / bs;
		nblocks  = (u32)(arg.length / bs);
	}

	seg_len   = XCOPY_SEG_SIZE - 4; /* descriptor length field excludes first 4 bytes */
	param_len = XCOPY_HDR_SIZE + XCOPY_SEG_SIZE;

	param = kzalloc(param_len, GFP_KERNEL);
	if (!param)
		return -ENOMEM;

	/* Header */
	param[0] = 0x00;                        /* list identifier */
	param[1] = 0x00;                        /* priority */
	put_unaligned_be16(0,             param + 2);  /* target desc list length = 0 */
	put_unaligned_be16(XCOPY_SEG_SIZE, param + 8); /* segment desc list length */

	/* Segment descriptor type 0x02 */
	param[XCOPY_HDR_SIZE + 0] = 0x02;
	put_unaligned_be16(seg_len,  param + XCOPY_HDR_SIZE + 2);
	put_unaligned_be16(0,        param + XCOPY_HDR_SIZE + 4);  /* src tgt idx */
	put_unaligned_be16(0,        param + XCOPY_HDR_SIZE + 6);  /* dst tgt idx */
	put_unaligned_be32(nblocks,  param + XCOPY_HDR_SIZE + 12);
	put_unaligned_be64(src_lba,  param + XCOPY_HDR_SIZE + 16);
	put_unaligned_be64(dst_lba,  param + XCOPY_HDR_SIZE + 24);

	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x83;                        /* EXTENDED COPY */
	put_unaligned_be32(param_len, &cdb[10]);

	ret = ocsfs_bsg_execute_cdb(sb, cdb, param, param_len, DMA_TO_DEVICE);
	if (ret)
		pr_debug("ocsfs: XCOPY src=%llu dst=%llu n=%u failed (%d)\n",
			 src_lba, dst_lba, nblocks, ret);

	kfree(param);
	return ret;
}
