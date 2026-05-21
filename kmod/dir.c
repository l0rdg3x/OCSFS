// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — dir.c
 * Directory operations: lookup, create, mkdir.
 * Rename, readdir, unlink, rmdir and operations tables are in dir_rename.c.
 * Large-directory B+ tree index is in dir_btree.c.
 */

#include "ocsfs.h"
#include "ocsfs_btree.h"


/* ═══════════════════════════════════════════════════════════════
 * DIRECTORY ENTRY I/O HELPERS
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Read a directory block. Returns the buffer_head or NULL.
 * The directory block is identified by its logical block number
 * within the directory inode's extent map.
 */
struct buffer_head *ocsfs_dir_bread(struct inode *dir, u64 logical_block)
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
	u64 btree_ino;

	if (sbi->s_clustered) {
		if (ocsfs_lock_acquire(dir->i_sb, &dir_oi->i_lock_res,
				       OCSFS_LOCK_SH))
			return 0;
	}

	/* Fast path: B+ tree index for large directories */
	btree_ino = ocsfs_dir_btree_lookup(dir, name, ft_out);
	if (btree_ino) {
		if (sbi->s_clustered)
			ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);
		return btree_ino;
	}

	/* Slow path: linear scan (small dirs or btree hash collision) */
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

int __ocsfs_add_dirent(struct inode *dir, const struct qstr *name,
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

	/* B+ tree index: insert or trigger migration on threshold */
	{
		struct ocsfs_inode_info *oi = OCSFS_I(dir);
		struct ocsfs_extent ext_tmp;
		u64 phys = 0;
		u32 phys_off = off;

		oi->i_dirent_count++;

		/* Resolve physical block for the slot we just filled */
		if (ocsfs_extent_lookup(dir, b, &ext_tmp) == 0 &&
		    ext_tmp.physical_block)
			phys = ext_tmp.physical_block + (b - ext_tmp.logical_block);

		if (oi->i_dir_btree_root && phys)
			ocsfs_dir_btree_insert(dir, name, phys, phys_off);
		else if (ocsfs_dir_btree_should_build(dir))
			ocsfs_dir_btree_migrate(dir);
	}

	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);

out:
	return ret;
}

int ocsfs_add_dirent(struct inode *dir, const struct qstr *name,
		     u64 ino, u8 file_type)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	struct ocsfs_inode_info *dir_oi = OCSFS_I(dir);
	int ret;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(dir->i_sb, &dir_oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	ret = __ocsfs_add_dirent(dir, name, ino, file_type);

	if (sbi->s_clustered)
		ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * DEL DIRENT — remove a directory entry by name
 * ═══════════════════════════════════════════════════════════════ */

int __ocsfs_del_dirent(struct inode *dir, const struct qstr *name)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	u64 dir_blocks = (dir->i_size + sbi->s_block_size - 1) /
			 sbi->s_block_size;
	u64 b;

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

			OCSFS_I(dir)->i_dirent_count--;
			ocsfs_dir_btree_delete(dir, name);

			inode_set_mtime_to_ts(dir,
				inode_set_ctime_current(dir));
			mark_inode_dirty(dir);
			return 0;
		}
		brelse(bh);
	}

	return -ENOENT;
}

int ocsfs_del_dirent(struct inode *dir, const struct qstr *name)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	struct ocsfs_inode_info *dir_oi = OCSFS_I(dir);
	int ret;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(dir->i_sb, &dir_oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	ret = __ocsfs_del_dirent(dir, name);

	if (sbi->s_clustered)
		ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);
	return ret;
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

int __ocsfs_empty_dir(struct inode *dir)
{
	struct empty_ctx ctx = { .count = 0 };

	ocsfs_dir_foreach(dir, empty_actor, &ctx);
	return ctx.count == 0;
}

int ocsfs_empty_dir(struct inode *dir)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	struct ocsfs_inode_info *dir_oi = OCSFS_I(dir);
	int ret;

	if (sbi->s_clustered)
		ocsfs_lock_acquire(dir->i_sb, &dir_oi->i_lock_res,
				   OCSFS_LOCK_SH);

	ret = __ocsfs_empty_dir(dir);

	if (sbi->s_clustered)
		ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * VFS LOOKUP
 * ═══════════════════════════════════════════════════════════════ */

struct dentry *ocsfs_lookup(struct inode *dir, struct dentry *dentry,
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

/* ═══════════════════════════════════════════════════════════════
 * VFS MKDIR
 * ═══════════════════════════════════════════════════════════════ */

struct dentry *ocsfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
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

