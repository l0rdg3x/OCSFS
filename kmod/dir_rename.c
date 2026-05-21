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
	struct inode *inode = d_inode(dentry);
	int ret;

	ret = ocsfs_del_dirent(dir, &dentry->d_name);
	if (ret)
		return ret;

	inode_dec_link_count(inode);
	inode_set_ctime_current(inode);
	mark_inode_dirty(inode);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * VFS RMDIR
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);

	if (!ocsfs_empty_dir(inode))
		return -ENOTEMPTY;

	ocsfs_del_dirent(dir, &dentry->d_name);
	clear_nlink(inode);
	mark_inode_dirty(inode);

	drop_nlink(dir);
	mark_inode_dirty(dir);

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * VFS RENAME
 * ═══════════════════════════════════════════════════════════════ */

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

	/* If target exists, remove it first */
	if (new_inode) {
		if (S_ISDIR(new_inode->i_mode)) {
			if (!__ocsfs_empty_dir(new_inode)) {
				ret = -ENOTEMPTY;
				goto out_unlock;
			}
			__ocsfs_del_dirent(new_dir, &new_dentry->d_name);
			clear_nlink(new_inode);
			drop_nlink(new_dir);
		} else {
			__ocsfs_del_dirent(new_dir, &new_dentry->d_name);
			inode_dec_link_count(new_inode);
		}
		mark_inode_dirty(new_inode);
	}

	/* Remove from old location */
	ret = __ocsfs_del_dirent(old_dir, &old_dentry->d_name);
	if (ret)
		goto out_unlock;

	/* Add to new location */
	ret = __ocsfs_add_dirent(new_dir, &new_dentry->d_name,
				 OCSFS_I(old_inode)->i_disk_ino,
				 ocsfs_mode_to_ft(old_inode->i_mode));
	if (ret)
		goto out_unlock;

	/* Update .. in moved directory */
	if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
		struct ocsfs_inode_info *moved_oi = OCSFS_I(old_inode);
		struct qstr dotdot = QSTR_INIT("..", 2);

		if (sbi->s_clustered) {
			ret = ocsfs_lock_acquire(old_dir->i_sb,
						 &moved_oi->i_lock_res,
						 OCSFS_LOCK_EX);
			if (ret)
				goto out_unlock;
		}

		__ocsfs_del_dirent(old_inode, &dotdot);
		__ocsfs_add_dirent(old_inode, &dotdot,
				   new_oi->i_disk_ino, OCSFS_FT_DIR);

		if (sbi->s_clustered)
			ocsfs_lock_release(old_dir->i_sb, &moved_oi->i_lock_res);

		drop_nlink(old_dir);
		inc_nlink(new_dir);
		mark_inode_dirty(old_dir);
		mark_inode_dirty(new_dir);
	}

	inode_set_ctime_current(old_inode);
	mark_inode_dirty(old_inode);
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
