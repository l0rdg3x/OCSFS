// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — thin.c
 * Thin provisioning support.
 *
 * Phase 3: Lazy allocation with UNMAP/DISCARD support:
 *   - UNWRITTEN extents for fallocate preallocation
 *   - Punch hole (FALLOC_FL_PUNCH_HOLE) to free blocks in the middle of a file
 *   - DISCARD passthrough to underlying storage (SSD TRIM / SAN UNMAP)
 *   - Zero range support for efficient sparse file creation
 *
 * Thin provisioning means blocks are only allocated on first write,
 * and can be returned to the pool via punch_hole/DISCARD.
 */

#include <linux/types.h>
#include <linux/falloc.h>
#include <linux/pagemap.h>
#include <linux/iomap.h>
#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * PUNCH HOLE
 *
 * Deallocate blocks in the range [offset, offset+len), creating
 * a hole in the file. File size is not changed.
 *
 * This is the core of thin provisioning reclaim — VM images that
 * have been zeroed or trimmed can return blocks to the pool.
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Zero a sub-block byte range [byte_off, byte_off+byte_len) that lies entirely
 * within ONE logical block, physically (the block stays allocated).  Used for
 * the partial edges of a punch/zero range.  Caller holds i_extent_lock.  A hole
 * or an UNWRITTEN block already reads as zero, so those are no-ops.
 */
static int ocsfs_zero_within_block(struct inode *inode, loff_t byte_off,
				   loff_t byte_len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	u64 lblk = (u64)byte_off / sbi->s_block_size;
	u32 boff = (u32)((u64)byte_off % sbi->s_block_size);
	struct ocsfs_extent ext;
	struct buffer_head *bh;
	u64 phys;
	bool did_cow = false;
	int ret;

	if (byte_len <= 0)
		return 0;
	if (boff + byte_len > sbi->s_block_size)        /* defensive: keep in-block */
		byte_len = sbi->s_block_size - boff;

	ret = ocsfs_extent_lookup(inode, lblk, &ext);
	if (ret || ext.physical_block == 0)
		return 0;                               /* hole — already zero */
	if (ext.flags & OCSFS_EXT_UNWRITTEN)
		return 0;                               /* unwritten — reads zero */
	if (ext.flags & OCSFS_EXT_COMPRESSED) {
		ret = ocsfs_extent_decompress_for_write(inode, lblk);
		if (ret)
			return ret;
		ret = ocsfs_extent_lookup(inode, lblk, &ext);
		if (ret || ext.physical_block == 0)
			return ret;
	}

	/* COW a shared (reflinked/snapshot/dedup) block before zeroing it in
	 * place: the block is mutated with a raw memset, so without CoW the other
	 * sharers would also see the zeroed bytes (a partial punch/zero_range of
	 * one file would corrupt the data of every file sharing that block).
	 * Caller holds i_extent_lock (and DLM EX in cluster mode), satisfying
	 * ocsfs_cow_extent's contract; re-lookup since the extent map changes. */
	phys = ext.physical_block + (lblk - ext.logical_block);
	if (ocsfs_needs_cow(inode->i_sb, phys)) {
		ret = ocsfs_cow_extent(inode, lblk, 1);
		if (ret)
			return ret;
		ret = ocsfs_extent_lookup(inode, lblk, &ext);
		if (ret || ext.physical_block == 0)
			return ret;
		phys = ext.physical_block + (lblk - ext.logical_block);
		did_cow = true;
	}

	bh = sb_getblk(inode->i_sb, phys);
	if (!bh)
		return -EIO;
	if (did_cow) {
		/* ocsfs_cow_extent() just copied the source data into this block
		 * and wrote it to disk synchronously (submit_bh), leaving the buffer
		 * uptodate.  Use it as the CoW left it — do NOT force a re-read that
		 * could race the just-issued write; the on-disk copy is already
		 * current.  The memset + sync_dirty_buffer below then persists the
		 * zeroed-edge result on top of it. */
		if (!buffer_uptodate(bh) && bh_read(bh, 0) < 0) {
			brelse(bh);
			return -EIO;
		}
		lock_buffer(bh);
	} else {
		/* No CoW: OCSFS data lives in the iomap page cache (folios), so a
		 * cached buffer_head for this block may be stale (zero) — sb_bread()
		 * would then read zeros, and after the memset+sync below the
		 * PRESERVED bytes of the block would be lost (caught by fsx: a punch
		 * ending mid-block zeroed the block's tail).  The caller
		 * (ocsfs_fallocate) has already flushed this block's folio to disk
		 * (round_down..round_up of the range), so the on-disk copy is
		 * current; FORCE a fresh disk read. */
		lock_buffer(bh);
		clear_buffer_uptodate(bh);
		unlock_buffer(bh);
		if (bh_read(bh, 0) < 0) {
			brelse(bh);
			return -EIO;
		}
		lock_buffer(bh);
	}
	memset(bh->b_data + boff, 0, byte_len);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

/*
 * Zero a sub-block edge range [byte_off, byte_off+byte_len) of a punch/zero,
 * choosing a page-cache-coherent path.
 *
 * NON-shared block: use iomap_zero_range() — it reads, zeroes and dirties the
 * iomap FOLIO (proper RMW), so the zeroing reaches disk through the one data
 * path.  The old buffer_head path (ocsfs_zero_within_block) was a SECOND cache
 * for the block; the inode's folio could be written back by the async flusher
 * and race/overwrite it, corrupting the block non-deterministically (fsx: pure
 * punch + write/read, no reflink/CoW).
 *
 * SHARED (reflink/snapshot/dedup) block: must be CoW'd first, and the CoW'd
 * block lives only in cow_extent's dirty buffer (cow_extent cannot sync under
 * i_extent_lock without deadlocking writeback).  iomap_zero_range would RMW
 * from the not-yet-persisted on-disk block and lose the preserved tail, so a
 * shared edge keeps the CoW-aware buffer_head path (ocsfs_zero_within_block),
 * which copies from that dirty buffer.
 */
static int ocsfs_punch_zero_edge(struct inode *inode, loff_t byte_off,
				 loff_t byte_len)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 bs = OCSFS_SB(inode->i_sb)->s_block_size;
	u64 lblk = (u64)byte_off / bs;
	struct ocsfs_extent ext;
	bool shared = false;
	int ret;

	if (byte_len <= 0)
		return 0;

	mutex_lock(&oi->i_extent_lock);
	if (ocsfs_extent_lookup(inode, lblk, &ext) == 0 && ext.physical_block &&
	    !(ext.flags & OCSFS_EXT_UNWRITTEN)) {
		u64 phys = ext.physical_block + (lblk - ext.logical_block);

		shared = ocsfs_needs_cow(inode->i_sb, phys);
	}
	if (shared) {
		/* CoW-aware buffer_head path, run under i_extent_lock as it
		 * requires (it calls ocsfs_cow_extent). */
		ret = ocsfs_zero_within_block(inode, byte_off, byte_len);
		mutex_unlock(&oi->i_extent_lock);
		return ret;
	}
	mutex_unlock(&oi->i_extent_lock);

	/* iomap_zero_range re-enters ocsfs_iomap_begin (takes i_extent_lock), so
	 * it must run WITHOUT the lock held. */
	return iomap_zero_range(inode, byte_off, byte_len, NULL,
				&ocsfs_iomap_ops, NULL, NULL);
}

int ocsfs_punch_hole(struct inode *inode, loff_t offset, loff_t len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 bs = sbi->s_block_size;
	loff_t pend = offset + len;
	/* Whole-block range that is FULLY inside the punch: [full_start, full_end).
	 * Partially-covered blocks at the edges stay allocated; their punched bytes
	 * are zeroed in place below (the previous code used floor()/floor() for
	 * both ends, which (a) did nothing for a sub-block punch within one block
	 * and (b) could deallocate a partially-covered edge block, losing data
	 * outside the hole). */
	u64 full_start = (u64)(offset + bs - 1) / bs;
	u64 full_end   = (u64)pend / bs;
	u64 start_block = full_start;
	u64 end_block = full_end;
	loff_t head_end = (loff_t)full_start * bs;
	loff_t tail_start = (loff_t)full_end * bs;
	int i;
	int ret = 0;

	if (len <= 0)
		return 0;

	/* Zero the partial (sub-block) edge bytes via the iomap PAGE CACHE, so the
	 * zeroing is coherent with the data path.  The old code zeroed the edges
	 * through a raw buffer_head (sb_getblk + memset + sync_dirty_buffer) — a
	 * SECOND cache for the same block.  The inode's iomap folio for that edge
	 * block could then be written back by the async flusher and race with /
	 * overwrite the buffer_head write, corrupting the block NON-deterministically
	 * (caught by fsx: pure punch + write/read, no reflink/CoW; the on-disk
	 * result varied run-to-run).  iomap_zero_range() reads, zeroes and dirties
	 * the folio itself (proper RMW); a shared edge block is CoW'd inside
	 * ocsfs_iomap_begin (IOMAP_ZERO is now handled like IOMAP_WRITE there).
	 * It MUST run WITHOUT i_extent_lock: it re-enters ocsfs_iomap_begin, which
	 * takes that lock (holding it here would self-deadlock). */
	if (offset < head_end) {
		ret = ocsfs_punch_zero_edge(inode, offset,
					    min_t(loff_t, pend, head_end) - offset);
		if (ret)
			return ret;
	}
	if (pend > tail_start && tail_start >= head_end) {
		ret = ocsfs_punch_zero_edge(inode,
					    max_t(loff_t, offset, tail_start),
					    pend - max_t(loff_t, offset, tail_start));
		if (ret)
			return ret;
	}

	/* Drop the page cache for the WHOLE (middle) blocks that are about to be
	 * freed — but NOT the partial edge blocks, whose folios we just dirtied
	 * above (truncate_pagecache_range would discard that zeroing).  After the
	 * free those middle blocks read back as a hole (zeros). */
	if (head_end < tail_start)
		truncate_pagecache_range(inode, head_end, tail_start - 1);

	if (oi->i_extent_tree_root)
		return ocsfs_extent_btree_punch_hole(inode, full_start, full_end);

	if (start_block >= end_block)
		return 0;   /* only partial bytes punched — no whole blocks to free */

	/* ALTO-N1: journal-before-free — collect blocks to free, modify extents
	 * in memory, journal the inode, THEN free.  Prevents cross-link on crash
	 * (without this, free commits to bitmap before inode flush, so crashed
	 * block can be reallocated while this inode still references it). */
	struct { u64 phys; u32 count; } deferred_frees[OCSFS_INLINE_EXTENTS + 1];
	int nfrees = 0;

	mutex_lock(&oi->i_extent_lock);

	/*
	 * Walk extents backwards (safe for removal/shrink).
	 * For each extent overlapping [start_block, end_block):
	 *   - Fully contained:    remove the extent, free blocks
	 *   - Head overlap:       shrink from front
	 *   - Tail overlap:       shrink from back
	 *   - Middle punch:       split into two extents
	 */
	for (i = oi->i_extent_count - 1; i >= 0; i--) {
		struct ocsfs_extent *e = &oi->i_extents[i];
		u64 ext_start = e->logical_block;
		u64 ext_end = e->logical_block + e->length;

		if (ext_end <= start_block || ext_start >= end_block)
			continue; /* no overlap */

		if (ext_start >= start_block && ext_end <= end_block) {
			/* Case 1: Entire extent is punched */
			u32 phys = ocsfs_ext_phys_blocks(e);

			deferred_frees[nfrees].phys  = e->physical_block;
			deferred_frees[nfrees].count = phys;
			nfrees++;
			inode->i_blocks -= (u64)phys * (sbi->s_block_size / 512);

			/* Remove from array */
			if (i + 1 < oi->i_extent_count) {
				memmove(&oi->i_extents[i],
					&oi->i_extents[i + 1],
					(oi->i_extent_count - i - 1) *
					sizeof(struct ocsfs_extent));
			}
			oi->i_extent_count--;
		} else if (e->flags & OCSFS_EXT_COMPRESSED) {
			/*
			 * Partial punch on a compressed extent: physical blocks
			 * cannot be sliced at an arbitrary logical boundary.
			 * Decompress in-place first, then re-visit this index.
			 */
			int dr = ocsfs_extent_decompress_for_write(
					inode, e->logical_block);
			if (dr) {
				ret = dr;
				break;
			}
			i++; /* re-visit after loop decrements i */
			continue;
		} else if (ext_start >= start_block && ext_start < end_block) {
			/* Case 2: Punch head of extent */
			u32 removed = (u32)(end_block - ext_start);

			deferred_frees[nfrees].phys  = e->physical_block;
			deferred_frees[nfrees].count = removed;
			nfrees++;
			inode->i_blocks -= (u64)removed *
					   (sbi->s_block_size / 512);

			e->logical_block += removed;
			e->physical_block += removed;
			e->length -= removed;
		} else if (ext_end > start_block && ext_end <= end_block) {
			/* Case 3: Punch tail of extent */
			u32 removed = (u32)(ext_end - start_block);

			deferred_frees[nfrees].phys  = e->physical_block +
						       (e->length - removed);
			deferred_frees[nfrees].count = removed;
			nfrees++;
			inode->i_blocks -= (u64)removed *
					   (sbi->s_block_size / 512);

			e->length -= removed;
		} else {
			/* Case 4: Punch hole in middle — split extent */
			u32 head_len = (u32)(start_block - ext_start);
			u32 tail_len = (u32)(ext_end - end_block);
			u64 tail_phys = e->physical_block +
					(end_block - ext_start);
			u32 removed = (u32)(end_block - start_block);

			deferred_frees[nfrees].phys  = e->physical_block + head_len;
			deferred_frees[nfrees].count = removed;
			nfrees++;
			inode->i_blocks -= (u64)removed *
					   (sbi->s_block_size / 512);

			/* Shrink current extent to head */
			e->length = head_len;

			/* Insert new extent for tail (after the hole) */
			ret = ocsfs_extent_insert(inode, end_block,
						  tail_phys, tail_len,
						  e->flags);
			if (ret) {
				/*
				 * Insert failed — deferred_free still holds the
				 * middle blocks; they will be freed below after
				 * inode flush, so no cross-link on crash.
				 */
				pr_warn("ocsfs: punch_hole split failed: "
					"inode %llu, lost %u blocks\n",
					oi->i_disk_ino, tail_len);
			}
			/* Don't adjust i_blocks for tail — it already existed */
			break; /* only one split possible per call */
		}
	}

	/* Journal inode with updated extent map BEFORE freeing blocks */
	if (nfrees > 0) {
		int fr = ocsfs_flush_inode_locked(inode, false);

		if (fr)
			pr_warn_ratelimited(
				"ocsfs: punch_hole inode flush failed (%d)\n",
				fr);
	}
	mark_inode_dirty(inode);
	mutex_unlock(&oi->i_extent_lock);

	/* Free blocks only after inode is safely on disk.  Use the REFCOUNT-AWARE
	 * free: a punched block may be shared (reflink / clone / dedup), and a plain
	 * ocsfs_free_blocks() would clear its bitmap bit out from under the other
	 * owners while their refcount stays > 1 — a bitmap/refcount inconsistency
	 * that later makes the allocator hand the still-referenced block back out
	 * (observed as a CoW that allocates its own source block and self-deadlocks
	 * in ocsfs_cow_extent, and as latent cross-link corruption).  The extent-
	 * btree punch path already frees refcount-aware; the inline path must match. */
	{
		int k;

		for (k = 0; k < nfrees; k++)
			ocsfs_free_blocks_rc(inode->i_sb,
					     deferred_frees[k].phys,
					     deferred_frees[k].count);
	}
	return ret;
}

/*
 * Clear [start_block, end_block) of an inode's extent map, freeing the
 * physical blocks REFCOUNT-AWARE (ocsfs_free_blocks_rc, so shared/reflinked
 * blocks are only released when their last reference goes away).
 *
 * Used by reflink to turn the destination range into a clean hole BEFORE
 * sharing the source's blocks, so the destination mirrors the source exactly
 * (including the source's holes).  Block-aligned range only.  Caller holds
 * i_rwsem EX (and DLM EX in cluster mode); this takes i_extent_lock itself.
 */
int ocsfs_clear_block_range(struct inode *inode, u64 start_block, u64 end_block)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct { u64 phys; u32 count; } frees[OCSFS_INLINE_EXTENTS + 1];
	int nfrees = 0;
	int i;
	int ret = 0;

	if (start_block >= end_block)
		return 0;

	/* Btree-backed: the btree punch already frees refcount-aware. */
	if (oi->i_extent_tree_root)
		return ocsfs_extent_btree_punch_hole(inode, start_block, end_block);

	mutex_lock(&oi->i_extent_lock);

	for (i = oi->i_extent_count - 1; i >= 0; i--) {
		struct ocsfs_extent *e = &oi->i_extents[i];
		u64 ext_start = e->logical_block;
		u64 ext_end = e->logical_block + e->length;

		if (ext_end <= start_block || ext_start >= end_block)
			continue;

		if (e->flags & OCSFS_EXT_COMPRESSED) {
			ret = ocsfs_extent_decompress_for_write(inode, ext_start);
			if (ret)
				goto out;
			i++;            /* re-visit this (now uncompressed) index */
			continue;
		}

		if (ext_start >= start_block && ext_end <= end_block) {
			/* Fully inside: remove and free. */
			frees[nfrees].phys  = e->physical_block;
			frees[nfrees].count = ocsfs_ext_phys_blocks(e);
			nfrees++;
			inode->i_blocks -= (u64)e->length * (sbi->s_block_size / 512);
			if (i + 1 < oi->i_extent_count)
				memmove(&oi->i_extents[i], &oi->i_extents[i + 1],
					(oi->i_extent_count - i - 1) *
					sizeof(struct ocsfs_extent));
			oi->i_extent_count--;
		} else if (ext_start >= start_block) {
			/* Head overlap: shrink from front. */
			u32 removed = (u32)(end_block - ext_start);

			frees[nfrees].phys  = e->physical_block;
			frees[nfrees].count = removed;
			nfrees++;
			inode->i_blocks -= (u64)removed * (sbi->s_block_size / 512);
			e->logical_block  += removed;
			e->physical_block += removed;
			e->length         -= removed;
		} else if (ext_end <= end_block) {
			/* Tail overlap: shrink from back. */
			u32 removed = (u32)(ext_end - start_block);

			frees[nfrees].phys  = e->physical_block +
					      (e->length - removed);
			frees[nfrees].count = removed;
			nfrees++;
			inode->i_blocks -= (u64)removed * (sbi->s_block_size / 512);
			e->length -= removed;
		} else {
			/* Middle: split into head + tail, free the middle. */
			u32 head_len = (u32)(start_block - ext_start);
			u32 tail_len = (u32)(ext_end - end_block);
			u64 tail_phys = e->physical_block + (end_block - ext_start);
			u32 removed = (u32)(end_block - start_block);

			frees[nfrees].phys  = e->physical_block + head_len;
			frees[nfrees].count = removed;
			nfrees++;
			inode->i_blocks -= (u64)removed * (sbi->s_block_size / 512);
			e->length = head_len;
			ret = ocsfs_extent_insert(inode, end_block, tail_phys,
						  tail_len, e->flags);
			if (ret)
				pr_warn("ocsfs: reflink dst-clear split failed: "
					"inode %llu\n", oi->i_disk_ino);
			break;          /* only one split possible per range */
		}
	}

	if (nfrees > 0) {
		int fr = ocsfs_flush_inode_locked(inode, false);

		if (fr)
			pr_warn_ratelimited(
				"ocsfs: reflink dst-clear flush failed (%d)\n", fr);
	}
	mark_inode_dirty(inode);
out:
	mutex_unlock(&oi->i_extent_lock);

	for (i = 0; i < nfrees; i++)
		ocsfs_free_blocks_rc(inode->i_sb, frees[i].phys, frees[i].count);

	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * ZERO RANGE
 *
 * Zero a range of the file without deallocating. If the range
 * covers allocated blocks, convert them to UNWRITTEN (reads
 * return zeroes but blocks stay allocated).
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_zero_range(struct inode *inode, loff_t offset, loff_t len)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 bs = sbi->s_block_size;
	loff_t pend = offset + len;
	/* Whole blocks FULLY inside [offset, pend): [full_start, full_end).
	 * The partial edge blocks keep their data outside the range; only their
	 * in-range bytes are zeroed in place.  The previous code used
	 * floor(offset/bs) for start_block and zeroed/UNWROTE whole edge blocks,
	 * destroying the preserved prefix/suffix bytes when offset or pend fell
	 * mid-block (caught by fsx: a zero_range starting mid-block wiped the
	 * block's leading bytes). */
	u64 full_start = (u64)(offset + bs - 1) / bs;
	u64 full_end   = (u64)pend / bs;
	loff_t head_end   = (loff_t)full_start * bs;
	loff_t tail_start = (loff_t)full_end * bs;
	int eret;
	u16 i;

	if (len <= 0)
		return 0;

	/* Zero the partial (sub-block) edge bytes via the page cache, preserving
	 * the surrounding bytes.  Uses the same folio-coherent helper as the punch
	 * path (ocsfs_punch_zero_edge): the old raw buffer_head zeroing was a
	 * second cache for the block and raced with the async folio writeback,
	 * corrupting it non-deterministically.  Runs WITHOUT i_extent_lock. */
	if (offset < head_end) {
		eret = ocsfs_punch_zero_edge(inode, offset,
					     min_t(loff_t, pend, head_end) - offset);
		if (eret)
			return eret;
	}
	if (pend > tail_start && tail_start >= head_end) {
		eret = ocsfs_punch_zero_edge(inode,
					     max_t(loff_t, offset, tail_start),
					     pend - max_t(loff_t, offset, tail_start));
		if (eret)
			return eret;
	}

	/* Drop the page cache for the WHOLE (middle) blocks that become UNWRITTEN
	 * — NOT the edge blocks just dirtied above (truncate would discard that
	 * zeroing); those middle blocks read back as zeros. */
	if (head_end < tail_start)
		truncate_pagecache_range(inode, head_end, tail_start - 1);

	/*
	 * CoW any SHARED whole block in [full_start, full_end) before the
	 * conversion below.  A block shared via reflink/snapshot/dedup must never
	 * be modified in place: the inline path memset()s it (corrupting every
	 * other owner immediately), and both paths would otherwise mark it
	 * UNWRITTEN while it still points at the shared physical block — a later
	 * write to that UNWRITTEN block skips CoW (UNWRITTEN is assumed private)
	 * and overwrites the shared block, corrupting the other owners.  Break the
	 * sharing here so the conversion only ever touches PRIVATE blocks.  The
	 * partial edges are already CoW'd in ocsfs_punch_zero_edge().  Caught by
	 * fsx (clone + zero_range): the other clone read zeros / stale bytes.
	 */
	if (full_start < full_end) {
		u64 b = full_start;
		int cret = 0;

		mutex_lock(&oi->i_extent_lock);
		while (b < full_end) {
			struct ocsfs_extent ext;
			u64 ext_end, cow_end, phys;

			if (ocsfs_extent_lookup(inode, b, &ext) != 0 ||
			    ext.physical_block == 0) {
				b++;                    /* hole — nothing shared */
				continue;
			}
			ext_end = ext.logical_block + ext.length;
			if (ext.flags & OCSFS_EXT_UNWRITTEN) {
				b = ext_end;            /* already reads zero */
				continue;
			}
			cow_end = min(ext_end, full_end);
			phys = ext.physical_block + (b - ext.logical_block);
			if (ocsfs_needs_cow(inode->i_sb, phys)) {
				cret = ocsfs_cow_extent(inode, b,
							(u32)(cow_end - b));
				if (cret)
					break;
			}
			b = cow_end;
		}
		mutex_unlock(&oi->i_extent_lock);
		if (cret)
			return cret;
	}

	if (oi->i_extent_tree_root) {
		if (full_start < full_end)
			return ocsfs_extent_btree_zero_range(inode, full_start,
							     full_end);
		return 0;
	}

	if (full_start >= full_end)
		return 0;   /* only partial edges — no whole blocks to zero */

	mutex_lock(&oi->i_extent_lock);

	/* Whole-block interior only: mark UNWRITTEN (reads return zeros) or
	 * physically zero the full blocks that intersect [full_start, full_end). */
	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];
		u64 ext_end = e->logical_block + e->length;

		if (ext_end <= full_start || e->logical_block >= full_end)
			continue;
		if (!e->physical_block || (e->flags & OCSFS_EXT_UNWRITTEN))
			continue;   /* hole / unwritten already reads as zero */

		if (e->logical_block >= full_start && ext_end <= full_end) {
			/* Fully inside the interior: reads return zeros. */
			e->flags = OCSFS_EXT_UNWRITTEN;
		} else if (!(e->flags & OCSFS_EXT_COMPRESSED)) {
			/* Spans the interior boundary — physically zero the full
			 * blocks that intersect [full_start, full_end). */
			u64 zs   = max(e->logical_block, full_start);
			u64 ze   = min(ext_end, full_end);
			u64 phys = e->physical_block + (zs - e->logical_block);
			u64 k;

			for (k = 0; k < ze - zs; k++) {
				struct buffer_head *zbh =
					sb_getblk(inode->i_sb, phys + k);

				if (!zbh)
					continue;
				lock_buffer(zbh);
				memset(zbh->b_data, 0, sbi->s_block_size);
				set_buffer_uptodate(zbh);
				mark_buffer_dirty(zbh);
				unlock_buffer(zbh);
				sync_dirty_buffer(zbh);
				brelse(zbh);
			}
		}
	}

	/* ALTO-N1: journal flag change before returning */
	{
		int fr = ocsfs_flush_inode_locked(inode, false);

		if (fr)
			pr_warn_ratelimited(
				"ocsfs: zero_range inode flush failed (%d)\n",
				fr);
	}
	mark_inode_dirty(inode);
	mutex_unlock(&oi->i_extent_lock);

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * FALLOCATE
 *
 * Implements the fallocate(2) system call:
 *   - Default:           preallocate blocks (UNWRITTEN)
 *   - KEEP_SIZE:         preallocate but don't change i_size
 *   - PUNCH_HOLE:        deallocate blocks in a range
 *   - ZERO_RANGE:        zero a range (keep blocks allocated)
 *
 * This is the key thin provisioning API for VM disk images.
 * ═══════════════════════════════════════════════════════════════ */

long ocsfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	int ret = 0;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
		     FALLOC_FL_ZERO_RANGE))
		return -EOPNOTSUPP;

	if ((mode & FALLOC_FL_PUNCH_HOLE) && !(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	inode_lock(inode);

	/*
	 * All three operations (punch_hole, zero_range, prealloc) modify the
	 * inode's extent map. In clustered mode, hold DLM EX across the entire
	 * operation and flush before releasing so other nodes see a coherent
	 * extent map immediately after we release EX.
	 */
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			goto out_inode_unlock;
	}

	/* Punch and zero_range change the data visible through the extent map
	 * (data -> hole/zeros).  Flush dirty pages first, then (after the op, in
	 * out:) drop the now-stale clean pages so a buffered read reflects the new
	 * state instead of returning pre-punch/zero cached data.  The clustered
	 * lr_inv_lo/hi mechanism only covers *peer* nodes; the local page cache
	 * must be invalidated here too (caught by fsx).
	 *
	 * Flush the FULL edge blocks, not just [offset, offset+len): the partial
	 * sub-block zeroing (ocsfs_zero_within_block) reads/rewrites the whole
	 * edge block via the buffer_head, but the live data is in the iomap page
	 * cache.  The PRESERVED bytes of a partial edge block fall OUTSIDE
	 * [offset, offset+len); unless those whole blocks are flushed first, the
	 * buffer_head read sees stale on-disk data and the preserved bytes are
	 * lost (caught by fsx: a punch ending mid-block zeroed the bytes just past
	 * its end, because that block's tail had only-in-page-cache data). */
	if (mode & (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE)) {
		u64 bs = sbi->s_block_size;

		filemap_write_and_wait_range(inode->i_mapping,
					     round_down(offset, bs),
					     round_up(offset + len, bs) - 1);
	}

	if (mode & FALLOC_FL_PUNCH_HOLE) {
		ret = ocsfs_punch_hole(inode, offset, len);
		goto out;
	}

	if (mode & FALLOC_FL_ZERO_RANGE) {
		ret = ocsfs_zero_range(inode, offset, len);
		if (ret)
			goto out;
		if (!(mode & FALLOC_FL_KEEP_SIZE) &&
		    offset + len > inode->i_size) {
			i_size_write(inode, offset + len);
			mark_inode_dirty(inode);
		}
		goto out;
	}

	ret = ocsfs_prealloc_blocks(inode, offset, len);
	if (ret)
		goto out;

	if (!(mode & FALLOC_FL_KEEP_SIZE) && offset + len > inode->i_size) {
		i_size_write(inode, offset + len);
		mark_inode_dirty(inode);
	}

	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);

out:
	/* Drop stale clean pages over the punched/zeroed range on the local
	 * mapping; subsequent buffered reads repopulate from the updated extent
	 * map (hole -> zeros, or the zeroed blocks). */
	if (ret == 0 && (mode & (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE))) {
		filemap_invalidate_lock(inode->i_mapping);
		invalidate_inode_pages2_range(inode->i_mapping,
			offset >> PAGE_SHIFT,
			(offset + len - 1) >> PAGE_SHIFT);
		filemap_invalidate_unlock(inode->i_mapping);
	}

	if (sbi->s_clustered) {
		if (ret == 0) {
			int fr;

			/* ALTO-V3-2: record the modified block range so the next SH
			 * acquirer can do selective page cache invalidation (ARCH-7). */
			oi->i_lock_res.lr_inv_lo = (u64)(offset / sbi->s_block_size);
			oi->i_lock_res.lr_inv_hi = (u64)((offset + len +
							   sbi->s_block_size - 1) /
							  sbi->s_block_size);

			fr = ocsfs_flush_inode_locked(inode, true);
			if (fr)
				pr_warn_ratelimited(
					"ocsfs: fallocate inode flush failed (%d)\n", fr);
		}
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	}
out_inode_unlock:
	inode_unlock(inode);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * DISCARD / UNMAP PASSTHROUGH
 *
 * When blocks are freed (via punch_hole, truncate, or unlink),
 * issue a discard request to the underlying block device so the
 * SAN/SSD can reclaim the physical space.
 *
 * This is called from ocsfs_free_blocks() in bitmap.c if the
 * block device supports discard.
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_discard_blocks(struct super_block *sb, u64 block, u32 count)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	sector_t start_sector;
	sector_t nr_sectors;

	/* Check if the device supports discard */
	if (!bdev_max_discard_sectors(sb->s_bdev))
		return 0; /* silently skip — not an error */

	start_sector = block * (sbi->s_block_size / 512);
	nr_sectors = (sector_t)count * (sbi->s_block_size / 512);

	return blkdev_issue_discard(sb->s_bdev, start_sector, nr_sectors,
				    GFP_NOFS);
}

/* ═══════════════════════════════════════════════════════════════
 * THIN PROVISIONING REPORTING
 *
 * Helper to calculate actual (written) vs reserved (preallocated)
 * block counts for an inode.
 * ═══════════════════════════════════════════════════════════════ */

struct thin_stats_ctx { u64 written; u64 unwritten; };

static int thin_stats_iter(u64 logical, u64 physical, u32 length,
			   u16 flags, void *ctx)
{
	struct thin_stats_ctx *ts = ctx;

	(void)logical; (void)physical;
	if (flags & OCSFS_EXT_UNWRITTEN)
		ts->unwritten += length;
	else
		ts->written += length;
	return 0;
}

void ocsfs_thin_stats(struct inode *inode, u64 *written, u64 *unwritten)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	u64 w = 0, u = 0;
	u16 i;

	mutex_lock(&oi->i_extent_lock);

	if (oi->i_extent_tree_root) {
		struct thin_stats_ctx ts = {};

		ocsfs_extent_btree_iterate(inode, thin_stats_iter, &ts);
		mutex_unlock(&oi->i_extent_lock);
		*written   = ts.written;
		*unwritten = ts.unwritten;
		return;
	}

	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];

		if (e->flags & OCSFS_EXT_UNWRITTEN)
			u += e->length;
		else
			w += e->length;
	}

	mutex_unlock(&oi->i_extent_lock);

	*written = w;
	*unwritten = u;
}
