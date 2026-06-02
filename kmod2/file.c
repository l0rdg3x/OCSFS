// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — file.c
 * L2 completeness: fallocate (preallocate / punch hole / zero range), fiemap,
 * and SEEK_HOLE / SEEK_DATA. Preallocation creates UNWRITTEN extents (allocated
 * but read-as-zero); a later write converts the touched sub-range to WRITTEN in
 * iomap_begin. Hole-punch zeros the partial edge blocks (CoW-aware via
 * iomap_zero_range) and frees the full blocks (refcount-aware). Single-node.
 */
#include "ocsfs.h"
#include <linux/iomap.h>
#include <linux/falloc.h>
#include <linux/fiemap.h>
#include <linux/align.h>
#include <linux/pagemap.h>

/* ── preallocate: reserve blocks as UNWRITTEN over [offset, offset+len) ── */
static int ocsfs2_preallocate(struct inode *inode, loff_t offset, loff_t len,
			      int mode)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(inode->i_sb);
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	u32 bs = sbi->s_block_size;
	u64 lblk = (u64)offset / bs;
	u64 end = ((u64)offset + len + bs - 1) / bs;
	int ret = 0;

	mutex_lock(&oi->i_meta_lock);
	while (lblk < end) {
		struct ocsfs2_extent cover;
		u64 next = U64_MAX, phys;
		u32 want;

		if (ocsfs2_extent_find(inode, lblk, &cover, &next) == 0) {
			lblk = cover.logical + cover.length;   /* already mapped */
			continue;
		}
		want = (u32)min_t(u64, end - lblk, OCSFS2_ALLOC_CAP_BLOCKS);
		if (next != U64_MAX && next - lblk < want)
			want = (u32)(next - lblk);
		ret = ocsfs2_alloc_blocks(inode->i_sb, oi->i_ag, want, &phys);
		if (ret == -ENOSPC && want > 1) {
			want = 1;
			ret = ocsfs2_alloc_blocks(inode->i_sb, oi->i_ag, 1, &phys);
		}
		if (ret)
			break;
		ret = ocsfs2_extent_insert(inode, lblk, phys, want,
					   OCSFS2_EXT_UNWRITTEN);
		if (ret) {
			ocsfs2_free_blocks(inode->i_sb, phys, want);
			break;
		}
		inode->i_blocks += (u64)want * (bs / 512);
		lblk += want;
	}
	mutex_unlock(&oi->i_meta_lock);

	if (!ret && !(mode & FALLOC_FL_KEEP_SIZE) &&
	    (u64)offset + len > i_size_read(inode))
		i_size_write(inode, offset + len);
	return ret;
}

/* ── punch hole: zero partial edges, free the full interior blocks ── */
static int ocsfs2_punch_hole(struct inode *inode, loff_t offset, loff_t len)
{
	u32 bs = inode->i_sb->s_blocksize;
	loff_t isize = i_size_read(inode);
	loff_t end = offset + len;
	loff_t ha, hb, ta, tb;
	u64 fstart, fend;
	int ret;

	if (offset >= isize)
		return 0;
	if (end > isize)
		end = isize;
	if (end <= offset)
		return 0;

	ret = filemap_write_and_wait_range(inode->i_mapping, offset, end - 1);
	if (ret)
		return ret;

	/* zero the partial head and tail sub-block bytes (mapped blocks only;
	 * iomap_zero_range CoWs shared blocks and skips holes) */
	ha = offset;
	hb = min(end, (loff_t)ALIGN(offset, bs));
	if (hb > ha) {
		ret = iomap_zero_range(inode, ha, hb - ha, NULL,
				       &ocsfs2_iomap_ops, NULL, NULL);
		if (ret)
			return ret;
	}
	ta = max(offset, (loff_t)ALIGN_DOWN(end, bs));
	tb = end;
	if (tb > ta && ta >= hb) {
		ret = iomap_zero_range(inode, ta, tb - ta, NULL,
				       &ocsfs2_iomap_ops, NULL, NULL);
		if (ret)
			return ret;
	}

	truncate_pagecache_range(inode, offset, end - 1);

	/* free the fully-covered blocks -> a hole within i_size (reads zero) */
	fstart = ((u64)offset + bs - 1) / bs;
	fend = (u64)end / bs;
	if (fend > fstart) {
		mutex_lock(&OCSFS2_I(inode)->i_meta_lock);
		ret = ocsfs2_extent_punch_range(inode, fstart, fend);
		mutex_unlock(&OCSFS2_I(inode)->i_meta_lock);
	}
	return ret;
}

/* ── zero range: make [offset, offset+len) read as zero ── */
static int ocsfs2_zero_range(struct inode *inode, loff_t offset, loff_t len,
			     int mode)
{
	loff_t end = offset + len;
	int ret;

	ret = filemap_write_and_wait_range(inode->i_mapping, offset, end - 1);
	if (ret)
		return ret;
	ret = iomap_zero_range(inode, offset, len, NULL, &ocsfs2_iomap_ops,
			       NULL, NULL);
	if (ret)
		return ret;
	if (!(mode & FALLOC_FL_KEEP_SIZE) && (u64)end > i_size_read(inode))
		i_size_write(inode, end);
	return 0;
}

long ocsfs2_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	int ret;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
		     FALLOC_FL_ZERO_RANGE))
		return -EOPNOTSUPP;
	if (!S_ISREG(inode->i_mode))
		return -ENODEV;
	if (offset < 0 || len <= 0)
		return -EINVAL;

	inode_lock(inode);
	if (mode & FALLOC_FL_PUNCH_HOLE)
		ret = ocsfs2_punch_hole(inode, offset, len);
	else if (mode & FALLOC_FL_ZERO_RANGE)
		ret = ocsfs2_zero_range(inode, offset, len, mode);
	else
		ret = ocsfs2_preallocate(inode, offset, len, mode);

	if (!ret) {
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
		mark_inode_dirty(inode);
	}
	inode_unlock(inode);
	return ret;
}

/* ── fiemap / SEEK_HOLE / SEEK_DATA via iomap ── */
int ocsfs2_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		  u64 start, u64 len)
{
	int ret;

	ret = fiemap_prep(inode, fieinfo, start, &len, 0);
	if (ret)
		return ret;
	inode_lock_shared(inode);
	ret = iomap_fiemap(inode, fieinfo, start, len, &ocsfs2_iomap_ops);
	inode_unlock_shared(inode);
	return ret;
}

loff_t ocsfs2_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;

	switch (whence) {
	case SEEK_HOLE:
		inode_lock_shared(inode);
		offset = iomap_seek_hole(inode, offset, &ocsfs2_iomap_ops);
		inode_unlock_shared(inode);
		break;
	case SEEK_DATA:
		inode_lock_shared(inode);
		offset = iomap_seek_data(inode, offset, &ocsfs2_iomap_ops);
		inode_unlock_shared(inode);
		break;
	default:
		return generic_file_llseek(file, offset, whence);
	}
	if (offset < 0)
		return offset;
	return vfs_setpos(file, offset, inode->i_sb->s_maxbytes);
}
