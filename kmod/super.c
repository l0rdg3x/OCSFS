// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — super.c
 * Superblock operations, module init/exit, mount/unmount.
 *
 * Phase 1: single-node operation (no cluster locking).
 */

#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include "ocsfs.h"

static struct kmem_cache *ocsfs_inode_cachep;

struct ocsfs_fs_context {
	u8   fc_secret[32];
	bool fc_has_secret;
};

/* ─── Inode slab ─────────────────────────────────────────────── */

static struct inode *ocsfs_alloc_inode(struct super_block *sb)
{
	struct ocsfs_inode_info *oi;

	oi = alloc_inode_sb(sb, ocsfs_inode_cachep, GFP_KERNEL);
	if (!oi)
		return NULL;

	mutex_init(&oi->i_extent_lock);
	mutex_init(&oi->i_lock_res.lr_mutex);
	INIT_LIST_HEAD(&oi->i_lock_res.lr_list);
	oi->i_lock_res.lr_mode = OCSFS_LOCK_NL;
	oi->i_extent_count = 0;
	oi->i_extent_tree_root = 0;
	oi->i_flags = 0;
	oi->i_ag = 0;
	oi->i_disk_ino = 0;
	oi->i_symlink    = NULL;
	oi->i_xattr_block = 0;

	return &oi->vfs_inode;
}

static void ocsfs_free_inode(struct inode *inode)
{
	kmem_cache_free(ocsfs_inode_cachep, OCSFS_I(inode));
}

static void ocsfs_inode_init_once(void *obj)
{
	struct ocsfs_inode_info *oi = obj;
	inode_init_once(&oi->vfs_inode);
}

/* ─── Superblock validation ──────────────────────────────────── */

static int ocsfs_validate_super(struct ocsfs_disk_super *ds,
				struct super_block *sb, int silent)
{
	u32 crc;

	if (le32_to_cpu(ds->s_magic) != OCSFS_MAGIC) {
		if (!silent)
			pr_err("ocsfs: bad magic 0x%08x (expected 0x%08x)\n",
			       le32_to_cpu(ds->s_magic), OCSFS_MAGIC);
		return -EINVAL;
	}

	if (le16_to_cpu(ds->s_version_major) != OCSFS_VERSION_MAJOR) {
		pr_err("ocsfs: unsupported version %u.%u\n",
		       le16_to_cpu(ds->s_version_major),
		       le16_to_cpu(ds->s_version_minor));
		return -EINVAL;
	}

	/* Validate CRC32C (covers bytes 0..4091) */
	crc = ocsfs_crc32c(~0U, ds, OCSFS_SUPERBLOCK_SIZE - 4);
	if (crc != le32_to_cpu(ds->s_checksum)) {
		pr_err("ocsfs: superblock checksum mismatch "
		       "(computed 0x%08x, stored 0x%08x)\n",
		       crc, le32_to_cpu(ds->s_checksum));
		return -EINVAL;
	}

	if (le32_to_cpu(ds->s_block_size) != OCSFS_DEFAULT_BLOCK_SIZE) {
		pr_err("ocsfs: unsupported block size %u (only 4096 supported)\n",
		       le32_to_cpu(ds->s_block_size));
		return -EINVAL;
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * LOAD ALLOCATION GROUPS
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_load_ags(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct buffer_head *bh;
	u32 i;
	u64 ag_desc_block;

	sbi->s_ags = kvmalloc_array(sbi->s_ag_count,
				    sizeof(struct ocsfs_ag_info), GFP_KERNEL);
	if (!sbi->s_ags)
		return -ENOMEM;

	for (i = 0; i < sbi->s_ag_count; i++) {
		struct ocsfs_disk_ag *dag;
		struct ocsfs_ag_info *ag = &sbi->s_ags[i];

		ag_desc_block = ocsfs_byte_to_block(sbi,
				sbi->s_ag_desc_off + (u64)i * sizeof(*dag));
		bh = sb_bread(sb, ag_desc_block);
		if (!bh) {
			pr_err("ocsfs: failed to read AG %u descriptor\n", i);
			kvfree(sbi->s_ags);
			sbi->s_ags = NULL;
			return -EIO;
		}

		dag = (struct ocsfs_disk_ag *)bh->b_data;

		if (le32_to_cpu(dag->ag_magic) != OCSFS_AG_MAGIC) {
			pr_err("ocsfs: AG %u bad magic\n", i);
			brelse(bh);
			kvfree(sbi->s_ags);
			sbi->s_ags = NULL;
			return -EINVAL;
		}

		ag->ag_no = i;
		ag->block_start = le64_to_cpu(dag->ag_block_start);
		ag->block_count = le64_to_cpu(dag->ag_block_count);
		ag->free_blocks = le64_to_cpu(dag->ag_free_blocks);
		ag->bitmap_off = le64_to_cpu(dag->ag_bitmap_off);
		ag->bitmap_size = le64_to_cpu(dag->ag_bitmap_size);
		ag->inode_table_off = le64_to_cpu(dag->ag_inode_table_off);
		ag->inode_count = le64_to_cpu(dag->ag_inode_count);
		ag->free_inodes = le64_to_cpu(dag->ag_free_inodes);
		mutex_init(&ag->ag_lock);
		ocsfs_lock_init(&ag->ag_lock_res,
				ocsfs_lock_hash_ag(i), OCSFS_LOCKRES_AG);

		brelse(bh);
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * FILL SUPER — called during mount
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct ocsfs_sb_info *sbi;
	struct ocsfs_disk_super *ds;
	struct buffer_head *bh;
	struct inode *root_inode;
	int silent = fc->sb_flags & SB_SILENT;
	int ret;

	/* Set block size before first sb_bread */
	if (!sb_set_blocksize(sb, OCSFS_DEFAULT_BLOCK_SIZE)) {
		pr_err("ocsfs: unable to set block size %d\n",
		       OCSFS_DEFAULT_BLOCK_SIZE);
		return -EINVAL;
	}

	/* Read superblock from block 0 */
	bh = sb_bread(sb, 0);
	if (!bh) {
		pr_err("ocsfs: unable to read superblock\n");
		return -EIO;
	}

	ds = (struct ocsfs_disk_super *)bh->b_data;
	ret = ocsfs_validate_super(ds, sb, silent);
	if (ret) {
		brelse(bh);
		return ret;
	}

	/* Allocate in-memory superblock info */
	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi) {
		brelse(bh);
		return -ENOMEM;
	}

	sb->s_fs_info = sbi;
	sbi->s_sb = sb;
	sbi->s_sbh = bh;
	sbi->s_ds = ds;

	/* Apply cluster_secret mount option */
	if (fc->fs_private) {
		struct ocsfs_fs_context *ctx = fc->fs_private;

		if (ctx->fc_has_secret) {
			memcpy(sbi->s_cluster_secret, ctx->fc_secret, 32);
			sbi->s_auth_required = true;
		}
	}

	/* Cache frequently-used fields */
	sbi->s_block_size = le32_to_cpu(ds->s_block_size);
	sbi->s_extent_size = le32_to_cpu(ds->s_extent_size);
	sbi->s_total_blocks = le64_to_cpu(ds->s_total_blocks);
	sbi->s_free_blocks = le64_to_cpu(ds->s_free_blocks);
	sbi->s_ag_count = le32_to_cpu(ds->s_ag_count);
	sbi->s_ag_size = le64_to_cpu(ds->s_ag_size);
	sbi->s_max_nodes = le16_to_cpu(ds->s_max_nodes);
	sbi->s_feature_flags = le64_to_cpu(ds->s_feature_flags);
	sbi->s_data_off = le64_to_cpu(ds->s_data_off);
	sbi->s_ag_desc_off = le64_to_cpu(ds->s_ag_desc_off);

	/* Enforce: auth feature requires cluster_secret= mount option */
	if ((sbi->s_feature_flags & OCSFS_FEAT_AUTH) && !sbi->s_auth_required) {
		pr_err("ocsfs: volume requires cluster_secret= mount option\n");
		ret = -EACCES;
		goto fail;
	}

	init_rwsem(&sbi->s_global_lock);
	spin_lock_init(&sbi->s_free_lock);
	mutex_init(&sbi->s_decompress_lock);

	/* Set up super_block fields */
	sb->s_magic = OCSFS_MAGIC;
	sb->s_op = &ocsfs_sops;
	sb->s_xattr = ocsfs_xattr_handlers;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;  /* nanosecond timestamps */

	/* Load allocation group descriptors */
	ret = ocsfs_load_ags(sb);
	if (ret)
		goto fail;

	/*
	 * Clustering init FIRST: claims a node slot, sets s_node_slot.
	 * Journal init depends on s_node_slot to find our journal region.
	 */
	ret = ocsfs_cluster_init(sb);
	if (ret) {
		pr_err("ocsfs: cluster init failed\n");
		goto fail_ags;
	}

	sbi->s_caw_supported = ocsfs_scsi_caw_probe(sb);
	if (sbi->s_caw_supported)
		pr_info("ocsfs: SCSI CAW supported — atomic lock writes enabled\n");

	/* Initialize journal at our node slot's region */
	ret = ocsfs_journal_init(sb);
	if (ret)
		goto fail_cluster;

	/* Replay our journal (crash recovery for this node) */
	ret = ocsfs_journal_replay(sb);
	if (ret) {
		pr_err("ocsfs: journal replay failed\n");
		goto fail_journal;
	}

	ocsfs_orphan_scan(sb);
	root_inode = ocsfs_iget(sb, OCSFS_ROOT_INO);
	if (IS_ERR(root_inode)) {
		pr_err("ocsfs: failed to read root inode\n");
		ret = PTR_ERR(root_inode);
		goto fail_journal;
	}

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto fail_journal;
	}

	ds->s_mount_count = cpu_to_le64(le64_to_cpu(ds->s_mount_count) + 1);
	ds->s_last_mount_time = cpu_to_le64(ktime_get_real_ns());
	ds->s_checksum = cpu_to_le32(ocsfs_crc32c(~0U, ds, OCSFS_SUPERBLOCK_SIZE - 4));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	pr_info("ocsfs: mounted volume \"%.64s\" (%u AGs, %llu blocks free, "
		"node slot %u%s)\n",
		ds->s_label, sbi->s_ag_count, sbi->s_free_blocks,
		sbi->s_node_slot,
		sbi->s_clustered ? ", clustered" : "");

	return 0;

fail_journal:
	ocsfs_journal_exit(sb);
fail_cluster:
	ocsfs_cluster_exit(sb);
fail_ags:
	kvfree(sbi->s_ags);
fail:
	brelse(bh);
	kfree(sbi);
	sb->s_fs_info = NULL;
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * PUT SUPER — called during unmount
 * ═══════════════════════════════════════════════════════════════ */

void ocsfs_put_super(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	if (!sbi)
		return;

	ocsfs_journal_exit(sb);   /* flush journal before releasing cluster slot */
	ocsfs_cluster_exit(sb);
	kvfree(sbi->s_decompress_wksp);
	mutex_destroy(&sbi->s_decompress_lock);
	kvfree(sbi->s_ags);
	brelse(sbi->s_sbh);
	kfree(sbi);
	sb->s_fs_info = NULL;
}

/* ─── statfs ─────────────────────────────────────────────────── */

int ocsfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 total_inodes = 0;
	u64 free_inodes = 0;
	u32 i;

	buf->f_type = OCSFS_MAGIC;
	buf->f_bsize = sbi->s_block_size;
	buf->f_blocks = sbi->s_total_blocks;

	for (i = 0; i < sbi->s_ag_count; i++) {
		total_inodes += sbi->s_ags[i].inode_count;
		free_inodes  += sbi->s_ags[i].free_inodes;
	}

	buf->f_bfree = sbi->s_free_blocks;
	buf->f_bavail = sbi->s_free_blocks;
	buf->f_files = total_inodes;
	buf->f_ffree = free_inodes;
	buf->f_namelen = OCSFS_MAX_NAME_LEN;
	buf->f_frsize = sbi->s_block_size;

	return 0;
}

/* ─── sync_fs ────────────────────────────────────────────────── */

int ocsfs_sync_fs(struct super_block *sb, int wait)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	if (wait) {
		down_write(&sbi->s_global_lock);
		mark_buffer_dirty(sbi->s_sbh);
		sync_dirty_buffer(sbi->s_sbh);
		up_write(&sbi->s_global_lock);
	}
	return 0;
}

/* ─── super_operations ───────────────────────────────────────── */

const struct super_operations ocsfs_sops = {
	.alloc_inode    = ocsfs_alloc_inode,
	.free_inode     = ocsfs_free_inode,
	.write_inode    = ocsfs_write_inode,
	.evict_inode    = ocsfs_evict_inode,
	.put_super      = ocsfs_put_super,
	.statfs         = ocsfs_statfs,
	.sync_fs        = ocsfs_sync_fs,
};

/* ═══════════════════════════════════════════════════════════════
 * MOUNT / MODULE INIT
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct ocsfs_fs_context *ctx = fc->fs_private;
	const char *hex;
	size_t len;
	int i;

	if (strcmp(param->key, "cluster_secret") != 0)
		return -ENOPARAM;

	hex = param->string;
	len = strlen(hex);
	if (len != 64) {
		pr_err("ocsfs: cluster_secret must be 64 hex chars (32 bytes)\n");
		return -EINVAL;
	}
	for (i = 0; i < 32; i++) {
		unsigned int byte;

		if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
			pr_err("ocsfs: cluster_secret: invalid hex at position %d\n",
			       i * 2);
			return -EINVAL;
		}
		ctx->fc_secret[i] = (u8)byte;
	}
	ctx->fc_has_secret = true;
	return 0;
}

static void ocsfs_free_fc(struct fs_context *fc)
{
	kfree(fc->fs_private);
}

static int ocsfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, ocsfs_fill_super);
}

static const struct fs_context_operations ocsfs_context_ops = {
	.parse_param = ocsfs_parse_param,
	.get_tree    = ocsfs_get_tree,
	.free        = ocsfs_free_fc,
};

static int ocsfs_init_fs_context(struct fs_context *fc)
{
	struct ocsfs_fs_context *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	fc->fs_private = ctx;
	fc->ops = &ocsfs_context_ops;
	return 0;
}

static void ocsfs_kill_sb(struct super_block *sb)
{
	kill_block_super(sb);
}

static struct file_system_type ocsfs_fs_type = {
	.owner           = THIS_MODULE,
	.name            = "ocsfs",
	.init_fs_context = ocsfs_init_fs_context,
	.kill_sb         = ocsfs_kill_sb,
	.fs_flags        = FS_REQUIRES_DEV,
};

static int __init ocsfs_init(void)
{
	int ret;

	ocsfs_inode_cachep = kmem_cache_create("ocsfs_inode_cache",
					       sizeof(struct ocsfs_inode_info),
					       0,
					       SLAB_RECLAIM_ACCOUNT |
					       SLAB_ACCOUNT,
					       ocsfs_inode_init_once);
	if (!ocsfs_inode_cachep)
		return -ENOMEM;

	ret = register_filesystem(&ocsfs_fs_type);
	if (ret) {
		kmem_cache_destroy(ocsfs_inode_cachep);
		return ret;
	}

	pr_info("ocsfs: Open Cluster Shared FileSystem v%d.%d loaded\n",
		OCSFS_VERSION_MAJOR, OCSFS_VERSION_MINOR);
	return 0;
}

static void __exit ocsfs_exit(void)
{
	unregister_filesystem(&ocsfs_fs_type);
	rcu_barrier();
	kmem_cache_destroy(ocsfs_inode_cachep);

	pr_info("ocsfs: module unloaded\n");
}

module_init(ocsfs_init);
module_exit(ocsfs_exit);
MODULE_AUTHOR("OCSFS Project Contributors");
MODULE_DESCRIPTION("OCSFS — Open Cluster Shared FileSystem");
MODULE_LICENSE("GPL");
MODULE_ALIAS_FS("ocsfs");
