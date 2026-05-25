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
#include <linux/fiemap.h>

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

static int ocsfs_write_begin(const struct kiocb *iocb,
			     struct address_space *mapping,
			     loff_t pos, unsigned len,
			     struct folio **foliop, void **fsdata)
{
	return block_write_begin(mapping, pos, len, foliop, ocsfs_get_block);
}

static int ocsfs_write_end(const struct kiocb *iocb,
			   struct address_space *mapping,
			   loff_t pos, unsigned len, unsigned copied,
			   struct folio *folio, void *fsdata)
{
	int ret;

	ret = generic_write_end(iocb, mapping, pos, len, copied, folio, fsdata);
	if (ret > 0)
		mark_inode_dirty(mapping->host);
	return ret;
}

static sector_t ocsfs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, ocsfs_get_block);
}

/* ═══════════════════════════════════════════════════════════════
 * FIEMAP — physical extent layout for backup tools (vzdump, qemu-img)
 * ═══════════════════════════════════════════════════════════════ */

struct ocsfs_fiemap_ctx {
	struct fiemap_extent_info *fieinfo;
	u32  blksize;
	u64  start_b;   /* requested range in bytes */
	u64  end_b;
	bool pending;
	u64  p_log, p_phys, p_len;
	u32  p_flags;
	int  ret;
};

static int ocsfs_fiemap_cb(u64 logical, u64 physical, u32 length,
			   u16 flags, void *priv)
{
	struct ocsfs_fiemap_ctx *c = priv;
	u64 log_b  = (u64)logical  * c->blksize;
	u64 phys_b = (u64)physical * c->blksize;
	u64 len_b  = (u64)length   * c->blksize;
	u32 fflags = 0;

	if (log_b + len_b <= c->start_b || log_b >= c->end_b)
		return 0;

	if (flags & OCSFS_EXT_UNWRITTEN)
		fflags |= FIEMAP_EXTENT_UNWRITTEN;
	if (flags & OCSFS_EXT_COMPRESSED)
		fflags |= FIEMAP_EXTENT_ENCODED;

	if (c->pending) {
		c->ret = fiemap_fill_next_extent(c->fieinfo,
						 c->p_log, c->p_phys,
						 c->p_len, c->p_flags);
		if (c->ret)
			return c->ret;
	}
	c->p_log   = log_b;
	c->p_phys  = phys_b;
	c->p_len   = len_b;
	c->p_flags = fflags;
	c->pending = true;
	return 0;
}

int ocsfs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		 u64 start, u64 len)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_fiemap_ctx c = {
		.fieinfo  = fieinfo,
		.blksize  = sbi->s_block_size,
		.start_b  = start,
		.end_b    = start + len,
	};
	int ret;
	u16 i;

	ret = fiemap_prep(inode, fieinfo, start, &len, FIEMAP_FLAG_SYNC);
	if (ret)
		return ret;

	mutex_lock(&oi->i_extent_lock);

	if (oi->i_extent_tree_root) {
		ret = ocsfs_extent_btree_iterate(inode, ocsfs_fiemap_cb, &c);
		if (ret > 0) ret = 0;
	} else {
		for (i = 0; i < oi->i_extent_count; i++) {
			ocsfs_fiemap_cb(oi->i_extents[i].logical_block,
					oi->i_extents[i].physical_block,
					oi->i_extents[i].length,
					oi->i_extents[i].flags, &c);
			if (c.ret) break;
		}
		ret = (c.ret > 0) ? 0 : c.ret;
	}

	if (!ret && c.pending) {
		c.p_flags |= FIEMAP_EXTENT_LAST;
		ret = fiemap_fill_next_extent(fieinfo,
					      c.p_log, c.p_phys,
					      c.p_len, c.p_flags);
		if (ret > 0) ret = 0;
	}

	mutex_unlock(&oi->i_extent_lock);
	return ret;
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

/*
 * ocsfs_file_llseek — extent-aware SEEK_HOLE / SEEK_DATA.
 *
 * generic_file_llseek() uses the page cache for SEEK_HOLE/SEEK_DATA,
 * which gives wrong results for sparse regions never faulted in.
 * Use iomap_seek_{hole,data} instead — they walk the on-disk extent
 * map directly, enabling cp --sparse / qemu-img convert to work correctly.
 */
static loff_t ocsfs_file_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file_inode(file);

	switch (whence) {
	case SEEK_HOLE:
		offset = iomap_seek_hole(inode, offset, &ocsfs_iomap_ops);
		break;
	case SEEK_DATA:
		offset = iomap_seek_data(inode, offset, &ocsfs_iomap_ops);
		break;
	default:
		return generic_file_llseek(file, offset, whence);
	}

	if (offset < 0)
		return offset;
	return vfs_setpos(file, offset, inode->i_sb->s_maxbytes);
}

static int ocsfs_fsync(struct file *file, loff_t start, loff_t end,
		       int datasync)
{
	struct inode *inode = file_inode(file);
	int ret;

	/*
	 * Lazily compress inline extents before syncing to disk.
	 * If compression fails for any extent, log and proceed — the data
	 * is already safely on disk uncompressed.
	 */
	if (ocsfs_get_compression_algo(inode) != OCSFS_COMPRESS_NONE) {
		int cr = ocsfs_compress_file(inode);

		if (cr)
			pr_warn_ratelimited("ocsfs: compress_file failed (%d), "
					    "syncing uncompressed\n", cr);
	}

	ret = file_write_and_wait_range(file, start, end);
	if (ret)
		return ret;

	/* Flush inode metadata */
	if (!datasync || inode_state_read(inode) & I_DIRTY_DATASYNC) {
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_ALL,
			.nr_to_write = 0,
		};
		ret = ocsfs_write_inode(inode, &wbc);
		if (ret)
			return ret;
	}

	/*
	 * Flush the block device write cache so data is durable on the SAN.
	 * Without this, fsync() only guarantees the OS page cache is written
	 * to the HBA — not that it has reached stable storage.
	 */
	return blkdev_issue_flush(inode->i_sb->s_bdev);
}

const struct file_operations ocsfs_file_fops = {
	.llseek          = ocsfs_file_llseek,
	.read_iter       = ocsfs_file_read_iter,   /* iomap-based (iomap.c) */
	.write_iter      = ocsfs_file_write_iter,  /* iomap-based (iomap.c) */
	.mmap            = generic_file_mmap,
	.open            = ocsfs_open,
	.fsync           = ocsfs_fsync,
	.fallocate       = ocsfs_fallocate,        /* thin.c */
	.splice_read     = filemap_splice_read,
};
