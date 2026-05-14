// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — dir.c
 * Directory operations: lookup, create, mkdir, rmdir, unlink, rename, readdir.
 *
 * Phase 1: Directories are stored as a flat list of ocsfs_disk_dirent
 * packed into the directory's data blocks (allocated via extents).
 * Future phases will add a B+ tree index for large directories.
 */

#include "ocsfs.h"

/* Size of a directory entry on disk (fixed for simplicity) */
#define OCSFS_DIRENT_SIZE  sizeof(struct ocsfs_disk_dirent)

/* ═══════════════════════════════════════════════════════════════
 * DIRECTORY ENTRY I/O HELPERS
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Read a directory block. Returns the buffer_head or NULL.
 * The directory block is identified by its logical block number
 * within the directory inode's extent map.
 */
static struct buffer_head *ocsfs_dir_bread(struct inode *dir, u64 logical_block)
{
	struct ocsfs_extent ext;
	int ret;

	ret = ocsfs_extent_lookup(dir, logical_block, &ext);
	if (ret || ext.physical_block == 0)
		return NULL;

	return sb_bread(dir->i_sb, ext.physical_block +
			(logical_block - ext.logical_block));
}

/*
 * Iterate over all directory entries. Calls @actor for each valid entry.
 * Returns 0 on success, or the first non-zero return from @actor.
 */
static int ocsfs_dir_foreach(struct inode *dir,
			     int (*actor)(struct ocsfs_disk_dirent *de,
					  u64 block, u32 offset,
					  void *priv),
			     void *priv)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	u64 dir_blocks = (dir->i_size + sbi->s_block_size - 1) /
			 sbi->s_block_size;
	u64 b;
	int ret;

	for (b = 0; b < dir_blocks; b++) {
		struct buffer_head *bh;
		u32 off;

		bh = ocsfs_dir_bread(dir, b);
		if (!bh)
			continue;

		for (off = 0; off + OCSFS_DIRENT_SIZE <= sbi->s_block_size;
		     off += OCSFS_DIRENT_SIZE) {
			struct ocsfs_disk_dirent *de =
				(struct ocsfs_disk_dirent *)(bh->b_data + off);

			if (le32_to_cpu(de->de_magic) != OCSFS_DIRENT_MAGIC)
				continue;
			if (de->de_name_len == 0)
				continue;

			ret = actor(de, b, off, priv);
			if (ret) {
				brelse(bh);
				return ret;
			}
		}

		brelse(bh);
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * FIND DIRENT — returns inode number, optionally file type
 * ═══════════════════════════════════════════════════════════════ */

struct find_ctx {
	const struct qstr *name;
	u64 ino;
	u8  ft;
};

static int find_actor(struct ocsfs_disk_dirent *de, u64 block, u32 offset,
		      void *priv)
{
	struct find_ctx *ctx = priv;

	if (de->de_name_len != ctx->name->len)
		return 0;
	if (memcmp(de->de_name, ctx->name->name, ctx->name->len) != 0)
		return 0;

	ctx->ino = le64_to_cpu(de->de_ino);
	ctx->ft = de->de_file_type;
	return 1;  /* found — stop iteration */
}

u64 ocsfs_find_dirent(struct inode *dir, const struct qstr *name, u8 *ft_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	struct ocsfs_inode_info *dir_oi = OCSFS_I(dir);
	struct find_ctx ctx = { .name = name, .ino = 0, .ft = 0 };

	if (sbi->s_clustered)
		ocsfs_lock_acquire(dir->i_sb, &dir_oi->i_lock_res,
				   OCSFS_LOCK_SH);

	ocsfs_dir_foreach(dir, find_actor, &ctx);

	if (sbi->s_clustered)
		ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);

	if (ft_out && ctx.ino)
		*ft_out = ctx.ft;
	return ctx.ino;
}

/* ═══════════════════════════════════════════════════════════════
 * ADD DIRENT — append a directory entry
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_add_dirent(struct inode *dir, const struct qstr *name,
		     u64 ino, u8 file_type)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	struct ocsfs_inode_info *dir_oi = OCSFS_I(dir);
	u64 dir_blocks = (dir->i_size + sbi->s_block_size - 1) /
			 sbi->s_block_size;
	struct buffer_head *bh = NULL;
	u64 b;
	u32 off = 0;
	int ret = 0;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(dir->i_sb, &dir_oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	/* Scan for a free slot in existing blocks */
	for (b = 0; b < dir_blocks; b++) {
		bh = ocsfs_dir_bread(dir, b);
		if (!bh)
			continue;

		for (off = 0; off + OCSFS_DIRENT_SIZE <= sbi->s_block_size;
		     off += OCSFS_DIRENT_SIZE) {
			struct ocsfs_disk_dirent *de =
				(struct ocsfs_disk_dirent *)(bh->b_data + off);

			if (le32_to_cpu(de->de_magic) == OCSFS_DIRENT_MAGIC &&
			    de->de_name_len > 0)
				continue;  /* slot in use */

			/* Free slot — fill it */
			goto fill;
		}
		brelse(bh);
		bh = NULL;
	}

	/* No free slot — allocate a new directory block */
	{
		u64 phys;

		ret = ocsfs_alloc_blocks(dir->i_sb, dir_oi->i_ag, 1, &phys);
		if (ret)
			goto out;

		ret = ocsfs_extent_insert(dir, dir_blocks, phys, 1,
					  OCSFS_EXT_WRITTEN);
		if (ret) {
			ocsfs_free_blocks(dir->i_sb, phys, 1);
			goto out;
		}

		dir->i_size += sbi->s_block_size;

		bh = sb_bread(dir->i_sb, phys);
		if (!bh) {
			ret = -EIO;
			goto out;
		}

		memset(bh->b_data, 0, sbi->s_block_size);
		off = 0;
	}

fill:
	{
		struct ocsfs_disk_dirent *de =
			(struct ocsfs_disk_dirent *)(bh->b_data + off);

		de->de_magic = cpu_to_le32(OCSFS_DIRENT_MAGIC);
		de->de_ino = cpu_to_le64(ino);
		de->de_file_type = file_type;
		de->de_name_len = name->len;
		memset(de->de_name, 0, OCSFS_MAX_NAME_LEN + 1);
		memcpy(de->de_name, name->name, name->len);
		de->de_rec_len = cpu_to_le16(OCSFS_DIRENT_SIZE);

		mark_buffer_dirty(bh);
		brelse(bh);
		bh = NULL;
	}

	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);

out:
	if (sbi->s_clustered)
		ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * DEL DIRENT — remove a directory entry by name
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_del_dirent(struct inode *dir, const struct qstr *name)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	struct ocsfs_inode_info *dir_oi = OCSFS_I(dir);
	u64 dir_blocks = (dir->i_size + sbi->s_block_size - 1) /
			 sbi->s_block_size;
	u64 b;
	int ret;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(dir->i_sb, &dir_oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	/*
	 * We can't use ocsfs_dir_foreach here because we need to modify
	 * the buffer and mark it dirty. Walk manually.
	 */
	for (b = 0; b < dir_blocks; b++) {
		struct buffer_head *bh;
		u32 off;

		bh = ocsfs_dir_bread(dir, b);
		if (!bh)
			continue;

		for (off = 0; off + OCSFS_DIRENT_SIZE <= sbi->s_block_size;
		     off += OCSFS_DIRENT_SIZE) {
			struct ocsfs_disk_dirent *de =
				(struct ocsfs_disk_dirent *)(bh->b_data + off);

			if (le32_to_cpu(de->de_magic) != OCSFS_DIRENT_MAGIC)
				continue;
			if (de->de_name_len != name->len)
				continue;
			if (memcmp(de->de_name, name->name, name->len) != 0)
				continue;

			/* Found — zero it */
			de->de_name_len = 0;
			de->de_ino = 0;
			de->de_magic = 0;
			mark_buffer_dirty(bh);
			brelse(bh);

			inode_set_mtime_to_ts(dir,
				inode_set_ctime_current(dir));
			mark_inode_dirty(dir);
			if (sbi->s_clustered)
				ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);
			return 0;
		}
		brelse(bh);
	}

	if (sbi->s_clustered)
		ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);
	return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════
 * EMPTY DIR CHECK
 * ═══════════════════════════════════════════════════════════════ */

struct empty_ctx {
	int count;
};

static int empty_actor(struct ocsfs_disk_dirent *de, u64 block, u32 offset,
		       void *priv)
{
	struct empty_ctx *ctx = priv;

	/* Skip . and .. */
	if (de->de_name_len == 1 && de->de_name[0] == '.')
		return 0;
	if (de->de_name_len == 2 &&
	    de->de_name[0] == '.' && de->de_name[1] == '.')
		return 0;

	ctx->count++;
	return 1;  /* stop — not empty */
}

int ocsfs_empty_dir(struct inode *dir)
{
	struct empty_ctx ctx = { .count = 0 };

	ocsfs_dir_foreach(dir, empty_actor, &ctx);
	return ctx.count == 0;
}

/* ═══════════════════════════════════════════════════════════════
 * VFS LOOKUP
 * ═══════════════════════════════════════════════════════════════ */

static struct dentry *ocsfs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct inode *inode = NULL;
	u64 ino;

	if (dentry->d_name.len > OCSFS_MAX_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	ino = ocsfs_find_dirent(dir, &dentry->d_name, NULL);
	if (ino) {
		inode = ocsfs_iget(dir->i_sb, ino);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	}

	return d_splice_alias(inode, dentry);
}

/* ═══════════════════════════════════════════════════════════════
 * VFS CREATE (regular file)
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_create(struct mnt_idmap *idmap, struct inode *dir,
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

/* ═══════════════════════════════════════════════════════════════
 * VFS MKDIR
 * ═══════════════════════════════════════════════════════════════ */

static struct dentry *ocsfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int ret;

	inode = ocsfs_new_inode(dir, S_IFDIR | mode);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	/* Add . entry */
	ret = ocsfs_add_dirent(inode,
			       &(struct qstr)QSTR_INIT(".", 1),
			       OCSFS_I(inode)->i_disk_ino,
			       OCSFS_FT_DIR);
	if (ret)
		goto fail;

	/* Add .. entry */
	ret = ocsfs_add_dirent(inode,
			       &(struct qstr)QSTR_INIT("..", 2),
			       OCSFS_I(dir)->i_disk_ino,
			       OCSFS_FT_DIR);
	if (ret)
		goto fail;

	/* Add entry in parent */
	ret = ocsfs_add_dirent(dir, &dentry->d_name,
			       OCSFS_I(inode)->i_disk_ino,
			       OCSFS_FT_DIR);
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
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	int ret;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	/* If target exists, remove it first */
	if (new_inode) {
		if (S_ISDIR(new_inode->i_mode)) {
			if (!ocsfs_empty_dir(new_inode))
				return -ENOTEMPTY;
			ocsfs_del_dirent(new_dir, &new_dentry->d_name);
			clear_nlink(new_inode);
			drop_nlink(new_dir);
		} else {
			ocsfs_del_dirent(new_dir, &new_dentry->d_name);
			inode_dec_link_count(new_inode);
		}
		mark_inode_dirty(new_inode);
	}

	/* Remove from old location */
	ret = ocsfs_del_dirent(old_dir, &old_dentry->d_name);
	if (ret)
		return ret;

	/* Add to new location */
	ret = ocsfs_add_dirent(new_dir, &new_dentry->d_name,
			       OCSFS_I(old_inode)->i_disk_ino,
			       ocsfs_mode_to_ft(old_inode->i_mode));
	if (ret)
		return ret;

	/* Update .. in moved directory */
	if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
		struct qstr dotdot = QSTR_INIT("..", 2);

		ocsfs_del_dirent(old_inode, &dotdot);
		ocsfs_add_dirent(old_inode, &dotdot,
				 OCSFS_I(new_dir)->i_disk_ino,
				 OCSFS_FT_DIR);
		drop_nlink(old_dir);
		inc_nlink(new_dir);
		mark_inode_dirty(old_dir);
		mark_inode_dirty(new_dir);
	}

	inode_set_ctime_current(old_inode);
	mark_inode_dirty(old_inode);
	return 0;
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
