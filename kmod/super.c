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
	bool fc_scrub;
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
			/* CRIT-O1: the per-node journal array must start at or after the
			 * fixed cluster-coordination metadata region (CAS lease, recovery
			 * leader, HB summary, key store).  Older volumes placed the journal
			 * at LOCK_TABLE end, overlapping all of them — in clustered mode this
			 * causes immediate cross-corruption.  Reject such volumes outright. */
			if (j_off < OCSFS_METADATA_RESERVED_END) {
				pr_err("ocsfs: s_journal_off (%llu) overlaps cluster metadata region (ends at %llu) — volume uses the broken pre-fix layout; reformat with current mkfs.ocsfs\n",
				       j_off, (u64)OCSFS_METADATA_RESERVED_END);
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

/* Byte offset of AG @i's on-disk descriptor.  AGs below the primary count live
 * in the primary region at s_ag_desc_off; the rest (added by a grow) live in the
 * extension region at s_ag_desc_ext_off.  primary_count == 0 is legacy/no-grow:
 * every descriptor is in the primary region (unchanged behaviour). */
static u64 ocsfs_ag_desc_byte_off(struct ocsfs_sb_info *sbi, u32 i)
{
	u32 primary = sbi->s_ag_desc_primary_count;

	if (primary == 0 || i < primary)
		return sbi->s_ag_desc_off +
		       (u64)i * sizeof(struct ocsfs_disk_ag);
	return sbi->s_ag_desc_ext_off +
	       (u64)(i - primary) * sizeof(struct ocsfs_disk_ag);
}

/* load allocation groups */
static int ocsfs_load_ags(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct buffer_head *bh;
	u32 i;
	u64 ag_desc_block;

	/* Over-allocate by OCSFS_AG_GROW_RESERVE so an online grow can append AGs
	 * in place without moving the array (the embedded AG lock_res are on the
	 * global DLM list and cannot be relocated). */
	sbi->s_ag_capacity = sbi->s_ag_count + OCSFS_AG_GROW_RESERVE;
	mutex_init(&sbi->s_grow_lock);
	sbi->s_ags = kvmalloc_array(sbi->s_ag_capacity,
				    sizeof(struct ocsfs_ag_info), GFP_KERNEL);
	if (!sbi->s_ags)
		return -ENOMEM;

	for (i = 0; i < sbi->s_ag_count; i++) {
		struct ocsfs_disk_ag *dag;
		struct ocsfs_ag_info *ag = &sbi->s_ags[i];

		ag_desc_block = ocsfs_byte_to_block(sbi,
				ocsfs_ag_desc_byte_off(sbi, i));
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
	int ret;

	if (!sbi->s_clustered)
		return 0;

	ret = ocsfs_lock_release(sb, &sbi->s_freeze_lock_res);
	/* MEDIO-N6: always return 0 so thaw_super completes the VFS unfreeze
	 * regardless of DLM state.  If the DLM release failed the filesystem
	 * should still be accessible; the coordinator lock times out and the
	 * stranded entry is cleaned up on next recovery.  Returning an error
	 * here causes some kernel versions to skip the VFS unfreeze, leaving
	 * the filesystem permanently frozen for all local writers. */
	if (ret)
		pr_err_ratelimited("ocsfs: unfreeze: DLM release failed (%d) — "
				   "freeze lock may time out on next recovery\n",
				   ret);
	return 0;
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
		sbi->s_scrub_enabled = ctx->fc_scrub;
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
	sbi->s_dedup_index_root      = le64_to_cpu(ds->s_dedup_index_root);
	sbi->s_data_off = le64_to_cpu(ds->s_data_off);
	sbi->s_ag_desc_off = le64_to_cpu(ds->s_ag_desc_off);
	/* AG grow: descriptors for AGs >= s_ag_desc_primary_count live in the
	 * extension region.  0 = legacy / no grow → all AGs in the primary region. */
	sbi->s_ag_desc_primary_count = le32_to_cpu(ds->s_ag_desc_primary_count);
	sbi->s_ag_desc_ext_off       = le64_to_cpu(ds->s_ag_desc_ext_off);
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
	mutex_init(&sbi->s_cas_mutex);
	mutex_init(&sbi->s_dedup_index_lock);
	ocsfs_lock_init(&sbi->s_freeze_lock_res, 0, OCSFS_LOCKRES_FREEZE);
	ocsfs_lock_init(&sbi->s_keystore_lock_res, 0, OCSFS_LOCKRES_KEYSTORE);

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

	/* If we reclaimed our own slot left ACTIVE by a crash, release the DLM
	 * locks the dead incarnation never unlocked.  No peer runs recovery for
	 * a node that crashes and remounts itself, so without this we deadlock
	 * for OCSFS_LOCK_ACQUIRE_TIMEOUT_MS against our own stale EX locks on
	 * every inode/dir the crash had held. */
	if (sbi->s_self_recover_gen) {
		pr_info("ocsfs: releasing stale locks from crashed gen %u\n",
			sbi->s_self_recover_gen);
		ocsfs_lock_recover_node(sb, sbi->s_node_slot,
					sbi->s_self_recover_gen);
		sbi->s_self_recover_gen = 0;
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

	/* SB-1: only stamp the superblock on a read-write mount.  On a read-only
	 * mount we must not write block 0 at all; in cluster mode two nodes mounting
	 * concurrently would otherwise race on the shared superblock without any DLM
	 * serialization, each clobbering the other's mount_count / checksum. */
	if (!sb_rdonly(sb)) {
		ds->s_mount_count = cpu_to_le64(le64_to_cpu(ds->s_mount_count) + 1);
		ds->s_last_mount_time = cpu_to_le64(ktime_get_real_ns());
		ds->s_checksum = cpu_to_le32(ocsfs_crc32c(~0U, ds, OCSFS_SUPERBLOCK_SIZE - 4));
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		ocsfs_update_super_mirror(sb);
	}

	pr_info("ocsfs: mounted \"%.64s\" AGs=%u free=%llu slot=%u%s%s\n",
		ds->s_label, sbi->s_ag_count, sbi->s_free_blocks, sbi->s_node_slot,
		sbi->s_clustered ? " clustered" : "",
		sbi->s_degraded  ? " degraded"  : "");

	ocsfs_dedup_scrub_start(sb);
	ocsfs_lazy_revoke_start(sb);
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
	/*
	 * Defense in depth: fill_super must return a negative errno on failure.
	 * A positive value (e.g. a leaked SCSI status like RESERVATION CONFLICT)
	 * makes get_tree_bdev report bogus success, tripping the BUG() in
	 * vfs_get_tree ("didn't set fc->root, returned N") and oopsing the kernel.
	 */
	if (ret > 0)
		ret = -EIO;
	if (ret == 0)
		ret = -EINVAL;   /* never reach the success path via fail: */
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
	ocsfs_lazy_revoke_stop(sb);
	/*
	 * Really-release any AG locks the allocator left held lazily, so we don't
	 * strand them on disk (held EX by a now-departing slot) for the next mount
	 * or a peer to have to recover.  The sweep is already stopped, so no race.
	 */
	if (sbi->s_clustered && sbi->s_ags) {
		u32 a;

		for (a = 0; a < sbi->s_ag_count; a++) {
			struct ocsfs_lock_res *lr = &sbi->s_ags[a].ag_lock_res;

			if (lr->lr_lazy || lr->lr_mode != OCSFS_LOCK_NL)
				ocsfs_lock_release(sb, lr);
		}
	}
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

	/* Best-effort: pick up a peer's online grow so df reflects the new size
	 * (no-op on the grower / single node; -ENXIO if we still need a LUN
	 * rescan, which we simply ignore here). */
	if (sbi->s_clustered)
		ocsfs_grow_refresh(sb);

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
		u64 off  = ocsfs_ag_desc_byte_off(sbi, i);
		u64 blk  = ocsfs_byte_to_block(sbi, off);
		/*
		 * Read-modify-write: we only update the mutable counters but
		 * recompute the CRC over the WHOLE descriptor, so the immutable
		 * fields (ag_magic, geometry, bitmap/inode offsets, reserved)
		 * must already be present.  sb_bread guarantees the buffer is
		 * uptodate; sb_getblk would hand us an uninitialised buffer when
		 * the block is not cached (e.g. after drop_caches), and we would
		 * then write garbage with a valid CRC over every AG descriptor —
		 * corrupting the whole filesystem ("AG bad magic" at next mount).
		 */
		struct buffer_head *bh = sb_bread(sb, blk);
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

/* ═══════════════════════════════════════════════════════════════
 * ONLINE GROW — extend the filesystem into an expanded LUN while mounted.
 *
 * New AGs are described in an extension descriptor region placed in the new
 * space (the primary region has no slack); each descriptor stores absolute
 * geometry, so no existing AG moves.  The in-memory s_ags array was
 * over-allocated at mount (s_ag_capacity), so new slots are appended in place
 * and published by bumping s_ag_count last.  Peers pick the grow up via
 * ocsfs_grow_refresh() from the heartbeat thread.
 * ═══════════════════════════════════════════════════════════════ */

/* Write a freshly-built metadata block synchronously.  These are brand-new
 * blocks not referenced by anything until the SB update commits the grow, so a
 * crash before that just leaves unused space — no journaling needed. */
static int grow_sync_block(struct super_block *sb, u64 blkno,
			   const void *data, u32 len)
{
	struct buffer_head *bh = sb_getblk(sb, blkno);
	int ret;

	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memset(bh->b_data, 0, sb->s_blocksize);
	if (data)
		memcpy(bh->b_data, data, len);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	sync_dirty_buffer(bh);
	ret = buffer_uptodate(bh) ? 0 : -EIO;
	brelse(bh);
	return ret;
}

/* Initialise one in-memory AG slot from a disk descriptor. */
static void grow_init_ag_slot(struct ocsfs_sb_info *sbi, u32 i,
			      const struct ocsfs_disk_ag *dag)
{
	struct ocsfs_ag_info *ag = &sbi->s_ags[i];
	u64 abs_base;

	ag->ag_no       = i;
	ag->block_start = le64_to_cpu(dag->ag_block_start);
	ag->block_count = le64_to_cpu(dag->ag_block_count);
	ag->free_blocks = le64_to_cpu(dag->ag_free_blocks);
	abs_base = ag->block_start * (u64)sbi->s_block_size;
	ag->bitmap_off      = abs_base + le64_to_cpu(dag->ag_bitmap_off);
	ag->inode_table_off = abs_base + le64_to_cpu(dag->ag_inode_table_off);
	ag->bitmap_size = le64_to_cpu(dag->ag_bitmap_size);
	ag->inode_count = le64_to_cpu(dag->ag_inode_count);
	ag->free_inodes = le64_to_cpu(dag->ag_free_inodes);
	ag->rc_btree_root = le64_to_cpu(dag->ag_rc_btree_root);
	mutex_init(&ag->ag_lock);
	ocsfs_lock_init(&ag->ag_lock_res, ocsfs_lock_hash_ag(i), OCSFS_LOCKRES_AG);
	ocsfs_lock_init(&ag->ag_rc_lock_res, ocsfs_lock_hash_rc(i),
			OCSFS_LOCKRES_REFCOUNT);
}

/* Build + write one new AG's on-disk metadata; returns its descriptor + free. */
static int grow_write_new_ag(struct super_block *sb, u32 agno,
			     u64 ag_data_start_byte, u64 ag_blocks, u64 ext_desc_byte,
			     u16 max_nodes, u64 *ag_free, struct ocsfs_disk_ag *dag_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u32 bs = sbi->s_block_size;
	u64 ag_data_blk = ag_data_start_byte / bs;
	u64 bitmap_blocks = (ag_blocks + (u64)bs * 8 - 1) / ((u64)bs * 8);
	u64 inodes_per_ag = ag_blocks / 64;
	u64 inode_table_blocks, metadata_blocks, free, b;
	struct ocsfs_disk_ag dag;
	u8 *bitmap;
	int ret;

	if (inodes_per_ag < 64)
		inodes_per_ag = 64;
	inode_table_blocks = (inodes_per_ag * OCSFS_INODE_SIZE + bs - 1) / bs;
	metadata_blocks = 1 + bitmap_blocks + inode_table_blocks;
	free = ag_blocks - metadata_blocks;

	memset(&dag, 0, sizeof(dag));
	dag.ag_magic           = cpu_to_le32(OCSFS_AG_MAGIC);
	dag.ag_number          = cpu_to_le32(agno);
	dag.ag_block_start     = cpu_to_le64(ag_data_blk);
	dag.ag_block_count     = cpu_to_le64(ag_blocks);
	dag.ag_free_blocks     = cpu_to_le64(free);
	dag.ag_free_extents    = cpu_to_le64(1);
	dag.ag_bitmap_off      = cpu_to_le64(bs);
	dag.ag_bitmap_size     = cpu_to_le64(bitmap_blocks * bs);
	dag.ag_inode_table_off = cpu_to_le64((1 + bitmap_blocks) * bs);
	dag.ag_inode_count     = cpu_to_le64(inodes_per_ag);
	dag.ag_free_inodes     = cpu_to_le64(inodes_per_ag);
	dag.ag_owner_node      = cpu_to_le16(agno % (max_nodes ? max_nodes : 1));
	dag.ag_checksum        = cpu_to_le32(ocsfs_crc32c(~0U, &dag,
				   offsetof(struct ocsfs_disk_ag, ag_checksum)));

	for (b = 0; b < inode_table_blocks; b++) {
		ret = grow_sync_block(sb, ag_data_blk + 1 + bitmap_blocks + b, NULL, 0);
		if (ret)
			return ret;
	}
	bitmap = kvzalloc(bitmap_blocks * bs, GFP_KERNEL);
	if (!bitmap)
		return -ENOMEM;
	for (b = 0; b < metadata_blocks; b++)
		bitmap[b / 8] |= (1u << (b % 8));
	for (b = 0; b < bitmap_blocks; b++) {
		ret = grow_sync_block(sb, ag_data_blk + 1 + b, bitmap + b * bs, bs);
		if (ret) {
			kvfree(bitmap);
			return ret;
		}
	}
	kvfree(bitmap);

	ret = grow_sync_block(sb, ag_data_blk, &dag, sizeof(dag));
	if (ret)
		return ret;
	ret = grow_sync_block(sb, ext_desc_byte / bs, &dag, sizeof(dag));
	if (ret)
		return ret;

	*ag_free = free;
	*dag_out = dag;
	return 0;
}

int ocsfs_grow_online(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u32 bs = sbi->s_block_size;
	u64 ag_blocks = sbi->s_ag_size;
	u64 ag_bytes = ag_blocks * bs;
	u32 old_ags, new_ags, max_add, j;
	u64 dev_size, old_end, avail, per_ag, ext_off, new_data_start, added_free = 0;
	int ret = 0;

	mutex_lock(&sbi->s_grow_lock);

	old_ags  = sbi->s_ag_count;
	dev_size = bdev_nr_bytes(sb->s_bdev);
	old_end  = sbi->s_data_off + (u64)old_ags * ag_bytes;
	if (dev_size <= old_end) {
		pr_warn("ocsfs: grow: no new space (device %llu, fs end %llu)\n",
			dev_size, old_end);
		ret = -ENOSPC;
		goto out;
	}

	avail   = dev_size - old_end;
	per_ag  = ag_bytes + sizeof(struct ocsfs_disk_ag);
	new_ags = (u32)(avail / per_ag);
	max_add = sbi->s_ag_capacity - old_ags;
	if (new_ags > max_add) {
		pr_warn("ocsfs: grow: new space fits %u AGs but reserve holds %u; "
			"adding %u (remount to use the rest)\n",
			new_ags, max_add, max_add);
		new_ags = max_add;
	}
	if (new_ags == 0) {
		ret = -ENOSPC;
		goto out;
	}

	ext_off        = old_end;
	new_data_start = ext_off + (u64)new_ags * sizeof(struct ocsfs_disk_ag);

	for (j = 0; j < new_ags; j++) {
		u32 agno = old_ags + j;
		u64 ag_data_start = new_data_start + (u64)j * ag_bytes;
		u64 ext_desc = ext_off + (u64)j * sizeof(struct ocsfs_disk_ag);
		struct ocsfs_disk_ag dag;
		u64 free;

		ret = grow_write_new_ag(sb, agno, ag_data_start, ag_blocks, ext_desc,
					sbi->s_max_nodes, &free, &dag);
		if (ret)
			goto out;            /* uncommitted: SB not yet updated */
		grow_init_ag_slot(sbi, agno, &dag);  /* slot ready but not yet published */
		added_free += free;
	}

	/* Commit point: persist the superblock (primary + mirror). */
	sbi->s_ds->s_ag_count = cpu_to_le32(old_ags + new_ags);
	sbi->s_ds->s_ag_desc_primary_count = cpu_to_le32(
		sbi->s_ag_desc_primary_count ? sbi->s_ag_desc_primary_count : old_ags);
	sbi->s_ds->s_ag_desc_ext_off = cpu_to_le64(
		sbi->s_ag_desc_ext_off ? sbi->s_ag_desc_ext_off : ext_off);
	sbi->s_ds->s_total_blocks = cpu_to_le64(sbi->s_total_blocks +
						(u64)new_ags * ag_blocks);
	sbi->s_ds->s_free_blocks  = cpu_to_le64(sbi->s_free_blocks + added_free);
	sbi->s_ds->s_feature_incompat = cpu_to_le64(sbi->s_feature_incompat |
						    OCSFS_FEATURE_INCOMPAT_AG_GROW);
	sbi->s_ds->s_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, sbi->s_ds, OCSFS_SUPERBLOCK_SIZE - 4));
	mark_buffer_dirty(sbi->s_sbh);
	sync_dirty_buffer(sbi->s_sbh);
	ocsfs_update_super_mirror(sb);

	/* Publish to this node's allocators. */
	if (!sbi->s_ag_desc_primary_count)
		sbi->s_ag_desc_primary_count = old_ags;
	if (!sbi->s_ag_desc_ext_off)
		sbi->s_ag_desc_ext_off = ext_off;
	sbi->s_feature_incompat |= OCSFS_FEATURE_INCOMPAT_AG_GROW;
	sbi->s_total_blocks += (u64)new_ags * ag_blocks;
	spin_lock(&sbi->s_free_lock);
	sbi->s_free_blocks += added_free;
	spin_unlock(&sbi->s_free_lock);
	smp_wmb();
	WRITE_ONCE(sbi->s_ag_count, old_ags + new_ags);

	pr_info("ocsfs: online grow complete: %u -> %u AGs, +%llu free blocks\n",
		old_ags, old_ags + new_ags, added_free);
out:
	mutex_unlock(&sbi->s_grow_lock);
	return ret;
}

int ocsfs_grow_refresh(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct buffer_head *bh;
	struct ocsfs_disk_super *ds;
	u32 disk_ags, disk_prim, old_ags, j;
	u64 disk_ext_off, disk_total, disk_free;
	int ret = 0;

	if (!sbi->s_clustered)
		return 0;
	if (le32_to_cpu(sbi->s_ds->s_ag_count) == READ_ONCE(sbi->s_ag_count) &&
	    !sbi->s_ag_desc_ext_off) {
		/* cheap pre-check using our cached SB copy; still confirm on disk
		 * only when it might have changed (see fresh read below) */
	}

	/* Fresh on-disk read of the superblock (block 0). */
	bh = sb_getblk(sb, OCSFS_SUPERBLOCK_OFFSET / sbi->s_block_size);
	if (!bh)
		return -EIO;
	clear_buffer_uptodate(bh);
	if (bh_read(bh, 0) < 0) {
		brelse(bh);
		return -EIO;
	}
	ds = (struct ocsfs_disk_super *)bh->b_data;
	disk_ags     = le32_to_cpu(ds->s_ag_count);
	disk_prim    = le32_to_cpu(ds->s_ag_desc_primary_count);
	disk_ext_off = le64_to_cpu(ds->s_ag_desc_ext_off);
	disk_total   = le64_to_cpu(ds->s_total_blocks);
	disk_free    = le64_to_cpu(ds->s_free_blocks);
	brelse(bh);

	if (disk_ags <= READ_ONCE(sbi->s_ag_count))
		return 0;

	/* Safety: do not adopt AGs whose data lies past this node's view of the
	 * device.  A peer that has not yet rescanned the expanded LUN must not load
	 * AGs it cannot address — it will pick them up once its bdev catches up. */
	if (disk_total * (u64)sbi->s_block_size >
	    bdev_nr_bytes(sb->s_bdev)) {
		pr_warn_ratelimited("ocsfs: volume grew to %llu blocks but this node's "
				    "device is smaller — rescan the LUN (iscsiadm -m node -R)\n",
				    disk_total);
		return -ENXIO;
	}

	mutex_lock(&sbi->s_grow_lock);
	old_ags = sbi->s_ag_count;
	if (disk_ags <= old_ags)
		goto out;
	if (disk_ags > sbi->s_ag_capacity) {
		pr_warn("ocsfs: peer grew to %u AGs; our reserve holds %u — remount to use all the new space\n",
			disk_ags, sbi->s_ag_capacity);
		disk_ags = sbi->s_ag_capacity;
		if (disk_ags <= old_ags)
			goto out;
	}

	/* Route descriptor reads to the extension region. */
	sbi->s_ag_desc_primary_count = disk_prim ? disk_prim : old_ags;
	sbi->s_ag_desc_ext_off       = disk_ext_off;

	for (j = old_ags; j < disk_ags; j++) {
		struct buffer_head *db;
		struct ocsfs_disk_ag *dag;
		u64 blk = ocsfs_byte_to_block(sbi, ocsfs_ag_desc_byte_off(sbi, j));

		db = sb_getblk(sb, blk);
		if (!db) {
			ret = -EIO;
			break;
		}
		clear_buffer_uptodate(db);
		if (bh_read(db, 0) < 0) {
			brelse(db);
			ret = -EIO;
			break;
		}
		dag = (struct ocsfs_disk_ag *)db->b_data;
		if (le32_to_cpu(dag->ag_magic) != OCSFS_AG_MAGIC) {
			brelse(db);
			ret = -EUCLEAN;
			break;
		}
		grow_init_ag_slot(sbi, j, dag);
		brelse(db);
	}
	if (!ret) {
		sbi->s_feature_incompat |= OCSFS_FEATURE_INCOMPAT_AG_GROW;
		sbi->s_total_blocks = disk_total;
		spin_lock(&sbi->s_free_lock);
		sbi->s_free_blocks = disk_free;
		spin_unlock(&sbi->s_free_lock);
		smp_wmb();
		WRITE_ONCE(sbi->s_ag_count, disk_ags);
		pr_info("ocsfs: picked up peer grow: now %u AGs\n", disk_ags);
	}
out:
	mutex_unlock(&sbi->s_grow_lock);
	return ret;
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
	if (strcmp(param->key, "scrub") == 0) { ctx->fc_scrub = true; return 0; }
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
