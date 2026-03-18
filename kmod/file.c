// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — file.c
 * File operations and address_space operations.
 *
 * Phase 1: buffer_head based I/O via get_block callback.
 * Phase 3: iomap-based I/O for direct I/O and buffered I/O,
 *           fallocate (prealloc, punch hole, zero range),
 *           O_DIRECT via iomap_dio_rw.
 *
 * The buffer_head path is kept as a fallback. The iomap path
 * (defined in iomap.c) is used for read_iter/write_iter and
 * the iomap address_space_ops are set on regular files.
 */

#include "ocsfs.h"
#include <linux/iomap.h>

/* ═══════════════════════════════════════════════════════════════
 * GET_BLOCK — maps logical file block → physical disk block
 *
 * This is the core callback used by the buffer_head layer.
 * Retained for directory I/O and other non-data paths.
 * Data file I/O uses iomap (see iomap.c).
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_get_block(struct inode *inode, sector_t iblock,
			   struct buffer_head *bh_result, int create)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_extent ext;
	int ret;

	mutex_lock(&oi->i_extent_lock);

	ret = ocsfs_extent_lookup(inode, iblock, &ext);
	if (ret == 0 && ext.physical_block != 0) {
		/* Found an existing mapping */
		u64 offset_in_ext = iblock - ext.logical_block;
		map_bh(bh_result, inode->i_sb,
		       ext.physical_block + offset_in_ext);
		if (ext.flags & OCSFS_EXT_UNWRITTEN)
			set_buffer_new(bh_result);
		mutex_unlock(&oi->i_extent_lock);
		return 0;
	}

	if (!create) {
		/* No mapping and not creating — return unmapped (hole) */
		mutex_unlock(&oi->i_extent_lock);
		return 0;
	}

	/* Allocate a new block */
	{
		u64 phys;

		ret = ocsfs_alloc_blocks(inode->i_sb, oi->i_ag, 1, &phys);
		if (ret) {
			mutex_unlock(&oi->i_extent_lock);
			return ret;
		}

		ret = ocsfs_extent_insert(inode, iblock, phys, 1,
					  OCSFS_EXT_WRITTEN);
		if (ret) {
			ocsfs_free_blocks(inode->i_sb, phys, 1);
			mutex_unlock(&oi->i_extent_lock);
			return ret;
		}

		inode->i_blocks += sbi->s_block_size / 512;

		map_bh(bh_result, inode->i_sb, phys);
		set_buffer_new(bh_result);
	}

	mutex_unlock(&oi->i_extent_lock);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * ADDRESS SPACE OPERATIONS — buffer_head fallback
 *
 * Used for directories and as fallback if iomap is not available.
 * Regular files use ocsfs_iomap_aops (see iomap.c).
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, ocsfs_get_block);
}

static void ocsfs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ocsfs_get_block);
}

static int ocsfs_writepages(struct address_space *mapping,
			    struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, ocsfs_get_block);
}

static int ocsfs_write_begin(struct file *file,
			     struct address_space *mapping,
			     loff_t pos, unsigned len,
			     struct page **pagep, void **fsdata)
{
	return block_write_begin(mapping, pos, len, pagep, ocsfs_get_block);
}

static int ocsfs_write_end(struct file *file,
			   struct address_space *mapping,
			   loff_t pos, unsigned len, unsigned copied,
			   struct page *page, void *fsdata)
{
	int ret;

	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret > 0)
		mark_inode_dirty(mapping->host);
	return ret;
}

static sector_t ocsfs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, ocsfs_get_block);
}

const struct address_space_operations ocsfs_aops = {
	.dirty_folio    = block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
	.read_folio     = ocsfs_read_folio,
	.readahead      = ocsfs_readahead,
	.writepages     = ocsfs_writepages,
	.write_begin    = ocsfs_write_begin,
	.write_end      = ocsfs_write_end,
	.bmap           = ocsfs_bmap,
};

/* ═══════════════════════════════════════════════════════════════
 * FILE OPERATIONS
 *
 * Phase 3: read_iter/write_iter use iomap (defined in iomap.c)
 * for data files. O_DIRECT is handled transparently by the iomap
 * read/write iter implementations.
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_open(struct inode *inode, struct file *file)
{
	return generic_file_open(inode, file);
}

static int ocsfs_fsync(struct file *file, loff_t start, loff_t end,
		       int datasync)
{
	struct inode *inode = file_inode(file);
	int ret;

	ret = file_write_and_wait_range(file, start, end);
	if (ret)
		return ret;

	/* Flush inode metadata */
	if (!datasync || inode->i_state & I_DIRTY_DATASYNC) {
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_ALL,
			.nr_to_write = 0,
		};
		ret = ocsfs_write_inode(inode, &wbc);
	}

	return ret;
}

const struct file_operations ocsfs_file_fops = {
	.llseek         = generic_file_llseek,
	.read_iter      = ocsfs_file_read_iter,   /* iomap-based (iomap.c) */
	.write_iter     = ocsfs_file_write_iter,  /* iomap-based (iomap.c) */
	.mmap           = generic_file_mmap,
	.open           = ocsfs_open,
	.fsync          = ocsfs_fsync,
	.fallocate      = ocsfs_fallocate,        /* thin.c */
	.splice_read    = filemap_splice_read,
};
