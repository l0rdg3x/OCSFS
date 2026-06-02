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

/* Share src's extents overlapping [src_lblk, src_lblk+nblk) into dst at
 * dst_lblk: bump refcount on each shared phys sub-range, insert a SHARED extent
 * into dst, and flag the source extent SHARED. Callers hold both i_meta_locks. */
static int reflink_share_extents(struct inode *src, u64 src_lblk,
				 struct inode *dst, u64 dst_lblk, u64 nblk)
{
	struct super_block *sb = src->i_sb;
	u32 spb = sb->s_blocksize / 512;
	u64 src_end = src_lblk + nblk;
	u64 cur = src_lblk;
	int ret;

	/* Walk the source range by LOGICAL position via ocsfs2_extent_find: this
	 * re-reads the map each step, so it is robust to the destination inserts
	 * shifting the array when src == dst (same-file clone — generic prep
	 * forbids overlapping src/dst ranges), and it works for both the inline
	 * and the B+tree extent maps (the old index walk handled only inline). */
	while (cur < src_end) {
		struct ocsfs2_extent e;
		u64 next = U64_MAX, ov_e, sphys, dlogical;
		u32 ov_len;

		ret = ocsfs2_extent_find(src, cur, &e, &next);
		if (ret) {                       /* hole in the source: skip it */
			if (next == U64_MAX || next >= src_end)
				break;
			cur = next;
			continue;
		}
		ov_e = min(e.logical + e.length, src_end);
		ov_len = (u32)(ov_e - cur);
		sphys = e.physical + (cur - e.logical);
		dlogical = dst_lblk + (cur - src_lblk);

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
		/* flag the whole source extent shared so a later write CoWs it
		 * (exact-match update; the insert above never splits it as the
		 * src and dst logical ranges are disjoint) */
		ocsfs2_extent_update_phys(src, e.logical, e.length, e.physical,
					  e.flags | OCSFS2_EXT_SHARED);
		cur = ov_e;
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

	ret = ocsfs2_extent_punch_range(dst, dst_lblk, dst_lblk + nblk);
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

	/* Drop dst's now-stale cached pages over the CLONED RANGE ONLY so reads
	 * hit the shared blocks. Must NOT be truncate_inode_pages(.., doff) (to
	 * EOF): that would discard a dirty folio *outside* the clone range whose
	 * data generic_remap_file_range_prep never flushed (it only flushes the
	 * src/dst ranges), silently losing that write — caught by fsx. */
	truncate_inode_pages_range(dst->i_mapping, doff, doff + len - 1);
	mark_inode_dirty(dst);
	mark_inode_dirty(src);
	return len;
}

/* Handles both FICLONE/copy_file_range (clone) and FIDEDUPERANGE (dedup).
 * For dedup the VFS path (vfs_dedupe_file_range_one) does NOT lock the inodes —
 * we do, as for clone — and generic_remap_file_range_prep (called inside
 * ocsfs2_reflink_range) performs the mandatory byte-for-byte compare of the two
 * ranges under those locks, only then letting us share the storage. So dedup is
 * exactly "share-if-identical": reuse the refcount/CoW path, the content compare
 * is the only extra step and the VFS gives it to us for free. */
loff_t ocsfs2_remap_file_range(struct file *src_file, loff_t src_off,
			       struct file *dst_file, loff_t dst_off,
			       loff_t len, unsigned int remap_flags)
{
	struct inode *src = file_inode(src_file);
	struct inode *dst = file_inode(dst_file);
	loff_t ret;

	if (remap_flags & ~(REMAP_FILE_DEDUP | REMAP_FILE_CAN_SHORTEN))
		return -EINVAL;               /* unknown remap flag */
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
	case OCSFS_IOC_GROWFS:              /* D2: force an autogrow check now */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		return ocsfs2_grow_check(file_inode(file)->i_sb, true);
	case OCSFS_IOC_SCRUB: {            /* D5: online metadata scrub */
		struct ocsfs2_scrub_result res;
		int ret;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		ret = ocsfs2_scrub(file_inode(file)->i_sb, &res);
		if (ret && ret != -EUCLEAN)
			return ret;             /* hard error; not a scrub finding */
		if (copy_to_user((void __user *)arg, &res, sizeof(res)))
			return -EFAULT;
		return 0;                       /* findings are reported in res.errors */
	}
	case FITRIM: {                      /* D4: thin-reclaim via SCSI UNMAP */
		struct super_block *sb = file_inode(file)->i_sb;
		struct fstrim_range range;
		int ret;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (copy_from_user(&range, (void __user *)arg, sizeof(range)))
			return -EFAULT;
		ret = ocsfs2_fitrim(sb, &range);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &range, sizeof(range)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}
