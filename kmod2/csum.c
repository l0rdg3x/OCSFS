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
		if (!ocsfs2_current_txn())
			sync_dirty_buffer(bh);
		brelse(bh);
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

/* Store CRCs for the folio range [pos, pos+len) being written back (buffered).
 * The data in the folio is final here, so the CRC matches what hits disk. */
void ocsfs2_csum_folio_range(struct super_block *sb, struct folio *folio,
			     u64 pos, unsigned int len, const struct iomap *iomap)
{
	u32 bs = sb->s_blocksize;
	u64 fpos = folio_pos(folio), o;

	if (!OCSFS2_SB(sb)->s_datacsum || iomap->type != IOMAP_MAPPED)
		return;
	for (o = pos; o < pos + len &&
		      o - iomap->offset < iomap->length; o += bs) {
		u64 phys = (iomap->addr + (o - iomap->offset)) / bs;
		void *k = kmap_local_folio(folio, o - fpos);
		u32 crc = ocsfs2_data_crc(sb, k);

		kunmap_local(k);
		ocsfs2_csum_set(sb, phys, crc);
	}
}

/* Store CRCs for the blocks of a write bio (O_DIRECT submit path). The user data
 * is in the bio (block-aligned for O_DIRECT) and final before submission. */
void ocsfs2_csum_bio(struct super_block *sb, struct bio *bio)
{
	u32 bs = sb->s_blocksize;
	u64 phys = bio->bi_iter.bi_sector >> (sb->s_blocksize_bits - 9);
	struct bvec_iter iter;
	struct bio_vec bv;

	if (!OCSFS2_SB(sb)->s_datacsum)
		return;
	bio_for_each_segment(bv, bio, iter) {
		unsigned int off = 0;

		while (off + bs <= bv.bv_len) {
			void *base = kmap_local_page(bv.bv_page);
			u32 crc = ocsfs2_data_crc(sb, base + bv.bv_offset + off);

			kunmap_local(base);
			ocsfs2_csum_set(sb, phys, crc);
			phys++;
			off += bs;
		}
	}
}
