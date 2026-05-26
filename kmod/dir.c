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
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	struct ocsfs_extent ext;
	u64 phys_block;
	int ret;

	ret = ocsfs_extent_lookup(dir, logical_block, &ext);
	if (ret || ext.physical_block == 0)
		return NULL;

	phys_block = ext.physical_block + (logical_block - ext.logical_block);

	if (sbi->s_clustered) {
		/*
		 * In cluster mode, another node may have written to this
		 * directory block since our cached copy was loaded.  Force a
		 * fresh read so we see the latest entries.
		 */
		struct buffer_head *bh = sb_getblk(dir->i_sb, phys_block);

		if (!bh)
			return NULL;
		clear_buffer_uptodate(bh);
		if (bh_read(bh, 0) < 0) {
			brelse(bh);
			return NULL;
		}
		return bh;
	}
	return sb_bread(dir->i_sb, phys_block);
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
		if (ocsfs_inode_refresh(dir)) {
			ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);
			return 0;
		}
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
	struct ocsfs_txn *txn;
	u64 phys_for_btree = 0;
	u32 phys_off_for_btree = 0;
	u64 b; u32 off = 0;
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

		/* Zero and flush the block BEFORE linking it into the extent tree
		 * so a crash after extent_insert but before the dirent write does
		 * not leave garbage in a reachable block (ALTO-2). */
		bh = sb_getblk(dir->i_sb, phys);
		if (!bh) {
			ocsfs_free_blocks(dir->i_sb, phys, 1);
			ret = -EIO;
			goto out;
		}
		memset(bh->b_data, 0, sbi->s_block_size);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);

		mutex_lock(&dir_oi->i_extent_lock);
		ret = ocsfs_extent_insert(dir, dir_blocks, phys, 1,
					  OCSFS_EXT_WRITTEN);
		if (ret) {
			mutex_unlock(&dir_oi->i_extent_lock);
			ocsfs_free_blocks(dir->i_sb, phys, 1);
			brelse(bh);
			bh = NULL;
			goto out;
		}
		dir->i_size += sbi->s_block_size;
		mutex_unlock(&dir_oi->i_extent_lock);
		off = 0;
	}
fill:
	txn = ocsfs_txn_begin(dir->i_sb);
	if (IS_ERR(txn)) { brelse(bh); ret = PTR_ERR(txn); goto out; }
	ret = ocsfs_txn_add_bh(txn, bh);
	if (ret) { ocsfs_txn_abort(txn); brelse(bh); goto out; }
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

		brelse(bh);
		bh = NULL;
	}
	{
		struct ocsfs_extent ext_tmp;

		phys_off_for_btree = off;
		if (ocsfs_extent_lookup(dir, b, &ext_tmp) == 0 &&
		    ext_tmp.physical_block)
			phys_for_btree = ext_tmp.physical_block +
					 (b - ext_tmp.logical_block);
	}
	ret = ocsfs_txn_commit(txn);
	if (ret)
		goto out;
	OCSFS_I(dir)->i_dirent_count++;
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);
	/* B+ tree index: separate txn, avoids j_lock re-entry */
	if (dir_oi->i_dir_btree_root && phys_for_btree)
		ocsfs_dir_btree_insert(dir, name, phys_for_btree,
				       phys_off_for_btree);
	else if (ocsfs_dir_btree_should_build(dir))
		ocsfs_dir_btree_migrate(dir);
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

	if (sbi->s_clustered) {
		/*
		 * Flush i_size / i_dirent_count to disk while we still hold EX.
		 * Without this, another node that acquires EX immediately after
		 * our release would call ocsfs_inode_invalidate_cache and read
		 * a fresh block from the device — but see the old inode data
		 * because we only called mark_inode_dirty (async writeback).
		 */
		if (ret == 0) {
			int fr = ocsfs_flush_inode_locked(dir, true);

			if (fr)
				pr_warn_ratelimited(
					"ocsfs: add_dirent inode flush failed (%d)\n",
					fr);
		}
		ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);
	}
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * DEL DIRENT — remove a directory entry by name
 * ═══════════════════════════════════════════════════════════════ */

static int del_dirent_at(struct inode *dir, struct buffer_head *bh, u32 off,
			 const struct qstr *name)
{
	struct ocsfs_disk_dirent *de = (struct ocsfs_disk_dirent *)(bh->b_data + off);
	struct ocsfs_txn *txn;
	int ret;

	txn = ocsfs_txn_begin(dir->i_sb);
	if (IS_ERR(txn)) { brelse(bh); return PTR_ERR(txn); }
	ret = ocsfs_txn_add_bh(txn, bh);
	if (ret) { ocsfs_txn_abort(txn); brelse(bh); return ret; }
	de->de_name_len = 0;
	de->de_ino      = 0;
	de->de_magic    = 0;
	brelse(bh);
	ret = ocsfs_txn_commit(txn);
	if (ret)
		return ret;
	OCSFS_I(dir)->i_dirent_count--;
	ocsfs_dir_btree_delete(dir, name);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);
	return 0;
}

int __ocsfs_del_dirent(struct inode *dir, const struct qstr *name)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	u64 phys_block;
	u32 phys_off;

	/* Fast path: B+ tree index → O(log N) */
	if (!ocsfs_dir_btree_locate(dir, name, &phys_block, &phys_off)) {
		struct buffer_head *bh;

		if (sbi->s_clustered) {
			bh = sb_getblk(dir->i_sb, phys_block);
			if (!bh)
				return -EIO;
			clear_buffer_uptodate(bh);
			if (bh_read(bh, 0) < 0) { brelse(bh); return -EIO; }
		} else {
			bh = sb_bread(dir->i_sb, phys_block);
			if (!bh)
				return -EIO;
		}
		return del_dirent_at(dir, bh, phys_off, name);
	}

	/* Slow path: linear scan (small dir or hash collision fallback) */
	{
		u64 dir_blocks = (dir->i_size + sbi->s_block_size - 1) /
				 sbi->s_block_size;
		u64 b;

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
				return del_dirent_at(dir, bh, off, name);
			}
			brelse(bh);
		}
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

	if (sbi->s_clustered) {
		/* Same coherency flush as ocsfs_add_dirent — see comment there. */
		if (ret == 0) {
			int fr = ocsfs_flush_inode_locked(dir, true);

			if (fr)
				pr_warn_ratelimited(
					"ocsfs: del_dirent inode flush failed (%d)\n",
					fr);
		}
		ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);
	}
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * UPDATE DIRENT — in-place inode number / file-type swap (RENAME_EXCHANGE)
 * ═══════════════════════════════════════════════════════════════ */

int __ocsfs_update_dirent_ino(struct inode *dir, const struct qstr *name,
			      u64 new_ino, u8 new_ft)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(dir->i_sb);
	u64 dir_blocks = (dir->i_size + sbi->s_block_size - 1) /
			 sbi->s_block_size;
	u64 b;

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

			{
				struct ocsfs_txn *txn;
				int tr;

				txn = ocsfs_txn_begin(dir->i_sb);
				if (IS_ERR(txn)) { brelse(bh); return PTR_ERR(txn); }
				tr = ocsfs_txn_add_bh(txn, bh);
				if (tr) { ocsfs_txn_abort(txn); brelse(bh); return tr; }
				de->de_ino       = cpu_to_le64(new_ino);
				de->de_file_type = new_ft;
				brelse(bh);
				tr = ocsfs_txn_commit(txn);
				if (tr) return tr;
			}
			ocsfs_dir_btree_delete(dir, name);
			ocsfs_dir_btree_insert(dir, name, new_ino, new_ft);
			inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
			mark_inode_dirty(dir);
			return 0;
		}
		brelse(bh);
	}
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

	if (sbi->s_clustered) {
		if (ocsfs_lock_acquire(dir->i_sb, &dir_oi->i_lock_res,
				       OCSFS_LOCK_SH))
			return 0; /* conservative: lock failed → assume not empty */
		if (ocsfs_inode_refresh(dir)) {
			ocsfs_lock_release(dir->i_sb, &dir_oi->i_lock_res);
			return 0;
		}
	}

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


