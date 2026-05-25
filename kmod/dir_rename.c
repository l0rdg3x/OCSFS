// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — dir_rename.c
 * VFS rename, readdir, unlink, rmdir and directory operations tables.
 */

#include <linux/security.h>
#include <linux/xattr.h>
#include <linux/posix_acl.h>
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

/*
 * Atomically replace new_dir[new_name] in-place (de_ino=old_ino, de_ft=old_ft)
 * and zero old_dir[old_name] — all in one WAL transaction.
 *
 * Returns -ENOENT if either entry lacks a btree index; caller must fall back
 * to the three-step del+add+del path.  Does NOT touch nlink or dotdot.
 */
static int rename_replace_atomic(struct inode *old_dir, const struct qstr *old_name,
				  struct inode *new_dir, const struct qstr *new_name,
				  u64 old_ino, u8 old_ft)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(old_dir->i_sb);
	u64 old_block, new_block;
	u32 old_off, new_off;
	struct buffer_head *old_bh, *new_bh;
	struct ocsfs_disk_dirent *old_de, *new_de;
	struct ocsfs_txn *txn;
	int ret;

	if (ocsfs_dir_btree_locate(old_dir, old_name, &old_block, &old_off))
		return -ENOENT;
	if (ocsfs_dir_btree_locate(new_dir, new_name, &new_block, &new_off))
		return -ENOENT;

	if (sbi->s_clustered) {
		old_bh = sb_getblk(old_dir->i_sb, old_block);
		if (!old_bh)
			return -EIO;
		clear_buffer_uptodate(old_bh);
		if (bh_read(old_bh, 0) < 0) { brelse(old_bh); return -EIO; }
	} else {
		old_bh = sb_bread(old_dir->i_sb, old_block);
		if (!old_bh)
			return -EIO;
	}

	if (new_block == old_block) {
		new_bh = old_bh;
		get_bh(new_bh);
	} else if (sbi->s_clustered) {
		new_bh = sb_getblk(new_dir->i_sb, new_block);
		if (!new_bh) { brelse(old_bh); return -EIO; }
		clear_buffer_uptodate(new_bh);
		if (bh_read(new_bh, 0) < 0) { brelse(new_bh); brelse(old_bh); return -EIO; }
	} else {
		new_bh = sb_bread(new_dir->i_sb, new_block);
		if (!new_bh) { brelse(old_bh); return -EIO; }
	}

	txn = ocsfs_txn_begin(old_dir->i_sb);
	if (IS_ERR(txn)) {
		ret = PTR_ERR(txn);
		goto out_bh;
	}

	ret = ocsfs_txn_add_bh(txn, old_bh);
	if (ret) { ocsfs_txn_abort(txn); goto out_bh; }

	ret = ocsfs_txn_add_bh(txn, new_bh);	/* idempotent when same block */
	if (ret) { ocsfs_txn_abort(txn); goto out_bh; }

	/* Update new_dir entry in-place; name and de_magic are unchanged */
	new_de = (struct ocsfs_disk_dirent *)(new_bh->b_data + new_off);
	new_de->de_ino       = cpu_to_le64(old_ino);
	new_de->de_file_type = old_ft;

	/* Zero the old_dir entry */
	old_de = (struct ocsfs_disk_dirent *)(old_bh->b_data + old_off);
	old_de->de_magic    = 0;
	old_de->de_name_len = 0;
	old_de->de_ino      = 0;

	mark_buffer_dirty(old_bh);
	if (new_bh != old_bh)
		mark_buffer_dirty(new_bh);

	ret = ocsfs_txn_commit(txn);
	if (ret)
		goto out_bh;

	ocsfs_dir_btree_delete(old_dir, old_name);
	OCSFS_I(old_dir)->i_dirent_count--;
	inode_set_mtime_to_ts(old_dir, inode_set_ctime_current(old_dir));
	mark_inode_dirty(old_dir);
	inode_set_mtime_to_ts(new_dir, inode_set_ctime_current(new_dir));
	mark_inode_dirty(new_dir);

out_bh:
	brelse(new_bh);
	brelse(old_bh);
	return ret;
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

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
		return -EINVAL;

	/* RENAME_NOREPLACE: fail if target already exists */
	if ((flags & RENAME_NOREPLACE) && new_inode)
		return -EEXIST;

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
		/* Lock old_inode: ".." update (dir crossing) or RENAME_EXCHANGE ctime */
		{
			bool need = (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) ||
				    (flags & RENAME_EXCHANGE);
			if (need) {
				struct ocsfs_inode_info *mi = OCSFS_I(old_inode);

				if (mi != old_oi && mi != new_oi &&
				    !(new_inode && OCSFS_I(new_inode) == mi))
					lock_arr[n_locks++] = mi;
			}
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

	/* RENAME_EXCHANGE: swap the two directory entries in-place */
	if (flags & RENAME_EXCHANGE) {
		bool old_is_dir = S_ISDIR(old_inode->i_mode);
		bool new_is_dir = S_ISDIR(new_inode->i_mode);

		ret = __ocsfs_update_dirent_ino(old_dir, &old_dentry->d_name,
						OCSFS_I(new_inode)->i_disk_ino,
						ocsfs_mode_to_ft(new_inode->i_mode));
		if (ret)
			goto out_unlock;

		ret = __ocsfs_update_dirent_ino(new_dir, &new_dentry->d_name,
						OCSFS_I(old_inode)->i_disk_ino,
						ocsfs_mode_to_ft(old_inode->i_mode));
		if (ret) {
			int comp = __ocsfs_update_dirent_ino(
					old_dir, &old_dentry->d_name,
					OCSFS_I(old_inode)->i_disk_ino,
					ocsfs_mode_to_ft(old_inode->i_mode));
			if (comp)
				pr_err("ocsfs: exchange rollback failed (%d) — "
				       "inode %llu may be orphaned, run fsck\n",
				       comp, OCSFS_I(old_inode)->i_disk_ino);
			goto out_unlock;
		}

		if (old_dir != new_dir) {
			if (old_is_dir) {
				ret = ocsfs_rename_update_dotdot(old_inode,
								 new_oi->i_disk_ino);
				if (ret)
					goto out_unlock;
				drop_nlink(old_dir);
				inc_nlink(new_dir);
			}
			if (new_is_dir) {
				ret = ocsfs_rename_update_dotdot(new_inode,
								 old_oi->i_disk_ino);
				if (ret)
					goto out_unlock;
				drop_nlink(new_dir);
				inc_nlink(old_dir);
			}
			if (old_is_dir || new_is_dir) {
				mark_inode_dirty(old_dir);
				mark_inode_dirty(new_dir);
			}
		}

		inode_set_ctime_current(old_inode);
		inode_set_ctime_current(new_inode);
		mark_inode_dirty(old_inode);
		mark_inode_dirty(new_inode);
		ret = 0;
		goto out_unlock;
	}

	/* Remove or replace target if it exists */
	if (new_inode) {
		bool dir_target = S_ISDIR(new_inode->i_mode);

		if (dir_target && !__ocsfs_empty_dir(new_inode)) {
			ret = -ENOTEMPTY;
			goto out_unlock;
		}

		/*
		 * Fast path: single-txn atomic replace — update new_dir's dirent
		 * in-place (de_ino/de_ft) and zero old_dir's dirent atomically.
		 * Eliminates the del+add window where the target is transiently
		 * absent and the add+del window where the inode appears in both dirs.
		 */
		ret = rename_replace_atomic(old_dir, &old_dentry->d_name,
					    new_dir, &new_dentry->d_name,
					    OCSFS_I(old_inode)->i_disk_ino,
					    ocsfs_mode_to_ft(old_inode->i_mode));
		if (ret == 0) {
			/* Atomic replace succeeded — update nlinks and skip add+del */
			if (dir_target) {
				clear_nlink(new_inode);
				drop_nlink(new_dir);
			} else {
				inode_dec_link_count(new_inode);
			}
			mark_inode_dirty(new_inode);
			goto post_dirent;
		}
		if (ret != -ENOENT)
			goto out_unlock;

		/* Slow path: no btree index — fall through to del+add+del */
		ret = __ocsfs_del_dirent(new_dir, &new_dentry->d_name);
		if (ret)
			goto out_unlock;
		if (dir_target) {
			clear_nlink(new_inode);
			drop_nlink(new_dir);
		} else {
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
		int comp = __ocsfs_del_dirent(new_dir, &new_dentry->d_name);

		if (comp)
			pr_err("ocsfs: rename rollback failed (%d) — "
			       "inode %llu may appear in both directories, "
			       "run fsck\n",
			       comp, OCSFS_I(old_inode)->i_disk_ino);
		goto out_unlock;
	}

post_dirent:
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

	ret = ocsfs_init_acl(idmap, inode, dir);
	if (ret)
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

	ret = ocsfs_init_acl(idmap, inode, dir);
	if (ret)
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
 * MKNOD — special files (FIFO, socket, device)
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct inode *inode;
	int ret;

	inode = ocsfs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	init_special_inode(inode, inode->i_mode, rdev);

	ret = security_inode_init_security(inode, dir, &dentry->d_name,
					   ocsfs_initxattrs, NULL);
	if (ret && ret != -EOPNOTSUPP)
		goto fail;

	ret = ocsfs_init_acl(idmap, inode, dir);
	if (ret)
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

/* ═══════════════════════════════════════════════════════════════
 * LINK — hard links
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_link(struct dentry *old_dentry, struct inode *dir,
		      struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	int ret;

	if (S_ISDIR(inode->i_mode))
		return -EPERM;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(dir->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	inode_set_ctime_current(inode);
	inode_inc_link_count(inode);
	ihold(inode);

	ret = ocsfs_add_dirent(dir, &dentry->d_name,
			       oi->i_disk_ino,
			       ocsfs_mode_to_ft(inode->i_mode));
	if (ret) {
		inode_dec_link_count(inode);
		iput(inode);
	} else {
		mark_inode_dirty(inode);
		d_instantiate(dentry, inode);
	}

	if (sbi->s_clustered)
		ocsfs_lock_release(dir->i_sb, &oi->i_lock_res);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * TMPFILE — O_TMPFILE support (vim, make, gcc, etc.)
 *
 * Creates a nameless inode.  OCSFS_IFLAG_ORPHAN is kept set — the inode
 * has no directory entry and is freed by evict_inode when the last fd
 * closes (i_nlink == 0).  On crash the orphan scan at next mount reports
 * it; fsck reclaims the blocks.
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_tmpfile(struct mnt_idmap *idmap, struct inode *dir,
			 struct file *file, umode_t mode)
{
	struct inode *inode;
	int ret;

	inode = ocsfs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	ret = security_inode_init_security(inode, dir, NULL,
					   ocsfs_initxattrs, NULL);
	if (ret && ret != -EOPNOTSUPP)
		goto fail;

	ret = ocsfs_init_acl(idmap, inode, dir);
	if (ret)
		goto fail;

	mark_inode_dirty(inode);
	d_tmpfile(file, inode);   /* decrements i_nlink to 0, instantiates */
	return finish_open_simple(file, 0);
fail:
	inode_dec_link_count(inode);
	discard_new_inode(inode);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * OPERATIONS TABLES
 * ═══════════════════════════════════════════════════════════════ */

const struct inode_operations ocsfs_dir_inode_ops = {
	.lookup         = ocsfs_lookup,
	.create         = ocsfs_create,
	.link           = ocsfs_link,
	.mkdir          = ocsfs_mkdir,
	.mknod          = ocsfs_mknod,
	.rmdir          = ocsfs_rmdir,
	.unlink         = ocsfs_unlink,
	.rename         = ocsfs_rename,
	.symlink        = ocsfs_symlink,
	.listxattr      = ocsfs_listxattr,
	.setattr        = ocsfs_setattr,
	.getattr        = ocsfs_getattr,
	.fileattr_get   = ocsfs_fileattr_get,
	.fileattr_set   = ocsfs_fileattr_set,
	.get_inode_acl  = ocsfs_get_inode_acl,
	.set_acl        = ocsfs_set_acl,
	.tmpfile        = ocsfs_tmpfile,
};

const struct file_operations ocsfs_dir_fops = {
	.llseek         = generic_file_llseek,
	.read           = generic_read_dir,
	.iterate_shared = ocsfs_readdir,
	.fsync          = generic_file_fsync,
};
