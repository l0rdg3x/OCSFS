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
#include <linux/pagemap.h>

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

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_SH);
		if (ret)
			return ret;
		ret = ocsfs_inode_refresh(inode);
		if (ret) {
			ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
			return ret;
		}
	}

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
	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
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
	int ret = generic_file_open(inode, file);

	if (ret)
		return ret;
	/*
	 * Advertise O_DIRECT support.  Our read/write iterators dispatch to
	 * iomap_dio_rw() on IOCB_DIRECT, but since we have no legacy
	 * a_ops->direct_IO method the VFS would reject every O_DIRECT open with
	 * -EINVAL unless we set FMODE_CAN_ODIRECT here (same as ext4/xfs/btrfs).
	 */
	file->f_mode |= FMODE_CAN_ODIRECT;
	return 0;
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
	 * In cluster mode we must hold DLM EX for the entire compress to
	 * avoid a race where a peer reads an extent map mid-rewrite.
	 */
	if (ocsfs_get_compression_algo(inode) != OCSFS_COMPRESS_NONE) {
		struct ocsfs_inode_info *oi = OCSFS_I(inode);
		struct ocsfs_sb_info    *sbi_c = OCSFS_SB(inode->i_sb);
		int cr;

		if (sbi_c->s_clustered) {
			cr = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
						OCSFS_LOCK_EX);
			if (cr) {
				pr_warn_ratelimited(
					"ocsfs: compress_file: DLM EX failed "
					"(%d), skipping compression\n", cr);
				goto after_compress;
			}
		}

		cr = ocsfs_compress_file(inode);
		if (cr)
			pr_warn_ratelimited("ocsfs: compress_file failed (%d), "
					    "syncing uncompressed\n", cr);

		if (sbi_c->s_clustered) {
			ocsfs_flush_inode_locked(inode, false);
			ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
		}
after_compress:;
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
/* REMAP FILE RANGE — extent sharing for cp --reflink / FICLONE (DEDUP not supported) */
/* Collect-first reflink: the source's extents are gathered into a local array
 * (read-only) BEFORE the destination is cleared and the shared extents are
 * inserted.  This is required when src == dst (fsx clones within one file):
 * iterating the source extent tree while inserting into the same tree would
 * corrupt the walk.  Source holes / UNWRITTEN ranges are simply not collected,
 * so after the dst is cleared they remain holes in the destination (the dest
 * mirrors the source exactly). */
struct ocsfs_remap_ent {
	u64 phys;
	u64 log_dst;
	u32 clip;
};

struct ocsfs_remap_collect {
	struct ocsfs_remap_ent *ents;
	u32 n;
	u32 cap;
	u64 src_blk, dst_blk, end_blk;
	int ret;
};

/* First pass: count shareable source extents overlapping the range. */
static int remap_count_cb(u64 logical, u64 physical, u32 length,
			  u16 flags, void *priv)
{
	struct ocsfs_remap_collect *c = priv;
	u64 ov_s = max(logical, c->src_blk);
	u64 ov_e = min(logical + (u64)length, c->end_blk);

	if (ov_s < ov_e && physical &&
	    !(flags & (OCSFS_EXT_UNWRITTEN | OCSFS_EXT_COMPRESSED)))
		c->cap++;
	return 0;
}

/* Second pass: record them (no map mutation, no refcount/quota yet). */
static int remap_collect_cb(u64 logical, u64 physical, u32 length,
			    u16 flags, void *priv)
{
	struct ocsfs_remap_collect *c = priv;
	u64 ov_s = max(logical, c->src_blk);
	u64 ov_e = min(logical + (u64)length, c->end_blk);

	if (ov_s >= ov_e || !physical ||
	    (flags & (OCSFS_EXT_UNWRITTEN | OCSFS_EXT_COMPRESSED)))
		return 0;
	if (c->n >= c->cap) {            /* sized from the count pass; be safe */
		c->ret = -ENOSPC;
		return -ENOSPC;
	}
	c->ents[c->n].phys    = physical + (ov_s - logical);
	c->ents[c->n].log_dst = c->dst_blk + (ov_s - c->src_blk);
	c->ents[c->n].clip    = (u32)(ov_e - ov_s);
	c->n++;
	return 0;
}

static loff_t ocsfs_remap_file_range(struct file *src_file, loff_t pos_in,
				     struct file *dst_file, loff_t pos_out,
				     loff_t remap_len, unsigned int remap_flags)
{
	struct inode *src = file_inode(src_file);
	struct inode *dst = file_inode(dst_file);
	struct ocsfs_inode_info *src_oi = OCSFS_I(src);
	struct ocsfs_inode_info *dst_oi = OCSFS_I(dst);
	struct ocsfs_sb_info *sbi = OCSFS_SB(src->i_sb);
	u64 src_blk, dst_blk, len_blks;
	loff_t ret;
	u16 i;

	/* Dedup (FIDEDUPERANGE / REMAP_FILE_DEDUP) is not supported: report
	 * -EOPNOTSUPP (not -EINVAL) so callers treat it as "unsupported, skip"
	 * rather than a hard error. */
	if (remap_flags & REMAP_FILE_DEDUP)
		return -EOPNOTSUPP;
	if (remap_flags & ~REMAP_FILE_CAN_SHORTEN)
		return -EINVAL;

	/* Reflink charges the destination's block quota for every shared block
	 * (logical accounting, like XFS): the clone counts at full size even
	 * though the blocks are physically shared.  Load dst's dquots so the
	 * per-extent dquot_alloc_space below can enforce the limit. */
	dquot_initialize(dst);

	lock_two_nondirectories(src, dst);

	if (sbi->s_clustered) {
		/* Acquire EX in ino order to avoid deadlock with concurrent remaps. */
		struct ocsfs_lock_res *lr_lo, *lr_hi;

		if (src_oi->i_disk_ino <= dst_oi->i_disk_ino) {
			lr_lo = &src_oi->i_lock_res;
			lr_hi = &dst_oi->i_lock_res;
		} else {
			lr_lo = &dst_oi->i_lock_res;
			lr_hi = &src_oi->i_lock_res;
		}
		ret = ocsfs_lock_acquire(src->i_sb, lr_lo, OCSFS_LOCK_EX);
		if (ret)
			goto out_unlock_vfs;
		if (src != dst) {
			ret = ocsfs_lock_acquire(src->i_sb, lr_hi, OCSFS_LOCK_EX);
			if (ret) {
				ocsfs_lock_release(src->i_sb, lr_lo);
				goto out_unlock_vfs;
			}
		}
		ret = ocsfs_inode_refresh(src);
		if (!ret && src != dst)
			ret = ocsfs_inode_refresh(dst);
		if (ret) {
			if (src != dst)
				ocsfs_lock_release(src->i_sb, lr_hi);
			ocsfs_lock_release(src->i_sb, lr_lo);
			goto out_unlock_vfs;
		}
	}

	/* Reflink shares whole blocks: it cannot serve a sub-block-aligned
	 * source/dest offset.  Reject BEFORE generic_remap_file_range_prep()
	 * (which would itself return -EINVAL) using -EOPNOTSUPP, so that
	 * vfs_copy_file_range() — which tries ->remap_file_range first for
	 * same-superblock copies — falls back to a normal (splice) copy instead
	 * of propagating the error.  Otherwise an unaligned copy_file_range()
	 * fails with EINVAL (caught by fsx).  A direct unaligned FICLONE gets
	 * -EOPNOTSUPP, which callers treat as "clone unsupported, fall back". */
	if (!IS_ALIGNED(pos_in, sbi->s_block_size) ||
	    !IS_ALIGNED(pos_out, sbi->s_block_size)) {
		ret = -EOPNOTSUPP;
		goto out_unlock_dlm;
	}

	ret = generic_remap_file_range_prep(src_file, pos_in, dst_file, pos_out,
					    &remap_len, remap_flags);
	if (ret || remap_len == 0)
		goto out_unlock_dlm;

	/* len may still be sub-block after prep (e.g. shortened to EOF); a
	 * whole-block clone cannot represent the tail, so fall back to copy. */
	if (!IS_ALIGNED(remap_len, sbi->s_block_size)) {
		ret = -EOPNOTSUPP;
		goto out_unlock_dlm;
	}

	src_blk  = (u64)pos_in  / sbi->s_block_size;
	dst_blk  = (u64)pos_out / sbi->s_block_size;
	len_blks = (u64)remap_len / sbi->s_block_size;

	/* Phase 1: collect the source's shareable extents into a local array,
	 * READ-ONLY.  Required when src == dst: we must not iterate the source
	 * extent tree while inserting clones into the same tree.  Source holes /
	 * UNWRITTEN ranges are not collected, so after the dst is cleared (phase
	 * 2) they remain holes in the destination. */
	{
		struct ocsfs_remap_collect c = {
			.src_blk = src_blk,
			.dst_blk = dst_blk,
			.end_blk = src_blk + len_blks,
		};
		u32 k;

		mutex_lock(&src_oi->i_extent_lock);
		if (src_oi->i_extent_tree_root) {
			ocsfs_extent_btree_iterate(src, remap_count_cb, &c);
		} else {
			for (i = 0; i < src_oi->i_extent_count; i++)
				remap_count_cb(src_oi->i_extents[i].logical_block,
					src_oi->i_extents[i].physical_block,
					src_oi->i_extents[i].length,
					src_oi->i_extents[i].flags, &c);
		}
		if (c.cap) {
			c.ents = kvmalloc_array(c.cap, sizeof(*c.ents), GFP_NOFS);
			if (!c.ents) {
				mutex_unlock(&src_oi->i_extent_lock);
				ret = -ENOMEM;
				goto out_unlock_dlm;
			}
			if (src_oi->i_extent_tree_root) {
				ocsfs_extent_btree_iterate(src, remap_collect_cb,
							   &c);
			} else {
				for (i = 0; i < src_oi->i_extent_count; i++)
					remap_collect_cb(
						src_oi->i_extents[i].logical_block,
						src_oi->i_extents[i].physical_block,
						src_oi->i_extents[i].length,
						src_oi->i_extents[i].flags, &c);
			}
		}
		mutex_unlock(&src_oi->i_extent_lock);
		ret = c.ret;
		if (ret) {
			kvfree(c.ents);
			goto out_unlock_dlm;
		}

		/* Phase 2: turn the dst range into a clean hole (refcount-aware),
		 * so the source's holes are reflected in the destination. */
		ret = ocsfs_clear_block_range(dst, dst_blk, dst_blk + len_blks);
		if (ret) {
			kvfree(c.ents);
			goto out_unlock_dlm;
		}

		/* Phase 3: insert the collected shared extents into the dst. */
		mutex_lock(&dst_oi->i_extent_lock);
		for (k = 0; k < c.n && !ret; k++) {
			struct ocsfs_remap_ent *e = &c.ents[k];

			ret = dquot_alloc_space_nodirty(dst,
					(u64)e->clip * sbi->s_block_size);
			if (ret)
				break;
			ret = ocsfs_refcount_inc(dst->i_sb, e->phys, e->clip);
			if (ret) {
				dquot_free_space_nodirty(dst,
					(u64)e->clip * sbi->s_block_size);
				break;
			}
			ret = ocsfs_extent_insert(dst, e->log_dst, e->phys,
						  e->clip, OCSFS_EXT_WRITTEN);
			if (ret) {
				ocsfs_refcount_dec(dst->i_sb, e->phys, e->clip,
						   NULL);
				dquot_free_space_nodirty(dst,
					(u64)e->clip * sbi->s_block_size);
				break;
			}
			dst->i_blocks += (u64)e->clip * (sbi->s_block_size / 512);
		}

		if (!ret) {
			if (pos_out + remap_len > i_size_read(dst))
				i_size_write(dst, pos_out + remap_len);
			mark_inode_dirty(dst);
			if (sbi->s_clustered) {
				ocsfs_flush_inode_locked(dst, true);
				if (src != dst)
					ocsfs_flush_inode_locked(src, true);
			}
		}
		mutex_unlock(&dst_oi->i_extent_lock);
		kvfree(c.ents);
	}

	/* Evict stale clean pages over the destination range.  The extent map
	 * now points at the (shared) source blocks, but clean cached pages from
	 * before the clone still hold the destination's old contents, so a
	 * subsequent buffered read would return stale data.
	 * generic_remap_file_range_prep() only writes back *dirty* pages before
	 * the remap; it never evicts clean pages afterwards — that is the
	 * filesystem's job (cf. ext4/xfs).  Done outside i_extent_lock to avoid
	 * an ABBA with writeback (which takes i_extent_lock under a page lock),
	 * while i_rwsem EX (held via lock_two_nondirectories) keeps writers out.
	 * Caught by fsx / xfstests generic/075,091,112,127,263. */
	if (remap_len > 0) {
		filemap_invalidate_lock(dst->i_mapping);
		invalidate_inode_pages2_range(dst->i_mapping,
			pos_out >> PAGE_SHIFT,
			(pos_out + remap_len - 1) >> PAGE_SHIFT);
		filemap_invalidate_unlock(dst->i_mapping);
	}

out_unlock_dlm:
	if (sbi->s_clustered) {
		struct ocsfs_lock_res *lr_lo, *lr_hi;

		if (src_oi->i_disk_ino <= dst_oi->i_disk_ino) {
			lr_lo = &src_oi->i_lock_res;
			lr_hi = &dst_oi->i_lock_res;
		} else {
			lr_lo = &dst_oi->i_lock_res;
			lr_hi = &src_oi->i_lock_res;
		}
		if (src != dst)
			ocsfs_lock_release(src->i_sb, lr_hi);
		ocsfs_lock_release(src->i_sb, lr_lo);
	}

out_unlock_vfs:
	unlock_two_nondirectories(src, dst);
	return ret ? ret : remap_len;
}

long ocsfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mnt_idmap *idmap = file_mnt_idmap(file);
	struct inode *inode = file_inode(file);
	struct ocsfs_snap_arg sa;
	struct inode *dir;
	struct qstr qname;
	int ret;

	if (cmd == OCSFS_IOC_SNAP_DELETE) {
		if (!inode_owner_or_capable(idmap, inode))
			return -EPERM;
		return ocsfs_snapshot_delete(inode);
	}

	if (cmd == OCSFS_IOC_DEDUP) {
		struct ocsfs_inode_info *oi = OCSFS_I(inode);
		struct ocsfs_dedup_result res;
		unsigned long now = jiffies;

		if (!inode_owner_or_capable(idmap, inode))
			return -EPERM;

		/* SEC-V3-8: Per-inode rate-limit to prevent a file owner from
		 * triggering unbounded dedup scans on large files as a DoS.
		 * Minimum interval: 60 s. */
		if (oi->i_dedup_last_jiffies &&
		    time_before(now, oi->i_dedup_last_jiffies + 60 * HZ))
			return -EBUSY;
		oi->i_dedup_last_jiffies = now;

		ret = ocsfs_dedup_file(inode, &res.bytes_deduped);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &res, sizeof(res)))
			return -EFAULT;
		return 0;
	}

	if (cmd == OCSFS_IOC_DEDUP_GC) {
		u64 total = 0;
		int passes = 0;

		/* Whole-FS reclaim of index-only cross-file canonicals. */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (!(OCSFS_SB(inode->i_sb)->s_feature_ro_compat &
		      OCSFS_FEATURE_RO_COMPAT_DEDUP_INDEX))
			return -EOPNOTSUPP;

		/* Each call reclaims one bounded batch; loop until a pass frees
		 * nothing.  Cap the pass count as a runaway backstop. */
		for (passes = 0; passes < 100000; passes++) {
			u64 freed = 0;

			ret = ocsfs_dedup_index_gc(inode->i_sb, &freed);
			if (ret)
				return ret;
			if (freed == 0)
				break;
			total += freed;
		}
		if (copy_to_user((void __user *)arg, &total, sizeof(total)))
			return -EFAULT;
		return 0;
	}

	/* ARCH-V3-6: cluster-wide filesystem freeze / thaw */
	if (cmd == OCSFS_IOC_GROW) {
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		return ocsfs_grow_online(inode->i_sb);
	}
	if (cmd == OCSFS_IOC_FREEZE_FS) {
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		return freeze_super(inode->i_sb, FREEZE_HOLDER_USERSPACE, NULL);
	}
	if (cmd == OCSFS_IOC_THAW_FS) {
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		return thaw_super(inode->i_sb, FREEZE_HOLDER_USERSPACE, NULL);
	}

	/* VAAI offload commands — require CAP_SYS_ADMIN or device owner */
	if (cmd == OCSFS_IOC_WRITE_SAME) {
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		return ocsfs_vaai_write_same(inode,
					     (const struct ocsfs_vaai_arg __user *)arg);
	}

	if (cmd == OCSFS_IOC_UNMAP) {
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		return ocsfs_vaai_unmap(inode,
					(const struct ocsfs_vaai_arg __user *)arg);
	}

	if (cmd == OCSFS_IOC_XCOPY) {
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		return ocsfs_vaai_xcopy(inode->i_sb,
					(const struct ocsfs_vaai_xcopy_arg __user *)arg);
	}

	if (cmd != OCSFS_IOC_SNAP_CREATE)
		return -ENOTTY;

	/* SEC-V3-7: Snapshot creation mutates metadata; reject on read-only mounts. */
	if (inode->i_sb->s_flags & SB_RDONLY)
		return -EROFS;

	/* Caller must own (or be capable of) the source file */
	if (!inode_owner_or_capable(idmap, inode))
		return -EPERM;

	if (copy_from_user(&sa, (void __user *)arg, sizeof(sa)))
		return -EFAULT;
	sa.name[OCSFS_SNAP_NAME_MAX] = '\0';
	qname.len = strnlen(sa.name, OCSFS_SNAP_NAME_MAX);
	if (!qname.len)
		return -EINVAL;
	qname.name = sa.name;
	if (sa.dir_ino == 0)
		return -EINVAL;
	dir = ocsfs_iget(inode->i_sb, sa.dir_ino);
	if (IS_ERR(dir))
		return PTR_ERR(dir);
	if (!S_ISDIR(dir->i_mode)) {
		iput(dir);
		return -ENOTDIR;
	}
	ret = inode_permission(idmap, dir, MAY_WRITE | MAY_EXEC);
	if (ret) {
		iput(dir);
		return ret;
	}
	ret = ocsfs_snapshot_create(inode, dir, &qname);
	iput(dir);
	return ret;
}
/*
 * mmap write fault: go through iomap so that a write to a mapped page
 * allocates the backing blocks (iomap_begin with IOMAP_WRITE) and sets the
 * iomap per-block folio state, keeping mmap coherent with the iomap
 * read_iter/write_iter/fallocate paths.  Using the generic
 * filemap_page_mkwrite instead leaves an mmap write to a hole with no backing
 * blocks — the dirty data is then lost on writeback (caught by fsx mmap ops).
 */
static vm_fault_t ocsfs_page_mkwrite(struct vm_fault *vmf)
{
	return iomap_page_mkwrite(vmf, &ocsfs_iomap_ops, NULL);
}

static const struct vm_operations_struct ocsfs_file_vm_ops = {
	.fault        = filemap_fault,
	.map_pages    = filemap_map_pages,
	.page_mkwrite = ocsfs_page_mkwrite,
};

static int ocsfs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	/*
	 * In cluster mode, writable mmap requires DLM EX on every page fault
	 * (page_mkwrite), which is not yet implemented. Shared writable
	 * mappings would silently bypass inter-node coherence; disallow them.
	 * Read-only and private mappings are safe: COW semantics mean dirty
	 * pages never propagate back to the shared SAN block.
	 */
	if (OCSFS_SB(file_inode(file)->i_sb)->s_clustered &&
	    (vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_WRITE))
		return -EOPNOTSUPP;

	file_accessed(file);
	vma->vm_ops = &ocsfs_file_vm_ops;
	return 0;
}

const struct file_operations ocsfs_file_fops = {
	.llseek           = ocsfs_file_llseek,
	.read_iter        = ocsfs_file_read_iter,   /* iomap-based (iomap.c) */
	.write_iter       = ocsfs_file_write_iter,  /* iomap-based (iomap.c) */
	.mmap             = ocsfs_file_mmap,
	.open             = ocsfs_open,
	.fsync            = ocsfs_fsync,
	.fallocate        = ocsfs_fallocate,         /* thin.c */
	.splice_read      = filemap_splice_read,
	.splice_write     = iter_file_splice_write,  /* needed for copy_file_range
						      * splice fallback: do_splice_from
						      * returns -EINVAL without it */
	.remap_file_range = ocsfs_remap_file_range,
	.unlocked_ioctl   = ocsfs_ioctl,
	.lock             = ocsfs_file_lock,         /* POSIX distributed locking (flock.c) */
};
