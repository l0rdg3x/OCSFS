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
#include <linux/sched/mm.h>   /* memalloc_nofs_save/restore */

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
		if ((flags & (IOMAP_WRITE | IOMAP_ZERO)) &&
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
		if ((flags & (IOMAP_WRITE | IOMAP_ZERO)) &&
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
		iomap->length = mapped_len;
		iomap->bdev = inode->i_sb->s_bdev;
		/*
		 * #13: iomap->offset MUST be the file offset that corresponds to
		 * iomap->addr — i.e. the start of the mapped block — not the
		 * (possibly unaligned) request pos.  iomap_sector() computes the
		 * device sector as addr + (block_start - iomap->offset); with
		 * offset=pos on a sub-block partial write that sector is misaligned,
		 * so the read-modify-write of the unwritten part reads garbage/zero
		 * instead of the on-disk data, and the next writeback persists the
		 * zeroed folio — destroying previously written bytes.
		 */
		iomap->offset = (loff_t)logical_block * sbi->s_block_size;

		if (ext.flags & OCSFS_EXT_UNWRITTEN)
			iomap->type = IOMAP_UNWRITTEN;
		else
			iomap->type = IOMAP_MAPPED;

		mutex_unlock(&oi->i_extent_lock);
		return 0;
	}

	/* No mapping exists */
	if (!(flags & IOMAP_WRITE)) {
		u64 hole_blocks;
		u64 k;

		/* Read from hole — return zeroes */
		iomap->type = IOMAP_HOLE;
		iomap->addr = IOMAP_NULL_ADDR;
		iomap->offset = pos;

		/* Tentative hole length: to end of file, but never more than the
		 * caller asked for — iomap re-enters for the rest, and this bounds
		 * the forward probe below to the request size (so a small read in a
		 * huge sparse region does not scan every block to EOF). */
		end_block = (inode->i_size + sbi->s_block_size - 1) /
			    sbi->s_block_size;
		hole_blocks = (end_block > logical_block) ?
			      (end_block - logical_block) : 1;
		{
			u64 req_blocks = (length + sbi->s_block_size - 1) /
					 sbi->s_block_size;

			if (req_blocks < 1)
				req_blocks = 1;
			if (hole_blocks > req_blocks)
				hole_blocks = req_blocks;
		}

		/*
		 * #13: the hole MUST stop at the next allocated extent.  The old
		 * code extended the hole straight to end-of-file, so a read or
		 * readahead starting in a leading hole (e.g. blk0 punched) returned
		 * one IOMAP_HOLE spanning the following MAPPED blocks too.  iomap
		 * then populated their page-cache folios with zeroes and marked
		 * them uptodate — stale, since those blocks hold real on-disk data.
		 * A later sub-block write to such a block finds the uptodate-zero
		 * folio, skips the read-modify-write disk read, and writes the
		 * zeroed folio back, destroying the previously written bytes.
		 * Probe forward and clamp the hole to the first mapped/unwritten
		 * block (same idea as the speculative-prealloc clamp below).
		 */
		for (k = 1; k < hole_blocks; k++) {
			struct ocsfs_extent probe;

			if (ocsfs_extent_lookup(inode, logical_block + k,
						&probe) == 0 &&
			    probe.physical_block != 0) {
				hole_blocks = k;
				break;
			}
		}

		iomap->length = min_t(loff_t, length,
				      (loff_t)hole_blocks * sbi->s_block_size);
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

		/*
		 * #13: clamp the speculative prealloc to the run of consecutive
		 * holes starting at logical_block.  We only entered this branch
		 * because logical_block itself is a hole, but the following blocks
		 * may already be mapped; allocating over them would insert an
		 * extent overlapping the existing one.  Overlapping extents make
		 * ocsfs_extent_lookup ambiguous (it returns the first match), so a
		 * read can hit a stale UNWRITTEN/old-phys extent and return zeros
		 * or stale data instead of the just-written bytes.
		 */
		{
			u32 k;

			for (k = 1; k < try_blocks; k++) {
				struct ocsfs_extent probe;

				if (ocsfs_extent_lookup(inode, logical_block + k,
							&probe) == 0 &&
				    probe.physical_block != 0) {
					try_blocks = k;
					break;
				}
			}
		}

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
		 * Insert as UNWRITTEN so that blocks not yet reached by the write are
		 * never exposed as MAPPED.  An extent is converted to WRITTEN only
		 * after the real data reaches disk: O_DIRECT converts in iomap_end
		 * (dio completion, data already durable); buffered writes convert in
		 * the writeback path (ocsfs_writeback_range) as the dirty pages are
		 * flushed.  This preserves the no-stale-data invariant — a crash
		 * before writeback leaves the extent UNWRITTEN, so reads return zeroes
		 * rather than another file's freed on-disk contents.
		 *
		 * iomap_end deliberately does NOT convert buffered writes: doing so
		 * held i_extent_lock across extent-btree I/O right after the data was
		 * dirtied (peak memory pressure), and the writeback worker — the only
		 * thing that can free that memory — blocks acquiring the same lock,
		 * deadlocking large writes.  Converting in the writeback path instead
		 * keeps the conversion in the memory-freeing context.
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
		/*
		 * #13: iomap->offset must be the file offset of the START of the
		 * mapped/allocated region (logical_block * block_size), NOT the
		 * unaligned request pos.  iomap_sector() derives the device sector
		 * as addr + (block_start - iomap->offset); with offset=pos on a
		 * sub-block partial allocating write the mapping is skewed by the
		 * intra-block offset, so a later read-modify-write or writeback
		 * lands on the wrong sector.  (Same fix as the existing-extent
		 * branch above.)
		 */
		iomap->offset = (loff_t)logical_block * sbi->s_block_size;
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
		 * Convert UNWRITTEN→WRITTEN only after the data is confirmed on
		 * disk, and ONLY for O_DIRECT: by the time the dio iomap_end runs
		 * the data is already durable, and O_DIRECT builds no page-cache
		 * pressure so there is no writeback/i_extent_lock deadlock.
		 *
		 * Buffered writes are deliberately NOT converted here: doing so held
		 * i_extent_lock across extent-btree I/O at peak dirty-page pressure
		 * and deadlocked against the writeback worker (the only thing that
		 * can free that memory) which needs the same lock. For buffered
		 * writes the conversion happens in ocsfs_writeback_range as the dirty
		 * pages are flushed — i.e. in the memory-freeing context, exactly
		 * when the real data reaches disk, so no stale data is ever exposed.
		 */
		if (iomap->type == IOMAP_UNWRITTEN && (flags & IOMAP_DIRECT)) {
			u64 start_block = pos / sbi->s_block_size;
			u32 nblocks = (u32)((written + sbi->s_block_size - 1) /
					    sbi->s_block_size);
			unsigned int nofs;
			int cr;

			mutex_lock(&oi->i_extent_lock);
			nofs = memalloc_nofs_save();
			cr = ocsfs_extent_convert_unwritten(inode,
							    start_block,
							    nblocks);
			memalloc_nofs_restore(nofs);
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
	bool fresh = false;
	ssize_t ret;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire_fresh(inode->i_sb, &oi->i_lock_res,
					       OCSFS_LOCK_SH, &fresh);
		if (ret)
			return ret;
		/*
		 * PERF (random-read hot path): only re-read the inode and
		 * invalidate the page cache on a *fresh* cross-node acquire.  A
		 * cache-hit re-acquire (we were lazily holding SH — or EX from a
		 * preceding write — since our last access) proves no peer took EX
		 * in between, so nothing on disk changed and our cache is coherent.
		 * Skipping the per-read forced inode read here is what lets random
		 * O_DIRECT reads run at device speed instead of one SAN round-trip
		 * per 4 KiB.  The lock is released lazily below so a sustained read
		 * stream re-takes it as a cache hit.
		 */
		if (fresh) {
			ret = ocsfs_inode_refresh(inode);
			if (ret) {
				ocsfs_lock_release_lazy(inode->i_sb,
							&oi->i_lock_res);
				return ret;
			}
			/*
			 * ARCH-7: page cache invalidation on a *fresh* acquire.
			 *
			 * A fresh acquire means we did NOT hold the lock
			 * continuously since our last access — a peer took EX in
			 * between and may have written.  So our cached pages may be
			 * stale and MUST be dropped before the read below repopulates
			 * from disk.  Use the dirty range the previous EX holder
			 * published (lr_inv_lo/hi) when available, else invalidate the
			 * whole mapping.
			 *
			 * We deliberately do NOT short-circuit on
			 * i_last_writer_slot == s_node_slot here: that field is a
			 * node-local hint that a peer's write does not clear, so
			 * "we wrote last" can be false after a peer EX and would
			 * wrongly skip invalidation, serving stale data on buffered
			 * reads (cross-node coherence bug).  The cache-hit case (we
			 * really did hold the lock throughout) is already handled by
			 * the !fresh path, which skips invalidation entirely.
			 */
			if (oi->i_lock_res.lr_inv_lo <
			    oi->i_lock_res.lr_inv_hi) {
				pgoff_t lo_pg =
					oi->i_lock_res.lr_inv_lo >> PAGE_SHIFT;
				pgoff_t hi_pg =
					(oi->i_lock_res.lr_inv_hi - 1) >> PAGE_SHIFT;

				invalidate_mapping_pages(inode->i_mapping,
							 lo_pg, hi_pg);
			} else {
				invalidate_inode_pages2(inode->i_mapping);
			}
		}
	}

	if (iocb->ki_flags & IOCB_DIRECT)
		ret = iomap_dio_rw(iocb, to, &ocsfs_dio_iomap_ops,
				   NULL, 0, NULL, 0);
	else
		ret = filemap_read(iocb, to, 0);

	if (sbi->s_clustered)
		ocsfs_lock_release_lazy(inode->i_sb, &oi->i_lock_res);

	return ret;
}

ssize_t ocsfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	ssize_t ret;
	loff_t start_pos = 0; /* ARCH-7: write start for dirty range tracking */
	loff_t i_size_before = 0;
	bool   meta_changed = false;
	bool   fresh = false;

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
		ret = ocsfs_lock_acquire_fresh(inode->i_sb, &oi->i_lock_res,
					       OCSFS_LOCK_EX, &fresh);
		if (ret)
			goto out_unlock;
		/*
		 * ARCH-7 write-path page cache invalidation, on a *fresh* acquire
		 * only.  A fresh EX acquire means a peer held EX in between and may
		 * have written, so any cached pages may be stale and must be
		 * dropped before a buffered read-modify-write reads them.  Use the
		 * previous EX holder's published dirty range (lr_inv_lo/hi) when
		 * available, else invalidate the whole mapping.  On a cache hit we
		 * held EX throughout, so the cache is coherent — skip.
		 *
		 * We must NOT gate on i_last_writer_slot == s_node_slot: it is a
		 * node-local hint that a peer's write does not clear, so it would
		 * wrongly skip invalidation after a peer EX (cross-node coherence
		 * bug).  After invalidation, zero lr_inv_lo/hi so iomap_end starts
		 * tracking only this write's range.
		 */
		if (fresh) {
			/*
			 * A peer held EX since our last access: pull in its
			 * flushed i_size / extent map before we map and write, so
			 * we don't allocate over or past the peer's changes.
			 */
			ret = ocsfs_inode_refresh(inode);
			if (ret) {
				ocsfs_lock_release_lazy(inode->i_sb,
							&oi->i_lock_res);
				goto out_unlock;
			}
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
	i_size_before = i_size_read(inode);

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
		 *
		 * PERF (VM-disk fast path): a *pure overwrite* — no extent-map
		 * change (oi->i_extents_dirty) and no i_size growth — leaves the
		 * on-disk inode already correct for a peer: the data went to the
		 * same WRITTEN blocks the existing map points at (O_DIRECT writes
		 * them synchronously; buffered ones are flushed just above).  Skip
		 * the synchronous journal-txn inode flush entirely; only mtime/
		 * ctime changed and that rides normal async writeback.  This is the
		 * dominant pattern for random writes into a pre-allocated VM image.
		 */
		meta_changed = oi->i_extents_dirty ||
			       i_size_read(inode) > i_size_before;

		if (ret > 0 && meta_changed) {
			int fr = ocsfs_flush_inode_locked(inode, true);
			if (fr)
				pr_warn_ratelimited(
					"ocsfs: write_iter inode flush failed (%d)\n",
					fr);
		}
		if (ret > 0) {

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

		/*
		 * PERF: lazy release.  Keep the inode EX held on-disk so the next
		 * write is a cache-hit re-acquire with no DLM round-trip — the win
		 * for a VM doing sustained writes to one disk image.  A peer that
		 * needs the lock (e.g. live migration) is handed it by the
		 * lazy-revoke sweep within one interval.  The accumulated dirty
		 * range (lr_inv_lo/hi) is published on the eventual real release.
		 */
		ocsfs_lock_release_lazy(inode->i_sb, &oi->i_lock_res);
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


static int ocsfs_iomap_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	u64 logical_block = (u64)(folio_pos(folio) / sbi->s_block_size);
	struct ocsfs_extent ext;
	int ret;

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

	/*
	 * #13: always remap per folio.  Caching wpc->iomap across folios is
	 * unsafe in OCSFS: the UNWRITTEN→WRITTEN conversion below splits the
	 * extent and sets wpc->iomap.type=MAPPED for the whole old range, so a
	 * following folio still inside that range would reuse type=MAPPED and
	 * SKIP its own conversion — leaving its block UNWRITTEN with the data
	 * already on disk, which reads back as zero (silent data loss).  The
	 * lookup is an in-memory extent search, so per-folio remap is cheap.
	 */
	ret = ocsfs_iomap_begin(wpc->inode, pos,
				wpc->inode->i_sb->s_blocksize,
				0, &wpc->iomap, NULL);
	if (ret < 0)
		return ret;

	/* #14: a DIRTY folio mapped to a HOLE means a write to this block had not
	 * yet been persisted when the block was freed (punch/truncate) and the
	 * extent removed.  But every free DISCARDS dirty folios over its range
	 * (truncate_pagecache_range), so a folio that is STILL dirty here is NEWER
	 * than any free — its data must NOT be lost (the plain iomap writeback
	 * skips holes, silently dropping the write; caught by fsx as on-disk
	 * corruption: a write survived in cache but the block read back stale).
	 * Re-allocate the block now (delalloc-at-writeback) and let the UNWRITTEN
	 * conversion below persist it.  A legitimately punched block has no dirty
	 * folio to reach here, so this never resurrects discarded data. */
	if (wpc->iomap.type == IOMAP_HOLE) {
		unsigned int nofs = memalloc_nofs_save();

		ret = ocsfs_iomap_begin(wpc->inode, pos,
					wpc->inode->i_sb->s_blocksize,
					IOMAP_WRITE, &wpc->iomap, NULL);
		memalloc_nofs_restore(nofs);
		if (ret < 0)
			return ret;
	}

	/* ALTO-N2: writeback path does not call iomap_end, so UNWRITTEN extents
	 * would remain UNWRITTEN after data reaches disk — subsequent reads
	 * return zeroes instead of the written data.  Convert here instead. */
	if (wpc->iomap.type == IOMAP_UNWRITTEN) {
		struct inode *inode = wpc->inode;
		struct ocsfs_inode_info *oi = OCSFS_I(inode);
		struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
		u64 start_block = pos / sbi->s_block_size;
		u32 nblocks = (u32)((len + sbi->s_block_size - 1) /
				    sbi->s_block_size);
		unsigned int nofs;
		int cr;

		/* Same reclaim-recursion guard as ocsfs_iomap_end (see there). */
		mutex_lock(&oi->i_extent_lock);
		nofs = memalloc_nofs_save();
		cr = ocsfs_extent_convert_unwritten(inode, start_block, nblocks);
		memalloc_nofs_restore(nofs);
		mutex_unlock(&oi->i_extent_lock);
		if (cr)
			pr_warn_ratelimited(
				"ocsfs: writeback UNWRITTEN→WRITTEN failed (%d)\n",
				cr);
		else
			wpc->iomap.type = IOMAP_MAPPED;
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
