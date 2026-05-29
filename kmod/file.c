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
#include <linux/fscrypt.h>
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
	int ret = fscrypt_file_open(inode, file);

	if (ret)
		return ret;
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
struct ocsfs_remap_ctx {
	struct inode         *dst;
	u64                   src_blk, dst_blk, end_blk;
	struct ocsfs_sb_info *sbi;
	int                   ret;
};

static int remap_extent_cb(u64 logical, u64 physical, u32 length,
			    u16 flags, void *priv)
{
	struct ocsfs_remap_ctx *rc = priv;
	u64 ov_s = max(logical, rc->src_blk);
	u64 ov_e = min(logical + (u64)length, rc->end_blk);
	u64 phys, log_dst;
	u32 clip;
	int ret;

	if (ov_s >= ov_e || !physical || (flags & OCSFS_EXT_UNWRITTEN))
		return 0;

	phys    = physical + (ov_s - logical);
	log_dst = rc->dst_blk + (ov_s - rc->src_blk);
	clip    = (u32)(ov_e - ov_s);

	ret = ocsfs_refcount_inc(rc->dst->i_sb, phys, clip);
	if (ret) { rc->ret = ret; return ret; }

	ret = ocsfs_extent_insert(rc->dst, log_dst, phys, clip,
				  OCSFS_EXT_WRITTEN);
	if (ret) {
		ocsfs_refcount_dec(rc->dst->i_sb, phys, clip, NULL);
		rc->ret = ret;
		return ret;
	}
	rc->dst->i_blocks += (u64)clip * (rc->sbi->s_block_size / 512);
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

	if (remap_flags & ~REMAP_FILE_CAN_SHORTEN)
		return -EINVAL;

	/* Sharing physical blocks between files with different fscrypt IVs
	 * (different ino or key) produces unreadable ciphertext. */
	if (IS_ENCRYPTED(src) || IS_ENCRYPTED(dst))
		return -EOPNOTSUPP;

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

	ret = generic_remap_file_range_prep(src_file, pos_in, dst_file, pos_out,
					    &remap_len, remap_flags);
	if (ret || remap_len == 0)
		goto out_unlock_dlm;

	if (!IS_ALIGNED(pos_in,    sbi->s_block_size) ||
	    !IS_ALIGNED(pos_out,   sbi->s_block_size) ||
	    !IS_ALIGNED(remap_len, sbi->s_block_size)) {
		ret = -EINVAL;
		goto out_unlock_dlm;
	}

	src_blk  = (u64)pos_in  / sbi->s_block_size;
	dst_blk  = (u64)pos_out / sbi->s_block_size;
	len_blks = (u64)remap_len / sbi->s_block_size;

	if (src_oi->i_disk_ino < dst_oi->i_disk_ino || src == dst) {
		mutex_lock(&src_oi->i_extent_lock);
		if (src != dst)
			mutex_lock_nested(&dst_oi->i_extent_lock,
					  SINGLE_DEPTH_NESTING);
	} else {
		mutex_lock(&dst_oi->i_extent_lock);
		mutex_lock_nested(&src_oi->i_extent_lock, SINGLE_DEPTH_NESTING);
	}

	if (src_oi->i_extent_tree_root) {
		struct ocsfs_remap_ctx rc = {
			dst, src_blk, dst_blk, src_blk + len_blks, sbi, 0
		};
		ocsfs_extent_btree_iterate(src, remap_extent_cb, &rc);
		ret = rc.ret;
	} else {
		ret = 0;
		for (i = 0; i < src_oi->i_extent_count && !ret; i++) {
			struct ocsfs_extent *e = &src_oi->i_extents[i];
			u64 ov_s = max(e->logical_block, src_blk);
			u64 ov_e = min(e->logical_block + (u64)e->length,
				       src_blk + len_blks);
			u64 phys, log_dst;
			u32 clip;

			if (ov_s >= ov_e || !e->physical_block ||
			    (e->flags & (OCSFS_EXT_UNWRITTEN |
					 OCSFS_EXT_COMPRESSED)))
				continue;

			phys    = e->physical_block + (ov_s - e->logical_block);
			log_dst = dst_blk + (ov_s - src_blk);
			clip    = (u32)(ov_e - ov_s);

			ret = ocsfs_refcount_inc(src->i_sb, phys, clip);
			if (ret) break;
			ret = ocsfs_extent_insert(dst, log_dst, phys, clip,
						  OCSFS_EXT_WRITTEN);
			if (ret) {
				ocsfs_refcount_dec(src->i_sb, phys, clip, NULL);
				break;
			}
			dst->i_blocks += (u64)clip * (sbi->s_block_size / 512);
		}
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

	if (src == dst) {
		mutex_unlock(&src_oi->i_extent_lock);
	} else if (src_oi->i_disk_ino < dst_oi->i_disk_ino) {
		mutex_unlock(&dst_oi->i_extent_lock);
		mutex_unlock(&src_oi->i_extent_lock);
	} else {
		mutex_unlock(&src_oi->i_extent_lock);
		mutex_unlock(&dst_oi->i_extent_lock);
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

static long ocsfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
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

	/* ARCH-V3-6: cluster-wide filesystem freeze / thaw */
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

	/* fscrypt key and policy management — requires CONFIG_FS_ENCRYPTION */
	switch (cmd) {
	case FS_IOC_SET_ENCRYPTION_POLICY:
		return fscrypt_ioctl_set_policy(file, (const void __user *)arg);
	case FS_IOC_GET_ENCRYPTION_POLICY_EX:
		return fscrypt_ioctl_get_policy_ex(file, (void __user *)arg);
	case FS_IOC_ADD_ENCRYPTION_KEY: {
		/* ARCH-V3-1: in cluster mode, persist the key in the shared
		 * encrypted key store so other nodes can retrieve and add it.
		 * We copy the raw key from userspace, encrypt it with
		 * ChaCha20-Poly1305 / cluster_secret, write to the LUN, then
		 * proceed with the standard fscrypt add-key path. */
		struct fscrypt_add_key_arg hdr;
		u8 raw_key[FSCRYPT_MAX_KEY_SIZE];
		struct ocsfs_sb_info *ks_sbi = OCSFS_SB(inode->i_sb);

		if (ks_sbi->s_clustered &&
		    (ks_sbi->s_feature_incompat & OCSFS_FEATURE_INCOMPAT_KEY_STORE)) {
			if (copy_from_user(&hdr, (void __user *)arg, sizeof(hdr))) {
				return -EFAULT;
			}
			if (hdr.raw_size > 0 && hdr.raw_size <= FSCRYPT_MAX_KEY_SIZE) {
				if (!copy_from_user(raw_key,
						    (u8 __user *)arg + sizeof(hdr),
						    hdr.raw_size)) {
					/* non-fatal: log on failure, proceed anyway */
					if (ocsfs_key_store_add(inode->i_sb,
								&hdr.key_spec,
								raw_key,
								(u16)hdr.raw_size))
						pr_warn_ratelimited(
							"ocsfs: key_store_add failed — "
							"key not persisted to cluster store\n");
				}
				memzero_explicit(raw_key, sizeof(raw_key));
			}
		}
		return fscrypt_ioctl_add_key(file, (void __user *)arg);
	}
	case FS_IOC_REMOVE_ENCRYPTION_KEY:
		return fscrypt_ioctl_remove_key(file, (void __user *)arg);
	case FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS:
		return fscrypt_ioctl_remove_key_all_users(file, (void __user *)arg);
	case FS_IOC_GET_ENCRYPTION_KEY_STATUS:
		return fscrypt_ioctl_get_key_status(file, (void __user *)arg);
	case FS_IOC_GET_ENCRYPTION_NONCE:
		return fscrypt_ioctl_get_nonce(file, (void __user *)arg);
	case OCSFS_IOC_KEY_LIST: {
		/* List key identifiers stored in the shared key store.
		 * Requires CAP_SYS_ADMIN: enumerating which key IDs are present
		 * reveals the cluster encryption topology without needing raw key
		 * material, which is more powerful than fscrypt's own KEY_STATUS
		 * ioctl (that requires prior knowledge of the ID). */
		struct ocsfs_key_list_arg kla;
		u32 count = 0;
		int kret;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;

		memset(&kla, 0, sizeof(kla));
		kret = ocsfs_key_store_list(inode->i_sb, kla.kla_keys,
					    OCSFS_KEY_STORE_MAX_ENTRIES, &count);
		if (kret)
			return kret;
		kla.kla_count = count;
		if (copy_to_user((void __user *)arg, &kla, sizeof(kla)))
			return -EFAULT;
		return 0;
	}
	case OCSFS_IOC_KEY_FETCH: {
		/* Fetch decrypted raw key material for a given identifier.
		 * Requires CAP_SYS_ADMIN — exposes raw key material. */
		struct ocsfs_key_fetch_arg kfa;
		u8 raw_key2[FSCRYPT_MAX_KEY_SIZE];
		u16 key_size = 0;
		int kret;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (copy_from_user(&kfa, (void __user *)arg, sizeof(kfa)))
			return -EFAULT;

		memset(raw_key2, 0, sizeof(raw_key2));
		kret = ocsfs_key_store_fetch(inode->i_sb, kfa.kfa_id,
					     raw_key2, &key_size);
		if (kret) {
			memzero_explicit(raw_key2, sizeof(raw_key2));
			return kret;
		}
		kfa.kfa_key_size = key_size;
		memcpy(kfa.kfa_key, raw_key2, key_size);
		memzero_explicit(raw_key2, sizeof(raw_key2));
		if (copy_to_user((void __user *)arg, &kfa, sizeof(kfa))) {
			memzero_explicit(&kfa, sizeof(kfa));
			return -EFAULT;
		}
		memzero_explicit(&kfa, sizeof(kfa));
		return 0;
	}
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
	return generic_file_mmap(file, vma);
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
	.remap_file_range = ocsfs_remap_file_range,
	.unlocked_ioctl   = ocsfs_ioctl,
	.lock             = ocsfs_file_lock,         /* POSIX distributed locking (flock.c) */
};
