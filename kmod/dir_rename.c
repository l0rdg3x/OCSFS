// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — dir_rename.c
 * VFS rename, readdir, unlink, rmdir and directory operations tables.
 */

#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * VFS UNLINK
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct ocsfs_sb_info *sbi     = OCSFS_SB(dir->i_sb);
	struct inode *inode           = d_inode(dentry);
	struct ocsfs_inode_info *oi   = OCSFS_I(inode);
	struct ocsfs_inode_info *d_oi = OCSFS_I(dir);
	int ret;

	/*
	 * Acquire EX on both dir and child ordered by on-disk ino —
	 * same ordering as ocsfs_rmdir — to prevent ABBA deadlocks.
	 * With both locks held from the start, the nlink==0 truncation
	 * path below reuses the already-held child lock.
	 */
	if (sbi->s_clustered) {
		struct ocsfs_inode_info *first  = d_oi;
		struct ocsfs_inode_info *second = oi;

		if (oi->i_disk_ino < d_oi->i_disk_ino) {
			first  = oi;
			second = d_oi;
		}

		ret = ocsfs_lock_acquire(dir->i_sb, &first->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;

		ret = ocsfs_lock_acquire(dir->i_sb, &second->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret) {
			ocsfs_lock_release(dir->i_sb, &first->i_lock_res);
			return ret;
		}
	}

	ret = __ocsfs_del_dirent(dir, &dentry->d_name);
	if (ret)
		goto out_unlock;

	inode_dec_link_count(inode);
	inode_set_ctime_current(inode);

	if (inode->i_nlink == 0 && oi->i_disk_ino >= OCSFS_FIRST_USER_INO) {
		mutex_lock(&oi->i_extent_lock);
		ocsfs_extent_truncate(inode, 0);
		mutex_unlock(&oi->i_extent_lock);
		i_size_write(inode, 0);
	}

	mark_inode_dirty(inode);

out_unlock:
	if (sbi->s_clustered) {
		struct ocsfs_inode_info *first  = d_oi;
		struct ocsfs_inode_info *second = oi;

		if (oi->i_disk_ino < d_oi->i_disk_ino) {
			first  = oi;
			second = d_oi;
		}
		ocsfs_lock_release(dir->i_sb, &second->i_lock_res);
		ocsfs_lock_release(dir->i_sb, &first->i_lock_res);
	}
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * VFS RMDIR
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	struct ocsfs_inode_info *dir_oi  = OCSFS_I(dir);
	struct inode *inode = d_inode(dentry);
	struct ocsfs_inode_info *child_oi = OCSFS_I(inode);
	int ret;

	/*
	 * Acquire EX on both dir and child ordered by on-disk ino to prevent
	 * deadlock with a concurrent cross-dir rename of the same inodes.
	 */
	if (sbi->s_clustered) {
		struct ocsfs_inode_info *first  = dir_oi;
		struct ocsfs_inode_info *second = child_oi;

		if (child_oi->i_disk_ino < dir_oi->i_disk_ino) {
			first  = child_oi;
			second = dir_oi;
		}

		ret = ocsfs_lock_acquire(dir->i_sb, &first->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;

		ret = ocsfs_lock_acquire(dir->i_sb, &second->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret) {
			ocsfs_lock_release(dir->i_sb, &first->i_lock_res);
			return ret;
		}
	}

	if (!__ocsfs_empty_dir(inode)) {
		ret = -ENOTEMPTY;
		goto out_unlock;
	}

	ret = __ocsfs_del_dirent(dir, &dentry->d_name);
	if (ret)
		goto out_unlock;

	clear_nlink(inode);
	mark_inode_dirty(inode);

	drop_nlink(dir);
	mark_inode_dirty(dir);
	ret = 0;

out_unlock:
	if (sbi->s_clustered) {
		struct ocsfs_inode_info *first  = dir_oi;
		struct ocsfs_inode_info *second = child_oi;

		if (child_oi->i_disk_ino < dir_oi->i_disk_ino) {
			first  = child_oi;
			second = dir_oi;
		}
		ocsfs_lock_release(dir->i_sb, &second->i_lock_res);
		ocsfs_lock_release(dir->i_sb, &first->i_lock_res);
	}
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * VFS RENAME
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Update the ".." entry of a moved directory in a single journaled txn.
 * Safer than del+add (avoids the window where ".." is missing entirely).
 */
static int ocsfs_rename_update_dotdot(struct inode *inode, u64 new_parent_ino)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	u64 dir_blocks = (inode->i_size + sbi->s_block_size - 1) /
			 sbi->s_block_size;
	u64 b;

	for (b = 0; b < dir_blocks; b++) {
		struct buffer_head *bh;
		u32 off;

		bh = ocsfs_dir_bread(inode, b);
		if (!bh)
			continue;

		for (off = 0; off + OCSFS_DIRENT_SIZE <= sbi->s_block_size;
		     off += OCSFS_DIRENT_SIZE) {
			struct ocsfs_disk_dirent *de =
				(struct ocsfs_disk_dirent *)(bh->b_data + off);

			if (le32_to_cpu(de->de_magic) != OCSFS_DIRENT_MAGIC ||
			    de->de_name_len != 2 ||
			    de->de_name[0] != '.' || de->de_name[1] != '.')
				continue;

			{
				struct ocsfs_txn *txn = ocsfs_txn_begin(inode->i_sb);
				int tr;

				if (IS_ERR(txn)) { brelse(bh); return PTR_ERR(txn); }
				tr = ocsfs_txn_add_bh(txn, bh);
				if (tr) { ocsfs_txn_abort(txn); brelse(bh); return tr; }
				de->de_ino = cpu_to_le64(new_parent_ino);
				mark_buffer_dirty(bh);
				brelse(bh);
				return ocsfs_txn_commit(txn);
			}
		}
		brelse(bh);
	}
	return -ENOENT;
}

static int ocsfs_rename(struct mnt_idmap *idmap,
			struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry,
			unsigned int flags)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(old_dir->i_sb);
	struct ocsfs_inode_info *old_oi = OCSFS_I(old_dir);
	struct ocsfs_inode_info *new_oi = OCSFS_I(new_dir);
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	int ret;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	/*
	 * Acquire EX DLM on both directories atomically, ordered by
	 * on-disk inode number to prevent deadlock with a concurrent
	 * rename in the opposite direction.
	 */
	if (sbi->s_clustered) {
		struct ocsfs_inode_info *first = old_oi, *second = new_oi;

		if (old_dir != new_dir &&
		    new_oi->i_disk_ino < old_oi->i_disk_ino) {
			first  = new_oi;
			second = old_oi;
		}

		ret = ocsfs_lock_acquire(old_dir->i_sb, &first->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;

		if (old_dir != new_dir) {
			ret = ocsfs_lock_acquire(old_dir->i_sb,
						 &second->i_lock_res,
						 OCSFS_LOCK_EX);
			if (ret) {
				ocsfs_lock_release(old_dir->i_sb,
						   &first->i_lock_res);
				return ret;
			}
		}
	}

	/* Remove target if it exists */
	if (new_inode) {
		if (S_ISDIR(new_inode->i_mode)) {
			if (!__ocsfs_empty_dir(new_inode)) {
				ret = -ENOTEMPTY;
				goto out_unlock;
			}
			ret = __ocsfs_del_dirent(new_dir, &new_dentry->d_name);
			if (ret)
				goto out_unlock;
			clear_nlink(new_inode);
			drop_nlink(new_dir);
		} else {
			ret = __ocsfs_del_dirent(new_dir, &new_dentry->d_name);
			if (ret)
				goto out_unlock;
			inode_dec_link_count(new_inode);
		}
		mark_inode_dirty(new_inode);
	}

	/*
	 * Add to new location BEFORE removing from old.  On crash between
	 * these two commits the file appears in both dirs (fsck fixes the
	 * link count) rather than disappearing from the namespace entirely.
	 */
	ret = __ocsfs_add_dirent(new_dir, &new_dentry->d_name,
				 OCSFS_I(old_inode)->i_disk_ino,
				 ocsfs_mode_to_ft(old_inode->i_mode));
	if (ret)
		goto out_unlock;

	ret = __ocsfs_del_dirent(old_dir, &old_dentry->d_name);
	if (ret)
		goto out_unlock;

	/* Update ".." in moved directory — single in-place txn */
	if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
		struct ocsfs_inode_info *moved_oi = OCSFS_I(old_inode);

		if (sbi->s_clustered) {
			ret = ocsfs_lock_acquire(old_dir->i_sb,
						 &moved_oi->i_lock_res,
						 OCSFS_LOCK_EX);
			if (ret)
				goto out_unlock;
		}

		ret = ocsfs_rename_update_dotdot(old_inode, new_oi->i_disk_ino);
		inode_set_ctime_current(old_inode);
		mark_inode_dirty(old_inode);

		if (sbi->s_clustered)
			ocsfs_lock_release(old_dir->i_sb, &moved_oi->i_lock_res);

		if (ret)
			goto out_unlock;

		drop_nlink(old_dir);
		inc_nlink(new_dir);
		mark_inode_dirty(old_dir);
		mark_inode_dirty(new_dir);
	} else {
		inode_set_ctime_current(old_inode);
		mark_inode_dirty(old_inode);
	}
	ret = 0;

out_unlock:
	if (sbi->s_clustered) {
		if (old_dir != new_dir)
			ocsfs_lock_release(old_dir->i_sb, &new_oi->i_lock_res);
		ocsfs_lock_release(old_dir->i_sb, &old_oi->i_lock_res);
	}
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * VFS READDIR (iterate_shared)
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	u64 dir_blocks = (dir->i_size + sbi->s_block_size - 1) /
			 sbi->s_block_size;
	u64 b;
	u32 off;
	loff_t pos = ctx->pos;
	u64 entry_idx = 0;

	for (b = 0; b < dir_blocks; b++) {
		struct buffer_head *bh;

		bh = ocsfs_dir_bread(dir, b);
		if (!bh)
			continue;

		for (off = 0; off + OCSFS_DIRENT_SIZE <= sbi->s_block_size;
		     off += OCSFS_DIRENT_SIZE, entry_idx++) {
			struct ocsfs_disk_dirent *de =
				(struct ocsfs_disk_dirent *)(bh->b_data + off);

			if (entry_idx < pos)
				continue;

			if (le32_to_cpu(de->de_magic) != OCSFS_DIRENT_MAGIC ||
			    de->de_name_len == 0)
				continue;

			if (!dir_emit(ctx, (char *)de->de_name,
				      de->de_name_len,
				      le64_to_cpu(de->de_ino),
				      ocsfs_type_to_dt(de->de_file_type))) {
				brelse(bh);
				return 0;
			}

			ctx->pos = entry_idx + 1;
		}

		brelse(bh);
	}

	ctx->pos = entry_idx;
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * VFS CREATE AND MKDIR
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_create(struct mnt_idmap *idmap, struct inode *dir,
		 struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;
	int ret;

	inode = ocsfs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	ret = ocsfs_add_dirent(dir, &dentry->d_name,
			       OCSFS_I(inode)->i_disk_ino,
			       ocsfs_mode_to_ft(mode));
	if (ret) {
		inode_dec_link_count(inode);
		discard_new_inode(inode);
		return ret;
	}

	d_instantiate(dentry, inode);
	return 0;
}

struct dentry *ocsfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			   struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int ret;

	inode = ocsfs_new_inode(dir, S_IFDIR | mode);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	ret = ocsfs_add_dirent(inode, &(struct qstr)QSTR_INIT(".", 1),
			       OCSFS_I(inode)->i_disk_ino, OCSFS_FT_DIR);
	if (ret)
		goto fail;

	ret = ocsfs_add_dirent(inode, &(struct qstr)QSTR_INIT("..", 2),
			       OCSFS_I(dir)->i_disk_ino, OCSFS_FT_DIR);
	if (ret)
		goto fail;

	ret = ocsfs_add_dirent(dir, &dentry->d_name,
			       OCSFS_I(inode)->i_disk_ino, OCSFS_FT_DIR);
	if (ret)
		goto fail;

	inc_nlink(dir);
	mark_inode_dirty(dir);
	d_instantiate(dentry, inode);
	return NULL;

fail:
	clear_nlink(inode);
	discard_new_inode(inode);
	return ERR_PTR(ret);
}

/* ═══════════════════════════════════════════════════════════════
 * OPERATIONS TABLES
 * ═══════════════════════════════════════════════════════════════ */

const struct inode_operations ocsfs_dir_inode_ops = {
	.lookup         = ocsfs_lookup,
	.create         = ocsfs_create,
	.mkdir          = ocsfs_mkdir,
	.rmdir          = ocsfs_rmdir,
	.unlink         = ocsfs_unlink,
	.rename         = ocsfs_rename,
	.setattr        = ocsfs_setattr,
	.getattr        = ocsfs_getattr,
};

const struct file_operations ocsfs_dir_fops = {
	.llseek         = generic_file_llseek,
	.read           = generic_read_dir,
	.iterate_shared = ocsfs_readdir,
	.fsync          = generic_file_fsync,
};
