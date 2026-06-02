// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — reflink.c
 * FICLONE / copy_file_range extent sharing and the snapshot ioctl (Plan 4).
 *
 * Sharing bumps each source extent's refcount and flags both files' extents
 * SHARED; a later write to a shared block copies-on-write in iomap.c so the
 * sharers stay isolated. All metadata mutations run inside one Plan-3 journal
 * transaction; on failure the txn is aborted and the in-core extent maps are
 * reloaded from disk so memory and disk stay consistent.
 *
 * Single-node: the inode locks (taken by the VFS clone path / the snapshot
 * ioctl) serialise everything; multi-node sharing comes with the cluster plan.
 */
#include "ocsfs.h"
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/uaccess.h>
#include <linux/minmax.h>
#include <linux/pagemap.h>

/* Free (refcount-aware) and remove every dst extent (or part) overlapping
 * [lblk, end). Caller holds dst's i_meta_lock. Returns 0, or -ENOSPC if a
 * mid-extent punch would need more than the inline extent slots. */
static int reflink_punch_dst(struct inode *dst, u64 lblk, u64 end)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(dst);
	struct super_block *sb = dst->i_sb;
	u32 spb = sb->s_blocksize / 512;
	u16 i = 0;

	while (i < oi->i_extent_count) {
		struct ocsfs2_extent *e = &oi->i_extents[i];
		u64 e_end = e->logical + e->length;

		if (e_end <= lblk || e->logical >= end) {
			i++;
			continue;            /* no overlap */
		}
		if (e->logical >= lblk && e_end <= end) {
			/* fully covered: free and remove */
			ocsfs2_free_blocks_rc(sb, e->physical, e->length);
			dst->i_blocks -= (u64)e->length * spb;
			memmove(e, e + 1,
				(oi->i_extent_count - i - 1) * sizeof(*e));
			oi->i_extent_count--;
			continue;            /* shifted next into i */
		}
		if (e->logical < lblk && e_end > end) {
			/* range strictly inside the extent: split into head + tail */
			u32 head = (u32)(lblk - e->logical);
			u32 holelen = (u32)(end - lblk);

			if (oi->i_extent_count >= OCSFS2_INLINE_EXTENTS)
				return -ENOSPC;
			ocsfs2_free_blocks_rc(sb, e->physical + head, holelen);
			dst->i_blocks -= (u64)holelen * spb;
			memmove(e + 1, e, (oi->i_extent_count - i) * sizeof(*e));
			oi->i_extent_count++;
			e[1].logical  = end;
			e[1].physical = e->physical + head + holelen;
			e[1].length   = (u32)(e_end - end);
			/* e[1].flags inherited from e via memmove */
			e->length = head;
			i += 2;
			continue;
		}
		if (e->logical < lblk) {
			/* overlaps the start of [lblk,end): trim the extent tail */
			u32 keep = (u32)(lblk - e->logical);

			ocsfs2_free_blocks_rc(sb, e->physical + keep,
					      e->length - keep);
			dst->i_blocks -= (u64)(e->length - keep) * spb;
			e->length = keep;
			i++;
			continue;
		}
		/* overlaps the end of [lblk,end): trim the extent head */
		{
			u32 cut = (u32)(end - e->logical);

			ocsfs2_free_blocks_rc(sb, e->physical, cut);
			dst->i_blocks -= (u64)cut * spb;
			e->logical = end;
			e->physical += cut;
			e->length -= cut;
			i++;
		}
	}
	return 0;
}

/* Share src's extents overlapping [src_lblk, src_lblk+nblk) into dst at
 * dst_lblk: bump refcount on each shared phys sub-range, insert a SHARED extent
 * into dst, and flag the source extent SHARED. Callers hold both i_meta_locks. */
static int reflink_share_extents(struct inode *src, u64 src_lblk,
				 struct inode *dst, u64 dst_lblk, u64 nblk)
{
	struct ocsfs2_inode_info *soi = OCSFS2_I(src);
	struct super_block *sb = src->i_sb;
	u32 spb = sb->s_blocksize / 512;
	u64 src_end = src_lblk + nblk;
	u16 i;
	int ret;

	for (i = 0; i < soi->i_extent_count; i++) {
		struct ocsfs2_extent *e = &soi->i_extents[i];
		u64 e_end = e->logical + e->length;
		u64 ov_s, ov_e, sphys, dlogical;
		u32 ov_len;

		if (e_end <= src_lblk || e->logical >= src_end)
			continue;
		ov_s = max(e->logical, src_lblk);
		ov_e = min(e_end, src_end);
		ov_len = (u32)(ov_e - ov_s);
		sphys = e->physical + (ov_s - e->logical);
		dlogical = dst_lblk + (ov_s - src_lblk);

		ret = ocsfs2_refcount_inc(sb, sphys, ov_len);
		if (ret)
			return ret;
		ret = ocsfs2_extent_insert(dst, dlogical, sphys, ov_len,
					   OCSFS2_EXT_SHARED);
		if (ret) {
			ocsfs2_free_blocks_rc(sb, sphys, ov_len);  /* undo the inc */
			return ret;
		}
		dst->i_blocks += (u64)ov_len * spb;
		e->flags |= OCSFS2_EXT_SHARED;   /* mark the source extent shared */
	}
	return 0;
}

loff_t ocsfs2_reflink_range(struct file *src_file, loff_t soff,
			    struct file *dst_file, loff_t doff,
			    loff_t len, unsigned int remap_flags)
{
	struct inode *src = file_inode(src_file);
	struct inode *dst = file_inode(dst_file);
	struct super_block *sb = src->i_sb;
	u32 bs = sb->s_blocksize;
	struct ocsfs2_txn *txn;
	struct mutex *m1, *m2;
	u64 src_lblk, dst_lblk, nblk;
	bool committed = false;
	int ret;

	if (src->i_sb != dst->i_sb)
		return -EXDEV;

	ret = generic_remap_file_range_prep(src_file, soff, dst_file, doff,
					    &len, remap_flags);
	if (ret < 0)
		return ret;
	if (len == 0)
		return 0;

	src_lblk = (u64)soff / bs;
	dst_lblk = (u64)doff / bs;
	nblk = ((u64)len + bs - 1) / bs;

	txn = ocsfs2_txn_begin(sb);
	if (!txn)
		return -ENOMEM;

	/* lock both extent maps in a fixed order (one mutex if src == dst) */
	m1 = &OCSFS2_I(src)->i_meta_lock;
	m2 = &OCSFS2_I(dst)->i_meta_lock;
	if (src == dst) {
		mutex_lock(m1);
	} else if (m1 < m2) {
		mutex_lock(m1);
		mutex_lock(m2);
	} else {
		mutex_lock(m2);
		mutex_lock(m1);
	}

	ret = reflink_punch_dst(dst, dst_lblk, dst_lblk + nblk);
	if (!ret)
		ret = reflink_share_extents(src, src_lblk, dst, dst_lblk, nblk);
	if (!ret) {
		if (doff + len > i_size_read(dst))
			i_size_write(dst, doff + len);
		inode_set_mtime_to_ts(dst, inode_set_ctime_current(dst));
		ret = ocsfs2_write_inode_block(src);   /* source SHARED flags */
	}
	if (!ret)
		ret = ocsfs2_write_inode_block(dst);
	if (!ret) {
		ret = ocsfs2_txn_commit(txn);
		committed = true;
	}
	if (!committed)
		ocsfs2_txn_abort(txn);
	if (ret) {
		ocsfs2_reload_extents(src);
		ocsfs2_reload_extents(dst);
	}

	if (src == dst) {
		mutex_unlock(m1);
	} else if (m1 < m2) {
		mutex_unlock(m2);
		mutex_unlock(m1);
	} else {
		mutex_unlock(m1);
		mutex_unlock(m2);
	}

	if (ret)
		return ret;

	/* drop dst's now-stale cached pages so reads hit the shared blocks */
	truncate_inode_pages(dst->i_mapping, doff);
	mark_inode_dirty(dst);
	mark_inode_dirty(src);
	return len;
}

loff_t ocsfs2_remap_file_range(struct file *src_file, loff_t src_off,
			       struct file *dst_file, loff_t dst_off,
			       loff_t len, unsigned int remap_flags)
{
	struct inode *src = file_inode(src_file);
	struct inode *dst = file_inode(dst_file);
	loff_t ret;

	if (remap_flags & REMAP_FILE_DEDUP)
		return -EOPNOTSUPP;            /* dedup is out of Plan-4 scope */
	if (!S_ISREG(src->i_mode) || !S_ISREG(dst->i_mode))
		return -EINVAL;

	lock_two_nondirectories(src, dst);
	ret = ocsfs2_reflink_range(src_file, src_off, dst_file, dst_off, len,
				   remap_flags);
	unlock_two_nondirectories(src, dst);
	return ret;
}

/* ── snapshot ioctl ──
 * Create a new regular file in the source's parent directory that reflinks the
 * entire source: a point-in-time copy that diverges on the next write to
 * either file. */
static int ocsfs2_ioc_snap_create(struct file *src_file, void __user *arg)
{
	struct inode *src = file_inode(src_file);
	struct dentry *src_dentry = file_dentry(src_file);
	struct mnt_idmap *idmap = file_mnt_idmap(src_file);
	struct dentry *parent, *new;
	struct inode *dir;
	struct file *dst_file = NULL;
	char name[OCSFS2_MAX_NAME + 1];
	struct qstr qname;
	int ret, nlen;
	loff_t cloned;

	if (!S_ISREG(src->i_mode))
		return -EINVAL;
	if (copy_from_user(name, arg, OCSFS2_MAX_NAME))
		return -EFAULT;
	name[OCSFS2_MAX_NAME] = '\0';
	nlen = strnlen(name, OCSFS2_MAX_NAME);
	if (nlen == 0 || memchr(name, '/', nlen))
		return -EINVAL;

	ret = mnt_want_write_file(src_file);
	if (ret)
		return ret;

	parent = dget_parent(src_dentry);
	dir = d_inode(parent);
	inode_lock_nested(dir, I_MUTEX_PARENT);

	qname = (struct qstr)QSTR_INIT(name, nlen);
	new = lookup_one(idmap, &qname, parent);
	if (IS_ERR(new)) {
		ret = PTR_ERR(new);
		goto unlock_dir;
	}
	if (d_really_is_positive(new)) {
		ret = -EEXIST;
		goto put_new;
	}

	ret = dir->i_op->create(idmap, dir, new,
				(src->i_mode & 07777) | S_IFREG, true);
	if (ret)
		goto put_new;

	dst_file = dentry_open(&(struct path){ .mnt = src_file->f_path.mnt,
					       .dentry = new },
			       O_WRONLY | O_LARGEFILE, current_cred());
	if (IS_ERR(dst_file)) {
		ret = PTR_ERR(dst_file);
		goto unlink_new;
	}

	lock_two_nondirectories(src, d_inode(new));
	cloned = ocsfs2_reflink_range(src_file, 0, dst_file, 0,
				      i_size_read(src), 0);
	unlock_two_nondirectories(src, d_inode(new));
	fput(dst_file);
	if (cloned < 0) {
		ret = (int)cloned;
		goto unlink_new;
	}
	ret = 0;
	goto put_new;

unlink_new:
	vfs_unlink(idmap, dir, new, NULL);   /* best-effort cleanup of the stub */
put_new:
	dput(new);
unlock_dir:
	inode_unlock(dir);
	dput(parent);
	mnt_drop_write_file(src_file);
	return ret;
}

long ocsfs2_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case OCSFS_IOC_SNAP_CREATE:
		return ocsfs2_ioc_snap_create(file, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}
