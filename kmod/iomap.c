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
#include <linux/fscrypt.h>

/* OCSFS_MIN_PREALLOC_BLOCKS defined in ocsfs.h */

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
		loff_t mapped_len;

		/*
		 * Decompress before write: writing raw data into a compressed
		 * extent would leave the COMPRESSED flag set with uncompressed
		 * data on disk, corrupting subsequent reads.  Decompress in
		 * place so the caller sees a plain WRITTEN extent.
		 */
		if ((flags & IOMAP_WRITE) &&
		    (ext.flags & OCSFS_EXT_COMPRESSED)) {
			ret = ocsfs_extent_decompress_for_write(inode,
								logical_block);
			if (ret) {
				mutex_unlock(&oi->i_extent_lock);
				return ret;
			}
			ret = ocsfs_extent_lookup(inode, logical_block, &ext);
			if (ret || ext.physical_block == 0) {
				mutex_unlock(&oi->i_extent_lock);
				return ret ? ret : -EIO;
			}
			offset_in_ext    = logical_block - ext.logical_block;
			remaining_blocks = ext.length - offset_in_ext;
		}

		/*
		 * CoW: if writing to a shared extent (refcount > 1 from a
		 * snapshot), copy the blocks before mapping them for write.
		 * Caller (ocsfs_file_write_iter) holds DLM EX; we hold
		 * i_extent_lock — satisfies ocsfs_cow_extent's contract.
		 * Re-lookup after CoW since the extent map changed.
		 */
		if ((flags & IOMAP_WRITE) &&
		    !(ext.flags & OCSFS_EXT_UNWRITTEN) &&
		    ocsfs_needs_cow(inode->i_sb, ext.physical_block)) {
			u32 cow_blocks = min_t(u32, (u32)remaining_blocks,
				(u32)((length + sbi->s_block_size - 1) /
				      sbi->s_block_size));

			ret = ocsfs_cow_extent(inode, logical_block, cow_blocks);
			if (ret) {
				mutex_unlock(&oi->i_extent_lock);
				return ret;
			}
			ret = ocsfs_extent_lookup(inode, logical_block, &ext);
			if (ret || ext.physical_block == 0) {
				mutex_unlock(&oi->i_extent_lock);
				return ret ? ret : -EIO;
			}
			offset_in_ext    = logical_block - ext.logical_block;
			remaining_blocks = ext.length - offset_in_ext;
		}

		mapped_len = (loff_t)remaining_blocks * sbi->s_block_size;
		iomap->addr = (ext.physical_block + offset_in_ext) *
			      (u64)sbi->s_block_size;
		iomap->length = min_t(loff_t, length, mapped_len);
		iomap->bdev = inode->i_sb->s_bdev;
		iomap->offset = pos;

		if (ext.flags & OCSFS_EXT_UNWRITTEN)
			iomap->type = IOMAP_UNWRITTEN;
		else
			iomap->type = IOMAP_MAPPED;

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
		u32 write_blocks, try_blocks;

		/*
		 * Staged fallback: try OCSFS_MIN_PREALLOC_BLOCKS first to
		 * amortize txn overhead across many small writes (e.g. VM I/O).
		 * If pre-alloc fails, retry with the exact write size, then
		 * with a single block as last resort.
		 */
		write_blocks = max_t(u32,
			(length + sbi->s_block_size - 1) / sbi->s_block_size,
			1);
		try_blocks = max_t(u32, write_blocks, OCSFS_MIN_PREALLOC_BLOCKS);

		ret = ocsfs_alloc_blocks(inode->i_sb, oi->i_ag,
					 try_blocks, &phys);
		if (ret && try_blocks > write_blocks) {
			try_blocks = write_blocks;
			ret = ocsfs_alloc_blocks(inode->i_sb, oi->i_ag,
						 try_blocks, &phys);
		}
		if (ret && try_blocks > 1) {
			try_blocks = 1;
			ret = ocsfs_alloc_blocks(inode->i_sb, oi->i_ag,
						 1, &phys);
		}
		if (ret) {
			mutex_unlock(&oi->i_extent_lock);
			return ret;
		}

		alloc_blocks = try_blocks;

		/* Charge block quota before inserting the extent */
		ret = dquot_alloc_space_nodirty(inode,
			(u64)alloc_blocks * sbi->s_block_size);
		if (ret) {
			ocsfs_free_blocks(inode->i_sb, phys, alloc_blocks);
			mutex_unlock(&oi->i_extent_lock);
			return ret;
		}

		/*
		 * Insert as UNWRITTEN so that blocks not yet reached by the
		 * write are never exposed as MAPPED.  iomap_end converts the
		 * written portion to WRITTEN only after confirmed I/O, preventing
		 * reads of preallocated-but-unwritten blocks from returning stale
		 * data from a previously freed file (information leak).
		 *
		 * Crash window: if the node crashes between this extent_insert
		 * and iomap_end, the allocated blocks remain UNWRITTEN on disk.
		 * i_size is never advanced until iomap_end → i_size_write, so no
		 * data is exposed.  fsck identifies them as allocated-but-
		 * unreferenced blocks (refcount=1, no inode extent points here)
		 * and reclaims them during repair.
		 */
		ret = ocsfs_extent_insert(inode, logical_block, phys,
					  alloc_blocks, OCSFS_EXT_UNWRITTEN);
		if (ret) {
			ocsfs_free_blocks(inode->i_sb, phys, alloc_blocks);
			dquot_free_space_nodirty(inode,
				(u64)alloc_blocks * sbi->s_block_size);
			mutex_unlock(&oi->i_extent_lock);
			return ret;
		}

		inode->i_blocks += (u64)alloc_blocks *
				   (sbi->s_block_size / 512);

		iomap->addr = phys * (u64)sbi->s_block_size;
		iomap->length = (loff_t)alloc_blocks * sbi->s_block_size;
		iomap->type = IOMAP_UNWRITTEN;
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
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);

	if ((flags & IOMAP_WRITE) && written > 0) {
		/*
		 * Convert UNWRITTEN→WRITTEN only after confirmed I/O.
		 * Doing this in iomap_begin was premature: if the write
		 * failed, the extent would be WRITTEN over garbage data.
		 */
		if (iomap->type == IOMAP_UNWRITTEN) {
			u64 start_block = pos / sbi->s_block_size;
			u32 nblocks = (u32)((written + sbi->s_block_size - 1) /
					    sbi->s_block_size);
			int cr;

			mutex_lock(&oi->i_extent_lock);
			cr = ocsfs_extent_convert_unwritten(inode,
							    start_block,
							    nblocks);
			mutex_unlock(&oi->i_extent_lock);
			if (cr)
				pr_warn_ratelimited(
					"ocsfs: UNWRITTEN→WRITTEN failed (%d)\n",
					cr);
		}

		if (pos + written > inode->i_size)
			i_size_write(inode, pos + written);

		mark_inode_dirty(inode);
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
	struct inode *inode = file_inode(iocb->ki_filp);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	ssize_t ret;

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
		/*
		 * ARCH-7: selective page cache invalidation.
		 *
		 * - Same writer (local fast-path): our cache is coherent,
		 *   no invalidation needed.
		 * - Known range (lo < hi): invalidate only the pages the last
		 *   EX holder dirtied — avoids thrashing clean read-only pages.
		 * - No range info (lo == hi == 0): fall back to full invalidation
		 *   to guarantee correctness (epoch jumped >1 or first access).
		 */
		if (oi->i_last_writer_slot == sbi->s_node_slot) {
			/* we were the last EX holder: cache is still valid */
		} else if (oi->i_lock_res.lr_inv_lo < oi->i_lock_res.lr_inv_hi) {
			pgoff_t lo_pg = oi->i_lock_res.lr_inv_lo >> PAGE_SHIFT;
			pgoff_t hi_pg = (oi->i_lock_res.lr_inv_hi - 1) >> PAGE_SHIFT;

			invalidate_mapping_pages(inode->i_mapping, lo_pg, hi_pg);
		} else {
			invalidate_inode_pages2(inode->i_mapping);
		}
	}

	if (iocb->ki_flags & IOCB_DIRECT)
		ret = iomap_dio_rw(iocb, to, &ocsfs_dio_iomap_ops,
				   NULL, 0, NULL, 0);
	else
		ret = filemap_read(iocb, to, 0);

	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);

	return ret;
}

ssize_t ocsfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	ssize_t ret;
	loff_t start_pos = 0; /* ARCH-7: write start for dirty range tracking */

	/*
	 * Lock ordering: inode_lock → DLM EX.
	 *
	 * The VFS setattr path holds inode_lock and then acquires DLM EX
	 * inside ocsfs_setattr.  write_iter must acquire inode_lock first to
	 * avoid ABBA deadlock:
	 *   write_iter (DLM EX → inode_lock) vs setattr (inode_lock → DLM EX)
	 */
	inode_lock(inode);

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			goto out_unlock;
		/*
		 * ARCH-7 write-path selective invalidation.
		 *
		 * lock_acquire(EX) captured the previous EX holder's dirty range
		 * in lr_inv_lo/lr_inv_hi.  If we were the last writer our cache
		 * is coherent — skip.  Otherwise invalidate only the range that
		 * the previous EX holder actually dirtied; pages outside that
		 * range are still valid.  If no range was recorded, fall back to
		 * full invalidation.  After invalidation, zero lr_inv_lo/hi so
		 * iomap_end starts tracking only this write's range.
		 */
		if (oi->i_last_writer_slot != sbi->s_node_slot) {
			if (oi->i_lock_res.lr_inv_lo < oi->i_lock_res.lr_inv_hi) {
				pgoff_t lo_pg = oi->i_lock_res.lr_inv_lo >> PAGE_SHIFT;
				pgoff_t hi_pg =
					(oi->i_lock_res.lr_inv_hi - 1) >> PAGE_SHIFT;
				invalidate_mapping_pages(inode->i_mapping,
							 lo_pg, hi_pg);
			} else {
				invalidate_inode_pages2(inode->i_mapping);
			}
			oi->i_lock_res.lr_inv_lo = 0;
			oi->i_lock_res.lr_inv_hi = 0;
		}
	}

	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	start_pos = iocb->ki_pos; /* capture before write advances ki_pos */

	if (iocb->ki_flags & IOCB_DIRECT) {
		ret = iomap_dio_rw(iocb, from, &ocsfs_dio_iomap_ops,
				   NULL, 0, NULL, 0);
	} else {
		/*
		 * iomap_file_buffered_write (kernel >= 6.0, 5-arg form) updates
		 * iocb->ki_pos internally before returning. Do NOT add ret here
		 * again — that would double-advance the file position.
		 */
		ret = iomap_file_buffered_write(iocb, from,
						&ocsfs_iomap_ops, NULL, NULL);
	}

out:
	if (ret > 0) {
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
		mark_inode_dirty(inode);
	}

	if (sbi->s_clustered) {
		/*
		 * Flush dirty pages to the block device BEFORE releasing EX.
		 * Without this, another node that acquires SH immediately after
		 * our release would call invalidate_inode_pages2 and then read
		 * from the block device, seeing the pre-write data because our
		 * pages haven't been written back yet.
		 *
		 * Buffered writes only: direct I/O already bypasses the page
		 * cache and writes synchronously to the block device.
		 */
		if (ret > 0 && !(iocb->ki_flags & IOCB_DIRECT))
			filemap_write_and_wait(inode->i_mapping);

		/*
		 * Flush inode metadata (i_size, mtime, updated extent map from
		 * any CoW that occurred) before releasing EX.  Another node's
		 * ocsfs_iget reads the inode from disk after acquiring SH; if
		 * the updated extent map is not on disk yet it will see stale
		 * block mappings and silently read wrong data.
		 */
		if (ret > 0) {
			int fr = ocsfs_flush_inode_locked(inode, true);
			if (fr)
				pr_warn_ratelimited(
					"ocsfs: write_iter inode flush failed (%d)\n",
					fr);

			/* ARCH-7: update dirty range in lock_res so lock_release
			 * can store it on disk for the next SH acquirer. */
			{
				u64 inv_lo = (u64)start_pos;
				u64 inv_hi = (u64)(start_pos + ret);
				struct ocsfs_lock_res *ilr = &oi->i_lock_res;

				if (!ilr->lr_inv_lo && !ilr->lr_inv_hi) {
					ilr->lr_inv_lo = inv_lo;
					ilr->lr_inv_hi = inv_hi;
				} else {
					ilr->lr_inv_lo = min(ilr->lr_inv_lo, inv_lo);
					ilr->lr_inv_hi = max(ilr->lr_inv_hi, inv_hi);
				}
				oi->i_last_writer_slot = sbi->s_node_slot;
			}
		}

		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	}

out_unlock:
	inode_unlock(inode);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * IOMAP-BASED ADDRESS SPACE OPERATIONS
 * ═══════════════════════════════════════════════════════════════ */

/*
 * ocsfs_decompress_folio() — Read and decompress a compressed extent into a folio.
 *
 * Reads all compressed physical blocks from disk, decompresses the entire
 * extent, then copies the slice that overlaps this folio into its pages.
 * Called with the folio locked; returns with it still locked (caller unlocks).
 */
static int ocsfs_decompress_folio(struct inode *inode,
				  const struct ocsfs_extent *ext,
				  struct folio *folio)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u8 algo = ocsfs_ext_comp_algo(ext->flags);
	u32 phys_blks = ext->phys_length ? (u32)ext->phys_length : ext->length;
	size_t comp_size   = (size_t)phys_blks   * sbi->s_block_size;
	size_t decomp_size = (size_t)ext->length  * sbi->s_block_size;
	loff_t ext_start        = (loff_t)ext->logical_block * sbi->s_block_size;
	loff_t folio_off_in_ext = folio_pos(folio) - ext_start;
	void *comp_buf, *decomp_buf;
	struct buffer_head *bh;
	u32 i;
	size_t copied;
	int ret;

	if (folio_off_in_ext < 0 || (size_t)folio_off_in_ext >= decomp_size)
		return -EIO;

	/* Guard against corrupted/malicious extents causing multi-GiB allocs */
	if (comp_size > (1u << 20) || decomp_size > (1u << 20))
		return -EFBIG;

	comp_buf = kvmalloc(comp_size, GFP_NOFS);
	if (!comp_buf)
		return -ENOMEM;

	decomp_buf = kvmalloc(decomp_size, GFP_NOFS);
	if (!decomp_buf) {
		kvfree(comp_buf);
		return -ENOMEM;
	}

	/* Read compressed blocks from disk synchronously */
	copied = 0;
	for (i = 0; i < phys_blks && copied < comp_size; i++) {
		bh = sb_bread(sb, ext->physical_block + i);
		if (!bh) {
			ret = -EIO;
			goto out;
		}
		memcpy(comp_buf + copied, bh->b_data,
		       min_t(size_t, sbi->s_block_size, comp_size - copied));
		copied += sbi->s_block_size;
		brelse(bh);
	}

	ret = ocsfs_decompress_data(sb, algo, comp_buf, comp_size,
				    decomp_buf, decomp_size);
	if (ret)
		goto out;

	/* Copy the folio's slice of decompressed data into the folio pages */
	{
		size_t avail   = decomp_size - (size_t)folio_off_in_ext;
		size_t to_copy = min_t(size_t, folio_size(folio), avail);
		size_t pg;

		for (pg = 0; pg < folio_nr_pages(folio); pg++) {
			size_t off   = pg * PAGE_SIZE;
			size_t chunk;
			void *kaddr;

			if (off >= to_copy)
				break;
			chunk = min_t(size_t, PAGE_SIZE, to_copy - off);
			kaddr = kmap_local_page(folio_page(folio, pg));
			memcpy(kaddr,
			       decomp_buf + (size_t)folio_off_in_ext + off,
			       chunk);
			if (chunk < PAGE_SIZE)
				memset(kaddr + chunk, 0, PAGE_SIZE - chunk);
			kunmap_local(kaddr);
		}
	}

	folio_mark_uptodate(folio);
	ret = 0;
out:
	kvfree(decomp_buf);
	kvfree(comp_buf);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * ENCRYPTED FILE I/O (bounce-page path, needs_bounce_pages=1)
 *
 * For encrypted files we bypass iomap's async bio path and use a
 * synchronous block-read + fscrypt_decrypt_pagecache_blocks() for reads,
 * and fscrypt_encrypt_pagecache_blocks() + synchronous bio for writes.
 * Readahead is disabled for encrypted files; the kernel falls back to
 * individual read_folio calls.
 * ═══════════════════════════════════════════════════════════════ */

#ifdef CONFIG_FS_ENCRYPTION

static int ocsfs_enc_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi_r = OCSFS_I(inode);
	loff_t file_pos = folio_pos(folio);

	/* In cluster mode enc_read_folio must be called with at least DLM SH.
	 * Paths like splice_read or userfaultfd may arrive without it, leading
	 * to a stale extent map.  Warn so the call site can be audited. */
	WARN_ONCE(sbi->s_clustered &&
		  oi_r->i_lock_res.lr_mode < OCSFS_LOCK_SH,
		  "ocsfs: enc_read_folio without DLM SH (ino=%lu)\n",
		  inode->i_ino);
	size_t folio_sz = folio_size(folio);
	size_t done = 0;
	int ret = 0;

	while (done < folio_sz && ret == 0) {
		loff_t cur_off = file_pos + done;
		u64 lblk;
		struct ocsfs_extent ext;
		size_t chunk = min_t(size_t, sbi->s_block_size, folio_sz - done);

		if (cur_off >= i_size_read(inode)) {
			folio_zero_range(folio, done, folio_sz - done);
			break;
		}

		lblk = (u64)(cur_off >> inode->i_sb->s_blocksize_bits);
		mutex_lock(&OCSFS_I(inode)->i_extent_lock);
		ret = ocsfs_extent_lookup(inode, lblk, &ext);
		mutex_unlock(&OCSFS_I(inode)->i_extent_lock);

		if (ret) {
			folio_zero_range(folio, done, chunk);
			ret = 0;
		} else {
			u64 pblk = ext.physical_block + (lblk - ext.logical_block);
			struct buffer_head *bh = sb_bread(inode->i_sb, pblk);

			if (!bh) {
				ret = -EIO;
				break;
			}
			memcpy_to_folio(folio, done, bh->b_data, chunk);
			brelse(bh);
		}
		done += chunk;
	}

	if (ret == 0)
		ret = fscrypt_decrypt_pagecache_blocks(folio, folio_sz, 0);

	if (!ret)
		folio_mark_uptodate(folio);
	folio_unlock(folio);
	return ret;
}

static int ocsfs_enc_writepages(struct address_space *mapping,
				struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct folio *folio = NULL;
	int ret = 0;

	/* Background writeback (kswapd, bdi_writeback) does not hold DLM EX.
	 * In cluster mode, writing encrypted data without EX can clobber blocks
	 * reallocated to another inode.  Skip — ocsfs_file_write_iter calls
	 * filemap_write_and_wait() under DLM EX, so all dirty pages are flushed
	 * synchronously before the lock is released. */
	if (sbi->s_clustered && oi->i_lock_res.lr_mode != OCSFS_LOCK_EX)
		return 0;

	while ((folio = writeback_iter(mapping, wbc, folio, &ret))) {
		loff_t pos = folio_pos(folio);
		loff_t isize = i_size_read(inode);
		size_t len;
		u64 lblk;
		struct ocsfs_extent ext;
		struct page *enc_page;
		struct bio *bio;
		u64 pblk;
		int wr;

		if (pos >= isize) {
			folio_unlock(folio);
			continue;
		}
		len = min_t(size_t, folio_size(folio),
			    (size_t)(isize - pos));

		lblk = (u64)(pos >> inode->i_sb->s_blocksize_bits);
		mutex_lock(&OCSFS_I(inode)->i_extent_lock);
		ret = ocsfs_extent_lookup(inode, lblk, &ext);
		mutex_unlock(&OCSFS_I(inode)->i_extent_lock);
		if (ret) {
			folio_unlock(folio);
			break;
		}

		enc_page = fscrypt_encrypt_pagecache_blocks(folio, len, 0,
							    GFP_NOFS);
		if (IS_ERR(enc_page)) {
			ret = PTR_ERR(enc_page);
			folio_unlock(folio);
			break;
		}

		folio_start_writeback(folio);
		folio_unlock(folio);

		pblk = ext.physical_block + (lblk - ext.logical_block);
		bio  = bio_alloc(inode->i_sb->s_bdev, 1,
				 REQ_OP_WRITE | REQ_SYNC, GFP_NOIO);
		bio->bi_iter.bi_sector = pblk * (sbi->s_block_size >> SECTOR_SHIFT);
		if (!bio_add_page(bio, enc_page, len, 0)) {
			bio_put(bio);
			fscrypt_free_bounce_page(enc_page);
			folio_end_writeback(folio);
			ret = -ENOMEM;
			break;
		}
		wr  = submit_bio_wait(bio);
		bio_put(bio);
		fscrypt_free_bounce_page(enc_page);

		folio_end_writeback(folio);
		if (wr && !ret)
			ret = wr;
	}

	return ret;
}

#endif /* CONFIG_FS_ENCRYPTION */

static int ocsfs_iomap_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	u64 logical_block = (u64)(folio_pos(folio) / sbi->s_block_size);
	struct ocsfs_extent ext;
	int ret;

#ifdef CONFIG_FS_ENCRYPTION
	if (IS_ENCRYPTED(inode) && S_ISREG(inode->i_mode))
		return ocsfs_enc_read_folio(file, folio);
#endif

	mutex_lock(&oi->i_extent_lock);
	ret = ocsfs_extent_lookup(inode, logical_block, &ext);
	mutex_unlock(&oi->i_extent_lock);

	if (ret == 0 && (ext.flags & OCSFS_EXT_COMPRESSED)) {
		ret = ocsfs_decompress_folio(inode, &ext, folio);
		folio_unlock(folio);
		return ret;
	}

	iomap_bio_read_folio(folio, &ocsfs_iomap_ops);
	return 0;
}

static void ocsfs_iomap_readahead(struct readahead_control *rac)
{
	struct inode *inode = rac->mapping->host;
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	u64 logical_block = (u64)(readahead_pos(rac) / sbi->s_block_size);
	struct ocsfs_extent ext;
	int ret;

#ifdef CONFIG_FS_ENCRYPTION
	/* No readahead for encrypted files — decrypt in read_folio instead */
	if (IS_ENCRYPTED(inode) && S_ISREG(inode->i_mode))
		return;
#endif

	mutex_lock(&oi->i_extent_lock);
	ret = ocsfs_extent_lookup(inode, logical_block, &ext);
	mutex_unlock(&oi->i_extent_lock);

	/* Skip readahead for compressed extents — read_folio handles decompression */
	if (ret == 0 && (ext.flags & OCSFS_EXT_COMPRESSED))
		return;

	iomap_bio_readahead(rac, &ocsfs_iomap_ops);
}

static sector_t ocsfs_iomap_bmap(struct address_space *mapping, sector_t bno)
{
	return iomap_bmap(mapping, bno, &ocsfs_iomap_ops);
}

/*
 * Writeback map_blocks callback: find the physical mapping for a page being
 * written back. Flags=0 (no IOMAP_WRITE) so we only look up existing extents
 * without allocating new blocks or triggering CoW. Dirty pages always have
 * blocks allocated from the original write path; holes would produce
 * IOMAP_HOLE and iomap_writepages skips them safely.
 *
 * Passing IOMAP_WRITE here would be wrong in clustered mode: writepages is
 * called by the VM without holding DLM EX, but ocsfs_cow_extent requires it.
 */
static ssize_t ocsfs_writeback_range(struct iomap_writepage_ctx *wpc,
				     struct folio *folio, u64 pos,
				     unsigned int len, u64 end_pos)
{
	int ret;

	/* Remap solo se il blocco corrente non copre pos */
	if (!wpc->iomap.length || pos < wpc->iomap.offset ||
	    pos >= wpc->iomap.offset + wpc->iomap.length) {
		ret = ocsfs_iomap_begin(wpc->inode, pos,
					wpc->inode->i_sb->s_blocksize,
					0, &wpc->iomap, NULL);
		if (ret < 0)
			return ret;
	}

	return iomap_add_to_ioend(wpc, folio, pos, end_pos, len);
}

static const struct iomap_writeback_ops ocsfs_writeback_ops = {
	.writeback_range  = ocsfs_writeback_range,
	.writeback_submit = iomap_ioend_writeback_submit,
};

static int ocsfs_writepages(struct address_space *mapping,
			    struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct iomap_writepage_ctx wpc;

#ifdef CONFIG_FS_ENCRYPTION
	if (IS_ENCRYPTED(inode) && S_ISREG(inode->i_mode))
		return ocsfs_enc_writepages(mapping, wbc);
#endif

	wpc = (struct iomap_writepage_ctx){
		.inode = inode,
		.wbc   = wbc,
		.ops   = &ocsfs_writeback_ops,
	};
	return iomap_writepages(&wpc);
}

const struct address_space_operations ocsfs_iomap_aops = {
	.dirty_folio      = iomap_dirty_folio,
	.invalidate_folio = iomap_invalidate_folio,
	.release_folio    = iomap_release_folio,
	.read_folio       = ocsfs_iomap_read_folio,
	.readahead        = ocsfs_iomap_readahead,
	.writepages       = ocsfs_writepages,
	.bmap             = ocsfs_iomap_bmap,
};
