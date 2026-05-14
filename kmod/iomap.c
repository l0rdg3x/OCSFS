// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — iomap.c
 * iomap-based I/O for the data path.
 *
 * Phase 3: Replace buffer_head-based I/O with iomap for:
 *   - Direct I/O (O_DIRECT) — zero-copy between userspace and block device
 *   - Buffered I/O — iomap-based page cache management
 *   - Readahead — iomap-driven readahead for sequential patterns
 *
 * iomap is the modern Linux VFS I/O path (used by XFS, ext4, btrfs).
 * It maps file logical offsets to physical device offsets and lets
 * the VFS handle the actual I/O submission.
 */

#include "ocsfs.h"
#include <linux/iomap.h>

/* ═══════════════════════════════════════════════════════════════
 * IOMAP BEGIN / END CALLBACKS
 *
 * These are the core iomap callbacks. They translate file offsets
 * into device offsets using the extent map.
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_iomap_begin(struct inode *inode, loff_t pos, loff_t length,
			     unsigned flags, struct iomap *iomap,
			     struct iomap *srcmap)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_extent ext;
	u64 logical_block = pos / sbi->s_block_size;
	u64 end_block;
	int ret;

	mutex_lock(&oi->i_extent_lock);

	ret = ocsfs_extent_lookup(inode, logical_block, &ext);

	if (ret == 0 && ext.physical_block != 0) {
		/* Found an existing extent */
		u64 offset_in_ext = logical_block - ext.logical_block;
		u64 remaining_blocks = ext.length - offset_in_ext;
		loff_t mapped_len = (loff_t)remaining_blocks * sbi->s_block_size;

		iomap->addr = (ext.physical_block + offset_in_ext) *
			      (u64)sbi->s_block_size;
		iomap->length = min_t(loff_t, length, mapped_len);
		iomap->bdev = inode->i_sb->s_bdev;
		iomap->offset = pos;

		if (ext.flags & OCSFS_EXT_UNWRITTEN) {
			iomap->type = IOMAP_UNWRITTEN;

			/* For writes: convert UNWRITTEN → WRITTEN */
			if (flags & IOMAP_WRITE) {
				ocsfs_extent_convert_unwritten(inode,
					ext.logical_block + offset_in_ext,
					min_t(u32, remaining_blocks,
					      (length + sbi->s_block_size - 1) /
					      sbi->s_block_size));
			}
		} else {
			iomap->type = IOMAP_MAPPED;
		}

		mutex_unlock(&oi->i_extent_lock);
		return 0;
	}

	/* No mapping exists */
	if (!(flags & IOMAP_WRITE)) {
		/* Read from hole — return zeroes */
		iomap->type = IOMAP_HOLE;
		iomap->addr = IOMAP_NULL_ADDR;
		iomap->offset = pos;

		/* Calculate hole length to next extent or end of file */
		end_block = (inode->i_size + sbi->s_block_size - 1) /
			    sbi->s_block_size;
		iomap->length = min_t(loff_t, length,
				      (loff_t)(end_block - logical_block) *
				      sbi->s_block_size);
		if (iomap->length <= 0)
			iomap->length = sbi->s_block_size;

		mutex_unlock(&oi->i_extent_lock);
		return 0;
	}

	/* Write to unallocated region — allocate blocks */
	{
		u64 phys;
		u32 alloc_blocks;
		u32 hint_blocks;

		/* Try to allocate a multi-block extent for write efficiency */
		hint_blocks = (length + sbi->s_block_size - 1) /
			      sbi->s_block_size;
		hint_blocks = max_t(u32, hint_blocks, 1);

		/* Attempt multi-block allocation, fall back to single */
		ret = ocsfs_alloc_blocks(inode->i_sb, oi->i_ag,
					 hint_blocks, &phys);
		if (ret && hint_blocks > 1) {
			/* Fall back to single block */
			hint_blocks = 1;
			ret = ocsfs_alloc_blocks(inode->i_sb, oi->i_ag,
						 1, &phys);
		}
		if (ret) {
			mutex_unlock(&oi->i_extent_lock);
			return ret;
		}

		alloc_blocks = hint_blocks;

		ret = ocsfs_extent_insert(inode, logical_block, phys,
					  alloc_blocks, OCSFS_EXT_WRITTEN);
		if (ret) {
			ocsfs_free_blocks(inode->i_sb, phys, alloc_blocks);
			mutex_unlock(&oi->i_extent_lock);
			return ret;
		}

		inode->i_blocks += (u64)alloc_blocks *
				   (sbi->s_block_size / 512);

		iomap->addr = phys * (u64)sbi->s_block_size;
		iomap->length = (loff_t)alloc_blocks * sbi->s_block_size;
		iomap->type = IOMAP_MAPPED;
		iomap->flags |= IOMAP_F_NEW;
		iomap->bdev = inode->i_sb->s_bdev;
		iomap->offset = pos;
	}

	mutex_unlock(&oi->i_extent_lock);
	return 0;
}

static int ocsfs_iomap_end(struct inode *inode, loff_t pos, loff_t length,
			   ssize_t written, unsigned flags,
			   struct iomap *iomap)
{
	/* Update file size if we wrote past the end */
	if ((flags & IOMAP_WRITE) && written > 0) {
		loff_t new_size = pos + written;

		if (new_size > inode->i_size) {
			i_size_write(inode, new_size);
			mark_inode_dirty(inode);
		}
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * IOMAP OPERATIONS TABLES
 * ═══════════════════════════════════════════════════════════════ */

const struct iomap_ops ocsfs_iomap_ops = {
	.iomap_begin    = ocsfs_iomap_begin,
	.iomap_end      = ocsfs_iomap_end,
};

/* Direct I/O uses the same iomap ops */
const struct iomap_ops ocsfs_dio_iomap_ops = {
	.iomap_begin    = ocsfs_iomap_begin,
	.iomap_end      = ocsfs_iomap_end,
};

/* ═══════════════════════════════════════════════════════════════
 * IOMAP-BASED FILE OPERATIONS
 *
 * These replace the buffer_head-based read_iter/write_iter when
 * iomap is available (all modern kernels).
 * ═══════════════════════════════════════════════════════════════ */

ssize_t ocsfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	if (iocb->ki_flags & IOCB_DIRECT)
		return iomap_dio_rw(iocb, to, &ocsfs_dio_iomap_ops,
				    NULL, 0, NULL, 0);

	return filemap_read(iocb, to, 0);
}

ssize_t ocsfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	ssize_t ret;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;
		/*
		 * Invalidate the page cache so we start with clean pages.
		 * Another node may have written to this file since our last
		 * access; dropping stale pages prevents us from writing
		 * outdated data or exposing stale reads post-write.
		 */
		invalidate_inode_pages2(inode->i_mapping);
	}

	inode_lock(inode);

	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	if (iocb->ki_flags & IOCB_DIRECT) {
		ret = iomap_dio_rw(iocb, from, &ocsfs_dio_iomap_ops,
				   NULL, 0, NULL, 0);
	} else {
		ret = iomap_file_buffered_write(iocb, from,
						&ocsfs_iomap_ops, NULL, NULL);
		if (ret > 0)
			iocb->ki_pos += ret;
	}

out:
	inode_unlock(inode);

	if (ret > 0) {
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
		mark_inode_dirty(inode);
	}

	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);

	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * IOMAP-BASED ADDRESS SPACE OPERATIONS
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_iomap_read_folio(struct file *file, struct folio *folio)
{
	iomap_bio_read_folio(folio, &ocsfs_iomap_ops);
	return 0;
}

static void ocsfs_iomap_readahead(struct readahead_control *rac)
{
	iomap_bio_readahead(rac, &ocsfs_iomap_ops);
}

static sector_t ocsfs_iomap_bmap(struct address_space *mapping, sector_t bno)
{
	return iomap_bmap(mapping, bno, &ocsfs_iomap_ops);
}

const struct address_space_operations ocsfs_iomap_aops = {
	.dirty_folio    = iomap_dirty_folio,
	.read_folio     = ocsfs_iomap_read_folio,
	.readahead      = ocsfs_iomap_readahead,
	.bmap           = ocsfs_iomap_bmap,
};
