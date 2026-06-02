// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — dir.c
 * Directory namespace: fixed-stride 512-byte dirents in the directory's data
 * blocks (8 per 4 KiB block), plus the VFS dir/file operation tables.
 * Single-node (Plan 1): the VFS i_rwsem on the directory serialises mutations.
 */
#include "ocsfs.h"
#include <linux/fs.h>

/* Read directory logical block @lblk; returns a held buffer_head or NULL. */
static struct buffer_head *dir_get_block(struct inode *dir, u64 lblk)
{
	u64 phys;

	if (ocsfs2_bmap(dir, lblk, &phys))
		return NULL;
	return ocsfs2_meta_bread(dir->i_sb, phys);   /* fresh on a clustered volume */
}

static struct ocsfs2_disk_dirent *slot_ptr(struct buffer_head *bh, unsigned s)
{
	return (struct ocsfs2_disk_dirent *)(bh->b_data + s * OCSFS2_DIRENT_SIZE);
}

static bool name_eq(const struct ocsfs2_disk_dirent *de, const struct qstr *n)
{
	return de->de_name_len == n->len &&
	       !memcmp(de->de_name, n->name, n->len);
}

/* ── lookup primitive ── */

u64 ocsfs2_find_dirent(struct inode *dir, const struct qstr *name, u8 *ft_out)
{
	u64 nblocks = dir->i_size / OCSFS2_BLOCK_SIZE;
	u64 l;

	for (l = 0; l < nblocks; l++) {
		struct buffer_head *bh = dir_get_block(dir, l);
		unsigned s;

		if (!bh)
			continue;
		for (s = 0; s < OCSFS2_DIRENTS_PER_BLOCK; s++) {
			struct ocsfs2_disk_dirent *de = slot_ptr(bh, s);

			if (le32_to_cpu(de->de_magic) != OCSFS2_DIRENT_MAGIC)
				continue;
			if (!ocsfs2_dirent_csum_ok(de)) {
				pr_warn_ratelimited("ocsfs2: dir %llu: bad dirent csum, skipping\n",
						    OCSFS2_I(dir)->i_disk_ino);
				continue;
			}
			if (name_eq(de, name)) {
				u64 ino = le64_to_cpu(de->de_ino);

				if (ft_out)
					*ft_out = de->de_file_type;
				brelse(bh);
				return ino;
			}
		}
		brelse(bh);
	}
	return 0;
}

/* ── add ── */

static void write_dirent(struct ocsfs2_disk_dirent *de, const struct qstr *name,
			 u64 ino, u8 ft)
{
	memset(de, 0, OCSFS2_DIRENT_SIZE);
	de->de_magic = cpu_to_le32(OCSFS2_DIRENT_MAGIC);
	de->de_ino = cpu_to_le64(ino);
	de->de_name_hash = cpu_to_le64(ocsfs2_name_hash(name->name, name->len));
	de->de_file_type = ft;
	de->de_name_len = (u8)name->len;
	memcpy(de->de_name, name->name, name->len);
	ocsfs2_dirent_set_csum(de);
}

int ocsfs2_add_dirent(struct inode *dir, const struct qstr *name,
		      u64 ino, u8 ft)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(dir);
	struct super_block *sb = dir->i_sb;
	struct buffer_head *bh;
	struct timespec64 now;
	u64 nblocks, l, phys;
	unsigned s;
	int ret;

	if (name->len == 0 || name->len > OCSFS2_MAX_NAME)
		return -ENAMETOOLONG;

	mutex_lock(&oi->i_meta_lock);
	nblocks = dir->i_size / OCSFS2_BLOCK_SIZE;

	/* find a free slot in an existing block */
	for (l = 0; l < nblocks; l++) {
		bh = dir_get_block(dir, l);
		if (!bh) { ret = -EIO; goto out; }
		for (s = 0; s < OCSFS2_DIRENTS_PER_BLOCK; s++) {
			struct ocsfs2_disk_dirent *de = slot_ptr(bh, s);

			if (le32_to_cpu(de->de_magic) == OCSFS2_DIRENT_MAGIC)
				continue;
			ret = ocsfs2_jbuf(bh);
			if (ret) { brelse(bh); goto out; }
			write_dirent(de, name, ino, ft);
			mark_buffer_dirty(bh);
			brelse(bh);
			goto added;
		}
		brelse(bh);
	}

	/* no free slot: append a new directory block */
	ret = ocsfs2_inode_append_block(dir, &phys);
	if (ret)
		goto out;
	dir->i_size += OCSFS2_BLOCK_SIZE;
	bh = sb_bread(sb, phys);
	if (!bh) { ret = -EIO; goto out; }
	ret = ocsfs2_jbuf(bh);
	if (ret) { brelse(bh); goto out; }
	memset(bh->b_data, 0, OCSFS2_BLOCK_SIZE);
	write_dirent(slot_ptr(bh, 0), name, ino, ft);
	mark_buffer_dirty(bh);
	brelse(bh);

added:
	oi->i_dirent_count++;
	now = current_time(dir);
	inode_set_mtime_to_ts(dir, now);
	inode_set_ctime_to_ts(dir, now);
	mark_inode_dirty(dir);
	ret = 0;
out:
	mutex_unlock(&oi->i_meta_lock);
	return ret;
}

/* ── delete ── */

int ocsfs2_del_dirent(struct inode *dir, const struct qstr *name)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(dir);
	u64 nblocks, l;
	unsigned s;
	int ret = -ENOENT;

	mutex_lock(&oi->i_meta_lock);
	nblocks = dir->i_size / OCSFS2_BLOCK_SIZE;
	for (l = 0; l < nblocks; l++) {
		struct buffer_head *bh = dir_get_block(dir, l);

		if (!bh)
			continue;
		for (s = 0; s < OCSFS2_DIRENTS_PER_BLOCK; s++) {
			struct ocsfs2_disk_dirent *de = slot_ptr(bh, s);

			if (le32_to_cpu(de->de_magic) != OCSFS2_DIRENT_MAGIC)
				continue;
			if (!name_eq(de, name))
				continue;
			if (ocsfs2_jbuf(bh)) { brelse(bh); ret = -ENOMEM; goto out; }
			memset(de, 0, OCSFS2_DIRENT_SIZE);
			mark_buffer_dirty(bh);
			brelse(bh);
			if (oi->i_dirent_count)
				oi->i_dirent_count--;
			inode_set_mtime_to_ts(dir, current_time(dir));
			inode_set_ctime_current(dir);
			mark_inode_dirty(dir);
			ret = 0;
			goto out;
		}
		brelse(bh);
	}
out:
	mutex_unlock(&oi->i_meta_lock);
	return ret;
}

/* Returns 1 if @dir contains only "." and "..", 0 otherwise. */
int ocsfs2_empty_dir(struct inode *dir)
{
	u64 nblocks = dir->i_size / OCSFS2_BLOCK_SIZE;
	u64 l;
	int ret = 1;

	for (l = 0; l < nblocks && ret; l++) {
		struct buffer_head *bh = dir_get_block(dir, l);
		unsigned s;

		if (!bh)
			continue;
		for (s = 0; s < OCSFS2_DIRENTS_PER_BLOCK; s++) {
			struct ocsfs2_disk_dirent *de = slot_ptr(bh, s);
			u8 nl = de->de_name_len;

			if (le32_to_cpu(de->de_magic) != OCSFS2_DIRENT_MAGIC)
				continue;
			if (nl == 1 && de->de_name[0] == '.')
				continue;
			if (nl == 2 && de->de_name[0] == '.' && de->de_name[1] == '.')
				continue;
			ret = 0;
			break;
		}
		brelse(bh);
	}
	return ret;
}

/* Populate a freshly-created directory with "." and "..". */
int ocsfs2_init_empty_dir(struct inode *dir, struct inode *parent)
{
	int ret;

	ret = ocsfs2_add_dirent(dir, &(struct qstr)QSTR_INIT(".", 1),
				OCSFS2_I(dir)->i_disk_ino, OCSFS2_FT_DIR);
	if (ret)
		return ret;
	return ocsfs2_add_dirent(dir, &(struct qstr)QSTR_INIT("..", 2),
				 OCSFS2_I(parent)->i_disk_ino, OCSFS2_FT_DIR);
}

/* ── VFS operations ── */

static struct dentry *ocsfs2_lookup(struct inode *dir, struct dentry *dentry,
				    unsigned int flags)
{
	struct inode *inode = NULL;
	u64 ino;

	if (dentry->d_name.len > OCSFS2_MAX_NAME)
		return ERR_PTR(-ENAMETOOLONG);

	ino = ocsfs2_find_dirent(dir, &dentry->d_name, NULL);
	if (ino) {
		inode = ocsfs2_iget(dir->i_sb, ino);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	}
	return d_splice_alias(inode, dentry);
}

/* Shared create path for regular files (create) and special files (mknod):
 * new inode + dirent + dir update, all in one journal transaction. */
static int ocsfs2_mknod_common(struct mnt_idmap *idmap, struct inode *dir,
			       struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct inode *inode;
	struct ocsfs2_txn *txn;
	int ret;

	ocsfs2_meta_lock(dir->i_sb, dir, NULL);   /* serialise + refresh dir (cluster) */
	txn = ocsfs2_txn_begin(dir->i_sb);
	if (!txn) {
		ret = -ENOMEM;
		goto out;
	}
	inode = ocsfs2_new_inode(idmap, dir, mode, rdev);  /* reserve enrols the slot */
	if (IS_ERR(inode)) {
		ocsfs2_txn_abort(txn);
		ret = PTR_ERR(inode);
		goto out;
	}
	ret = ocsfs2_init_acl(inode, dir);   /* inherit default ACL / apply umask */
	if (ret)
		goto fail;
	ret = ocsfs2_write_inode_block(inode);
	if (ret)
		goto fail;
	ret = ocsfs2_add_dirent(dir, &dentry->d_name,
				OCSFS2_I(inode)->i_disk_ino, ocsfs2_mode_to_ft(mode));
	if (ret)
		goto fail;
	ret = ocsfs2_write_inode_block(dir);   /* dir size/mtime/dirent_count, atomically */
	if (ret)
		goto fail;
	ret = ocsfs2_txn_commit(txn);          /* frees txn (success or fail) */
	if (ret)
		goto fail_committed;
	d_instantiate(dentry, inode);
	ret = 0;
	goto out;
fail:
	ocsfs2_txn_abort(txn);
fail_committed:
	inode_dec_link_count(inode);
	discard_new_inode(inode);
out:
	ocsfs2_meta_unlock(dir->i_sb);
	return ret;
}

static int ocsfs2_create(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode, bool excl)
{
	return ocsfs2_mknod_common(idmap, dir, dentry, mode, 0);
}

static int ocsfs2_mknod(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, dev_t rdev)
{
	return ocsfs2_mknod_common(idmap, dir, dentry, mode, rdev);
}

static int ocsfs2_symlink(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	struct ocsfs2_txn *txn;
	int ret, len = strlen(symname);

	if (len + 1 > OCSFS2_SYMLINK_INLINE_MAX)
		return -ENAMETOOLONG;          /* long-symlink (data block) not supported */

	ocsfs2_meta_lock(dir->i_sb, dir, NULL);
	txn = ocsfs2_txn_begin(dir->i_sb);
	if (!txn) {
		ret = -ENOMEM;
		goto out;
	}
	inode = ocsfs2_new_inode(idmap, dir, S_IFLNK | 0777, 0);
	if (IS_ERR(inode)) {
		ocsfs2_txn_abort(txn);
		ret = PTR_ERR(inode);
		goto out;
	}
	memcpy((char *)OCSFS2_I(inode)->i_extents, symname, len + 1);
	inode->i_link = (char *)OCSFS2_I(inode)->i_extents;
	inode->i_size = len;
	ret = ocsfs2_write_inode_block(inode);
	if (ret)
		goto fail;
	ret = ocsfs2_add_dirent(dir, &dentry->d_name,
				OCSFS2_I(inode)->i_disk_ino, OCSFS2_FT_SYMLINK);
	if (ret)
		goto fail;
	ret = ocsfs2_write_inode_block(dir);
	if (ret)
		goto fail;
	ret = ocsfs2_txn_commit(txn);
	if (ret)
		goto fail_committed;
	d_instantiate(dentry, inode);
	ret = 0;
	goto out;
fail:
	ocsfs2_txn_abort(txn);
fail_committed:
	inode_dec_link_count(inode);
	discard_new_inode(inode);
out:
	ocsfs2_meta_unlock(dir->i_sb);
	return ret;
}

static int ocsfs2_link(struct dentry *old_dentry, struct inode *dir,
		       struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	struct ocsfs2_txn *txn;
	int ret;

	ocsfs2_meta_lock(dir->i_sb, dir, NULL);
	txn = ocsfs2_txn_begin(dir->i_sb);
	if (!txn) {
		ret = -ENOMEM;
		goto out;
	}
	inode_set_ctime_current(inode);
	inc_nlink(inode);
	ihold(inode);                          /* d_instantiate takes a ref */
	ret = ocsfs2_add_dirent(dir, &dentry->d_name, inode->i_ino,
				ocsfs2_mode_to_ft(inode->i_mode));
	if (ret)
		goto fail;
	ret = ocsfs2_write_inode_block(inode);   /* nlink++ */
	if (ret)
		goto fail;
	ret = ocsfs2_write_inode_block(dir);
	if (ret)
		goto fail;
	ret = ocsfs2_txn_commit(txn);
	if (ret)
		goto fail_committed;
	d_instantiate(dentry, inode);
	ret = 0;
	goto out;
fail:
	ocsfs2_txn_abort(txn);
fail_committed:
	drop_nlink(inode);
	iput(inode);
out:
	ocsfs2_meta_unlock(dir->i_sb);
	return ret;
}

static struct dentry *ocsfs2_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				   struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	struct ocsfs2_txn *txn;
	int ret;

	ocsfs2_meta_lock(dir->i_sb, dir, NULL);
	txn = ocsfs2_txn_begin(dir->i_sb);
	if (!txn) {
		ret = -ENOMEM;
		goto out;
	}
	inode = ocsfs2_new_inode(idmap, dir, S_IFDIR | mode, 0);
	if (IS_ERR(inode)) {
		ocsfs2_txn_abort(txn);
		ret = PTR_ERR(inode);
		goto out;
	}
	ret = ocsfs2_init_acl(inode, dir);   /* inherit default ACL / apply umask */
	if (ret)
		goto fail;
	ret = ocsfs2_init_empty_dir(inode, dir);
	if (ret)
		goto fail;
	set_nlink(inode, 2);   /* "." + the entry in the parent */
	ret = ocsfs2_write_inode_block(inode);
	if (ret)
		goto fail;
	ret = ocsfs2_add_dirent(dir, &dentry->d_name,
				OCSFS2_I(inode)->i_disk_ino, OCSFS2_FT_DIR);
	if (ret)
		goto fail;
	inc_nlink(dir);        /* ".." in the new child points back at dir */
	ret = ocsfs2_write_inode_block(dir);
	if (ret)
		goto fail;
	ret = ocsfs2_txn_commit(txn);
	if (ret)
		goto fail_committed;
	d_instantiate(dentry, inode);
	ret = 0;
	goto out;
fail:
	ocsfs2_txn_abort(txn);
fail_committed:
	clear_nlink(inode);
	discard_new_inode(inode);
out:
	ocsfs2_meta_unlock(dir->i_sb);
	return ret ? ERR_PTR(ret) : NULL;
}

static int ocsfs2_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct ocsfs2_txn *txn;
	int ret;

	ocsfs2_meta_lock(dir->i_sb, dir, NULL);
	txn = ocsfs2_txn_begin(dir->i_sb);
	if (!txn) {
		ret = -ENOMEM;
		goto out;
	}
	ret = ocsfs2_del_dirent(dir, &dentry->d_name);
	if (ret)
		goto fail;
	inode_set_ctime_to_ts(inode, inode_get_ctime(dir));
	drop_nlink(inode);
	ret = ocsfs2_write_inode_block(inode);   /* nlink-- atomic with dirent removal */
	if (ret)
		goto fail;
	ret = ocsfs2_write_inode_block(dir);
	if (ret)
		goto fail;
	ret = ocsfs2_txn_commit(txn);
	goto out;
fail:
	ocsfs2_txn_abort(txn);
out:
	ocsfs2_meta_unlock(dir->i_sb);
	return ret;
}

static int ocsfs2_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct ocsfs2_txn *txn;
	int ret;

	/* under the meta lease so the emptiness check sees peers' entries */
	ocsfs2_meta_lock(dir->i_sb, dir, NULL);
	ocsfs2_inode_refresh_coherent(inode);   /* the dir being removed, too */
	if (!ocsfs2_empty_dir(inode)) {
		ret = -ENOTEMPTY;
		goto out;
	}
	txn = ocsfs2_txn_begin(dir->i_sb);
	if (!txn) {
		ret = -ENOMEM;
		goto out;
	}
	ret = ocsfs2_del_dirent(dir, &dentry->d_name);
	if (ret)
		goto fail;
	clear_nlink(inode);       /* drops the dir's own "." link to 0 */
	ret = ocsfs2_write_inode_block(inode);
	if (ret)
		goto fail;
	drop_nlink(dir);          /* parent loses the child's ".." backlink */
	ret = ocsfs2_write_inode_block(dir);
	if (ret)
		goto fail;
	ret = ocsfs2_txn_commit(txn);
	goto out;
fail:
	ocsfs2_txn_abort(txn);
out:
	ocsfs2_meta_unlock(dir->i_sb);
	return ret;
}

static int ocsfs2_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	u64 nblocks = dir->i_size / OCSFS2_BLOCK_SIZE;
	u64 total = nblocks * OCSFS2_DIRENTS_PER_BLOCK;

	while (ctx->pos < total) {
		u64 l = ctx->pos / OCSFS2_DIRENTS_PER_BLOCK;
		unsigned s = ctx->pos % OCSFS2_DIRENTS_PER_BLOCK;
		struct buffer_head *bh = dir_get_block(dir, l);
		struct ocsfs2_disk_dirent *de;

		if (!bh) { ctx->pos++; continue; }
		de = slot_ptr(bh, s);
		if (le32_to_cpu(de->de_magic) == OCSFS2_DIRENT_MAGIC &&
		    ocsfs2_dirent_csum_ok(de)) {
			if (!dir_emit(ctx, (char *)de->de_name, de->de_name_len,
				      le64_to_cpu(de->de_ino),
				      ocsfs2_ft_to_dt(de->de_file_type))) {
				brelse(bh);
				return 0;
			}
		}
		brelse(bh);
		ctx->pos++;
	}
	return 0;
}

const struct inode_operations ocsfs2_dir_iops = {
	.lookup  = ocsfs2_lookup,
	.create  = ocsfs2_create,
	.mkdir   = ocsfs2_mkdir,
	.unlink  = ocsfs2_unlink,
	.rmdir   = ocsfs2_rmdir,
	.rename  = ocsfs2_rename,
	.symlink = ocsfs2_symlink,
	.link    = ocsfs2_link,
	.mknod   = ocsfs2_mknod,
	.setattr = ocsfs2_setattr,
	.getattr = ocsfs2_getattr,
	.listxattr = ocsfs2_listxattr,
#ifdef CONFIG_FS_POSIX_ACL
	.get_inode_acl = ocsfs2_get_acl,
	.set_acl       = ocsfs2_set_acl,
#endif
};

const struct file_operations ocsfs2_dir_fops = {
	.llseek         = generic_file_llseek,
	.read           = generic_read_dir,
	.iterate_shared = ocsfs2_readdir,
	.fsync          = noop_fsync,
	.unlocked_ioctl = ocsfs2_ioctl,        /* FITRIM is issued on a dir fd */
};
