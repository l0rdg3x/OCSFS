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
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/splice.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/mm.h>

/* OCSFS2_ALLOC_CAP_BLOCKS lives in ocsfs.h (shared with fallocate). */

/* ── copy-on-write (reflink/snapshot, Plan 4) ──
 * File data never lives in the buffer cache (it flows through file folios +
 * bios), so the CoW copy must use bios too — using sb_bread/sb_getblk here
 * would create a buffer-cache alias that goes stale against later writeback
 * bios (the v1 cow_extent coherence bug). One on-stack bio per block. */
static int cow_rw_block(struct super_block *sb, u64 phys, struct page *page,
			blk_opf_t op)
{
	struct bio bio;
	struct bio_vec bvec;
	int ret;

	bio_init(&bio, sb->s_bdev, &bvec, 1, op);
	bio.bi_iter.bi_sector = phys * (u64)(sb->s_blocksize >> 9);
	__bio_add_page(&bio, page, sb->s_blocksize, 0);
	ret = submit_bio_wait(&bio);
	bio_uninit(&bio);
	return ret;
}

int ocsfs2_copy_blocks(struct super_block *sb, u64 oldphys, u64 newphys,
		       u32 n)
{
	struct page *page = alloc_page(GFP_NOFS);
	u32 i;
	int ret = 0;

	if (!page)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		ret = cow_rw_block(sb, oldphys + i, page, REQ_OP_READ);
		if (ret)
			break;
		ret = cow_rw_block(sb, newphys + i, page, REQ_OP_WRITE);
		if (ret)
			break;
		/* A8: carry the data checksum to the block's new physical home so
		 * CoW'd / defrag'd data stays verifiable (the source's CRC remains
		 * valid for any sharer left behind). No-op unless checksums are on. */
		if (OCSFS2_SB(sb)->s_datacsum) {
			void *k = kmap_local_page(page);

			ocsfs2_csum_set(sb, newphys + i, ocsfs2_data_crc(sb, k));
			kunmap_local(k);
		}
	}
	__free_page(page);
	if (!ret)
		blkdev_issue_flush(sb->s_bdev);
	return ret;
}

/* Copy-on-write only the part of the shared extent @cover that this write
 * touches, [lblk, end_blk) clamped to the extent and the allocation cap, to
 * freshly allocated private blocks — atomically (Plan 3 txn). Block-granular so
 * a small write to a large reflinked extent copies little (real space-efficient
 * sharing), splitting the extent into kept-shared head/tail + a new written
 * middle. The data copy is durable before the extent is repointed, so a later
 * sub-block write RMW reads the correct base. Caller holds i_meta_lock; the
 * inode write lock serialises writers. */
static int ocsfs2_cow_range(struct inode *inode, const struct ocsfs2_extent *cover,
			    u64 lblk, u64 end_blk)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	struct ocsfs2_txn *txn;
	u64 cover_end = cover->logical + cover->length;
	u64 cs = lblk;
	u64 ce = min(cover_end, end_blk);
	u64 oldphys, newphys;
	u32 n;
	int ret;

	if (ce <= cs)
		ce = cs + 1;
	if (ce - cs > OCSFS2_ALLOC_CAP_BLOCKS)
		ce = cs + OCSFS2_ALLOC_CAP_BLOCKS;
	n = (u32)(ce - cs);
	oldphys = cover->physical + (cs - cover->logical);

	txn = ocsfs2_txn_begin(sb);
	if (!txn)
		return -ENOMEM;

	ret = ocsfs2_alloc_blocks(sb, oi->i_ag, n, &newphys);  /* bitmap in txn */
	if (ret)
		goto abort;
	ret = ocsfs2_copy_blocks(sb, oldphys, newphys, n);
	if (ret) {
		ocsfs2_free_blocks(sb, newphys, n);
		goto abort;
	}
	ret = ocsfs2_extent_remap_range(inode, cs, n, newphys, OCSFS2_EXT_WRITTEN);
	if (ret) {
		ocsfs2_free_blocks(sb, newphys, n);
		goto abort;
	}
	ocsfs2_free_blocks_rc(sb, oldphys, n);     /* dec the CoW'd sub-range */

	ret = ocsfs2_write_inode_block(inode);     /* new extent map, in txn */
	if (ret) {
		ocsfs2_txn_abort(txn);
		ocsfs2_reload_extents(inode);      /* undo the in-core split */
		return ret;
	}
	return ocsfs2_txn_commit(txn);
abort:
	ocsfs2_txn_abort(txn);
	return ret;
}

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

	/* A modifying op is a buffered/direct write OR a zero-range (fallocate /
	 * hole-punch edges). Both must copy-on-write a shared block; only a real
	 * write allocates holes and converts unwritten extents. */
	{
	bool modifying = flags & (IOMAP_WRITE | IOMAP_ZERO);

	ret = ocsfs2_extent_find(inode, lblk, &cover, &next_logical);
	if (ret == 0) {
		u64 off_in, remaining;

		/* shared (reflink/snapshot) block being modified -> copy-on-write so
		 * the other sharers stay isolated, then map the new private blocks */
		if (modifying && (cover.flags & OCSFS2_EXT_SHARED)) {
			/* An extent flagged SHARED can hold a MIX of still-shared
			 * (refcount >= 2) and now-private (refcount == 1) blocks:
			 * sharing flags the whole source extent but bumps only the
			 * cloned sub-range, and a peer's later CoW drops some blocks
			 * back to 1. Decide from EVERY block the write touches, not
			 * just the first. Checking only the first block could either
			 * skip a needed CoW (a buffered store then lands on the shared
			 * block and writeback corrupts the other sharer) or clear the
			 * SHARED flag over blocks that are still shared. */
			u64 cover_end = cover.logical + cover.length;
			u64 last = min(end_blk, cover_end);
			u64 b;
			bool any_shared = false;

			for (b = lblk; b < last; b++)
				if (ocsfs2_needs_cow(inode->i_sb, cover.physical +
						     (b - cover.logical))) {
					any_shared = true;
					break;
				}
			if (any_shared) {
				ret = ocsfs2_cow_range(inode, &cover, lblk,
						       end_blk);
				if (ret)
					goto out;
				ret = ocsfs2_extent_find(inode, lblk, &cover,
							 &next_logical);
				if (ret)
					goto out;   /* must exist after CoW */
			} else if (lblk == cover.logical && last == cover_end) {
				/* the whole extent is private now: drop the stale
				 * SHARED hint so later writes skip the recheck */
				ocsfs2_extent_update_phys(inode, cover.logical,
							  cover.length, cover.physical,
							  cover.flags & ~OCSFS2_EXT_SHARED);
				cover.flags &= ~OCSFS2_EXT_SHARED;
			}
			/* else: the touched range is private but the extent still
			 * has shared blocks elsewhere — write it in place (those
			 * blocks aren't touched) and keep the SHARED flag. */
		}

		off_in = lblk - cover.logical;
		remaining = cover.length - off_in;
		iomap->addr = (cover.physical + off_in) * (u64)bs;
		iomap->length = remaining * (u64)bs;
		iomap->type = (cover.flags & OCSFS2_EXT_UNWRITTEN) ?
			      IOMAP_UNWRITTEN : IOMAP_MAPPED;
		ret = 0;
		goto out;
	}
	}

	if (flags & IOMAP_WRITE) {
		bool batched = !ocsfs2_current_txn() && sbi->s_max_nodes <= 1;
		u64 want = end_blk - lblk;
		u64 phys;
		u32 alloc;

		/* J5-D (deferred single-node): JOIN the running batch so the bitmap
		 * allocation + extent insert share one commit with neighbouring writes
		 * (a repeatedly-appended leaf / bitmap is journaled once per batch). The
		 * extent insert only ever fails at node_alloc (ENOSPC), BEFORE it touches
		 * the btree, so on failure the op undoes its own partial work with a
		 * normal free — it never aborts the shared running txn. */
		if (batched) {
			ret = ocsfs2_run_begin(inode->i_sb);
			if (ret)
				goto out;
		}

		if (next_logical != U64_MAX && next_logical - lblk < want)
			want = next_logical - lblk;     /* don't overlap the next extent */
		if (want > OCSFS2_ALLOC_CAP_BLOCKS)
			want = OCSFS2_ALLOC_CAP_BLOCKS;
		if (want == 0)
			want = 1;

		/* Cluster speculative-prealloc: each ocsfs2_alloc_blocks is a synchronous
		 * SCSI CAW (bitmap claim) round-trip. A scattered small-write workload (a
		 * qemu-img / vma restore writing 64K qcow2 clusters) pays one per cluster
		 * -> ~6 MB/s. Instead claim a physically-contiguous run ONCE and carve
		 * WRITTEN extents out of it without further CAWs. The reservation is sized
		 * by the file size (XFS-style: small files don't over-reserve), and its
		 * unwritten tail is returned on close/evict (and leaked->fsck-reclaimed on
		 * crash — the integrity trade for speed). */
		if (sbi->s_cluster && oi->i_prealloc_left >= want) {
			phys = oi->i_prealloc_phys;
			oi->i_prealloc_phys += want;
			oi->i_prealloc_left -= (u32)want;
			alloc = (u32)want;
		} else {
			u32 chunk = (u32)want;

			if (sbi->s_cluster) {
				u64 isz = (i_size_read(inode) + bs - 1) / bs;
				u64 res = min_t(u64, isz, OCSFS2_ALLOC_CAP_BLOCKS);

				if (res > chunk)
					chunk = (u32)res;
				if (oi->i_prealloc_left) {   /* drop the too-small remnant */
					ocsfs2_free_blocks(inode->i_sb, oi->i_prealloc_phys,
							   oi->i_prealloc_left);
					oi->i_prealloc_phys = 0;
					oi->i_prealloc_left = 0;
				}
			}
			ret = ocsfs2_alloc_blocks(inode->i_sb, oi->i_ag, chunk, &phys);
			if (ret == -ENOSPC && chunk > (u32)want) {   /* no big contiguous run */
				chunk = (u32)want;
				ret = ocsfs2_alloc_blocks(inode->i_sb, oi->i_ag, chunk, &phys);
			}
			if (ret == -ENOSPC && want > 1) {
				want = 1;
				chunk = 1;
				ret = ocsfs2_alloc_blocks(inode->i_sb, oi->i_ag, 1, &phys);
			}
			if (ret) {
				if (batched)
					ocsfs2_run_end(inode->i_sb);
				goto out;
			}
			alloc = (u32)want;
			if (chunk > alloc) {            /* stash the surplus for next writes */
				oi->i_prealloc_phys = phys + alloc;
				oi->i_prealloc_left = chunk - alloc;
			}
		}

		ret = ocsfs2_extent_insert(inode, lblk, phys, alloc, OCSFS2_EXT_WRITTEN);
		if (ret) {
			ocsfs2_free_blocks(inode->i_sb, phys, alloc); /* undo the alloc */
			if (batched)
				ocsfs2_run_end(inode->i_sb);
			goto out;
		}
		inode->i_blocks += (u64)alloc * (bs / 512);

		if (batched)
			ocsfs2_run_end(inode->i_sb);   /* leave; the batch commits later */

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

const struct iomap_ops ocsfs2_iomap_ops = {
	.iomap_begin = ocsfs2_iomap_begin,
};

/* ── address_space operations ── */

/* ── A8 inline read verification (buffered) ──
 * Verify every data block's CRC on read and return -EIO on mismatch instead of
 * serving corruption. To keep readahead PIPELINED (a synchronous bio-per-folio
 * is QD1 and throttles sequential reads to the link's per-op latency), this
 * mirrors the O_DIRECT path: at submit time (process context) pre-read the
 * expected CRCs into a per-bio context, submit the read bio ASYNC, and verify in
 * the completion (crc32c + kmap_local — no sleeping) before iomap_finish_folio_read.
 * iomap only calls this for real IOMAP_MAPPED data within i_size (holes /
 * unwritten / post-EOF are zeroed without a read). Opt-in: non-checksummed
 * volumes keep iomap's default fast async path untouched. */
struct ocsfs2_rdv {
	struct folio	*folio;
	struct super_block *sb;
	size_t		poff;
	size_t		plen;
	u64		phys0;
	u32		nblk;
	u32		expected[];	/* pre-read stored CRC per block (0 = skip) */
};

static void ocsfs2_csum_read_end(struct bio *bio)
{
	struct ocsfs2_rdv *rv = bio->bi_private;
	struct super_block *sb = rv->sb;
	u32 bs = sb->s_blocksize;
	int err = blk_status_to_errno(bio->bi_status);
	u32 i;

	for (i = 0; !err && i < rv->nblk; i++) {
		void *k;
		u32 got;

		if (!rv->expected[i])
			continue;          /* unset -> skip */
		k = kmap_local_folio(rv->folio, rv->poff + (size_t)i * bs);
		got = ocsfs2_data_crc(sb, k);
		kunmap_local(k);
		if (got != rv->expected[i]) {
			pr_err_ratelimited("ocsfs2: DATA checksum mismatch on read at block %llu (have 0x%08x want 0x%08x)\n",
				(unsigned long long)(rv->phys0 + i), got,
				rv->expected[i]);
			err = -EIO;
		}
	}
	iomap_finish_folio_read(rv->folio, rv->poff, rv->plen, err);
	bio_put(bio);
	kfree(rv);
}

static int ocsfs2_csum_read_folio_range(const struct iomap_iter *iter,
					struct iomap_read_folio_ctx *ctx,
					size_t plen)
{
	struct folio *folio = ctx->cur_folio;
	struct super_block *sb = iter->inode->i_sb;
	const struct iomap *iomap = &iter->iomap;
	loff_t pos = iter->pos;
	size_t poff = offset_in_folio(folio, pos);
	u32 nblk = plen >> sb->s_blocksize_bits;
	struct ocsfs2_rdv *rv;
	struct bio *bio;

	rv = kmalloc(struct_size(rv, expected, nblk), GFP_NOFS);
	if (!rv)
		return -ENOMEM;        /* setup failure: core cleans up (no finish) */
	rv->folio = folio;
	rv->sb    = sb;
	rv->poff  = poff;
	rv->plen  = plen;
	rv->phys0 = (iomap->addr + (pos - iomap->offset)) >> sb->s_blocksize_bits;
	rv->nblk  = nblk;
	/* pre-read the stored CRCs now (process context, may sleep); the bio
	 * completion only recomputes + compares (no sleeping) */
	ocsfs2_csum_read_range(sb, rv->phys0, rv->expected, nblk);

	bio = bio_alloc(sb->s_bdev, 1, REQ_OP_READ, GFP_NOFS);
	if (!bio) {
		kfree(rv);
		return -ENOMEM;
	}
	bio->bi_iter.bi_sector = iomap_sector(iomap, pos);
	bio_add_folio_nofail(bio, folio, plen, poff);
	bio->bi_private = rv;
	bio->bi_end_io = ocsfs2_csum_read_end;
	submit_bio(bio);               /* async — pipelines across the readahead window */
	return 0;                      /* queued; finish happens in the completion */
}

static const struct iomap_read_ops ocsfs2_csum_read_ops = {
	.read_folio_range = ocsfs2_csum_read_folio_range,
	/* .submit_read = NULL: each range's bio is submitted immediately (async) */
};

static int ocsfs2_read_folio(struct file *file, struct folio *folio)
{
	struct super_block *sb = folio->mapping->host->i_sb;

	if (OCSFS2_SB(sb)->s_datacsum) {
		struct iomap_read_folio_ctx ctx = {
			.ops	   = &ocsfs2_csum_read_ops,
			.cur_folio = folio,
		};

		iomap_read_folio(&ocsfs2_iomap_ops, &ctx, NULL);
		return 0;
	}
	iomap_bio_read_folio(folio, &ocsfs2_iomap_ops);
	return 0;
}

static void ocsfs2_readahead(struct readahead_control *rac)
{
	struct super_block *sb = rac->mapping->host->i_sb;

	if (OCSFS2_SB(sb)->s_datacsum) {
		struct iomap_read_folio_ctx ctx = {
			.ops = &ocsfs2_csum_read_ops,
			.rac = rac,
		};

		iomap_readahead(&ocsfs2_iomap_ops, &ctx, NULL);
		return;
	}
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
	/* A8: record per-block CRCs for the data hitting disk (no-op unless enabled).
	 * J5-D: enrol the csum blocks in the running batch (journaled + coalesced). */
	{
		struct super_block *sb = wpc->inode->i_sb;
		bool batched = !ocsfs2_current_txn() &&
			       OCSFS2_SB(sb)->s_datacsum &&
			       OCSFS2_SB(sb)->s_max_nodes <= 1;

		if (batched && ocsfs2_run_begin(sb))
			batched = false;
		ocsfs2_csum_folio_range(sb, folio, pos, len, &wpc->iomap);
		if (batched)
			ocsfs2_run_end(sb);
	}
	return iomap_add_to_ioend(wpc, folio, pos, end_pos, len);
}

static const struct iomap_writeback_ops ocsfs2_writeback_ops = {
	.writeback_range  = ocsfs2_writeback_range,
	.writeback_submit = iomap_ioend_writeback_submit,
};

/* ── A8 inline read verification (O_DIRECT) ──
 * A read bio's bi_end_io runs in interrupt/softirq context, but reading a stored
 * checksum slot sleeps (coherent metadata read). So we split the work: at submit
 * time (process context, may sleep) we pre-read the expected CRC of every block
 * the bio covers into a small context; in the bio completion (no sleeping) we
 * only recompute the data CRCs (crc32c + kmap_local — both atomic-safe) and
 * compare. A mismatch turns the bio into an I/O error so the read returns -EIO.
 * iomap owns bio->bi_private (the iomap_dio) and bi_end_io; we save and restore
 * them, chaining to iomap_dio_bio_end_io at the end. Opt-in (s_datacsum); on any
 * setup failure we just submit the read unverified (the scrub still covers it). */
struct ocsfs2_dio_rv {
	struct super_block *sb;
	u64		phys0;		/* first data block in the bio */
	unsigned int	nblk;		/* number of blocks covered */
	void		*orig_private;	/* the iomap_dio */
	bio_end_io_t	*orig_end_io;	/* iomap_dio_bio_end_io */
	struct bvec_iter saved_iter;	/* bi_iter snapshot (consumed after I/O) */
	u32		expected[];	/* stored CRC per block (0 = unset/skip) */
};

static void ocsfs2_dio_read_end(struct bio *bio)
{
	struct ocsfs2_dio_rv *rv = bio->bi_private;
	struct super_block *sb = rv->sb;
	u32 bs = sb->s_blocksize;

	if (!bio->bi_status) {			/* device read ok — verify data */
		struct bvec_iter it;
		struct bio_vec bv;
		u32 blk = 0, filled = 0, crc = ~0U;

		/* accumulate each block's CRC over the byte stream (incremental
		 * crc32c) so a 4 KiB block split across segments — O_DIRECT buffers
		 * are only 512 B-aligned — is verified against the right slot */
		__bio_for_each_segment(bv, bio, it, rv->saved_iter) {
			void *base = kmap_local_page(bv.bv_page);
			u8 *p = (u8 *)base + bv.bv_offset;
			u32 left = bv.bv_len;

			while (left && blk < rv->nblk) {
				u32 take = min(bs - filled, left);

				crc = ocsfs2_crc32c(crc, p, take);
				filled += take;
				p += take;
				left -= take;
				if (filled == bs) {
					u32 want = rv->expected[blk];

					if (want && crc != want) {
						pr_err_ratelimited("ocsfs2: DATA checksum mismatch on O_DIRECT read at block %llu (have 0x%08x want 0x%08x)\n",
							(unsigned long long)(rv->phys0 + blk),
							crc, want);
						bio->bi_status = BLK_STS_IOERR;
					}
					blk++;
					crc = ~0U;
					filled = 0;
				}
			}
			kunmap_local(base);
			if (bio->bi_status)
				break;
		}
	}
	bio->bi_private = rv->orig_private;	/* restore the iomap_dio */
	bio->bi_end_io = rv->orig_end_io;
	kfree(rv);
	iomap_dio_bio_end_io(bio);
}

/* Arm @bio (a read) for inline verification. Returns 0 if armed, negative if it
 * should be submitted unverified. */
static int ocsfs2_dio_read_arm(struct super_block *sb, struct bio *bio)
{
	u32 bs = sb->s_blocksize;
	unsigned int nblk = bio->bi_iter.bi_size / bs;
	u64 phys0 = bio->bi_iter.bi_sector >> (sb->s_blocksize_bits - 9);
	struct ocsfs2_dio_rv *rv;
	unsigned int i;
	bool any = false;

	/* only verify whole blocks starting on a block boundary; a sub-block /
	 * unaligned-start O_DIRECT read can't be mapped to per-block CRC slots */
	if (!nblk || (bio->bi_iter.bi_sector & ((bs >> SECTOR_SHIFT) - 1)))
		return -EINVAL;
	rv = kmalloc(struct_size(rv, expected, nblk), GFP_NOFS);
	if (!rv)
		return -ENOMEM;
	/* one coherent read per checksum block, not per data block */
	ocsfs2_csum_read_range(sb, phys0, rv->expected, nblk);
	for (i = 0; i < nblk; i++)
		if (rv->expected[i]) {
			any = true;
			break;
		}
	if (!any) {			/* nothing stored to verify (e.g. fresh) */
		kfree(rv);
		return -ENODATA;
	}
	rv->sb		 = sb;
	rv->phys0	 = phys0;
	rv->nblk	 = nblk;
	rv->orig_private = bio->bi_private;
	rv->orig_end_io	 = bio->bi_end_io;
	rv->saved_iter	 = bio->bi_iter;   /* the block layer consumes bi_iter on I/O */
	bio->bi_private	 = rv;
	bio->bi_end_io	 = ocsfs2_dio_read_end;
	return 0;
}

/* A8: O_DIRECT submit hook — checksum a write bio's blocks before submission
 * (the user data is in the bio and final here), or arm a read bio for inline
 * verification, then submit. */
static void ocsfs2_dio_submit(const struct iomap_iter *iter, struct bio *bio,
			      loff_t file_offset)
{
	struct super_block *sb = iter->inode->i_sb;

	(void)file_offset;
	if (op_is_write(bio_op(bio))) {
		/* J5-D: enrol the csum blocks in the running batch (journaled + coalesced
		 * with the data-write metadata) instead of a separate per-write sync. */
		bool batched = !ocsfs2_current_txn() &&
			       OCSFS2_SB(sb)->s_datacsum &&
			       OCSFS2_SB(sb)->s_max_nodes <= 1;

		if (batched && ocsfs2_run_begin(sb))
			batched = false;
		ocsfs2_csum_bio(sb, bio);
		if (batched)
			ocsfs2_run_end(sb);
	} else if (OCSFS2_SB(sb)->s_datacsum)
		ocsfs2_dio_read_arm(sb, bio);	/* best-effort; unverified on failure */
	submit_bio(bio);
}

static const struct iomap_dio_ops ocsfs2_dio_ops = {
	.submit_io = ocsfs2_dio_submit,
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
		ret = iomap_dio_rw(iocb, to, &ocsfs2_iomap_ops, &ocsfs2_dio_ops,
				   0, NULL, 0);
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

	/* take (or wait for) the EX write-owner lease — deferred from open so a
	 * live-migration destination could open the disk; the first write claims it */
	ret = ocsfs2_inode_ensure_writable(inode);
	if (ret)
		goto out;

	if (iocb->ki_flags & IOCB_DIRECT) {
		ret = iomap_dio_rw(iocb, from, &ocsfs2_iomap_ops, &ocsfs2_dio_ops,
				   0, NULL, 0);
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
	/* The deferred journal batches commits and no longer flushes per commit;
	 * fsync is a durability barrier, so force the running batch out to the ring
	 * and flush the device cache to make this inode's metadata + data durable. */
	if (!ret) {
		int r = ocsfs2_run_flush(inode->i_sb);

		if (r)
			ret = r;
	}
	ocsfs2_csum_flush(inode->i_sb);   /* make this data's deferred csums durable */
	if (!ret)
		blkdev_issue_flush(inode->i_sb->s_bdev);
	return ret;
}

/* O_DIRECT is opt-in per open: the VFS rejects open(O_DIRECT) unless the file
 * advertises FMODE_CAN_ODIRECT (the v1 ODIRECT-1 bug was the missing flag). */
static int ocsfs2_file_open(struct inode *inode, struct file *file)
{
	int ret;

	file->f_mode |= FMODE_CAN_ODIRECT;
	ret = generic_file_open(inode, file);
	if (ret)
		return ret;
	/* single-writer ownership: EX lease for write opens, SH for read-only.
	 * Fails (-EBUSY) if a live peer holds a conflicting lease. */
	return ocsfs2_inode_open_lease(inode, file->f_mode & FMODE_WRITE);
}

static int ocsfs2_file_release(struct inode *inode, struct file *file)
{
	ocsfs2_inode_close_lease(inode);
	return 0;
}

/* mmap is intentionally NOT supported. OCSFS v2 targets Proxmox image storage:
 * QEMU VM disks (raw/qcow2) are accessed via pread/pwrite/aio, and LXC containers
 * are stored as raw images on a loop device (the plugin advertises subvol=0, so
 * Proxmox never unpacks a container rootfs directly onto OCSFS). The loop driver
 * uses read_iter/write_iter on its backing file — never mmap. No path in the
 * Proxmox workload mmaps an OCSFS file, so omitting .mmap (mmap() -> -ENODEV)
 * costs nothing and removes the mmap+CoW stale-MAPREAD hazard class entirely
 * (a mapped page could otherwise survive a reflink/CoW that rewrote its backing
 * block, serving stale data). */

/* copy_file_range: byte-granular server-side copy. We go through the generic
 * splice helper (read + buffered write) so unaligned ranges work; block-aligned
 * fast cloning is available separately via FICLONE (->remap_file_range). */
static ssize_t ocsfs2_copy_file_range(struct file *in, loff_t pos_in,
				      struct file *out, loff_t pos_out,
				      size_t len, unsigned int flags)
{
	return splice_copy_file_range(in, pos_in, out, pos_out, len);
}

const struct file_operations ocsfs2_file_fops = {
	.open             = ocsfs2_file_open,
	.release          = ocsfs2_file_release,     /* drop the ownership lease */
	.llseek           = ocsfs2_llseek,           /* + SEEK_HOLE / SEEK_DATA */
	.read_iter        = ocsfs2_file_read_iter,
	.write_iter       = ocsfs2_file_write_iter,
	/* no .mmap: unused by the Proxmox workload; see comment above */
	.fsync            = ocsfs2_fsync,
	.fallocate        = ocsfs2_fallocate,        /* prealloc / punch / zero */
	.splice_read      = filemap_splice_read,
	.splice_write     = iter_file_splice_write,
	.copy_file_range  = ocsfs2_copy_file_range,  /* splice; reflink is FICLONE */
	.unlocked_ioctl   = ocsfs2_ioctl,           /* OCSFS_IOC_SNAP_CREATE */
	.remap_file_range = ocsfs2_remap_file_range, /* FICLONE / cp --reflink */
};
