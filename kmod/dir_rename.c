// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — dir_rename.c
 * VFS rename, readdir, unlink, rmdir and directory operations tables.
 */

#include <linux/security.h>
#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * LSM SECURITY LABEL INITIALISATION
 *
 * Called by security_inode_init_security() to set the initial xattrs
 * chosen by the active LSM (SELinux, Smack, etc.) on a newly created
 * inode.  The inode is not yet visible in the directory at this point.
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_initxattrs(struct inode *inode,
			     const struct xattr *xattr_array, void *fs_data)
{
	const struct xattr *xattr;
	int ret = 0;

	for (xattr = xattr_array; xattr->name != NULL; xattr++) {
		ret = ocsfs_xattr_set_internal(inode, OCSFS_XATTR_NS_SECURITY,
					       xattr->name,
					       xattr->value, xattr->value_len,
					       0);
		if (ret < 0)
			break;
	}
	return ret;
}

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

	/* Acquire EX on dir and child by ino order — prevents ABBA deadlock. */
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

		/* Flush both inodes while EX is still held — cluster coherency */
		if (ret == 0) {
			int fr = ocsfs_flush_inode_locked(dir, true);
			if (fr)
				pr_warn_ratelimited(
					"ocsfs: unlink dir flush failed (%d)\n", fr);
			fr = ocsfs_flush_inode_locked(inode, true);
			if (fr)
				pr_warn_ratelimited(
					"ocsfs: unlink inode flush failed (%d)\n", fr);
		}

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

	/* Acquire EX on dir and child by ino order — prevents ABBA deadlock. */
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

		if (ret == 0) {
			int fr = ocsfs_flush_inode_locked(dir, true);
			if (fr)
				pr_warn_ratelimited(
					"ocsfs: rmdir dir flush failed (%d)\n", fr);
			fr = ocsfs_flush_inode_locked(inode, true);
			if (fr)
				pr_warn_ratelimited(
					"ocsfs: rmdir inode flush failed (%d)\n", fr);
		}

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
	/* Sorted DLM EX set: old_dir, new_dir, new_inode, old_inode(dotdot) */
	struct ocsfs_inode_info *lock_arr[4];
	int n_locks = 0;
	int ret;
	int i;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	if (sbi->s_clustered) {
		int j;
		struct ocsfs_inode_info *key;

		lock_arr[n_locks++] = old_oi;
		if (old_dir != new_dir)
			lock_arr[n_locks++] = new_oi;
		if (new_inode) {
			struct ocsfs_inode_info *ni = OCSFS_I(new_inode);
			if (ni != old_oi && ni != new_oi)
				lock_arr[n_locks++] = ni;
		}
		if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
			struct ocsfs_inode_info *mi = OCSFS_I(old_inode);
			if (mi != old_oi && mi != new_oi)
				lock_arr[n_locks++] = mi;
		}

		/* Insertion sort by i_disk_ino — at most 4 elements */
		for (i = 1; i < n_locks; i++) {
			key = lock_arr[i];
			for (j = i - 1;
			     j >= 0 && lock_arr[j]->i_disk_ino > key->i_disk_ino;
			     j--)
				lock_arr[j + 1] = lock_arr[j];
			lock_arr[j + 1] = key;
		}

		for (i = 0; i < n_locks; i++) {
			ret = ocsfs_lock_acquire(old_dir->i_sb,
						 &lock_arr[i]->i_lock_res,
						 OCSFS_LOCK_EX);
			if (ret) {
				while (--i >= 0)
					ocsfs_lock_release(old_dir->i_sb,
							   &lock_arr[i]->i_lock_res);
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

	/* Add-before-remove: on crash, file in both dirs (fsck fixes nlink). */
	ret = __ocsfs_add_dirent(new_dir, &new_dentry->d_name,
				 OCSFS_I(old_inode)->i_disk_ino,
				 ocsfs_mode_to_ft(old_inode->i_mode));
	if (ret)
		goto out_unlock;

	ret = __ocsfs_del_dirent(old_dir, &old_dentry->d_name);
	if (ret) {
		/* Compensate: undo new entry to avoid ghost in new_dir */
		__ocsfs_del_dirent(new_dir, &new_dentry->d_name);
		goto out_unlock;
	}

	/* Update ".." in moved directory — single in-place txn */
	if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
		ret = ocsfs_rename_update_dotdot(old_inode, new_oi->i_disk_ino);
		inode_set_ctime_current(old_inode);
		mark_inode_dirty(old_inode);
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
		if (ret == 0) {
			int fr;

			if (new_inode) {
				fr = ocsfs_flush_inode_locked(new_inode, true);
				if (fr)
					pr_warn_ratelimited(
						"ocsfs: rename new_inode flush failed (%d)\n",
						fr);
			}
			fr = ocsfs_flush_inode_locked(old_inode, true);
			if (fr)
				pr_warn_ratelimited(
					"ocsfs: rename old_inode flush failed (%d)\n", fr);
			fr = ocsfs_flush_inode_locked(old_dir, true);
			if (fr)
				pr_warn_ratelimited(
					"ocsfs: rename old_dir flush failed (%d)\n", fr);
			if (old_dir != new_dir) {
				fr = ocsfs_flush_inode_locked(new_dir, true);
				if (fr)
					pr_warn_ratelimited(
						"ocsfs: rename new_dir flush failed (%d)\n",
						fr);
			}
		}
		for (i = n_locks - 1; i >= 0; i--)
			ocsfs_lock_release(old_dir->i_sb,
					   &lock_arr[i]->i_lock_res);
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
			    de->de_name_len == 0 ||
			    de->de_name_len > OCSFS_MAX_NAME_LEN)
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

	ret = security_inode_init_security(inode, dir, &dentry->d_name,
					   ocsfs_initxattrs, NULL);
	if (ret && ret != -EOPNOTSUPP)
		goto fail;

	ret = ocsfs_add_dirent(dir, &dentry->d_name,
			       OCSFS_I(inode)->i_disk_ino,
			       ocsfs_mode_to_ft(mode));
	if (ret)
		goto fail;

	OCSFS_I(inode)->i_flags &= ~OCSFS_IFLAG_ORPHAN;
	mark_inode_dirty(inode);
	d_instantiate(dentry, inode);
	return 0;

fail:
	inode_dec_link_count(inode);
	discard_new_inode(inode);
	return ret;
}

struct dentry *ocsfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			   struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int ret;

	inode = ocsfs_new_inode(dir, S_IFDIR | mode);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	ret = security_inode_init_security(inode, dir, &dentry->d_name,
					   ocsfs_initxattrs, NULL);
	if (ret && ret != -EOPNOTSUPP)
		goto fail;

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
	OCSFS_I(inode)->i_flags &= ~OCSFS_IFLAG_ORPHAN;
	mark_inode_dirty(inode);
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
 * VFS SYMLINK
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	struct ocsfs_inode_info *oi;
	size_t slen = strlen(symname);
	int ret;

	if (slen > OCSFS_MAX_INLINE_SYMLINK)
		return -ENAMETOOLONG;

	inode = ocsfs_new_inode(dir, S_IFLNK | S_IRWXUGO);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	oi = OCSFS_I(inode);

	oi->i_symlink = kmalloc(slen + 1, GFP_KERNEL);
	if (!oi->i_symlink) {
		inode_dec_link_count(inode);
		discard_new_inode(inode);
		return -ENOMEM;
	}
	memcpy(oi->i_symlink, symname, slen + 1);
	inode->i_size = slen;
	inode->i_op = &ocsfs_symlink_inode_ops;

	ret = security_inode_init_security(inode, dir, &dentry->d_name,
					   ocsfs_initxattrs, NULL);
	if (ret && ret != -EOPNOTSUPP)
		goto symlink_fail;

	ret = ocsfs_add_dirent(dir, &dentry->d_name,
			       oi->i_disk_ino,
			       ocsfs_mode_to_ft(inode->i_mode));
	if (ret) {
symlink_fail:
		kfree(oi->i_symlink);
		oi->i_symlink = NULL;
		inode_dec_link_count(inode);
		discard_new_inode(inode);
		return ret;
	}

	oi->i_flags &= ~OCSFS_IFLAG_ORPHAN;
	mark_inode_dirty(inode);
	d_instantiate(dentry, inode);
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
	.symlink        = ocsfs_symlink,
	.listxattr      = ocsfs_listxattr,
	.setattr        = ocsfs_setattr,
	.getattr        = ocsfs_getattr,
};

const struct file_operations ocsfs_dir_fops = {
	.llseek         = generic_file_llseek,
	.read           = generic_read_dir,
	.iterate_shared = ocsfs_readdir,
	.fsync          = generic_file_fsync,
};
