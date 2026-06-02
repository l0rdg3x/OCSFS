// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — iomap.c
 * File data path via iomap: buffered + O_DIRECT read/write, sparse files,
 * sub-block RMW, writeback. Single-node (Plan 2): single-writer ownership
 * makes this free of cross-node coherence concerns. Inline extents only.
 *
 * Fresh allocations are mapped IOMAP_MAPPED | IOMAP_F_NEW (iomap zeroes the
 * partial parts of a block), so there is no UNWRITTEN state and no
 * writeback-time conversion (a v1 bug source). Holes are clamped to the next
 * allocated extent so a read/readahead never caches following mapped blocks as
 * zero (the v1 #13 lesson).
 */
#include "ocsfs.h"
#include <linux/iomap.h>
#include <linux/uio.h>
#include <linux/pagemap.h>
#include <linux/sched/mm.h>
#include <linux/fs.h>

/* Cap a single allocation so one iomap_begin doesn't scan/claim too much. */
#define OCSFS2_ALLOC_CAP_BLOCKS  2048u   /* 8 MiB at 4 KiB blocks */

static int ocsfs2_iomap_begin(struct inode *inode, loff_t pos, loff_t length,
			      unsigned flags, struct iomap *iomap,
			      struct iomap *srcmap)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(inode->i_sb);
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	u32 bs = sbi->s_block_size;
	u64 lblk = (u64)pos / bs;
	u64 end_blk = ((u64)pos + length + bs - 1) / bs;   /* exclusive */
	struct ocsfs2_extent cover;
	u64 next_logical = U64_MAX;
	unsigned int nofs;
	int ret;

	iomap->bdev = inode->i_sb->s_bdev;
	iomap->offset = lblk * (u64)bs;

	mutex_lock(&oi->i_meta_lock);
	nofs = memalloc_nofs_save();

	ret = ocsfs2_extent_find(inode, lblk, &cover, &next_logical);
	if (ret == 0) {
		u64 off_in = lblk - cover.logical;
		u64 remaining = cover.length - off_in;

		iomap->addr = (cover.physical + off_in) * (u64)bs;
		iomap->length = remaining * (u64)bs;
		iomap->type = (cover.flags & OCSFS2_EXT_UNWRITTEN) ?
			      IOMAP_UNWRITTEN : IOMAP_MAPPED;
		ret = 0;
		goto out;
	}

	if (flags & IOMAP_WRITE) {
		u64 want = end_blk - lblk;
		u64 phys;
		u32 alloc;

		if (next_logical != U64_MAX && next_logical - lblk < want)
			want = next_logical - lblk;     /* don't overlap the next extent */
		if (want > OCSFS2_ALLOC_CAP_BLOCKS)
			want = OCSFS2_ALLOC_CAP_BLOCKS;
		if (want == 0)
			want = 1;

		ret = ocsfs2_alloc_blocks(inode->i_sb, oi->i_ag, (u32)want, &phys);
		if (ret == -ENOSPC && want > 1) {
			want = 1;
			ret = ocsfs2_alloc_blocks(inode->i_sb, oi->i_ag, 1, &phys);
		}
		if (ret)
			goto out;
		alloc = (u32)want;

		ret = ocsfs2_extent_insert(inode, lblk, phys, alloc, OCSFS2_EXT_WRITTEN);
		if (ret) {
			ocsfs2_free_blocks(inode->i_sb, phys, alloc);
			goto out;
		}
		inode->i_blocks += (u64)alloc * (bs / 512);

		iomap->addr = phys * (u64)bs;
		iomap->length = (u64)alloc * bs;
		iomap->type = IOMAP_MAPPED;
		iomap->flags |= IOMAP_F_NEW;
		ret = 0;
		goto out;
	}

	/* read / zero of a hole — clamp to the next extent (never swallow it) */
	{
		u64 hole_end = (next_logical != U64_MAX && next_logical < end_blk)
			       ? next_logical : end_blk;

		if (hole_end <= lblk)
			hole_end = lblk + 1;
		iomap->addr = IOMAP_NULL_ADDR;
		iomap->length = (hole_end - lblk) * (u64)bs;
		iomap->type = IOMAP_HOLE;
		ret = 0;
	}
out:
	memalloc_nofs_restore(nofs);
	mutex_unlock(&oi->i_meta_lock);
	return ret;
}

static const struct iomap_ops ocsfs2_iomap_ops = {
	.iomap_begin = ocsfs2_iomap_begin,
};

/* ── address_space operations ── */

static int ocsfs2_read_folio(struct file *file, struct folio *folio)
{
	iomap_bio_read_folio(folio, &ocsfs2_iomap_ops);
	return 0;
}

static void ocsfs2_readahead(struct readahead_control *rac)
{
	iomap_bio_readahead(rac, &ocsfs2_iomap_ops);
}

static ssize_t ocsfs2_writeback_range(struct iomap_writepage_ctx *wpc,
				      struct folio *folio, u64 pos,
				      unsigned int len, u64 end_pos)
{
	int ret = ocsfs2_iomap_begin(wpc->inode, pos,
				     wpc->inode->i_sb->s_blocksize,
				     0, &wpc->iomap, NULL);
	if (ret < 0)
		return ret;
	/* a dirty folio always has blocks allocated from the write path; a HOLE
	 * here would just be skipped by iomap (Plan 2 has no punch/truncate race) */
	return iomap_add_to_ioend(wpc, folio, pos, end_pos, len);
}

static const struct iomap_writeback_ops ocsfs2_writeback_ops = {
	.writeback_range  = ocsfs2_writeback_range,
	.writeback_submit = iomap_ioend_writeback_submit,
};

static int ocsfs2_writepages(struct address_space *mapping,
			     struct writeback_control *wbc)
{
	struct iomap_writepage_ctx wpc = {
		.inode = mapping->host,
		.wbc   = wbc,
		.ops   = &ocsfs2_writeback_ops,
	};

	return iomap_writepages(&wpc);
}

static sector_t ocsfs2_bmap_aop(struct address_space *mapping, sector_t bno)
{
	return iomap_bmap(mapping, bno, &ocsfs2_iomap_ops);
}

const struct address_space_operations ocsfs2_file_aops = {
	.read_folio       = ocsfs2_read_folio,
	.readahead        = ocsfs2_readahead,
	.writepages       = ocsfs2_writepages,
	.dirty_folio      = iomap_dirty_folio,
	.invalidate_folio = iomap_invalidate_folio,
	.release_folio    = iomap_release_folio,
	.bmap             = ocsfs2_bmap_aop,
	.migrate_folio    = filemap_migrate_folio,
};

/* ── file read / write ── */

ssize_t ocsfs2_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	if (iocb->ki_flags & IOCB_DIRECT) {
		struct inode *inode = file_inode(iocb->ki_filp);
		ssize_t ret;

		inode_lock_shared(inode);
		ret = iomap_dio_rw(iocb, to, &ocsfs2_iomap_ops, NULL, 0, NULL, 0);
		inode_unlock_shared(inode);
		return ret;
	}
	return filemap_read(iocb, to, 0);
}

ssize_t ocsfs2_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	if (iocb->ki_flags & IOCB_DIRECT) {
		ret = iomap_dio_rw(iocb, from, &ocsfs2_iomap_ops, NULL, 0, NULL, 0);
		/* iomap_file_buffered_write extends i_size itself; iomap_dio_rw
		 * does not — the FS owns it. For a synchronous O_DIRECT write
		 * ki_pos is advanced to the end offset, so grow i_size to match. */
		if (ret > 0 && iocb->ki_pos > i_size_read(inode))
			i_size_write(inode, iocb->ki_pos);
	} else {
		ret = iomap_file_buffered_write(iocb, from, &ocsfs2_iomap_ops,
						NULL, NULL);
	}

	if (ret > 0) {
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
		mark_inode_dirty(inode);
	}
out:
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}

static int ocsfs2_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file->f_mapping->host;
	int ret;

	ret = file_write_and_wait_range(file, start, end);
	if (ret)
		return ret;
	if (!datasync || (inode_state_read(inode) & I_DIRTY_DATASYNC))
		ret = sync_inode_metadata(inode, 1);
	return ret;
}

/* O_DIRECT is opt-in per open: the VFS rejects open(O_DIRECT) unless the file
 * advertises FMODE_CAN_ODIRECT (the v1 ODIRECT-1 bug was the missing flag). */
static int ocsfs2_file_open(struct inode *inode, struct file *file)
{
	file->f_mode |= FMODE_CAN_ODIRECT;
	return generic_file_open(inode, file);
}

const struct file_operations ocsfs2_file_fops = {
	.open         = ocsfs2_file_open,
	.llseek       = generic_file_llseek,
	.read_iter    = ocsfs2_file_read_iter,
	.write_iter   = ocsfs2_file_write_iter,
	.fsync        = ocsfs2_fsync,
	.splice_read  = filemap_splice_read,
	.splice_write = iter_file_splice_write,
};
