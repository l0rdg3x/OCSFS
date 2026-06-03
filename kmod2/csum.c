// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — csum.c
 * A8: per-data-block CRC32c checksums for silent-corruption detection on any SAN
 * (one that lacks its own integrity, unlike ZFS). Each DATA block has a u32 CRC
 * stored in a per-AG checksum region (reserved by mkfs when -C is given, gated by
 * the RO_COMPAT_DATACSUM feature). The checksum is keyed by PHYSICAL block, so it
 * naturally follows reflink/CoW sharing: a shared block has one checksum that all
 * sharers read; a CoW writes a fresh block with its own checksum.
 *
 *   write path -> ocsfs2_csum_set(phys, crc)   (writeback + O_DIRECT submit)
 *   scrub      -> ocsfs2_csum_read(phys) and compare to the recomputed CRC
 *
 * Coherence mirrors the bitmap / inode table: single-node writes the slot
 * directly (sync); cluster updates the 4-byte slot via SCSI Compare-and-Write
 * (peers update other slots in the same block) and reads it coherently.
 * A stored value of 0 means "unset" (never-written / fallocate-zeroed block):
 * the scrub skips it — so a block whose data CRC is genuinely 0 is simply not
 * verified (≈1/2^32, harmless: a missed check, never a false positive).
 */
#include "ocsfs.h"
#include <linux/buffer_head.h>
#include <linux/iomap.h>
#include <linux/highmem.h>
#include <linux/bio.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/blkdev.h>

static struct ocsfs2_ag_info *csum_ag(struct ocsfs2_sb_info *sbi, u64 phys)
{
	u32 ag;

	for (ag = 0; ag < READ_ONCE(sbi->s_ag_count); ag++) {
		struct ocsfs2_ag_info *ai = &sbi->s_ags[ag];

		if (phys >= ai->block_start &&
		    phys < ai->block_start + ai->block_count)
			return ai;
	}
	return NULL;
}

/* Locate the checksum slot for data block @phys: returns 0 and fills *byte_off
 * (absolute byte offset of the u32 slot), or -1 if @phys has no slot (not a data
 * block / checksums disabled for the AG). */
static int csum_slot(struct super_block *sb, u64 phys, u64 *byte_off)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_ag_info *ai = csum_ag(sbi, phys);
	u64 data_start, idx;

	if (!ai || !ai->csum_off)
		return -1;
	data_start = ai->data_off / sb->s_blocksize;
	if (phys < data_start)
		return -1;                 /* metadata block: has its own CRC */
	idx = phys - data_start;
	if (idx >= ai->data_blocks)
		return -1;
	*byte_off = ai->csum_off + idx * sizeof(__le32);
	return 0;
}

void ocsfs2_csum_set(struct super_block *sb, u64 phys, u32 crc)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u64 byte;
	__le32 v;

	if (!sbi->s_datacsum || csum_slot(sb, phys, &byte))
		return;
	v = cpu_to_le32(crc);

	if (sbi->s_cluster) {
		/* atomic + coherent update of the 4-byte slot (peers share the block) */
		ocsfs2_cl_caw_record(sb, byte, &v, sizeof(v));
		return;
	}
	{
		u64 blk = byte / sb->s_blocksize;
		u32 off = byte % sb->s_blocksize;
		struct buffer_head *bh = sb_bread(sb, blk);

		if (!bh)
			return;
		if (ocsfs2_current_txn())
			ocsfs2_jbuf(bh);
		memcpy(bh->b_data + off, &v, sizeof(v));
		mark_buffer_dirty(bh);
		if (!ocsfs2_current_txn() && !sbi->s_csum_async)
			sync_dirty_buffer(bh);
		brelse(bh);
	}
}

/* Verify one data block @data (sb->s_blocksize bytes) against the CRC stored for
 * physical block @phys. Returns 0 if it matches or no checksum is stored
 * (unset / checksums disabled), -EIO on a real mismatch. This is the inline
 * read-time check (the scrub uses the same compare in bulk). Sleeps (reads the
 * checksum slot coherently) — call only from process context (buffered read),
 * never from a bio completion; the O_DIRECT path pre-reads the expected CRCs in
 * ocsfs2_iomap.c and compares them in the non-sleeping bio end_io. */
int ocsfs2_csum_check(struct super_block *sb, u64 phys, const void *data)
{
	u32 want = ocsfs2_csum_read(sb, phys);
	u32 got;

	if (!want)
		return 0;
	got = ocsfs2_data_crc(sb, data);
	if (likely(got == want))
		return 0;
	pr_err_ratelimited("ocsfs2: DATA checksum mismatch on read at block %llu (have 0x%08x want 0x%08x)\n",
			   (unsigned long long)phys, got, want);
	return -EIO;
}

/* Drop the stored data checksum for blocks [phys, phys+count) — called when they
 * return to the allocator (ocsfs2_free_blocks). Without this a freed block keeps
 * its old CRC, and a later reuse that writes no data (fallocate preallocation, a
 * fresh-but-unwritten allocation) would read the previous owner's CRC over the
 * now-zeroed block and false-positive. A stored 0 means "unset" -> the verify is
 * skipped until the next real write restores it. No-op for metadata blocks (no
 * slot) and on non-checksummed volumes. */
void ocsfs2_csum_clear_range(struct super_block *sb, u64 phys, u32 count)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 i = 0;

	if (!sbi->s_datacsum || !count)
		return;

	if (sbi->s_cluster) {
		for (i = 0; i < count; i++)
			ocsfs2_csum_set(sb, phys + i, 0);   /* coherent CAW per slot */
		return;
	}
	/* single-node: zero contiguous slots one checksum block at a time */
	while (i < count) {
		struct ocsfs2_ag_info *ai = csum_ag(sbi, phys + i);
		u64 data_start, idx, byte, blk;
		u32 off, room, n;
		struct buffer_head *bh;

		if (!ai || !ai->csum_off) {
			i++;
			continue;
		}
		data_start = ai->data_off / sb->s_blocksize;
		if (phys + i < data_start) {       /* metadata block: no slot */
			i++;
			continue;
		}
		idx = (phys + i) - data_start;
		if (idx >= ai->data_blocks) {
			i++;
			continue;
		}
		byte = ai->csum_off + idx * sizeof(__le32);
		blk = byte / sb->s_blocksize;
		off = byte % sb->s_blocksize;
		n = count - i;                            /* clamp: block, AG, range */
		room = (sb->s_blocksize - off) / sizeof(__le32);
		if (n > room)
			n = room;
		room = ai->data_blocks - idx;
		if (n > room)
			n = room;

		bh = sb_bread(sb, blk);
		if (!bh) {
			i += n;
			continue;
		}
		if (ocsfs2_current_txn())
			ocsfs2_jbuf(bh);
		memset(bh->b_data + off, 0, (size_t)n * sizeof(__le32));
		mark_buffer_dirty(bh);
		if (!ocsfs2_current_txn())
			sync_dirty_buffer(bh);
		brelse(bh);
		i += n;
	}
}

u32 ocsfs2_csum_read(struct super_block *sb, u64 phys)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u64 byte, blk;
	u32 off, crc = 0;
	struct buffer_head *bh;

	if (!sbi->s_datacsum || csum_slot(sb, phys, &byte))
		return 0;
	blk = byte / sb->s_blocksize;
	off = byte % sb->s_blocksize;
	bh = ocsfs2_meta_bread(sb, blk);   /* coherent in cluster */
	if (!bh)
		return 0;
	crc = le32_to_cpu(*(__le32 *)(bh->b_data + off));
	brelse(bh);
	return crc;
}

/* Batched read of stored CRCs for contiguous blocks [phys0, phys0+n) into
 * out[0..n). Reads each containing checksum block ONCE (one coherent meta_bread
 * per checksum block) instead of once per data block — decisive for the inline
 * read verify in cluster mode, where every meta_bread is an *uncached* coherent
 * device read: a sequential read of 1024 blocks sharing one checksum block goes
 * from 1024 device reads to 1. out[i] = 0 means unset / no slot (skip verify). */
void ocsfs2_csum_read_range(struct super_block *sb, u64 phys0, u32 *out, u32 n)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 i;

	for (i = 0; i < n; i++)
		out[i] = 0;
	if (!sbi->s_datacsum)
		return;
	i = 0;
	while (i < n) {
		struct ocsfs2_ag_info *ai = csum_ag(sbi, phys0 + i);
		u64 data_start, idx, byte, blk;
		u32 off, room, cnt, k;
		struct buffer_head *bh;

		if (!ai || !ai->csum_off) {
			i++;
			continue;
		}
		data_start = ai->data_off / sb->s_blocksize;
		if (phys0 + i < data_start) {
			i++;
			continue;
		}
		idx = (phys0 + i) - data_start;
		if (idx >= ai->data_blocks) {
			i++;
			continue;
		}
		byte = ai->csum_off + idx * sizeof(__le32);
		blk = byte / sb->s_blocksize;
		off = byte % sb->s_blocksize;
		cnt = n - i;
		room = (sb->s_blocksize - off) / sizeof(__le32);
		if (cnt > room)
			cnt = room;
		room = ai->data_blocks - idx;
		if (cnt > room)
			cnt = room;
		bh = ocsfs2_meta_bread(sb, blk);   /* coherent; ONE read for cnt slots */
		if (bh) {
			__le32 *slot = (__le32 *)(bh->b_data + off);

			for (k = 0; k < cnt; k++)
				out[i + k] = le32_to_cpu(slot[k]);
			brelse(bh);
		}
		i += cnt;
	}
}

/* Batched store of CRCs for a contiguous run of physical blocks [phys0, phys0+n);
 * crcs[i] is the CRC for block phys0+i. Groups slots by their containing checksum
 * block so the single-node path syncs the block ONCE (not once per slot) and the
 * cluster path issues ONE CAW per block — decisive for large sequential writes (a
 * 1 MiB writeback = 256 contiguous blocks sharing a single checksum block: 1 sync
 * instead of 256). Equivalent on-disk result to calling ocsfs2_csum_set per block. */
void ocsfs2_csum_set_range(struct super_block *sb, u64 phys0, const u32 *crcs,
			   u32 n)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 i = 0;

	if (!sbi->s_datacsum || !n)
		return;
	while (i < n) {
		struct ocsfs2_ag_info *ai = csum_ag(sbi, phys0 + i);
		u64 data_start, idx, byte, blk;
		u32 off, room, cnt, k;

		if (!ai || !ai->csum_off) {
			i++;
			continue;
		}
		data_start = ai->data_off / sb->s_blocksize;
		if (phys0 + i < data_start) {
			i++;
			continue;
		}
		idx = (phys0 + i) - data_start;
		if (idx >= ai->data_blocks) {
			i++;
			continue;
		}
		byte = ai->csum_off + idx * sizeof(__le32);
		blk = byte / sb->s_blocksize;
		off = byte % sb->s_blocksize;
		cnt = n - i;                              /* clamp: block, AG, range */
		room = (sb->s_blocksize - off) / sizeof(__le32);
		if (cnt > room)
			cnt = room;
		room = ai->data_blocks - idx;
		if (cnt > room)
			cnt = room;

		if (sbi->s_cluster) {
			/* one CAW per logical block of contiguous slots */
			unsigned int lbs = bdev_logical_block_size(sb->s_bdev);
			__le32 *buf = kmalloc_array(cnt, sizeof(__le32), GFP_NOFS);
			u32 done = 0;

			if (!buf) {                       /* fall back to per-slot */
				for (k = 0; k < cnt; k++)
					ocsfs2_csum_set(sb, phys0 + i + k,
							crcs[i + k]);
				i += cnt;
				continue;
			}
			for (k = 0; k < cnt; k++)
				buf[k] = cpu_to_le32(crcs[i + k]);
			while (done < cnt) {
				u64 b = byte + (u64)done * sizeof(__le32);
				u32 lroom = (lbs - (u32)(b % lbs)) / sizeof(__le32);
				u32 m = min(cnt - done, lroom);

				ocsfs2_cl_caw_record(sb, b, &buf[done],
						     m * sizeof(__le32));
				done += m;
			}
			kfree(buf);
		} else {
			struct buffer_head *bh = sb_bread(sb, blk);

			if (bh) {
				__le32 *slot = (__le32 *)(bh->b_data + off);

				if (ocsfs2_current_txn())
					ocsfs2_jbuf(bh);
				for (k = 0; k < cnt; k++)
					slot[k] = cpu_to_le32(crcs[i + k]);
				mark_buffer_dirty(bh);
				/* sync once per block, unless -o csum_async defers it */
				if (!ocsfs2_current_txn() && !sbi->s_csum_async)
					sync_dirty_buffer(bh);
				brelse(bh);
			}
		}
		i += cnt;
	}
}

/* Store CRCs for the folio range [pos, pos+len) being written back (buffered).
 * The data in the folio is final here, so the CRC matches what hits disk. The
 * blocks are contiguous (one iomap extent), so collect their CRCs and store the
 * whole run in one batch (see ocsfs2_csum_set_range). */
void ocsfs2_csum_folio_range(struct super_block *sb, struct folio *folio,
			     u64 pos, unsigned int len, const struct iomap *iomap)
{
	u32 bs = sb->s_blocksize;
	u64 fpos = folio_pos(folio), o;
	u64 end = min_t(u64, pos + len, iomap->offset + iomap->length);
	u64 phys0;
	u32 n, cap;
	u32 *crcs;

	if (!OCSFS2_SB(sb)->s_datacsum || iomap->type != IOMAP_MAPPED ||
	    end <= pos)
		return;
	cap = (u32)((end - pos + bs - 1) / bs);
	crcs = kmalloc_array(cap, sizeof(u32), GFP_NOFS);
	if (!crcs) {                                  /* fallback: per-block */
		for (o = pos; o < end; o += bs) {
			u64 phys = (iomap->addr + (o - iomap->offset)) / bs;
			void *k = kmap_local_folio(folio, o - fpos);
			u32 crc = ocsfs2_data_crc(sb, k);

			kunmap_local(k);
			ocsfs2_csum_set(sb, phys, crc);
		}
		return;
	}
	phys0 = (iomap->addr + (pos - iomap->offset)) / bs;
	for (o = pos, n = 0; o < end; o += bs) {
		void *k = kmap_local_folio(folio, o - fpos);

		crcs[n++] = ocsfs2_data_crc(sb, k);
		kunmap_local(k);
	}
	ocsfs2_csum_set_range(sb, phys0, crcs, n);
	kfree(crcs);
}

/* Store CRCs for the blocks of a write bio (the O_DIRECT submit hook).
 *
 * The bio covers the device byte range [start, end). O_DIRECT buffers need only
 * be aligned to the device LOGICAL sector (512 B), and qemu/qcow2 issues
 * SUB-block writes (e.g. 8-byte qcow2 L1/L2 entries) — so the bio may start
 * mid-block and/or be smaller than one FS block. Handle that exactly:
 *   - a block FULLY inside [start,end): compute its CRC (crc32c accumulated over
 *     the byte stream, so a block split across bio segments is still correct);
 *   - a block only PARTIALLY written (head/tail): we don't have its other bytes
 *     here, so CLEAR its checksum (unset) rather than leave a stale one — the
 *     read verify then skips it (no false positive) until a full-block rewrite
 *     or the scrub.
 * (A naive per-segment, whole-block-assuming loop stored WRONG CRCs and failed
 * reads of a perfectly good image — found via `qemu-img convert -t none` onto a
 * -C volume, where a sub-block O_DIRECT write left the block's stale CRC of the
 * zero-init done earlier through the page cache.) */
void ocsfs2_csum_bio(struct super_block *sb, struct bio *bio)
{
	u32 bs = sb->s_blocksize;
	u64 start = (u64)bio->bi_iter.bi_sector << SECTOR_SHIFT;
	u32 size = bio->bi_iter.bi_size;
	u64 end = start + size;
	u64 ff = round_up(start, bs) / bs;        /* first fully-covered block */
	u64 fl = end / bs;                        /* one past last fully-covered */
	u32 nfull = (fl > ff) ? (u32)(fl - ff) : 0;
	struct bvec_iter iter;
	struct bio_vec bv;
	u32 *crcs;
	u32 n = 0, filled = 0, crc = ~0U;
	u64 skip;

	if (!OCSFS2_SB(sb)->s_datacsum || !size)
		return;
	/* partial head / tail blocks can't be checksummed from this bio alone */
	if (start & (bs - 1))
		ocsfs2_csum_clear_range(sb, start / bs, 1);
	if (end & (bs - 1))
		ocsfs2_csum_clear_range(sb, (end - 1) / bs, 1);
	if (!nfull)
		return;
	crcs = kmalloc_array(nfull, sizeof(u32), GFP_NOFS);
	if (!crcs) {
		ocsfs2_csum_clear_range(sb, ff, nfull);   /* can't csum -> unset (safe) */
		return;
	}
	skip = (u64)ff * bs - start;              /* bytes before the first full block */
	bio_for_each_segment(bv, bio, iter) {
		void *base = kmap_local_page(bv.bv_page);
		u8 *p = (u8 *)base + bv.bv_offset;
		u32 left = bv.bv_len;

		if (skip) {
			u32 s = min_t(u64, skip, left);

			p += s;
			left -= s;
			skip -= s;
		}
		while (left && n < nfull) {
			u32 take = min(bs - filled, left);

			crc = ocsfs2_crc32c(crc, p, take);   /* incremental */
			filled += take;
			p += take;
			left -= take;
			if (filled == bs) {
				crcs[n++] = crc;
				crc = ~0U;
				filled = 0;
			}
		}
		kunmap_local(base);
	}
	ocsfs2_csum_set_range(sb, ff, crcs, n);
	kfree(crcs);
}
