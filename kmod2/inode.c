// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — inode.c
 * On-disk inode <-> VFS inode, inode-number allocation, inline extent map.
 * Single-node (Plan 1): no DLM, no cluster refresh. In-memory state is
 * authoritative; the on-disk inode is read once at iget and written back on
 * flush.
 */
#include "ocsfs.h"
#include <linux/writeback.h>
#include <linux/mpage.h>
#include <linux/time.h>

struct kmem_cache *ocsfs2_inode_cachep;

/* ── inode slab ── */

struct inode *ocsfs2_alloc_inode(struct super_block *sb)
{
	struct ocsfs2_inode_info *oi;

	oi = alloc_inode_sb(sb, ocsfs2_inode_cachep, GFP_KERNEL);
	if (!oi)
		return NULL;
	mutex_init(&oi->i_meta_lock);
	oi->i_disk_ino = 0;
	oi->i_ag = 0;
	oi->i_flags = 0;
	oi->i_generation = 0;
	oi->i_extent_count = 0;
	oi->i_extent_tree_root = 0;
	oi->i_dir_btree_root = 0;
	oi->i_dirent_count = 0;
	return &oi->vfs_inode;
}

void ocsfs2_free_in_core_inode(struct inode *inode)
{
	kmem_cache_free(ocsfs2_inode_cachep, OCSFS2_I(inode));
}

/* ── on-disk inode I/O ── */

/* Read the 512-byte on-disk inode for @ino into @di. */
static int read_disk_inode(struct super_block *sb, u64 ino,
			   struct ocsfs2_disk_inode *di)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u64 byte_off = ocsfs2_inode_disk_off(sbi, ino);
	u64 blk;
	u32 off_in_blk;
	struct buffer_head *bh;

	if (!byte_off)
		return -EINVAL;
	blk = byte_off / sb->s_blocksize;
	off_in_blk = byte_off % sb->s_blocksize;

	bh = sb_bread(sb, blk);
	if (!bh)
		return -EIO;
	memcpy(di, bh->b_data + off_in_blk, sizeof(*di));
	brelse(bh);
	return 0;
}

static int validate_disk_inode(const struct ocsfs2_disk_inode *di, u64 ino)
{
	u32 crc;

	if (le32_to_cpu(di->i_magic) != OCSFS2_INODE_MAGIC)
		return -EINVAL;
	crc = ocsfs2_crc32c(~0U, di,
			    offsetof(struct ocsfs2_disk_inode, i_checksum));
	if (crc != le32_to_cpu(di->i_checksum))
		return -EUCLEAN;
	if (le64_to_cpu(di->i_ino) != ino)
		return -EUCLEAN;
	return 0;
}

static void parse_extents(struct ocsfs2_inode_info *oi,
			  const struct ocsfs2_disk_inode *di)
{
	u16 n = le16_to_cpu(di->i_extent_count);
	const struct ocsfs2_disk_extent *de =
		(const struct ocsfs2_disk_extent *)di->i_inline_extents;
	u16 i;

	if (n > OCSFS2_INLINE_EXTENTS)
		n = OCSFS2_INLINE_EXTENTS;
	for (i = 0; i < n; i++) {
		oi->i_extents[i].logical  = le64_to_cpu(de[i].e_logical);
		oi->i_extents[i].physical = le64_to_cpu(de[i].e_physical);
		oi->i_extents[i].length   = le32_to_cpu(de[i].e_length);
		oi->i_extents[i].flags    = le16_to_cpu(de[i].e_flags);
	}
	oi->i_extent_count = n;
}

/* Serialize the in-core inode into a 512-byte on-disk inode. */
static void fill_disk_inode(struct inode *inode, struct ocsfs2_disk_inode *di)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	struct ocsfs2_disk_extent *de =
		(struct ocsfs2_disk_extent *)di->i_inline_extents;
	struct timespec64 ts;
	u16 i;

	memset(di, 0, sizeof(*di));
	di->i_magic = cpu_to_le32(OCSFS2_INODE_MAGIC);
	di->i_generation = cpu_to_le32(oi->i_generation);
	di->i_ino = cpu_to_le64(oi->i_disk_ino);
	di->i_mode = cpu_to_le16(inode->i_mode);
	di->i_nlink = cpu_to_le16(inode->i_nlink);
	di->i_uid = cpu_to_le32(i_uid_read(inode));
	di->i_gid = cpu_to_le32(i_gid_read(inode));
	di->i_size = cpu_to_le64(inode->i_size);
	di->i_blocks = cpu_to_le64(inode->i_blocks);  /* 512-sector units */
	ts = inode_get_atime(inode);
	di->i_atime = cpu_to_le64(timespec64_to_ns(&ts));
	ts = inode_get_mtime(inode);
	di->i_mtime = cpu_to_le64(timespec64_to_ns(&ts));
	ts = inode_get_ctime(inode);
	di->i_ctime = cpu_to_le64(timespec64_to_ns(&ts));
	di->i_flags = cpu_to_le32(oi->i_flags);
	di->i_rdev = cpu_to_le32(new_encode_dev(inode->i_rdev));
	di->i_extent_tree_root = cpu_to_le64(oi->i_extent_tree_root);
	di->i_dir_btree_root = cpu_to_le64(oi->i_dir_btree_root);
	di->i_dirent_count = cpu_to_le32(oi->i_dirent_count);

	di->i_extent_count = cpu_to_le16(oi->i_extent_count);
	for (i = 0; i < oi->i_extent_count && i < OCSFS2_INLINE_EXTENTS; i++) {
		de[i].e_logical  = cpu_to_le64(oi->i_extents[i].logical);
		de[i].e_physical = cpu_to_le64(oi->i_extents[i].physical);
		de[i].e_length   = cpu_to_le32(oi->i_extents[i].length);
		de[i].e_flags    = cpu_to_le16(oi->i_extents[i].flags);
	}
	di->i_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, di,
				offsetof(struct ocsfs2_disk_inode, i_checksum)));
}

/* Read-modify-write the 512-byte inode slot inside its 4 KiB block. */
int ocsfs2_write_inode_block(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u64 byte_off = ocsfs2_inode_disk_off(sbi, OCSFS2_I(inode)->i_disk_ino);
	u64 blk;
	u32 off_in_blk;
	struct buffer_head *bh;

	if (!byte_off)
		return -EINVAL;
	blk = byte_off / sb->s_blocksize;
	off_in_blk = byte_off % sb->s_blocksize;

	bh = sb_bread(sb, blk);
	if (!bh)
		return -EIO;
	fill_disk_inode(inode, (struct ocsfs2_disk_inode *)(bh->b_data + off_in_blk));
	mark_buffer_dirty(bh);
	if (sb->s_flags & SB_SYNCHRONOUS || (inode_state_read(inode) & I_DIRTY_SYNC))
		sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

int ocsfs2_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	int ret = ocsfs2_write_inode_block(inode);

	if (!ret && wbc->sync_mode == WB_SYNC_ALL) {
		struct super_block *sb = inode->i_sb;
		u64 byte_off = ocsfs2_inode_disk_off(OCSFS2_SB(sb),
						     OCSFS2_I(inode)->i_disk_ino);
		struct buffer_head *bh = sb_bread(sb, byte_off / sb->s_blocksize);

		if (bh) {
			sync_dirty_buffer(bh);
			brelse(bh);
		}
	}
	return ret;
}

/* ── iget ── */

struct inode *ocsfs2_iget(struct super_block *sb, u64 ino)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct inode *inode;
	struct ocsfs2_inode_info *oi;
	struct ocsfs2_disk_inode di;
	int ret;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode_state_read(inode) & I_NEW))
		return inode;

	oi = OCSFS2_I(inode);
	oi->i_disk_ino = ino;

	ret = read_disk_inode(sb, ino, &di);
	if (!ret)
		ret = validate_disk_inode(&di, ino);
	if (ret) {
		iget_failed(inode);
		return ERR_PTR(ret);
	}

	inode->i_mode = le16_to_cpu(di.i_mode);
	set_nlink(inode, le16_to_cpu(di.i_nlink));
	i_uid_write(inode, le32_to_cpu(di.i_uid));
	i_gid_write(inode, le32_to_cpu(di.i_gid));
	inode->i_size = le64_to_cpu(di.i_size);
	inode->i_blocks = le64_to_cpu(di.i_blocks);
	inode->i_generation = le32_to_cpu(di.i_generation);
	inode_set_atime_to_ts(inode, ns_to_timespec64(le64_to_cpu(di.i_atime)));
	inode_set_mtime_to_ts(inode, ns_to_timespec64(le64_to_cpu(di.i_mtime)));
	inode_set_ctime_to_ts(inode, ns_to_timespec64(le64_to_cpu(di.i_ctime)));
	oi->i_flags = le32_to_cpu(di.i_flags);
	oi->i_generation = le32_to_cpu(di.i_generation);
	oi->i_ag = ocsfs2_ino_to_ag(sbi, ino);
	oi->i_extent_tree_root = le64_to_cpu(di.i_extent_tree_root);
	oi->i_dir_btree_root = le64_to_cpu(di.i_dir_btree_root);
	oi->i_dirent_count = le32_to_cpu(di.i_dirent_count);
	parse_extents(oi, &di);

	if (inode->i_size > sb->s_maxbytes) {
		pr_err_ratelimited("ocsfs2: inode %llu: i_size too large\n", ino);
		iget_failed(inode);
		return ERR_PTR(-EUCLEAN);
	}

	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &ocsfs2_file_iops;
		inode->i_fop = &ocsfs2_file_fops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &ocsfs2_dir_iops;
		inode->i_fop = &ocsfs2_dir_fops;
	} else {
		inode->i_op = &ocsfs2_special_iops;
		init_special_inode(inode, inode->i_mode,
				   new_decode_dev(le32_to_cpu(di.i_rdev)));
	}

	unlock_new_inode(inode);
	return inode;
}

/* ── evict ── */

void ocsfs2_evict_inode(struct inode *inode)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	bool freeing = (inode->i_nlink == 0);
	u16 i;

	truncate_inode_pages_final(&inode->i_data);

	if (freeing && !is_bad_inode(inode)) {
		/* free data blocks held by inline extents, then the inode number */
		for (i = 0; i < oi->i_extent_count; i++)
			ocsfs2_free_blocks(inode->i_sb, oi->i_extents[i].physical,
					   oi->i_extents[i].length);
		oi->i_extent_count = 0;
	}

	clear_inode(inode);

	if (freeing && !is_bad_inode(inode))
		ocsfs2_free_inode_num(inode->i_sb, oi->i_disk_ino);
}

/* ── inode-number allocation ──
 * "used" = on-disk slot has i_magic == OCSFS2_INODE_MAGIC. Allocation reserves
 * a slot atomically under ag_lock by writing a magic-only stub; the caller
 * fills it via new_inode + flush. Freeing zeroes the slot's magic.
 */
static int reserve_inode_slot(struct super_block *sb, u32 ag, u64 local)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u64 byte_off = sbi->s_ags[ag].inode_table_off + local * OCSFS2_INODE_SIZE;
	u64 blk = byte_off / sb->s_blocksize;
	u32 off = byte_off % sb->s_blocksize;
	struct ocsfs2_disk_inode *di;
	struct buffer_head *bh;

	bh = sb_bread(sb, blk);
	if (!bh)
		return -EIO;
	di = (struct ocsfs2_disk_inode *)(bh->b_data + off);
	memset(di, 0, sizeof(*di));
	di->i_magic = cpu_to_le32(OCSFS2_INODE_MAGIC);
	di->i_ino = cpu_to_le64((u64)ag * sbi->s_ag_size + local);
	di->i_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, di,
				offsetof(struct ocsfs2_disk_inode, i_checksum)));
	mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

static int slot_is_free(struct super_block *sb, u32 ag, u64 local, bool *freep)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u64 byte_off = sbi->s_ags[ag].inode_table_off + local * OCSFS2_INODE_SIZE;
	u64 blk = byte_off / sb->s_blocksize;
	u32 off = byte_off % sb->s_blocksize;
	struct buffer_head *bh;
	struct ocsfs2_disk_inode *di;

	bh = sb_bread(sb, blk);
	if (!bh)
		return -EIO;
	di = (struct ocsfs2_disk_inode *)(bh->b_data + off);
	*freep = (le32_to_cpu(di->i_magic) != OCSFS2_INODE_MAGIC);
	brelse(bh);
	return 0;
}

int ocsfs2_alloc_inode_num(struct super_block *sb, u32 ag_hint, u64 *ino_out)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 tried;

	for (tried = 0; tried < sbi->s_ag_count; tried++) {
		u32 ag = (ag_hint + tried) % sbi->s_ag_count;
		struct ocsfs2_ag_info *ai = &sbi->s_ags[ag];
		u64 start, local;

		mutex_lock(&ai->ag_lock);
		if (ai->free_inodes == 0) {
			mutex_unlock(&ai->ag_lock);
			continue;
		}
		start = ai->next_ino_hint;
		if (ag == 0 && start < OCSFS2_FIRST_USER_INO)
			start = OCSFS2_FIRST_USER_INO;

		for (local = start; local < ai->inodes_per_ag; local++) {
			bool freep;
			int ret = slot_is_free(sb, ag, local, &freep);

			if (ret) { mutex_unlock(&ai->ag_lock); return ret; }
			if (!freep)
				continue;
			ret = reserve_inode_slot(sb, ag, local);
			if (ret) { mutex_unlock(&ai->ag_lock); return ret; }
			ai->free_inodes--;
			ai->next_ino_hint = local + 1;
			mutex_unlock(&ai->ag_lock);
			spin_lock(&sbi->s_free_lock);
			if (sbi->s_free_inodes)
				sbi->s_free_inodes--;
			spin_unlock(&sbi->s_free_lock);
			*ino_out = (u64)ag * sbi->s_ag_size + local;
			return 0;
		}
		/* hint pointed past the last free slot; rescan from the base */
		ai->next_ino_hint = (ag == 0) ? OCSFS2_FIRST_USER_INO : 0;
		mutex_unlock(&ai->ag_lock);
	}
	return -ENOSPC;
}

void ocsfs2_free_inode_num(struct super_block *sb, u64 ino)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	u32 ag = ocsfs2_ino_to_ag(sbi, ino);
	u64 local = ino % sbi->s_ag_size;
	struct ocsfs2_ag_info *ai;
	u64 byte_off, blk;
	u32 off;
	struct buffer_head *bh;

	if (ag >= sbi->s_ag_count)
		return;
	ai = &sbi->s_ags[ag];
	byte_off = ai->inode_table_off + local * OCSFS2_INODE_SIZE;
	blk = byte_off / sb->s_blocksize;
	off = byte_off % sb->s_blocksize;

	mutex_lock(&ai->ag_lock);
	bh = sb_bread(sb, blk);
	if (bh) {
		memset(bh->b_data + off, 0, OCSFS2_INODE_SIZE);
		mark_buffer_dirty(bh);
		brelse(bh);
	}
	ai->free_inodes++;
	if (local < ai->next_ino_hint)
		ai->next_ino_hint = local;
	mutex_unlock(&ai->ag_lock);
	spin_lock(&sbi->s_free_lock);
	sbi->s_free_inodes++;
	spin_unlock(&sbi->s_free_lock);
}

/* ── new inode ── */

struct inode *ocsfs2_new_inode(struct mnt_idmap *idmap, struct inode *dir,
			       umode_t mode, dev_t rdev)
{
	struct super_block *sb = dir->i_sb;
	struct ocsfs2_inode_info *oi;
	struct inode *inode;
	u64 ino;
	int ret;

	ret = ocsfs2_alloc_inode_num(sb, OCSFS2_I(dir)->i_ag, &ino);
	if (ret)
		return ERR_PTR(ret);

	inode = new_inode(sb);
	if (!inode) {
		ocsfs2_free_inode_num(sb, ino);
		return ERR_PTR(-ENOMEM);
	}
	inode->i_ino = ino;
	inode_init_owner(idmap, inode, dir, mode);
	simple_inode_init_ts(inode);
	inode->i_blocks = 0;
	inode->i_generation = 1;

	oi = OCSFS2_I(inode);
	oi->i_disk_ino = ino;
	oi->i_ag = ocsfs2_ino_to_ag(OCSFS2_SB(sb), ino);
	oi->i_generation = 1;
	oi->i_extent_count = 0;
	oi->i_extent_tree_root = 0;
	oi->i_dir_btree_root = 0;
	oi->i_dirent_count = 0;

	if (S_ISREG(mode)) {
		inode->i_op = &ocsfs2_file_iops;
		inode->i_fop = &ocsfs2_file_fops;
	} else if (S_ISDIR(mode)) {
		inode->i_op = &ocsfs2_dir_iops;
		inode->i_fop = &ocsfs2_dir_fops;
	} else {
		inode->i_op = &ocsfs2_special_iops;
		init_special_inode(inode, mode, rdev);
	}

	insert_inode_hash(inode);
	mark_inode_dirty(inode);
	return inode;
}

/* ── inline extent map ── */

int ocsfs2_bmap(struct inode *inode, u64 logical_block, u64 *phys_out)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	u16 i;

	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs2_extent *e = &oi->i_extents[i];

		if (logical_block >= e->logical &&
		    logical_block < e->logical + e->length) {
			*phys_out = e->physical + (logical_block - e->logical);
			return 0;
		}
	}
	return -ENOENT;
}

/* Allocate one block and append it as the next logical block of @inode.
 * Extends the last extent if physically contiguous, else adds a new one.
 * Returns the physical block in *phys_out. Caller holds i_meta_lock. */
int ocsfs2_inode_append_block(struct inode *inode, u64 *phys_out)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	struct super_block *sb = inode->i_sb;
	u64 next_logical = inode->i_size / sb->s_blocksize;
	u64 phys;
	int ret;

	ret = ocsfs2_alloc_blocks(sb, oi->i_ag, 1, &phys);
	if (ret)
		return ret;

	if (oi->i_extent_count > 0) {
		struct ocsfs2_extent *last = &oi->i_extents[oi->i_extent_count - 1];

		if (last->logical + last->length == next_logical &&
		    last->physical + last->length == phys) {
			last->length++;
			goto done;
		}
	}
	if (oi->i_extent_count >= OCSFS2_INLINE_EXTENTS) {
		/* Plan 1: inline extents only. Extent B+tree spill is Plan 2. */
		ocsfs2_free_blocks(sb, phys, 1);
		return -ENOSPC;
	}
	oi->i_extents[oi->i_extent_count].logical = next_logical;
	oi->i_extents[oi->i_extent_count].physical = phys;
	oi->i_extents[oi->i_extent_count].length = 1;
	oi->i_extents[oi->i_extent_count].flags = OCSFS2_EXT_WRITTEN;
	oi->i_extent_count++;
done:
	inode->i_blocks += sb->s_blocksize / 512;
	*phys_out = phys;
	return 0;
}

/* ── attrs ── */

int ocsfs2_getattr(struct mnt_idmap *idmap, const struct path *path,
		   struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);

	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

int ocsfs2_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		   struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int ret;

	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		return ret;

	if ((attr->ia_valid & ATTR_SIZE) && attr->ia_size != inode->i_size) {
		/* Plan 1: no file data path yet; only metadata size tracking. */
		truncate_setsize(inode, attr->ia_size);
	}
	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

/* ── operation tables ── */

const struct inode_operations ocsfs2_file_iops = {
	.setattr = ocsfs2_setattr,
	.getattr = ocsfs2_getattr,
};

const struct inode_operations ocsfs2_special_iops = {
	.setattr = ocsfs2_setattr,
	.getattr = ocsfs2_getattr,
};

/* Plan 1: regular files have no data path. Allow open/seek; read/write of
 * content arrives in Plan 2. */
const struct file_operations ocsfs2_file_fops = {
	.llseek = generic_file_llseek,
};
