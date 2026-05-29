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
	bool fc_degraded;
};

/* inode slab */
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
	oi->i_last_writer_slot = OCSFS_INVALID_WRITER_SLOT;
	oi->i_crypt_info = NULL;

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
	memset(oi->i_dquot, 0, sizeof(oi->i_dquot));
}

static int ocsfs_validate_super(struct ocsfs_disk_super *ds,
				struct super_block *sb, int silent)
{
	u32 crc;

	if (le32_to_cpu(ds->s_magic) != OCSFS_MAGIC) {
		if (!silent)
			pr_err("ocsfs: bad magic 0x%08x\n", le32_to_cpu(ds->s_magic));
		return -EINVAL;
	}
	if (le16_to_cpu(ds->s_version_major) != OCSFS_VERSION_MAJOR) {
		pr_err("ocsfs: unsupported version %u.%u\n",
		       le16_to_cpu(ds->s_version_major),
		       le16_to_cpu(ds->s_version_minor));
		return -EINVAL;
	}
	crc = ocsfs_crc32c(~0U, ds, OCSFS_SUPERBLOCK_SIZE - 4);
	if (crc != le32_to_cpu(ds->s_checksum)) {
		pr_err("ocsfs: superblock checksum mismatch\n");
		return -EINVAL;
	}
	/*
	 * NUOV-MEDIO-6: Only 4096-byte blocks are supported.  Logical Block
	 * Size (LBS) support would require propagating s_block_size through
	 * every bitmap, extent, and journal calculation — ~12h redesign.
	 * Volumes formatted with a different block size are explicitly rejected.
	 */
	if (le32_to_cpu(ds->s_block_size) != OCSFS_DEFAULT_BLOCK_SIZE) {
		pr_err("ocsfs: unsupported block size %u (only %u supported)\n",
		       le32_to_cpu(ds->s_block_size), OCSFS_DEFAULT_BLOCK_SIZE);
		return -EINVAL;
	}
	if (le32_to_cpu(ds->s_ag_count) == 0 ||
	    le32_to_cpu(ds->s_ag_count) > 65536) {
		pr_err("ocsfs: invalid ag_count %u\n", le32_to_cpu(ds->s_ag_count));
		return -EINVAL;
	}
	if (le16_to_cpu(ds->s_max_nodes) == 0 ||
	    le16_to_cpu(ds->s_max_nodes) > OCSFS_MAX_NODES) {
		pr_err("ocsfs: invalid max_nodes %u\n",
		       le16_to_cpu(ds->s_max_nodes));
		return -EINVAL;
	}
	if (le64_to_cpu(ds->s_ag_size) == 0 ||
	    le64_to_cpu(ds->s_ag_size) > 268435456ULL) {
		pr_err("ocsfs: invalid ag_size %llu\n",
		       le64_to_cpu(ds->s_ag_size));
		return -EINVAL;
	}
	{
		u64 bdev_size = bdev_nr_bytes(sb->s_bdev);
		u64 blk       = le32_to_cpu(ds->s_block_size);
		u64 n_nodes   = le16_to_cpu(ds->s_max_nodes);
		u64 j_size    = (u64)le32_to_cpu(ds->s_journal_size);
		u64 j_off     = le64_to_cpu(ds->s_journal_off);
		u64 ag_off    = le64_to_cpu(ds->s_ag_desc_off);
		u64 data_off  = le64_to_cpu(ds->s_data_off);
		u64 total     = le64_to_cpu(ds->s_total_blocks);
		u64 ag_count  = le32_to_cpu(ds->s_ag_count);

		if (bdev_size > 0) {
			/* journal must not overlap superblock */
			if (j_off < blk) {
				pr_err("ocsfs: s_journal_off (%llu) overlaps superblock\n", j_off);
				return -EINVAL;
			}
			/* per-node journal regions must fit in device */
			if (j_size == 0 || j_off + n_nodes * j_size > bdev_size) {
				pr_err("ocsfs: journal layout exceeds device size\n");
				return -EINVAL;
			}
			/* AG descriptor area must come after journal */
			if (ag_off < j_off + n_nodes * j_size) {
				pr_err("ocsfs: s_ag_desc_off (%llu) overlaps journal\n", ag_off);
				return -EINVAL;
			}
			/* AG descriptor area size */
			if (ag_off + ag_count * sizeof(struct ocsfs_disk_ag) > bdev_size) {
				pr_err("ocsfs: AG descriptor area exceeds device size\n");
				return -EINVAL;
			}
			/* data area */
			if (data_off <= ag_off || data_off >= bdev_size) {
				pr_err("ocsfs: s_data_off (%llu) is out of range\n", data_off);
				return -EINVAL;
			}
			/* total block count */
			if (total == 0 || total * blk > bdev_size) {
				pr_err("ocsfs: s_total_blocks (%llu) exceeds device size\n", total);
				return -EINVAL;
			}
			/* AG geometry must not exceed total block count */
			if (ag_count * le64_to_cpu(ds->s_ag_size) > total) {
				pr_err("ocsfs: ag_count(%llu) * ag_size(%llu) exceeds s_total_blocks(%llu)\n",
				       ag_count, le64_to_cpu(ds->s_ag_size), total);
				return -EINVAL;
			}
		}
	}

	/* ARCH-1: compat / incompat / ro_compat enforcement.
	 * s_revision_level == 0 means legacy FS — skip enforcement so that
	 * old volumes still mount without upgrade. */
	{
		u32 rev       = le32_to_cpu(ds->s_revision_level);
		u64 incompat  = le64_to_cpu(ds->s_feature_incompat);
		u64 ro_compat = le64_to_cpu(ds->s_feature_ro_compat);

		if (rev > 0) {
			u64 unknown_incompat = incompat & ~OCSFS_FEATURE_INCOMPAT_SUPP;

			if (unknown_incompat) {
				pr_err("ocsfs: unsupported incompat features 0x%llx — cannot mount (upgrade kernel or run ocsfs-tool downgrade)\n",
				       unknown_incompat);
				return -EINVAL;
			}

			if (ro_compat & ~OCSFS_FEATURE_RO_COMPAT_SUPP) {
				pr_warn("ocsfs: unsupported ro_compat features 0x%llx — mounting read-only\n",
					ro_compat & ~OCSFS_FEATURE_RO_COMPAT_SUPP);
				sb->s_flags |= SB_RDONLY;
			}
		}
	}
	return 0;
}

/* load allocation groups */
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

		{
			u32 ag_crc = ocsfs_crc32c(~0U, dag,
					offsetof(struct ocsfs_disk_ag, ag_checksum));
			if (ag_crc != le32_to_cpu(dag->ag_checksum)) {
				pr_err("ocsfs: AG %u checksum mismatch (disk=%08x calc=%08x)\n",
				       i, le32_to_cpu(dag->ag_checksum), ag_crc);
				brelse(bh);
				kvfree(sbi->s_ags);
				sbi->s_ags = NULL;
				return -EINVAL;
			}
		}

		ag->ag_no = i;
		ag->block_start = le64_to_cpu(dag->ag_block_start);
		ag->block_count = le64_to_cpu(dag->ag_block_count);
		ag->free_blocks = le64_to_cpu(dag->ag_free_blocks);
		/* ag_bitmap_off and ag_inode_table_off are AG-relative on disk;
		 * convert to absolute device byte offsets at load time so every
		 * caller can use them directly without adding block_start. */
		{
			u64 abs_base = ag->block_start * (u64)sbi->s_block_size;
			ag->bitmap_off      = abs_base + le64_to_cpu(dag->ag_bitmap_off);
			ag->inode_table_off = abs_base + le64_to_cpu(dag->ag_inode_table_off);
		}
		ag->bitmap_size = le64_to_cpu(dag->ag_bitmap_size);
		ag->inode_count = le64_to_cpu(dag->ag_inode_count);
		ag->free_inodes    = le64_to_cpu(dag->ag_free_inodes);
		ag->rc_btree_root  = le64_to_cpu(dag->ag_rc_btree_root);
		if (unlikely(!ag->block_count || !ag->bitmap_size)) {
			pr_warn("ocsfs: AG %u has zero block_count=%llu or bitmap_size=%llu — possibly corrupt descriptor\n",
				i, ag->block_count, ag->bitmap_size);
			brelse(bh);
			kvfree(sbi->s_ags);
			sbi->s_ags = NULL;
			return -EINVAL;
		}
		mutex_init(&ag->ag_lock);
		ocsfs_lock_init(&ag->ag_lock_res,
				ocsfs_lock_hash_ag(i), OCSFS_LOCKRES_AG);
		ocsfs_lock_init(&ag->ag_rc_lock_res,
				ocsfs_lock_hash_rc(i), OCSFS_LOCKRES_REFCOUNT);

		brelse(bh);
	}

	return 0;
}

/* ARCH-V3-2: Write primary superblock contents to the on-disk mirror copy.
 * Called after every primary write so that the mirror is always consistent. */
static void ocsfs_update_super_mirror(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 mirror_blk = OCSFS_SUPERBLOCK_MIRROR / sbi->s_block_size;
	struct buffer_head *mbh;

	mbh = sb_getblk(sb, mirror_blk);
	if (!mbh)
		return;
	lock_buffer(mbh);
	memcpy(mbh->b_data, sbi->s_sbh->b_data, sbi->s_block_size);
	set_buffer_uptodate(mbh);
	mark_buffer_dirty(mbh);
	unlock_buffer(mbh);
	sync_dirty_buffer(mbh);
	brelse(mbh);
}

/* ARCH-V3-6: Cluster-aware freeze/thaw hooks called by the VFS freeze path.
 * In cluster mode: the freeze coordinator acquires EX on s_freeze_lock_res so
 * that two nodes cannot hold the freeze coordinator role simultaneously, and
 * the DLM lock entry is visible cluster-wide as a freeze-in-progress signal. */
static int ocsfs_freeze_fs(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	if (!sbi->s_clustered)
		return 0;

	return ocsfs_lock_acquire(sb, &sbi->s_freeze_lock_res, OCSFS_LOCK_EX);
}

static int ocsfs_unfreeze_fs(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	if (!sbi->s_clustered)
		return 0;

	return ocsfs_lock_release(sb, &sbi->s_freeze_lock_res);
}

/* fill_super — called during mount */
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

	/* ARCH-V3-2: Read superblock — try primary (block 0) first, then mirror.
	 * Mirror is at OCSFS_SUPERBLOCK_MIRROR bytes from start = block 1. */
	bh = sb_bread(sb, 0);
	if (bh) {
		ds = (struct ocsfs_disk_super *)bh->b_data;
		ret = ocsfs_validate_super(ds, sb, silent);
		if (ret) {
			brelse(bh);
			bh = NULL;
		}
	}
	if (!bh) {
		u64 mirror_blk = OCSFS_SUPERBLOCK_MIRROR / OCSFS_DEFAULT_BLOCK_SIZE;

		pr_warn("ocsfs: primary superblock invalid or unreadable, trying mirror at block %llu\n",
			mirror_blk);
		bh = sb_bread(sb, mirror_blk);
		if (!bh) {
			pr_err("ocsfs: unable to read primary or mirror superblock\n");
			return -EIO;
		}
		ds = (struct ocsfs_disk_super *)bh->b_data;
		ret = ocsfs_validate_super(ds, sb, silent);
		if (ret) {
			brelse(bh);
			pr_err("ocsfs: both primary and mirror superblocks are corrupt\n");
			return ret;
		}
		pr_warn("ocsfs: mounted from mirror superblock — run fsck to repair primary\n");
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

	/* Apply mount options */
	if (fc->fs_private) {
		struct ocsfs_fs_context *ctx = fc->fs_private;

		if (ctx->fc_has_secret) {
			memcpy(sbi->s_cluster_secret, ctx->fc_secret, 32);
			memzero_explicit(ctx->fc_secret, 32);
			sbi->s_auth_required = true;
		}
		sbi->s_degraded = ctx->fc_degraded;
	}

	/* Cache frequently-used fields */
	sbi->s_block_size = le32_to_cpu(ds->s_block_size);
	sbi->s_extent_size = le32_to_cpu(ds->s_extent_size);
	sbi->s_total_blocks = le64_to_cpu(ds->s_total_blocks);
	sbi->s_free_blocks = le64_to_cpu(ds->s_free_blocks);
	sbi->s_ag_count = le32_to_cpu(ds->s_ag_count);
	sbi->s_ag_size = le64_to_cpu(ds->s_ag_size);
	sbi->s_max_nodes = le16_to_cpu(ds->s_max_nodes);
	sbi->s_feature_flags    = le64_to_cpu(ds->s_feature_flags);
	sbi->s_revision_level   = le32_to_cpu(ds->s_revision_level);
	sbi->s_feature_compat   = le64_to_cpu(ds->s_feature_compat);
	sbi->s_feature_incompat = le64_to_cpu(ds->s_feature_incompat);
	sbi->s_feature_ro_compat = le64_to_cpu(ds->s_feature_ro_compat);
	sbi->s_lock_table_off_cached = le64_to_cpu(ds->s_lock_table_off);
	sbi->s_lock_primary_count    = le32_to_cpu(ds->s_lock_primary_count);
	sbi->s_data_off = le64_to_cpu(ds->s_data_off);
	sbi->s_ag_desc_off = le64_to_cpu(ds->s_ag_desc_off);
	sbi->s_ext_flags4 = !!(sbi->s_feature_incompat &
			       OCSFS_FEATURE_INCOMPAT_EXT_FLAGS4);

	/* Enforce: auth feature requires cluster_secret= mount option */
	if ((sbi->s_feature_flags & OCSFS_FEAT_AUTH) && !sbi->s_auth_required) {
		pr_err("ocsfs: volume requires cluster_secret= mount option\n");
		ret = -EACCES;
		goto fail;
	}

	init_rwsem(&sbi->s_global_lock);
	spin_lock_init(&sbi->s_free_lock);
	mutex_init(&sbi->s_decompress_lock);
	ocsfs_lock_init(&sbi->s_freeze_lock_res, 0, OCSFS_LOCKRES_FREEZE);

	sbi->s_rc_buf_pool = mempool_create_kmalloc_pool(4, sbi->s_block_size);
	if (!sbi->s_rc_buf_pool) {
		ret = -ENOMEM;
		goto fail;
	}

	/* Set up super_block fields */
	sb->s_magic  = OCSFS_MAGIC;
	sb->s_flags |= SB_POSIXACL;
	sb->s_op      = &ocsfs_sops;
	sb->s_xattr   = ocsfs_xattr_handlers;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;  /* nanosecond timestamps */
	sb->s_qcop    = &dquot_quotactl_sysfile_ops;
	sb->dq_op     = &dquot_operations;
	sb->s_quota_types = QTYPE_MASK_USR | QTYPE_MASK_GRP | QTYPE_MASK_PRJ;
#ifdef CONFIG_FS_ENCRYPTION
	sb->s_cop = &ocsfs_fscrypt_ops;
#endif

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

	ret = ocsfs_cas_probe(sb);  /* also sets s_pr_capable */
	if (ret < 0 || (sbi->s_clustered && sbi->s_cas_backend == CAS_BACKEND_NONE)) {
		pr_err("ocsfs: clustered mount requires CAS backend (ret=%d)\n", ret);
		ret = ret < 0 ? ret : -EOPNOTSUPP;
		goto fail_ags;
	}
	if (sbi->s_clustered && !sbi->s_pr_capable && !sbi->s_degraded) {
		pr_err("ocsfs: clustered mount requires SCSI PR for node fencing; "
		       "use mount option 'degraded' to override (zombie node risk)\n");
		ret = -EOPNOTSUPP;
		goto fail_cluster;
	}

	/* MEDIO-V3-6: create compression buffer mempool before I/O begins */
	ret = ocsfs_comp_pool_create(sbi);
	if (ret) {
		pr_err("ocsfs: failed to create compression buffer pool\n");
		goto fail_cluster;
	}

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
	ocsfs_update_super_mirror(sb);

	/* ARCH-V3-1: log keys present in shared store so admins know which
	 * FS_IOC_ADD_ENCRYPTION_KEY calls are needed on this node. */
	ocsfs_key_store_notify_mount(sb);

	pr_info("ocsfs: mounted \"%.64s\" AGs=%u free=%llu slot=%u%s%s\n",
		ds->s_label, sbi->s_ag_count, sbi->s_free_blocks, sbi->s_node_slot,
		sbi->s_clustered ? " clustered" : "",
		sbi->s_degraded  ? " degraded"  : "");

	ocsfs_dedup_scrub_start(sb);
	ocsfs_debugfs_init(sb);
	return 0;

fail_journal:
	ocsfs_journal_exit(sb);
fail_cluster:
	ocsfs_cluster_exit(sb);
fail_ags:
	kvfree(sbi->s_ags);
fail:
	brelse(bh);
	mempool_destroy(sbi->s_rc_buf_pool);
	kfree(sbi);
	sb->s_fs_info = NULL;
	return ret;
}

/* put_super — called during unmount */
void ocsfs_put_super(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	if (!sbi)
		return;

	ocsfs_debugfs_exit(sb);
	ocsfs_dedup_scrub_stop(sb);
	ocsfs_journal_exit(sb);   /* flush journal before releasing cluster slot */
	ocsfs_cluster_exit(sb);
	ocsfs_comp_pool_destroy(sbi);
	kvfree(sbi->s_decompress_wksp);
	mutex_destroy(&sbi->s_decompress_lock);
	mempool_destroy(sbi->s_rc_buf_pool);
	kvfree(sbi->s_ags);
	brelse(sbi->s_sbh);
	kfree(sbi);
	sb->s_fs_info = NULL;
}

/* statfs */
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

/* sync_fs */
int ocsfs_sync_fs(struct super_block *sb, int wait)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u32 i;

	if (!wait)
		return 0;

	down_write(&sbi->s_global_lock);

	/* Persist global free block count in superblock + recompute checksum */
	spin_lock(&sbi->s_free_lock);
	sbi->s_ds->s_free_blocks = cpu_to_le64(sbi->s_free_blocks);
	spin_unlock(&sbi->s_free_lock);
	sbi->s_ds->s_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, sbi->s_ds, OCSFS_SUPERBLOCK_SIZE - 4));
	mark_buffer_dirty(sbi->s_sbh);
	sync_dirty_buffer(sbi->s_sbh);
	ocsfs_update_super_mirror(sb);

	/* Persist per-AG free counts to AG descriptor blocks */
	for (i = 0; i < sbi->s_ag_count; i++) {
		struct ocsfs_ag_info *ag = &sbi->s_ags[i];
		u64 off  = sbi->s_ag_desc_off + (u64)i * sizeof(struct ocsfs_disk_ag);
		u64 blk  = ocsfs_byte_to_block(sbi, off);
		struct buffer_head *bh = sb_getblk(sb, blk);
		struct ocsfs_disk_ag *dag;

		if (!bh)
			continue;
		dag = (struct ocsfs_disk_ag *)bh->b_data;
		mutex_lock(&ag->ag_lock);
		dag->ag_free_blocks  = cpu_to_le64(ag->free_blocks);
		dag->ag_free_inodes  = cpu_to_le64(ag->free_inodes);
		dag->ag_rc_btree_root = cpu_to_le64(ag->rc_btree_root);
		dag->ag_checksum = cpu_to_le32(
			ocsfs_crc32c(~0U, dag, offsetof(struct ocsfs_disk_ag, ag_checksum)));
		mutex_unlock(&ag->ag_lock);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}

	up_write(&sbi->s_global_lock);
	return 0;
}

static struct dquot **ocsfs_get_dquots(struct inode *inode)
{
	return OCSFS_I(inode)->i_dquot;
}

/* super_operations table */
const struct super_operations ocsfs_sops = {
	.alloc_inode    = ocsfs_alloc_inode,
	.free_inode     = ocsfs_free_inode,
	.write_inode    = ocsfs_write_inode,
	.evict_inode    = ocsfs_evict_inode,
	.put_super      = ocsfs_put_super,
	.statfs         = ocsfs_statfs,
	.sync_fs        = ocsfs_sync_fs,
	.freeze_fs      = ocsfs_freeze_fs,
	.unfreeze_fs    = ocsfs_unfreeze_fs,
	.get_dquots     = ocsfs_get_dquots,
};

/* mount / module init */
static int ocsfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct ocsfs_fs_context *ctx = fc->fs_private;
	const char *hex;
	size_t len;
	int i;

	if (strcmp(param->key, "degraded") == 0) { ctx->fc_degraded = true; return 0; }
	if (strcmp(param->key, "cluster_secret") != 0)
		return -ENOPARAM;

	hex = param->string;
	if (!hex) {
		pr_err("ocsfs: cluster_secret: missing value\n");
		return -EINVAL;
	}
	len = strnlen(hex, 129);
	if (len > 128) {
		pr_err("ocsfs: cluster_secret: value too long (max 128 chars)\n");
		return -EINVAL;
	}
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
	/* Zero the raw hex string so it does not linger in kernel memory */
	memzero_explicit(param->string, len);
	return 0;
}

static void ocsfs_free_fc(struct fs_context *fc)
{
	struct ocsfs_fs_context *ctx = fc->fs_private;

	if (ctx)
		memzero_explicit(ctx->fc_secret, 32);
	kfree(ctx);
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

static void ocsfs_kill_sb(struct super_block *sb) { kill_block_super(sb); }

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

	ret = ocsfs_scsi_pool_init();
	if (ret) {
		kmem_cache_destroy(ocsfs_inode_cachep);
		return ret;
	}

	ocsfs_debugfs_module_init();

	ret = register_filesystem(&ocsfs_fs_type);
	if (ret) {
		ocsfs_debugfs_module_exit();
		ocsfs_scsi_pool_destroy();
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
	ocsfs_debugfs_module_exit();
	rcu_barrier();
	ocsfs_scsi_pool_destroy();
	kmem_cache_destroy(ocsfs_inode_cachep);
}

module_init(ocsfs_init);
module_exit(ocsfs_exit);
MODULE_AUTHOR("OCSFS Project Contributors");
MODULE_DESCRIPTION("OCSFS — Open Cluster Shared FileSystem");
MODULE_LICENSE("GPL");
MODULE_ALIAS_FS("ocsfs");
