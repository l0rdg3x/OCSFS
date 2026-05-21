// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — inode.c
 * Inode read/write, allocation, VFS inode operations.
 *
 * Phase 1: single-node, inline extents only.
 */

#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * READ INODE FROM DISK
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_read_disk_inode(struct super_block *sb, u64 ino,
				 struct ocsfs_disk_inode *di)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct buffer_head *bh;
	u64 off = ocsfs_inode_disk_off(sbi, ino);
	u64 block = off / sbi->s_block_size;
	u32 boff = off % sbi->s_block_size;

	bh = sb_bread(sb, block);
	if (!bh)
		return -EIO;

	memcpy(di, bh->b_data + boff, sizeof(*di));
	brelse(bh);

	if (le32_to_cpu(di->i_magic) != OCSFS_INODE_MAGIC)
		return -EINVAL;

	return 0;
}

/*
 * Force a fresh read of the block containing @ino's on-disk inode.
 * Called before ocsfs_read_disk_inode() in clustered mode to bypass
 * any stale buffer-cache entry that predates a remote write.
 */
static void ocsfs_inode_invalidate_cache(struct super_block *sb, u64 ino)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 off   = ocsfs_inode_disk_off(sbi, ino);
	u64 block = off / sbi->s_block_size;
	struct buffer_head *bh;

	bh = sb_getblk(sb, block);
	if (!bh)
		return;
	clear_buffer_uptodate(bh);
	bh_read(bh, 0);   /* re-read from block device */
	brelse(bh);
}

/* Parse inline extents from the on-disk inode */
static void ocsfs_parse_extents(struct ocsfs_inode_info *oi,
				struct ocsfs_disk_inode *di)
{
	u16 count = le16_to_cpu(di->i_extent_count);
	u16 i;

	if (count > OCSFS_INLINE_EXTENTS)
		count = OCSFS_INLINE_EXTENTS;

	oi->i_extent_count = count;

	for (i = 0; i < count; i++) {
		struct ocsfs_disk_extent *de =
			(struct ocsfs_disk_extent *)
			(di->i_inline_extents + i * sizeof(*de));
		oi->i_extents[i].logical_block = le64_to_cpu(de->e_logical_block);
		oi->i_extents[i].physical_block = le64_to_cpu(de->e_physical_block);
		oi->i_extents[i].length = le32_to_cpu(de->e_length);
		oi->i_extents[i].flags = le16_to_cpu(de->e_flags);
	}
}

/* ═══════════════════════════════════════════════════════════════
 * IGET — read and instantiate a VFS inode
 * ═══════════════════════════════════════════════════════════════ */

struct inode *ocsfs_iget(struct super_block *sb, u64 ino)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct inode *inode;
	struct ocsfs_inode_info *oi;
	struct ocsfs_disk_inode di;
	int ret;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode_state_read(inode) & I_NEW))
		return inode;

	oi = OCSFS_I(inode);
	oi->i_disk_ino = ino;

	/* Initialise the per-inode DLM lock resource. */
	ocsfs_lock_init(&oi->i_lock_res,
			ocsfs_lock_hash_inode(ino), OCSFS_LOCKRES_INODE);

	if (sbi->s_clustered) {
		/*
		 * Acquire shared lock before reading to ensure no other node
		 * is mid-write on this inode, then force a fresh buffer-cache
		 * read so we don't serve data written before we mounted.
		 */
		ret = ocsfs_lock_acquire(sb, &oi->i_lock_res, OCSFS_LOCK_SH);
		if (ret) {
			iget_failed(inode);
			return ERR_PTR(ret);
		}
		ocsfs_inode_invalidate_cache(sb, ino);
	}

	ret = ocsfs_read_disk_inode(sb, ino, &di);
	if (ret) {
		if (sbi->s_clustered)
			ocsfs_lock_release(sb, &oi->i_lock_res);
		iget_failed(inode);
		return ERR_PTR(ret);
	}

	/*
	 * Keep SH lock held across the entire VFS population below.
	 * Releasing earlier would let a remote EX holder delete/rename
	 * this inode while we are still writing its fields, producing a
	 * torn view in the dcache.  Released just before unlock_new_inode.
	 */

	/* Fill VFS inode from disk data — still holding SH lock */
	inode->i_mode = le16_to_cpu(di.i_mode);
	set_nlink(inode, le16_to_cpu(di.i_nlink));
	i_uid_write(inode, le32_to_cpu(di.i_uid));
	i_gid_write(inode, le32_to_cpu(di.i_gid));
	inode->i_size = le64_to_cpu(di.i_size);
	inode->i_blocks = le64_to_cpu(di.i_blocks) *
			  (OCSFS_SB(sb)->s_block_size / 512);

	inode_set_atime_to_ts(inode,
		ns_to_timespec64(le64_to_cpu(di.i_atime)));
	inode_set_mtime_to_ts(inode,
		ns_to_timespec64(le64_to_cpu(di.i_mtime)));
	inode_set_ctime_to_ts(inode,
		ns_to_timespec64(le64_to_cpu(di.i_ctime)));

	oi->i_flags = le32_to_cpu(di.i_flags);
	oi->i_ag = le32_to_cpu(di.i_ag);
	oi->i_extent_tree_root = le64_to_cpu(di.i_extent_tree_root);

	ocsfs_parse_extents(oi, &di);

	oi->i_dir_btree_root = le64_to_cpu(di.i_dir_btree_root);
	oi->i_dirent_count   = le32_to_cpu(di.i_dirent_count);

	/* Set up operations based on file type */
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &ocsfs_file_inode_ops;
		inode->i_fop = &ocsfs_file_fops;
		inode->i_mapping->a_ops = &ocsfs_aops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &ocsfs_dir_inode_ops;
		inode->i_fop = &ocsfs_dir_fops;
		inode->i_mapping->a_ops = &ocsfs_aops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &ocsfs_special_inode_ops;
	} else {
		inode->i_op = &ocsfs_special_inode_ops;
		init_special_inode(inode, inode->i_mode,
				   inode->i_rdev);
	}

	/*
	 * Release SH only after all VFS fields are published.
	 * unlock_new_inode clears I_NEW so other waiters can proceed;
	 * releasing the DLM just before that keeps the window minimal.
	 */
	if (sbi->s_clustered)
		ocsfs_lock_release(sb, &oi->i_lock_res);

	unlock_new_inode(inode);
	return inode;
}

/* ═══════════════════════════════════════════════════════════════
 * WRITE INODE TO DISK
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_txn *txn = NULL;
	struct buffer_head *bh;
	struct ocsfs_disk_inode *di;
	u64 off, block;
	u32 boff;
	u16 i;
	int ret;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	/*
	 * Wrap the inode buffer in a journal transaction so a crash between
	 * mark_buffer_dirty() and the disk write leaves a recoverable WAL
	 * record.  ocsfs_txn_begin grabs j_lock, serializing journal writers.
	 * We always journal — even in single-node mode — for crash recovery.
	 *
	 * Lock ordering: i_lock_res (DLM EX) → j_lock (mutex). Consistent
	 * with all other call sites.
	 */
	txn = ocsfs_txn_begin(inode->i_sb);
	if (IS_ERR(txn)) {
		ret = PTR_ERR(txn);
		if (sbi->s_clustered)
			ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
		return ret;
	}

	off = ocsfs_inode_disk_off(sbi, oi->i_disk_ino);
	block = off / sbi->s_block_size;
	boff = off % sbi->s_block_size;

	bh = sb_bread(inode->i_sb, block);
	if (!bh) {
		ret = -EIO;
		goto out_abort;
	}

	/* Snapshot BEFORE image before mutating the buffer. */
	ret = ocsfs_txn_add_bh(txn, bh);
	if (ret) {
		brelse(bh);
		goto out_abort;
	}

	di = (struct ocsfs_disk_inode *)(bh->b_data + boff);

	di->i_magic = cpu_to_le32(OCSFS_INODE_MAGIC);
	di->i_ino = cpu_to_le64(oi->i_disk_ino);
	di->i_mode = cpu_to_le16(inode->i_mode);
	di->i_nlink = cpu_to_le16(inode->i_nlink);
	di->i_uid = cpu_to_le32(i_uid_read(inode));
	di->i_gid = cpu_to_le32(i_gid_read(inode));
	di->i_size = cpu_to_le64(inode->i_size);
	di->i_blocks = cpu_to_le64(inode->i_blocks /
				   (sbi->s_block_size / 512));
	{
		struct timespec64 ts;
		ts = inode_get_atime(inode);
		di->i_atime = cpu_to_le64(timespec64_to_ns(&ts));
		ts = inode_get_mtime(inode);
		di->i_mtime = cpu_to_le64(timespec64_to_ns(&ts));
		ts = inode_get_ctime(inode);
		di->i_ctime = cpu_to_le64(timespec64_to_ns(&ts));
	}
	di->i_flags = cpu_to_le32(oi->i_flags);
	di->i_ag = cpu_to_le32(oi->i_ag);
	di->i_extent_count = cpu_to_le16(oi->i_extent_count);
	di->i_extent_max = cpu_to_le16(OCSFS_INLINE_EXTENTS);
	di->i_extent_tree_root = cpu_to_le64(oi->i_extent_tree_root);
	di->i_dir_btree_root = cpu_to_le64(oi->i_dir_btree_root);
	di->i_dirent_count   = cpu_to_le32(oi->i_dirent_count);

	for (i = 0; i < oi->i_extent_count && i < OCSFS_INLINE_EXTENTS; i++) {
		struct ocsfs_disk_extent *de =
			(struct ocsfs_disk_extent *)
			(di->i_inline_extents + i * sizeof(*de));
		de->e_logical_block = cpu_to_le64(oi->i_extents[i].logical_block);
		de->e_physical_block = cpu_to_le64(oi->i_extents[i].physical_block);
		de->e_length = cpu_to_le32(oi->i_extents[i].length);
		de->e_flags = cpu_to_le16(oi->i_extents[i].flags);
		de->e_checksum = 0;
	}

	di->i_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, di, OCSFS_INODE_SIZE - 4));

	mark_buffer_dirty(bh);
	if (wbc->sync_mode == WB_SYNC_ALL)
		sync_dirty_buffer(bh);
	brelse(bh);

	/*
	 * Commit writes the COMMIT marker and syncs the journal header
	 * (durability point), then releases j_lock and frees txn.
	 */
	ret = ocsfs_txn_commit(txn);
	txn = NULL;

	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	return ret;

out_abort:
	ocsfs_txn_abort(txn);
	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * EVICT INODE — cleanup on last iput / delete
 * ═══════════════════════════════════════════════════════════════ */

void ocsfs_evict_inode(struct inode *inode)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);

	/* If nlink dropped to 0, free on-disk resources */
	if (!inode->i_nlink && oi->i_disk_ino >= OCSFS_FIRST_USER_INO) {
		/*
		 * Need EX to free the inode on disk.  Acquire regardless of
		 * whatever mode (if any) we may still hold; ocsfs_lock_acquire
		 * is safe to call when lr_mode == NL.
		 */
		if (sbi->s_clustered)
			ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					   OCSFS_LOCK_EX);

		ocsfs_extent_truncate(inode, 0);
		ocsfs_free_inode_num(inode->i_sb, oi->i_disk_ino);

		if (sbi->s_clustered)
			ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	} else if (sbi->s_clustered &&
		   oi->i_lock_res.lr_mode != OCSFS_LOCK_NL) {
		/* Release any lock left over from an error path in iget. */
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	}
}

/* ═══════════════════════════════════════════════════════════════
 * ALLOCATE NEW INODE
 * ═══════════════════════════════════════════════════════════════ */

struct inode *ocsfs_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct inode *inode;
	struct ocsfs_inode_info *oi;
	u64 ino;
	u32 ag_hint;
	int ret;

	/* Prefer the parent directory's AG for locality */
	ag_hint = OCSFS_I(dir)->i_ag;

	ret = ocsfs_alloc_inode_num(sb, ag_hint, &ino);
	if (ret)
		return ERR_PTR(ret);

	inode = new_inode(sb);
	if (!inode) {
		ocsfs_free_inode_num(sb, ino);
		return ERR_PTR(-ENOMEM);
	}

	oi = OCSFS_I(inode);
	oi->i_disk_ino = ino;
	oi->i_ag = ocsfs_ino_to_ag(sbi, ino);
	oi->i_extent_count = 0;
	oi->i_extent_tree_root = 0;
	oi->i_flags = 0;
	oi->i_dir_btree_root = 0;
	oi->i_dirent_count   = 0;

	/* Initialise per-inode DLM lock and take EX during creation. */
	ocsfs_lock_init(&oi->i_lock_res,
			ocsfs_lock_hash_inode(ino), OCSFS_LOCKRES_INODE);
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(sb, &oi->i_lock_res, OCSFS_LOCK_EX);
		if (ret) {
			iput(inode);
			ocsfs_free_inode_num(sb, ino);
			return ERR_PTR(ret);
		}
	}

	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_ino = ino;
	inode->i_blocks = 0;
	inode->i_size = 0;
	simple_inode_init_ts(inode);

	if (S_ISREG(mode)) {
		inode->i_op = &ocsfs_file_inode_ops;
		inode->i_fop = &ocsfs_file_fops;
		inode->i_mapping->a_ops = &ocsfs_aops;
	} else if (S_ISDIR(mode)) {
		inode->i_op = &ocsfs_dir_inode_ops;
		inode->i_fop = &ocsfs_dir_fops;
		inode->i_mapping->a_ops = &ocsfs_aops;
		set_nlink(inode, 2);  /* . and .. */
	} else {
		inode->i_op = &ocsfs_special_inode_ops;
	}

	insert_inode_hash(inode);
	mark_inode_dirty(inode);

	if (sbi->s_clustered)
		ocsfs_lock_release(sb, &oi->i_lock_res);

	return inode;
}

/* ═══════════════════════════════════════════════════════════════
 * SETATTR / GETATTR
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int ret;

	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		return ret;

	if (attr->ia_valid & ATTR_SIZE) {
		if (attr->ia_size < inode->i_size) {
			/* Truncate: free extents beyond new size */
			u64 from_block = (attr->ia_size +
				OCSFS_SB(inode->i_sb)->s_block_size - 1) /
				OCSFS_SB(inode->i_sb)->s_block_size;
			ocsfs_extent_truncate(inode, from_block);
		}
		truncate_setsize(inode, attr->ia_size);
	}

	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

int ocsfs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);

	generic_fillattr(idmap, request_mask, inode, stat);
	stat->blksize = OCSFS_SB(inode->i_sb)->s_block_size;
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * INODE OPERATIONS TABLES
 * ═══════════════════════════════════════════════════════════════ */

const struct inode_operations ocsfs_file_inode_ops = {
	.setattr        = ocsfs_setattr,
	.getattr        = ocsfs_getattr,
};

const struct inode_operations ocsfs_special_inode_ops = {
	.setattr        = ocsfs_setattr,
	.getattr        = ocsfs_getattr,
};
