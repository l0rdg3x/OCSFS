// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — rename.c
 * VFS rename: same-dir, cross-dir, and directory moves (with ".." fixup).
 * Single-node (Plan 1): add-new-then-remove-old ordering keeps the inode
 * referenced across a crash window (Plan 2 journaling makes it atomic).
 */
#include "ocsfs.h"
#include <linux/fs.h>

/* Repoint the ".." entry of @dir to @new_parent_ino. */
static int repoint_dotdot(struct inode *dir, u64 new_parent_ino)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(dir);
	u64 nblocks = dir->i_size / OCSFS2_BLOCK_SIZE;
	u64 l;
	int ret = -ENOENT;

	mutex_lock(&oi->i_meta_lock);
	for (l = 0; l < nblocks; l++) {
		u64 phys;
		struct buffer_head *bh;
		unsigned s;

		if (ocsfs2_bmap(dir, l, &phys))
			continue;
		bh = sb_bread(dir->i_sb, phys);
		if (!bh)
			continue;
		for (s = 0; s < OCSFS2_DIRENTS_PER_BLOCK; s++) {
			struct ocsfs2_disk_dirent *de =
				(struct ocsfs2_disk_dirent *)(bh->b_data + s * OCSFS2_DIRENT_SIZE);

			if (le32_to_cpu(de->de_magic) != OCSFS2_DIRENT_MAGIC)
				continue;
			if (de->de_name_len == 2 &&
			    de->de_name[0] == '.' && de->de_name[1] == '.') {
				de->de_ino = cpu_to_le64(new_parent_ino);
				ocsfs2_dirent_set_csum(de);
				mark_buffer_dirty(bh);
				ret = 0;
				break;
			}
		}
		brelse(bh);
		if (!ret)
			break;
	}
	mutex_unlock(&oi->i_meta_lock);
	return ret;
}

int ocsfs2_rename(struct mnt_idmap *idmap, struct inode *old_dir,
		  struct dentry *old_dentry, struct inode *new_dir,
		  struct dentry *new_dentry, unsigned int flags)
{
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	bool is_dir = S_ISDIR(old_inode->i_mode);
	struct timespec64 now;
	int ret;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;   /* RENAME_EXCHANGE / RENAME_WHITEOUT unsupported */

	if (new_inode) {
		if (flags & RENAME_NOREPLACE)
			return -EEXIST;
		if (is_dir) {
			if (!S_ISDIR(new_inode->i_mode))
				return -ENOTDIR;
			if (!ocsfs2_empty_dir(new_inode))
				return -ENOTEMPTY;
		} else if (S_ISDIR(new_inode->i_mode)) {
			return -EISDIR;
		}
	}

	/* 1. install the new name pointing at old_inode */
	if (new_inode) {
		/* replace existing target's dirent ino in place would be neater,
		 * but for simplicity remove then add */
		ret = ocsfs2_del_dirent(new_dir, &new_dentry->d_name);
		if (ret && ret != -ENOENT)
			return ret;
	}
	ret = ocsfs2_add_dirent(new_dir, &new_dentry->d_name,
				OCSFS2_I(old_inode)->i_disk_ino,
				ocsfs2_mode_to_ft(old_inode->i_mode));
	if (ret)
		return ret;

	/* 2. drop the displaced target inode */
	if (new_inode) {
		if (S_ISDIR(new_inode->i_mode)) {
			clear_nlink(new_inode);
			drop_nlink(new_dir);   /* its ".." no longer counts */
		} else {
			drop_nlink(new_inode);
		}
		inode_set_ctime_current(new_inode);
		mark_inode_dirty(new_inode);
	}

	/* 3. remove the old name */
	ret = ocsfs2_del_dirent(old_dir, &old_dentry->d_name);
	if (ret) {
		/* best-effort rollback of the new name */
		ocsfs2_del_dirent(new_dir, &new_dentry->d_name);
		return ret;
	}

	/* 4. directory move across parents: fix ".." and nlinks */
	if (is_dir && old_dir != new_dir) {
		repoint_dotdot(old_inode, OCSFS2_I(new_dir)->i_disk_ino);
		drop_nlink(old_dir);   /* old parent loses child's ".." backlink */
		inc_nlink(new_dir);    /* new parent gains it */
		mark_inode_dirty(old_dir);
		mark_inode_dirty(new_dir);
	}

	now = current_time(old_inode);
	inode_set_ctime_to_ts(old_inode, now);
	mark_inode_dirty(old_inode);
	inode_set_mtime_to_ts(old_dir, now);
	inode_set_ctime_to_ts(old_dir, now);
	mark_inode_dirty(old_dir);
	inode_set_mtime_to_ts(new_dir, now);
	inode_set_ctime_to_ts(new_dir, now);
	mark_inode_dirty(new_dir);
	return 0;
}
