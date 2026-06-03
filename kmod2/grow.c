// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — grow.c
 * Autonomous online autogrow (D2): when the underlying LUN grows (thin SAN
 * resize, picked up by a SCSI rescan), append whole new allocation groups into
 * the freed space — hot, no unmount. A per-mount watcher thread polls the block
 * device size; an OCSFS_IOC_GROWFS ioctl forces a check immediately.
 *
 * Layout requirement (mkfs sets OCSFS2_FEAT_COMPAT_AUTOGROW): all AGs are
 * uniform (== s_ag_blocks), so AG[i]'s header sits at the fixed offset
 * ag_region_start + i*s_ag_blocks and new AGs append with no overlap.
 *
 * Concurrency: the in-core s_ags array is pre-sized with headroom and never
 * reallocated (its entries hold mutexes); grow fills new slots fully, then
 * publishes the higher count with a release barrier so lock-free readers see
 * either the old count or fully-initialised new AGs. All grow/refresh work is
 * serialised by s_grow_lock; cross-node, the grower holds the metadata lease
 * and writes the superblock via CAW, and every node's watcher imports a peer's
 * growth via geom_refresh.
 */
#include "ocsfs.h"
#include <linux/blkdev.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#define GROW_POLL_MS  30000   /* watcher cadence */

struct ag_geom {
	u64 start, bitmap_off, bitmap_blocks, itable_off, itable_blocks;
	u64 csum_off, csum_blocks;
	u64 meta_blocks, data_off, data_blocks;
};

static void compute_ag_geom(struct ocsfs2_sb_info *sbi, u32 bs, u32 idx,
			    struct ag_geom *g)
{
	u64 region = sbi->s_ag_desc_off / bs;

	g->start         = region + (u64)idx * sbi->s_ag_blocks;
	g->bitmap_blocks = DIV_ROUND_UP(DIV_ROUND_UP(sbi->s_ag_blocks, 8), bs);
	g->itable_blocks = (sbi->s_inodes_per_ag * OCSFS2_INODE_SIZE) / bs;
	/* A8/P3a: a checksummed volume reserves a per-AG CRC region (one __le32 per
	 * block of the AG), laid out exactly like mkfs (header|bitmap|itable|csum|data)
	 * so an autogrow-added AG is checksummed identically to the original AGs. */
	g->csum_blocks   = sbi->s_datacsum ?
			   DIV_ROUND_UP(sbi->s_ag_blocks * sizeof(__le32), bs) : 0;
	g->meta_blocks   = 1 + g->bitmap_blocks + g->itable_blocks + g->csum_blocks;
	g->bitmap_off    = (g->start + 1) * bs;
	g->itable_off    = (g->start + 1 + g->bitmap_blocks) * bs;
	g->csum_off      = g->csum_blocks ?
			   (g->start + 1 + g->bitmap_blocks + g->itable_blocks) * bs : 0;
	g->data_off      = (g->start + g->meta_blocks) * bs;
	g->data_blocks   = sbi->s_ag_blocks - g->meta_blocks;
}

/* write one 4 KiB block from @buf at absolute block @blk (coherent bio) */
static int grow_write_block(struct super_block *sb, u64 blk, const void *buf)
{
	u32 bs = sb->s_blocksize;
	void *tmp = kmalloc(bs, GFP_NOFS);
	int ret;

	if (!tmp)
		return -ENOMEM;
	memcpy(tmp, buf, bs);
	ret = ocsfs2_cl_bio(sb, blk * bs, tmp, bs, REQ_OP_WRITE);
	kfree(tmp);
	return ret;
}

/* Lay down a brand-new AG @idx on disk: zeroed inode table, a bitmap with only
 * the metadata blocks marked used, and a valid AG descriptor. */
static int write_new_ag(struct super_block *sb, u32 idx, struct ag_geom *g)
{
	u32 bs = sb->s_blocksize;
	struct ocsfs2_disk_ag *ag;
	u8 *blk;
	u64 i;
	int ret;

	compute_ag_geom(OCSFS2_SB(sb), bs, idx, g);
	if (g->data_blocks == 0 || g->data_blocks >= OCSFS2_SB(sb)->s_ag_blocks)
		return -EINVAL;

	/* zero the inode table so every inode reads as free (magic 0) */
	ret = blkdev_issue_zeroout(sb->s_bdev, (g->itable_off / 512),
				   g->itable_blocks * (bs / 512), GFP_NOFS, 0);
	if (ret)
		return ret;

	/* P3a: zero the per-AG checksum region (0 = "unset", so data blocks read as
	 * unverified until first written, never false-positive) */
	if (g->csum_blocks) {
		ret = blkdev_issue_zeroout(sb->s_bdev, (g->csum_off / 512),
					   g->csum_blocks * (bs / 512), GFP_NOFS, 0);
		if (ret)
			return ret;
	}

	blk = kzalloc(bs, GFP_NOFS);
	ag  = kzalloc(bs, GFP_NOFS);
	if (!blk || !ag) { kfree(blk); kfree(ag); return -ENOMEM; }

	/* bitmap: block 0 has the meta_blocks bits set, the rest are zero */
	for (i = 0; i < g->meta_blocks && i < (u64)bs * 8; i++)
		blk[i >> 3] |= (u8)(1u << (i & 7));
	ret = grow_write_block(sb, g->bitmap_off / bs, blk);
	if (ret)
		goto out;
	memset(blk, 0, bs);
	for (i = 1; i < g->bitmap_blocks; i++) {   /* remaining bitmap blocks: all free */
		ret = grow_write_block(sb, (g->bitmap_off / bs) + i, blk);
		if (ret)
			goto out;
	}

	/* AG descriptor */
	ag->ag_magic = cpu_to_le32(OCSFS2_AG_MAGIC);
	ag->ag_number = cpu_to_le32(idx);
	ag->ag_block_start = cpu_to_le64(g->start);
	ag->ag_block_count = cpu_to_le64(OCSFS2_SB(sb)->s_ag_blocks);
	ag->ag_free_blocks = cpu_to_le64(g->data_blocks);
	ag->ag_free_inodes = cpu_to_le64(OCSFS2_SB(sb)->s_inodes_per_ag);
	ag->ag_bitmap_off = cpu_to_le64(g->bitmap_off);
	ag->ag_bitmap_blocks = cpu_to_le64(g->bitmap_blocks);
	ag->ag_inode_table_off = cpu_to_le64(g->itable_off);
	ag->ag_inodes_per_ag = cpu_to_le64(OCSFS2_SB(sb)->s_inodes_per_ag);
	ag->ag_data_off = cpu_to_le64(g->data_off);
	ag->ag_data_blocks = cpu_to_le64(g->data_blocks);
	ag->ag_csum_off = cpu_to_le64(g->csum_off);        /* P3a: 0 unless -C */
	ag->ag_csum_blocks = cpu_to_le64(g->csum_blocks);
	ag->ag_rc_btree_root = cpu_to_le64(0);
	ag->ag_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, ag,
			  offsetof(struct ocsfs2_disk_ag, ag_checksum)));
	ret = grow_write_block(sb, g->start, ag);
out:
	kfree(blk);
	kfree(ag);
	return ret;
}

/* Populate the in-core AG slot @idx from a freshly-laid-out (or freshly-read)
 * geometry. Caller holds s_grow_lock and must publish s_ag_count afterwards. */
static void install_ag_incore(struct ocsfs2_sb_info *sbi, u32 idx,
			      const struct ag_geom *g, u32 bs,
			      u64 free_blocks, u64 free_inodes)
{
	struct ocsfs2_ag_info *ai = &sbi->s_ags[idx];

	memset(ai, 0, sizeof(*ai));
	ai->ag_no = idx;
	ai->block_start = g->start;
	ai->block_count = sbi->s_ag_blocks;
	ai->free_blocks = free_blocks;
	ai->free_inodes = free_inodes;
	ai->bitmap_off = g->bitmap_off;
	ai->bitmap_blocks = g->bitmap_blocks;
	ai->inode_table_off = g->itable_off;
	ai->inodes_per_ag = sbi->s_inodes_per_ag;
	ai->csum_off = g->csum_off;             /* P3a: 0 unless s_datacsum */
	ai->csum_blocks = g->csum_blocks;
	ai->data_off = g->data_off;
	ai->data_blocks = g->data_blocks;
	ai->rc_btree_root = 0;
	ai->next_blk_hint = (g->data_off / bs) - g->start;
	ai->next_ino_hint = 0;
	mutex_init(&ai->ag_lock);
	mutex_init(&ai->rc_lock);
}

/* Persist the grown geometry into the superblock (primary + mirror). Cluster:
 * CAW block 0 so we don't clobber a peer's concurrent free-count writeback. */
static int persist_super(struct super_block *sb, u32 new_count, u64 new_total,
			 u64 new_total_ino, u64 add_free, u64 add_free_ino)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_disk_super *ds = sbi->s_ds;
	u32 bs = sb->s_blocksize;
	int ret = 0;

	mutex_lock(&sbi->s_super_lock);
	if (sbi->s_cluster) {
		unsigned int lbs = bdev_logical_block_size(sb->s_bdev);
		u8 *old = kmalloc(lbs, GFP_NOFS), *new = kmalloc(lbs, GFP_NOFS);
		struct ocsfs2_disk_super *nd;
		int tries;

		if (!old || !new) { kfree(old); kfree(new); ret = -ENOMEM; goto out; }
		ret = -EBUSY;
		for (tries = 0; tries < 16; tries++) {
			if (ocsfs2_cl_bio(sb, 0, old, lbs, REQ_OP_READ)) { ret = -EIO; break; }
			memcpy(new, old, lbs);
			nd = (struct ocsfs2_disk_super *)new;
			nd->s_ag_count = cpu_to_le32(new_count);
			nd->s_total_blocks = cpu_to_le64(new_total);
			nd->s_total_inodes = cpu_to_le64(new_total_ino);
			nd->s_free_blocks = cpu_to_le64(le64_to_cpu(nd->s_free_blocks) + add_free);
			nd->s_free_inodes = cpu_to_le64(le64_to_cpu(nd->s_free_inodes) + add_free_ino);
			nd->s_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, nd,
					 offsetof(struct ocsfs2_disk_super, s_checksum)));
			if (ocsfs2_scsi_caw(sb, 0, old, new, lbs) == 0) {
				memcpy(ds, new, sizeof(*ds));   /* refresh our cache */
				ret = 0;
				break;
			}
		}
		kfree(old);
		kfree(new);
		/* mirror (best-effort, not load-bearing) */
		if (!ret)
			grow_write_block(sb, 1, ds);
	} else {
		ds->s_ag_count = cpu_to_le32(new_count);
		ds->s_total_blocks = cpu_to_le64(new_total);
		ds->s_total_inodes = cpu_to_le64(new_total_ino);
		spin_lock(&sbi->s_free_lock);
		ds->s_free_blocks = cpu_to_le64(sbi->s_free_blocks + add_free);
		ds->s_free_inodes = cpu_to_le64(sbi->s_free_inodes + add_free_ino);
		spin_unlock(&sbi->s_free_lock);
		ds->s_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, ds,
				 offsetof(struct ocsfs2_disk_super, s_checksum)));
		mark_buffer_dirty(sbi->s_sbh);
		ret = sync_dirty_buffer(sbi->s_sbh) ? -EIO : 0;
		(void)bs;
		grow_write_block(sb, 1, ds);   /* mirror */
	}
out:
	mutex_unlock(&sbi->s_super_lock);
	return ret;
}

/* Import on-disk growth performed by a peer: if the on-disk AG count exceeds
 * ours, read the new AG descriptors and install them. Caller holds s_grow_lock. */
static int geom_refresh(struct super_block *sb)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 bs = sb->s_blocksize, od_count, i;
	u64 od_total, od_total_ino, od_free, od_free_ino;
	struct ocsfs2_disk_super *ds;
	u8 *sbuf = NULL;
	int ret = 0;

	if (sbi->s_cluster) {
		sbuf = kmalloc(bs, GFP_NOFS);
		if (!sbuf)
			return -ENOMEM;
		if (ocsfs2_cl_bio(sb, 0, sbuf, bs, REQ_OP_READ)) { kfree(sbuf); return -EIO; }
		ds = (struct ocsfs2_disk_super *)sbuf;
		if (le32_to_cpu(ds->s_magic) != OCSFS2_MAGIC) { kfree(sbuf); return -EUCLEAN; }
	} else {
		ds = sbi->s_ds;                 /* single-node: our cache is the truth */
	}

	od_count     = le32_to_cpu(ds->s_ag_count);
	od_total     = le64_to_cpu(ds->s_total_blocks);
	od_total_ino = le64_to_cpu(ds->s_total_inodes);
	od_free      = le64_to_cpu(ds->s_free_blocks);
	od_free_ino  = le64_to_cpu(ds->s_free_inodes);

	if (od_count <= sbi->s_ag_count)
		goto out;                       /* nothing new to import */
	if (od_count > sbi->s_ag_capacity) {
		pr_warn("ocsfs2: peer grew to %u AGs > capacity %u; remount to use them\n",
			od_count, sbi->s_ag_capacity);
		od_count = sbi->s_ag_capacity;
		if (od_count <= sbi->s_ag_count)
			goto out;
	}

	for (i = sbi->s_ag_count; i < od_count; i++) {
		struct buffer_head *bh;
		struct ocsfs2_disk_ag *dag;
		struct ag_geom g;
		u64 region = sbi->s_ag_desc_off / bs;

		bh = ocsfs2_meta_bread(sb, region + (u64)i * sbi->s_ag_blocks);
		if (!bh) { ret = -EIO; goto out; }
		dag = (struct ocsfs2_disk_ag *)bh->b_data;
		if (le32_to_cpu(dag->ag_magic) != OCSFS2_AG_MAGIC) {
			brelse(bh);
			ret = -EUCLEAN;
			goto out;
		}
		compute_ag_geom(sbi, bs, i, &g);
		install_ag_incore(sbi, i, &g, bs,
				  le64_to_cpu(dag->ag_free_blocks),
				  le64_to_cpu(dag->ag_free_inodes));
		brelse(bh);
	}

	/* adopt the grown global counters + refresh our cached super (cluster) */
	if (sbi->s_cluster)
		memcpy(sbi->s_ds, ds, sizeof(*ds));
	sbi->s_total_blocks = od_total;
	sbi->s_total_inodes = od_total_ino;
	spin_lock(&sbi->s_free_lock);
	sbi->s_free_blocks = od_free;
	sbi->s_free_inodes = od_free_ino;
	spin_unlock(&sbi->s_free_lock);

	smp_wmb();                              /* AG slots visible before the count */
	WRITE_ONCE(sbi->s_ag_count, od_count);
	pr_info("ocsfs2: imported grown geometry — now %u AGs, %llu blocks\n",
		od_count, (unsigned long long)od_total);
out:
	kfree(sbuf);
	return ret;
}

/* Perform the growth: lay down [cur, target) AGs, persist the super, install
 * them in-core. Caller holds s_grow_lock (+ the metadata lease in cluster). */
static int grow_to(struct super_block *sb, u32 target)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 bs = sb->s_blocksize, cur = sbi->s_ag_count, i;
	u64 add_free = 0, add_ino = 0;
	struct ag_geom g;
	int ret = 0;

	for (i = cur; i < target; i++) {
		ret = write_new_ag(sb, i, &g);
		if (ret) {
			pr_err("ocsfs2: grow: writing AG%u failed: %d\n", i, ret);
			target = i;                 /* commit only the AGs we managed */
			break;
		}
		add_free += g.data_blocks;
		add_ino  += sbi->s_inodes_per_ag;
	}
	if (target <= cur)
		return ret;
	blkdev_issue_flush(sb->s_bdev);         /* AG metadata durable before super */

	ret = persist_super(sb, target,
			    sbi->s_ag_desc_off / bs + (u64)target * sbi->s_ag_blocks,
			    (u64)target * sbi->s_inodes_per_ag,
			    add_free, add_ino);
	if (ret) {
		pr_err("ocsfs2: grow: superblock update failed: %d\n", ret);
		return ret;
	}

	/* install the new AGs in-core, then publish the higher count */
	for (i = cur; i < target; i++) {
		compute_ag_geom(sbi, bs, i, &g);
		install_ag_incore(sbi, i, &g, bs, g.data_blocks, sbi->s_inodes_per_ag);
	}
	sbi->s_total_blocks = sbi->s_ag_desc_off / bs + (u64)target * sbi->s_ag_blocks;
	sbi->s_total_inodes = (u64)target * sbi->s_inodes_per_ag;
	spin_lock(&sbi->s_free_lock);
	sbi->s_free_blocks += add_free;
	sbi->s_free_inodes += add_ino;
	spin_unlock(&sbi->s_free_lock);
	smp_wmb();
	WRITE_ONCE(sbi->s_ag_count, target);

	pr_info("ocsfs2: grew %u -> %u AGs (+%llu blocks)\n",
		cur, target, (unsigned long long)add_free);
	return 0;
}

int ocsfs2_grow_check(struct super_block *sb, bool force)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 bs = sb->s_blocksize, new_full;
	u64 dev_blocks, cur_span;
	int ret = 0;

	if (!sbi->s_growable) {
		if (force)
			pr_info("ocsfs2: volume not autogrow-capable (no COMPAT_AUTOGROW)\n");
		return force ? -EOPNOTSUPP : 0;
	}
	if (sb_rdonly(sb))
		return 0;

	mutex_lock(&sbi->s_grow_lock);
	geom_refresh(sb);                       /* pick up a peer's growth first */

	dev_blocks = bdev_nr_bytes(sb->s_bdev) / bs;
	cur_span = sbi->s_ag_desc_off / bs + (u64)sbi->s_ag_count * sbi->s_ag_blocks;
	if (dev_blocks < cur_span + sbi->s_ag_blocks) {
		ret = 0;                        /* not even one full new AG fits */
		goto out;
	}
	new_full = (u32)min_t(u64, (dev_blocks - cur_span) / sbi->s_ag_blocks,
			      sbi->s_ag_capacity - sbi->s_ag_count);
	if (new_full == 0) {
		if (force)
			pr_info("ocsfs2: autogrow capacity reached (%u AGs); remount to grow further\n",
				sbi->s_ag_capacity);
		goto out;
	}

	if (sbi->s_cluster) {
		ocsfs2_meta_lock(sb, NULL, NULL);
		geom_refresh(sb);               /* a peer may have grown while we waited */
		cur_span = sbi->s_ag_desc_off / bs + (u64)sbi->s_ag_count * sbi->s_ag_blocks;
		dev_blocks = bdev_nr_bytes(sb->s_bdev) / bs;
		new_full = (dev_blocks < cur_span + sbi->s_ag_blocks) ? 0 :
			   (u32)min_t(u64, (dev_blocks - cur_span) / sbi->s_ag_blocks,
				      sbi->s_ag_capacity - sbi->s_ag_count);
	}
	if (new_full)
		ret = grow_to(sb, sbi->s_ag_count + new_full);
	if (sbi->s_cluster)
		ocsfs2_meta_unlock(sb);
out:
	mutex_unlock(&sbi->s_grow_lock);
	return ret;
}

static int grow_thread_fn(void *data)
{
	struct super_block *sb = data;

	while (!kthread_should_stop()) {
		ocsfs2_grow_check(sb, false);
		schedule_timeout_interruptible(msecs_to_jiffies(GROW_POLL_MS));
	}
	return 0;
}

int ocsfs2_grow_start(struct super_block *sb)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);

	if (!sbi->s_growable || sbi->s_grow_thread)
		return 0;
	sbi->s_grow_thread = kthread_run(grow_thread_fn, sb, "ocsfs2-grow/%s", sb->s_id);
	if (IS_ERR(sbi->s_grow_thread)) {
		int ret = PTR_ERR(sbi->s_grow_thread);

		sbi->s_grow_thread = NULL;
		return ret;
	}
	return 0;
}

void ocsfs2_grow_stop(struct super_block *sb)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);

	if (sbi->s_grow_thread) {
		kthread_stop(sbi->s_grow_thread);
		sbi->s_grow_thread = NULL;
	}
}
