// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — super.c
 * Module lifecycle, superblock validation, mount/unmount, statfs.
 * Single-node (Plan 1): the cluster coordination regions are validated for
 * non-overlap but otherwise untouched.
 */
#include "ocsfs.h"
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/statfs.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/unaligned.h>

/* mount options: the only one is "cluster" (opt-in multinode). Without it a
 * volume — even one formatted with mkfs -N >1 — mounts single-node. */
struct ocsfs2_fc {
	bool cluster;
};

/* ── inode slab lifecycle ── */

static void ocsfs2_inode_init_once(void *obj)
{
	struct ocsfs2_inode_info *oi = obj;

	inode_init_once(&oi->vfs_inode);
}

static void ocsfs2_free_inode_cb(struct inode *inode)
{
	ocsfs2_free_in_core_inode(inode);
}

/* ── superblock helpers ── */

static int validate_super(struct super_block *sb, struct ocsfs2_disk_super *ds)
{
	u32 crc;

	if (le32_to_cpu(ds->s_magic) != OCSFS2_MAGIC)
		return -EINVAL;
	if (le16_to_cpu(ds->s_major) != OCSFS2_VERSION_MAJOR)
		return -EINVAL;
	if (le32_to_cpu(ds->s_block_size) != OCSFS2_BLOCK_SIZE)
		return -EINVAL;
	crc = ocsfs2_crc32c(~0U, ds,
			    offsetof(struct ocsfs2_disk_super, s_checksum));
	if (crc != le32_to_cpu(ds->s_checksum))
		return -EUCLEAN;

	/* incompat feature gate */
	if (le64_to_cpu(ds->s_feat_incompat) & ~OCSFS2_FEATURE_INCOMPAT_SUPP)
		return -EINVAL;
	return 0;
}

/* Region ordering / range guard (the v1 CRIT-O1 lesson). */
static int validate_layout(struct super_block *sb, struct ocsfs2_disk_super *ds)
{
	u64 dev_sz = bdev_nr_bytes(sb->s_bdev);
	u64 a = le64_to_cpu(ds->s_node_table_off);
	u64 b = le64_to_cpu(ds->s_heartbeat_off);
	u64 c = le64_to_cpu(ds->s_lease_table_off);
	u64 d = le64_to_cpu(ds->s_recovery_off);
	u64 e = le64_to_cpu(ds->s_journal_off);
	u64 f = le64_to_cpu(ds->s_ag_desc_off);

	if (!(2 * OCSFS2_BLOCK_SIZE <= a && a < b && b < c && c < d &&
	      d < e && e < f && f < dev_sz)) {
		pr_err("ocsfs2: superblock region offsets invalid/overlapping\n");
		return -EINVAL;
	}
	if (le32_to_cpu(ds->s_ag_count) == 0 ||
	    le64_to_cpu(ds->s_ag_blocks) == 0 ||
	    le64_to_cpu(ds->s_inodes_per_ag) == 0) {
		pr_err("ocsfs2: superblock geometry zero\n");
		return -EINVAL;
	}
	return 0;
}

static int read_ag_headers(struct super_block *sb)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u64 ag_region_start = sbi->s_ag_desc_off / sb->s_blocksize;
	u32 i;

	/* Online autogrow appends AGs into the in-core array without reallocating
	 * it (its entries hold mutexes — moving them is unsafe). So pre-size to a
	 * capacity with headroom; growth beyond it needs a remount. */
	sbi->s_growable = (sbi->s_feat_compat & OCSFS2_FEAT_COMPAT_AUTOGROW) != 0;
	sbi->s_ag_capacity = sbi->s_ag_count;
	if (sbi->s_growable)   /* generous headroom: many repeated grows, no remount */
		sbi->s_ag_capacity += clamp_t(u32, sbi->s_ag_count * 15, 256u, 65536u);

	sbi->s_ags = kvcalloc(sbi->s_ag_capacity, sizeof(*sbi->s_ags), GFP_KERNEL);
	if (!sbi->s_ags)
		return -ENOMEM;

	for (i = 0; i < sbi->s_ag_count; i++) {
		u64 start = ag_region_start + (u64)i * sbi->s_ag_blocks;
		struct buffer_head *bh = sb_bread(sb, start);
		struct ocsfs2_disk_ag *dag;
		struct ocsfs2_ag_info *ai = &sbi->s_ags[i];
		u32 crc;

		if (!bh) {
			pr_err("ocsfs2: cannot read AG%u header\n", i);
			return -EIO;
		}
		dag = (struct ocsfs2_disk_ag *)bh->b_data;
		crc = ocsfs2_crc32c(~0U, dag,
				    offsetof(struct ocsfs2_disk_ag, ag_checksum));
		if (le32_to_cpu(dag->ag_magic) != OCSFS2_AG_MAGIC ||
		    crc != le32_to_cpu(dag->ag_checksum) ||
		    le32_to_cpu(dag->ag_number) != i) {
			pr_err("ocsfs2: AG%u header invalid\n", i);
			brelse(bh);
			return -EUCLEAN;
		}
		ai->ag_no = i;
		ai->block_start    = le64_to_cpu(dag->ag_block_start);
		ai->block_count    = le64_to_cpu(dag->ag_block_count);
		ai->free_blocks    = le64_to_cpu(dag->ag_free_blocks);
		ai->free_inodes    = le64_to_cpu(dag->ag_free_inodes);
		ai->bitmap_off     = le64_to_cpu(dag->ag_bitmap_off);
		ai->bitmap_blocks  = le64_to_cpu(dag->ag_bitmap_blocks);
		ai->inode_table_off = le64_to_cpu(dag->ag_inode_table_off);
		ai->inodes_per_ag  = le64_to_cpu(dag->ag_inodes_per_ag);
		ai->data_off       = le64_to_cpu(dag->ag_data_off);
		ai->data_blocks    = le64_to_cpu(dag->ag_data_blocks);
		ai->rc_btree_root  = le64_to_cpu(dag->ag_rc_btree_root);
		ai->csum_off       = le64_to_cpu(dag->ag_csum_off);
		ai->csum_blocks    = le64_to_cpu(dag->ag_csum_blocks);

		/* A6/S4: the AG header is CRC-valid but still attacker-/corruption-
		 * controlled. Bound-check its geometry against the device before we use
		 * it to compute block addresses or size allocations (else a crafted
		 * header could drive out-of-range I/O or a huge kvmalloc / OOM). */
		{
			u64 dev_blocks = bdev_nr_bytes(sb->s_bdev) / sb->s_blocksize;
			u64 bm_blk = ai->bitmap_off / sb->s_blocksize;
			u64 it_blk = ai->inode_table_off / sb->s_blocksize;
			u64 bpb = (u64)sb->s_blocksize * 8;

			if (ai->block_count == 0 ||
			    ai->block_count > sbi->s_ag_blocks ||
			    ai->block_start + ai->block_count > dev_blocks ||
			    ai->bitmap_blocks == 0 ||
			    ai->bitmap_blocks > sbi->s_ag_blocks ||
			    ai->bitmap_blocks * bpb < ai->block_count ||
			    bm_blk + ai->bitmap_blocks > dev_blocks ||
			    ai->inodes_per_ag == 0 ||
			    ai->inodes_per_ag > sbi->s_inodes_per_ag ||
			    it_blk >= dev_blocks ||
			    ai->data_off / sb->s_blocksize < ai->block_start ||
			    ai->data_off / sb->s_blocksize >= dev_blocks ||
			    (ai->csum_off &&                       /* A8 csum region */
			     (ai->csum_off / sb->s_blocksize +
			      ai->csum_blocks > dev_blocks ||
			      ai->csum_blocks * (sb->s_blocksize / sizeof(__le32)) <
			      ai->data_blocks))) {
				pr_err("ocsfs2: AG%u geometry out of range\n", i);
				brelse(bh);
				return -EUCLEAN;
			}
		}
		ai->next_blk_hint  = (ai->data_off / sb->s_blocksize) - ai->block_start;
		ai->next_ino_hint  = (i == 0) ? OCSFS2_FIRST_USER_INO : 0;
		mutex_init(&ai->ag_lock);
		mutex_init(&ai->rc_lock);
		brelse(bh);
	}
	return 0;
}

/* Persist in-memory free counters back to the AG headers + superblock. */
static int write_back_meta(struct super_block *sb)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u64 ag_region_start = sbi->s_ag_desc_off / sb->s_blocksize;
	struct ocsfs2_disk_super *ds;
	u32 i;

	if (sb_rdonly(sb))
		return 0;

	mutex_lock(&sbi->s_super_lock);
	for (i = 0; i < sbi->s_ag_count; i++) {
		u64 start = ag_region_start + (u64)i * sbi->s_ag_blocks;
		struct buffer_head *bh;
		struct ocsfs2_disk_ag *dag;

		/* Cluster: the AG header is SHARED metadata. ag_free_blocks/_inodes
		 * here are only this node's local hints (peers alloc/free without us
		 * seeing it) and writing them from our stale buffer cache would also
		 * CLOBBER ag_rc_btree_root, which the refcount path maintains by CAW
		 * (A1). The on-disk bitmap is authoritative; fsck/scrub recompute the
		 * counts (A4). So skip the per-AG header rewrite on a clustered volume. */
		if (sbi->s_cluster)
			continue;

		bh = sb_bread(sb, start);
		if (!bh)
			continue;
		dag = (struct ocsfs2_disk_ag *)bh->b_data;
		mutex_lock(&sbi->s_ags[i].ag_lock);
		dag->ag_free_blocks = cpu_to_le64(sbi->s_ags[i].free_blocks);
		dag->ag_free_inodes = cpu_to_le64(sbi->s_ags[i].free_inodes);
		mutex_unlock(&sbi->s_ags[i].ag_lock);
		dag->ag_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, dag,
				   offsetof(struct ocsfs2_disk_ag, ag_checksum)));
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}

	ds = (struct ocsfs2_disk_super *)sbi->s_sbh->b_data;
	spin_lock(&sbi->s_free_lock);
	ds->s_free_blocks = cpu_to_le64(sbi->s_free_blocks);
	ds->s_free_inodes = cpu_to_le64(sbi->s_free_inodes);
	spin_unlock(&sbi->s_free_lock);
	ds->s_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, ds,
			 offsetof(struct ocsfs2_disk_super, s_checksum)));
	mark_buffer_dirty(sbi->s_sbh);
	sync_dirty_buffer(sbi->s_sbh);
	/* mirror */
	{
		struct buffer_head *mbh = sb_bread(sb, 1);

		if (mbh) {
			memcpy(mbh->b_data, ds, sizeof(*ds));
			mark_buffer_dirty(mbh);
			sync_dirty_buffer(mbh);
			brelse(mbh);
		}
	}
	mutex_unlock(&sbi->s_super_lock);
	return 0;
}

/* ── super_operations ── */

int ocsfs2_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);

	buf->f_type = OCSFS2_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = sbi->s_total_blocks;
	/* A4: in cluster mode the cached free count is only a per-node view and
	 * drifts; recompute the true value from the (authoritative) bitmaps so df
	 * reflects every node's activity. Single-node keeps the cheap cached count. */
	if (sbi->s_cluster)
		ocsfs2_recompute_free(sb);
	spin_lock(&sbi->s_free_lock);
	buf->f_bfree = sbi->s_free_blocks;
	buf->f_bavail = sbi->s_free_blocks;
	buf->f_files = sbi->s_total_inodes;
	buf->f_ffree = sbi->s_free_inodes;
	spin_unlock(&sbi->s_free_lock);
	buf->f_namelen = OCSFS2_MAX_NAME;
	buf->f_fsid = u64_to_fsid(get_unaligned_le64(sbi->s_ds->s_uuid));
	return 0;
}

static int ocsfs2_sync_fs(struct super_block *sb, int wait)
{
	return write_back_meta(sb);
}

static void ocsfs2_put_super(struct super_block *sb)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);

	if (!sbi)
		return;
	ocsfs2_grow_stop(sb);      /* stop the autogrow watcher before tearing down */
	if (sbi->s_clustered)      /* A4: persist a true free count (coherent read) */
		ocsfs2_recompute_free(sb);
	ocsfs2_cluster_exit(sb);   /* stop heartbeat + release node slot first */
	write_back_meta(sb);
	ocsfs2_journal_exit(sb);
	kvfree(sbi->s_ags);
	if (sbi->s_sbh)
		brelse(sbi->s_sbh);
	sb->s_fs_info = NULL;
	kfree(sbi);
}

static const struct super_operations ocsfs2_sops = {
	.alloc_inode  = ocsfs2_alloc_inode,
	.free_inode   = ocsfs2_free_inode_cb,
	.write_inode  = ocsfs2_write_inode,
	.evict_inode  = ocsfs2_evict_inode,
	.put_super    = ocsfs2_put_super,
	.statfs       = ocsfs2_statfs,
	.sync_fs      = ocsfs2_sync_fs,
};

/* ── fill_super ── */

static int ocsfs2_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct ocsfs2_sb_info *sbi;
	struct ocsfs2_disk_super *ds;
	struct buffer_head *bh;
	struct inode *root;
	int ret;

	if (!sb_set_blocksize(sb, OCSFS2_BLOCK_SIZE)) {
		pr_err("ocsfs2: cannot set 4096-byte block size\n");
		return -EINVAL;
	}

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	sbi->s_sb = sb;
	spin_lock_init(&sbi->s_free_lock);
	mutex_init(&sbi->s_super_lock);
	mutex_init(&sbi->s_grow_lock);

	bh = sb_bread(sb, 0);
	if (!bh) {
		pr_err("ocsfs2: cannot read superblock\n");
		ret = -EIO;
		goto fail;
	}
	ds = (struct ocsfs2_disk_super *)bh->b_data;
	ret = validate_super(sb, ds);
	if (ret) {
		struct buffer_head *mbh = sb_bread(sb, 1);

		pr_warn("ocsfs2: primary superblock invalid (%d), trying mirror\n", ret);
		brelse(bh);
		bh = mbh;
		if (!bh || validate_super(sb, (struct ocsfs2_disk_super *)bh->b_data)) {
			pr_err("ocsfs2: no valid superblock\n");
			if (bh)
				brelse(bh);
			ret = -EINVAL;
			goto fail;
		}
		ds = (struct ocsfs2_disk_super *)bh->b_data;
	}

	ret = validate_layout(sb, ds);
	if (ret) {
		brelse(bh);
		goto fail;
	}

	sbi->s_sbh = bh;
	sbi->s_ds = ds;
	sbi->s_block_size   = OCSFS2_BLOCK_SIZE;
	sbi->s_inode_size   = le32_to_cpu(ds->s_inode_size);
	sbi->s_total_blocks = le64_to_cpu(ds->s_total_blocks);
	sbi->s_free_blocks  = le64_to_cpu(ds->s_free_blocks);
	sbi->s_total_inodes = le64_to_cpu(ds->s_total_inodes);
	sbi->s_free_inodes  = le64_to_cpu(ds->s_free_inodes);
	sbi->s_ag_count     = le32_to_cpu(ds->s_ag_count);
	sbi->s_ag_size      = le64_to_cpu(ds->s_ag_size);
	sbi->s_ag_blocks    = le64_to_cpu(ds->s_ag_blocks);
	sbi->s_inodes_per_ag = le64_to_cpu(ds->s_inodes_per_ag);
	sbi->s_max_nodes    = le16_to_cpu(ds->s_max_nodes);
	sbi->s_feat_compat   = le64_to_cpu(ds->s_feat_compat);
	sbi->s_feat_incompat = le64_to_cpu(ds->s_feat_incompat);
	sbi->s_feat_ro_compat = le64_to_cpu(ds->s_feat_ro_compat);
	sbi->s_datacsum = (sbi->s_feat_ro_compat & OCSFS2_FEAT_RO_COMPAT_DATACSUM) != 0;
	/* an unsupported ro_compat feature means we must not write (we'd corrupt its
	 * on-disk state) — mount read-only, like ext4. */
	if ((sbi->s_feat_ro_compat & ~OCSFS2_FEATURE_RO_COMPAT_SUPP) &&
	    !sb_rdonly(sb)) {
		pr_warn("ocsfs2: unsupported ro_compat features 0x%llx — mounting read-only\n",
			(unsigned long long)(sbi->s_feat_ro_compat &
					     ~OCSFS2_FEATURE_RO_COMPAT_SUPP));
		sb->s_flags |= SB_RDONLY;
	}
	sbi->s_node_table_off = le64_to_cpu(ds->s_node_table_off);
	sbi->s_heartbeat_off  = le64_to_cpu(ds->s_heartbeat_off);
	sbi->s_lease_table_off = le64_to_cpu(ds->s_lease_table_off);
	sbi->s_lease_count    = le64_to_cpu(ds->s_lease_count);
	sbi->s_recovery_off   = le64_to_cpu(ds->s_recovery_off);
	sbi->s_journal_off    = le64_to_cpu(ds->s_journal_off);
	sbi->s_journal_size   = le64_to_cpu(ds->s_journal_size);
	sbi->s_ag_desc_off    = le64_to_cpu(ds->s_ag_desc_off);
	sbi->s_data_off       = le64_to_cpu(ds->s_data_off);
	{
		struct ocsfs2_fc *ctx = fc->fs_private;

		sbi->s_clustered = ctx && ctx->cluster && sbi->s_max_nodes > 1;
	}

	/* L3: join the cluster (claim our node slot) BEFORE the journal, so the
	 * journal uses our own per-node region (journal[self_slot]). No-op unless
	 * mounted -o cluster on a -N volume. */
	ret = ocsfs2_cluster_init(sb);
	if (ret)
		goto fail_sbh;

	/* Bring up the (per-node) journal and replay any committed-but-
	 * uncheckpointed txn BEFORE reading AG headers / the root inode. */
	if (!sb_rdonly(sb)) {
		ret = ocsfs2_journal_init(sb);
		if (ret) {
			pr_err("ocsfs2: journal init failed: %d\n", ret);
			goto fail_cluster;
		}
		ret = ocsfs2_journal_replay(sb);
		if (ret) {
			pr_err("ocsfs2: journal replay failed: %d\n", ret);
			ocsfs2_journal_exit(sb);
			goto fail_cluster;
		}
	}

	ret = read_ag_headers(sb);
	if (ret)
		goto fail_journal;

	sb->s_magic = OCSFS2_MAGIC;
	sb->s_op = &ocsfs2_sops;
	sb->s_xattr = ocsfs2_xattr_handlers;
	sb->s_flags |= SB_POSIXACL;   /* FS applies umask/ACL via posix_acl_create */
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;

	root = ocsfs2_iget(sb, OCSFS2_ROOT_INO);
	if (IS_ERR(root)) {
		pr_err("ocsfs2: cannot read root inode\n");
		ret = PTR_ERR(root);
		goto fail_journal;
	}
	if (!S_ISDIR(root->i_mode)) {
		iput(root);
		ret = -EINVAL;
		goto fail_journal;
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto fail_journal;
	}

	/* L3: now fully mounted — start heartbeating so peers see us alive */
	ret = ocsfs2_cluster_start(sb);
	if (ret) {
		dput(sb->s_root);
		sb->s_root = NULL;
		goto fail_journal;
	}

	/* D2: autonomous online autogrow watcher (no-op on rdonly / non-growable) */
	if (!sb_rdonly(sb))
		ocsfs2_grow_start(sb);

	pr_info("ocsfs2: mounted %s (%u AGs, %llu blocks)\n",
		sb->s_id, sbi->s_ag_count,
		(unsigned long long)sbi->s_total_blocks);
	return 0;

fail_journal:
	ocsfs2_journal_exit(sb);
	kvfree(sbi->s_ags);
	sbi->s_ags = NULL;
fail_cluster:
	ocsfs2_cluster_exit(sb);   /* stops heartbeat + releases slot if joined */
fail_sbh:
	if (sbi->s_sbh)
		brelse(sbi->s_sbh);
fail:
	sb->s_fs_info = NULL;
	kfree(sbi);
	return ret;
}

/* ── mount glue ── */

enum { Opt_cluster };
static const struct fs_parameter_spec ocsfs2_param_specs[] = {
	fsparam_flag("cluster", Opt_cluster),
	{}
};

static int ocsfs2_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct ocsfs2_fc *ctx = fc->fs_private;
	struct fs_parse_result result;
	int opt = fs_parse(fc, ocsfs2_param_specs, param, &result);

	if (opt < 0)
		return opt;
	switch (opt) {
	case Opt_cluster:
		ctx->cluster = true;
		break;
	}
	return 0;
}

static int ocsfs2_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, ocsfs2_fill_super);
}

static void ocsfs2_free_fc(struct fs_context *fc)
{
	kfree(fc->fs_private);
}

static const struct fs_context_operations ocsfs2_context_ops = {
	.parse_param = ocsfs2_parse_param,
	.get_tree    = ocsfs2_get_tree,
	.free        = ocsfs2_free_fc,
};

static int ocsfs2_init_fs_context(struct fs_context *fc)
{
	struct ocsfs2_fc *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;
	fc->fs_private = ctx;
	fc->ops = &ocsfs2_context_ops;
	return 0;
}

static void ocsfs2_kill_sb(struct super_block *sb)
{
	kill_block_super(sb);
}

static struct file_system_type ocsfs2_fs_type = {
	.owner           = THIS_MODULE,
	.name            = "ocsfs2",
	.init_fs_context = ocsfs2_init_fs_context,
	.kill_sb         = ocsfs2_kill_sb,
	.fs_flags        = FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS("ocsfs2");

/* ── module lifecycle ── */

static int __init ocsfs2_init(void)
{
	int ret;

	/* Whitelist the inline-extent byte area for usercopy: a fast symlink
	 * stores its target there and readlink() copies it straight to user
	 * space — without this, HARDENED_USERCOPY BUGs (mm/usercopy.c). */
	ocsfs2_inode_cachep = kmem_cache_create_usercopy("ocsfs2_inode_cache",
				sizeof(struct ocsfs2_inode_info), 0,
				SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
				offsetof(struct ocsfs2_inode_info, i_extents),
				sizeof_field(struct ocsfs2_inode_info, i_extents),
				ocsfs2_inode_init_once);
	if (!ocsfs2_inode_cachep)
		return -ENOMEM;

	ret = ocsfs2_scsi_pool_init();
	if (ret)
		goto out_cache;

	ret = register_filesystem(&ocsfs2_fs_type);
	if (ret)
		goto out_pool;

	pr_info("ocsfs2: registered (v2, single-writer ownership)\n");
	return 0;

out_pool:
	ocsfs2_scsi_pool_destroy();
out_cache:
	kmem_cache_destroy(ocsfs2_inode_cachep);
	return ret;
}

static void __exit ocsfs2_exit(void)
{
	unregister_filesystem(&ocsfs2_fs_type);
	ocsfs2_scsi_pool_destroy();
	rcu_barrier();   /* let pending RCU frees of inodes drain */
	kmem_cache_destroy(ocsfs2_inode_cachep);
	pr_info("ocsfs2: unloaded\n");
}

module_init(ocsfs2_init);
module_exit(ocsfs2_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OCSFS v2 - clustered shared-disk filesystem (single-writer ownership)");
MODULE_AUTHOR("OCSFS Project Contributors");
