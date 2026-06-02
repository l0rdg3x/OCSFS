// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — scrub.c
 * Online metadata scrub (D5): walk the live filesystem and re-verify every
 * on-disk checksum — superblock, AG headers, all used inodes, extent- and
 * refcount-B+tree nodes, and xattr blocks — to catch silent metadata bitrot
 * without unmounting for fsck. Reads coherently (ocsfs2_meta_bread: fresh via
 * bio in cluster), so it sees committed peer state; every metadata block is
 * written atomically with its CRC (CAW / sync), so a concurrent mutation is
 * never observed half-written. Strictly read-only.
 *
 * Triggered by OCSFS_IOC_SCRUB (admin/cron, like xfs_scrub / fstrim).
 *
 * NOTE: this verifies *metadata* integrity. Per-data-block checksums are a
 * separate, larger change (a feature-flagged per-AG checksum region plus write
 * hooks on both the buffered writeback AND the O_DIRECT path Proxmox uses, with
 * CAW on the shared region in cluster) and are intentionally not bundled here.
 */
#include "ocsfs.h"
#include <linux/blkdev.h>

#define SCRUB_MAX_DEPTH 16

static bool block_crc_ok(const void *buf, u32 crc_off, __le32 stored)
{
	return ocsfs2_crc32c(~0U, buf, crc_off) == le32_to_cpu(stored);
}

/* refcount B+tree node (single block in Plan 4; verify its CRC + magic) */
static void scrub_rc_root(struct super_block *sb, u64 root,
			  struct ocsfs2_scrub_result *res)
{
	struct buffer_head *bh;
	struct ocsfs2_disk_rc_node *n;

	if (!root)
		return;
	bh = ocsfs2_meta_bread(sb, root);
	if (!bh) { res->errors++; return; }
	n = (struct ocsfs2_disk_rc_node *)bh->b_data;
	res->checked++;
	if (le32_to_cpu(n->rn_magic) != OCSFS2_RC_NODE_MAGIC ||
	    !block_crc_ok(n, offsetof(struct ocsfs2_disk_rc_node, rn_checksum),
			  n->rn_checksum)) {
		pr_warn("ocsfs2: scrub: bad refcount node at %llu\n",
			(unsigned long long)root);
		res->errors++;
	}
	brelse(bh);
}

/* extent B+tree: verify each node's CRC, recursing into internal children */
static void scrub_ext_node(struct super_block *sb, u64 blk,
			   struct ocsfs2_scrub_result *res, int depth)
{
	struct buffer_head *bh;
	struct ocsfs2_disk_ext_node *n;
	u16 level, nr, i;

	if (!blk || depth > SCRUB_MAX_DEPTH)
		return;
	bh = ocsfs2_meta_bread(sb, blk);
	if (!bh) { res->errors++; return; }
	n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	res->checked++;
	if (le32_to_cpu(n->en_magic) != OCSFS2_EXT_NODE_MAGIC ||
	    !block_crc_ok(n, offsetof(struct ocsfs2_disk_ext_node, en_checksum),
			  n->en_checksum)) {
		pr_warn("ocsfs2: scrub: bad extent node at %llu\n",
			(unsigned long long)blk);
		res->errors++;
		brelse(bh);
		return;
	}
	level = le16_to_cpu(n->en_level);
	nr = le16_to_cpu(n->en_nr);
	if (level > 0) {
		struct ocsfs2_disk_ext_ptr *p = (void *)n->en_body;
		u16 cap = sizeof(n->en_body) / sizeof(*p);

		for (i = 0; i < nr && i < cap; i++)
			scrub_ext_node(sb, le64_to_cpu(p[i].ep_child), res, depth + 1);
	}
	brelse(bh);
}

static void scrub_xattr(struct super_block *sb, u64 blk,
			struct ocsfs2_scrub_result *res)
{
	struct buffer_head *bh;
	struct ocsfs2_disk_xattr_header *h;
	u32 off = offsetof(struct ocsfs2_disk_xattr_header, xh_checksum);
	u32 bs = sb->s_blocksize, c;

	if (!blk)
		return;
	bh = ocsfs2_meta_bread(sb, blk);
	if (!bh) { res->errors++; return; }
	h = (struct ocsfs2_disk_xattr_header *)bh->b_data;
	res->checked++;
	c = ocsfs2_crc32c(~0U, bh->b_data, off);
	c = ocsfs2_crc32c(c, bh->b_data + off + 4, bs - off - 4);
	if (le32_to_cpu(h->xh_magic) != OCSFS2_XATTR_MAGIC ||
	    c != le32_to_cpu(h->xh_checksum)) {
		pr_warn("ocsfs2: scrub: bad xattr block at %llu\n",
			(unsigned long long)blk);
		res->errors++;
	}
	brelse(bh);
}

/* A8: verify the stored CRC of every data block in an extent (skip unset=0) */
static void scrub_data_extent(struct super_block *sb, u64 phys, u32 len,
			      struct ocsfs2_scrub_result *res)
{
	u32 i;

	for (i = 0; i < len; i++) {
		u32 stored = ocsfs2_csum_read(sb, phys + i);
		struct buffer_head *bh;
		u32 crc;

		if (!stored)
			continue;
		bh = ocsfs2_meta_bread(sb, phys + i);
		if (!bh) { res->errors++; continue; }
		crc = ocsfs2_data_crc(sb, bh->b_data);
		res->checked++;
		if (crc != stored) {
			pr_warn("ocsfs2: scrub: DATA checksum mismatch at block %llu (have 0x%08x want 0x%08x)\n",
				(unsigned long long)(phys + i), crc, stored);
			res->errors++;
		}
		brelse(bh);
		cond_resched();
	}
}

/* walk an extent tree (or one leaf), verifying each record's data checksums */
static void scrub_data_node(struct super_block *sb, u64 blk,
			    struct ocsfs2_scrub_result *res, int depth)
{
	struct buffer_head *bh;
	struct ocsfs2_disk_ext_node *n;
	u16 level, nr, i;

	if (!blk || depth > SCRUB_MAX_DEPTH)
		return;
	bh = ocsfs2_meta_bread(sb, blk);
	if (!bh) { res->errors++; return; }
	n = (struct ocsfs2_disk_ext_node *)bh->b_data;
	if (le32_to_cpu(n->en_magic) != OCSFS2_EXT_NODE_MAGIC) { brelse(bh); return; }
	level = le16_to_cpu(n->en_level);
	nr = le16_to_cpu(n->en_nr);
	if (level == 0) {
		struct ocsfs2_disk_ext_rec *r = (void *)n->en_body;
		u16 cap = sizeof(n->en_body) / sizeof(*r);

		for (i = 0; i < nr && i < cap; i++)
			scrub_data_extent(sb, le64_to_cpu(r[i].er_physical),
					  le32_to_cpu(r[i].er_length), res);
	} else {
		struct ocsfs2_disk_ext_ptr *p = (void *)n->en_body;
		u16 cap = sizeof(n->en_body) / sizeof(*p);

		for (i = 0; i < nr && i < cap; i++)
			scrub_data_node(sb, le64_to_cpu(p[i].ep_child), res, depth + 1);
	}
	brelse(bh);
}

/* A8: verify all data-block checksums of a regular file */
static void scrub_data_inode(struct super_block *sb, struct ocsfs2_disk_inode *in,
			     struct ocsfs2_scrub_result *res)
{
	if (!OCSFS2_SB(sb)->s_datacsum || !S_ISREG(le16_to_cpu(in->i_mode)))
		return;
	if (in->i_extent_tree_root) {
		scrub_data_node(sb, le64_to_cpu(in->i_extent_tree_root), res, 0);
	} else {
		struct ocsfs2_disk_extent *de = (void *)in->i_inline_extents;
		u16 n = le16_to_cpu(in->i_extent_count), i;

		for (i = 0; i < n && i < OCSFS2_INLINE_EXTENTS; i++)
			scrub_data_extent(sb, le64_to_cpu(de[i].e_physical),
					  le32_to_cpu(de[i].e_length), res);
	}
}

int ocsfs2_scrub(struct super_block *sb, struct ocsfs2_scrub_result *res)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 bs = sb->s_blocksize, ag;
	u32 ag_count = READ_ONCE(sbi->s_ag_count);
	struct buffer_head *bh;

	memset(res, 0, sizeof(*res));
	res->ag_count = ag_count;

	/* superblock */
	bh = ocsfs2_meta_bread(sb, 0);
	if (bh) {
		struct ocsfs2_disk_super *ds = (void *)bh->b_data;

		res->checked++;
		if (le32_to_cpu(ds->s_magic) != OCSFS2_MAGIC ||
		    !block_crc_ok(ds, offsetof(struct ocsfs2_disk_super, s_checksum),
				  ds->s_checksum)) {
			pr_warn("ocsfs2: scrub: superblock checksum bad\n");
			res->errors++;
		}
		brelse(bh);
	} else {
		res->errors++;
	}

	for (ag = 0; ag < ag_count; ag++) {
		struct ocsfs2_ag_info *ai = &sbi->s_ags[ag];
		u64 hdr = ai->block_start;
		u64 it = ai->inode_table_off / bs;
		u64 it_blocks = (ai->inodes_per_ag * OCSFS2_INODE_SIZE) / bs;
		u64 b;

		/* AG header */
		bh = ocsfs2_meta_bread(sb, hdr);
		if (bh) {
			struct ocsfs2_disk_ag *d = (void *)bh->b_data;

			res->checked++;
			if (le32_to_cpu(d->ag_magic) != OCSFS2_AG_MAGIC ||
			    le32_to_cpu(d->ag_number) != ag ||
			    !block_crc_ok(d, offsetof(struct ocsfs2_disk_ag, ag_checksum),
					  d->ag_checksum)) {
				pr_warn("ocsfs2: scrub: AG%u header bad\n", ag);
				res->errors++;
			}
			brelse(bh);
		} else {
			res->errors++;
		}

		scrub_rc_root(sb, ai->rc_btree_root, res);

		/* inode table: verify every used inode + its trees/xattrs */
		for (b = 0; b < it_blocks; b++) {
			unsigned int k;

			bh = ocsfs2_meta_bread(sb, it + b);
			if (!bh) { res->errors++; continue; }
			for (k = 0; k < bs / OCSFS2_INODE_SIZE; k++) {
				struct ocsfs2_disk_inode *in =
					(void *)(bh->b_data + k * OCSFS2_INODE_SIZE);

				if (le32_to_cpu(in->i_magic) != OCSFS2_INODE_MAGIC)
					continue;
				res->inodes++;
				res->checked++;
				if (!block_crc_ok(in, offsetof(struct ocsfs2_disk_inode,
							       i_checksum), in->i_checksum)) {
					pr_warn("ocsfs2: scrub: inode %llu checksum bad\n",
						(unsigned long long)le64_to_cpu(in->i_ino));
					res->errors++;
					continue;
				}
				scrub_ext_node(sb, le64_to_cpu(in->i_extent_tree_root), res, 0);
				scrub_xattr(sb, le64_to_cpu(in->i_xattr_block), res);
				scrub_data_inode(sb, in, res);   /* A8 data checksums */
			}
			brelse(bh);
			cond_resched();
		}
	}

	pr_info("ocsfs2: scrub: %llu structures, %llu inodes, %llu error(s)\n",
		res->checked, res->inodes, res->errors);
	return res->errors ? -EUCLEAN : 0;
}
