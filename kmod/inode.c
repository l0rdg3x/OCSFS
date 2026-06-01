// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — inode.c
 * Inode read/write, allocation, VFS inode operations.
 *
 * Phase 1: single-node, inline extents only.
 */

#include <linux/security.h>
#include <linux/posix_acl.h>
#include <linux/fileattr.h>
#include <linux/quotaops.h>
#include <linux/bio.h>
#include <linux/iomap.h>
#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * READ INODE FROM DISK
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Read one fs block straight from the device into a caller buffer, bypassing
 * the page/buffer cache.  Needed for cross-node inode refresh: an inode block
 * holds 8 inodes, so we must NOT clear_buffer_uptodate() the shared cached
 * buffer — that would corrupt a concurrent local transaction modifying a
 * co-located inode in the same block.  Reading into a private page leaves the
 * shared cache untouched while still returning the peer's latest flushed data.
 */
static int ocsfs_read_block_uncached(struct super_block *sb, u64 block,
				     void *buf)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct bio *bio;
	struct page *page;
	int ret;

	page = alloc_page(GFP_NOFS);
	if (!page)
		return -ENOMEM;

	bio = bio_alloc(sb->s_bdev, 1, REQ_OP_READ, GFP_NOFS);
	if (!bio) {
		__free_page(page);
		return -ENOMEM;
	}
	bio->bi_iter.bi_sector = block * (sbi->s_block_size >> 9);
	__bio_add_page(bio, page, sbi->s_block_size, 0);
	ret = submit_bio_wait(bio);
	bio_put(bio);

	if (ret == 0)
		memcpy(buf, page_address(page), sbi->s_block_size);
	__free_page(page);
	return ret;
}

static int ocsfs_read_disk_inode(struct super_block *sb, u64 ino,
				 struct ocsfs_disk_inode *di, bool uncached)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct buffer_head *bh;
	u64 off = ocsfs_inode_disk_off(sbi, ino);
	u64 block = off / sbi->s_block_size;
	u32 boff = off % sbi->s_block_size;

	if (uncached) {
		/* Cross-node refresh: read the block fresh from disk into a
		 * private buffer (the cached copy may be a stale peer write). */
		void *blk = kmalloc(sbi->s_block_size, GFP_NOFS);
		int ret;

		if (!blk)
			return -ENOMEM;
		ret = ocsfs_read_block_uncached(sb, block, blk);
		if (ret) {
			kfree(blk);
			return -EIO;
		}
		memcpy(di, (u8 *)blk + boff, sizeof(*di));
		kfree(blk);
	} else {
		bh = sb_bread(sb, block);
		if (!bh)
			return -EIO;

		memcpy(di, bh->b_data + boff, sizeof(*di));
		brelse(bh);
	}

	if (le32_to_cpu(di->i_magic) != OCSFS_INODE_MAGIC)
		return -EINVAL;

	if (le32_to_cpu(di->i_checksum) !=
	    ocsfs_crc32c(~0U, di, OCSFS_INODE_SIZE - 4)) {
		pr_err_ratelimited("ocsfs: inode %llu checksum mismatch\n", ino);
		return -EIO;
	}

	return 0;
}

/*
 * Cross-node inode coherence is now handled by reading the inode block fresh
 * from disk into a private buffer (ocsfs_read_block_uncached / the `uncached`
 * arg of ocsfs_read_disk_inode) at DLM-lock acquisition (iget) and refresh
 * time.  That avoids the old clear_buffer_uptodate()+bh_read() approach, which
 * corrupted concurrent local transactions on the other 7 inodes sharing the
 * block.  References to "ocsfs_inode_invalidate_cache" in nearby comments are
 * historical.
 */

/* Parse inline extents from the on-disk inode */
static void ocsfs_parse_extents(struct ocsfs_inode_info *oi,
				struct ocsfs_disk_inode *di)
{
	u16 count = le16_to_cpu(di->i_extent_count);
	u16 i;

	if (count > OCSFS_INLINE_EXTENTS)
		count = OCSFS_INLINE_EXTENTS;

	oi->i_extent_count = count;

	for (i = 0; i < count; i++) {
		struct ocsfs_disk_extent *de =
			(struct ocsfs_disk_extent *)
			(di->i_inline_extents + i * sizeof(*de));
		oi->i_extents[i].logical_block  = le64_to_cpu(de->e_logical_block);
		oi->i_extents[i].physical_block = le64_to_cpu(de->e_physical_block);
		oi->i_extents[i].length         = le32_to_cpu(de->e_length);
		oi->i_extents[i].flags          = le16_to_cpu(de->e_flags);
		/* e_checksum stores physical compressed block count for COMPRESSED extents */
		oi->i_extents[i].phys_length =
			(oi->i_extents[i].flags & OCSFS_EXT_COMPRESSED)
			? le16_to_cpu(de->e_checksum) : 0;
	}
}

/* ═══════════════════════════════════════════════════════════════
 * IGET — read and instantiate a VFS inode
 * ═══════════════════════════════════════════════════════════════ */

struct inode *ocsfs_iget(struct super_block *sb, u64 ino)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct inode *inode;
	struct ocsfs_inode_info *oi;
	struct ocsfs_disk_inode di;
	int ret;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode_state_read(inode) & I_NEW))
		return inode;

	oi = OCSFS_I(inode);
	oi->i_disk_ino = ino;

	/* Initialise the per-inode DLM lock resource. */
	ocsfs_lock_init(&oi->i_lock_res,
			ocsfs_lock_hash_inode(ino), OCSFS_LOCKRES_INODE);

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(sb, &oi->i_lock_res, OCSFS_LOCK_SH);
		if (ret) {
			iget_failed(inode);
			return ERR_PTR(ret);
		}
	}

	/* Cross-node: read the inode fresh from disk when clustered.  A peer may
	 * have just created this inode in a block we cached (8 inodes/block) when
	 * its slot was still empty — the cached copy would miss it (-> EINVAL). */
	ret = ocsfs_read_disk_inode(sb, ino, &di, sbi->s_clustered);
	if (ret) {
		if (sbi->s_clustered)
			ocsfs_lock_release(sb, &oi->i_lock_res);
		iget_failed(inode);
		return ERR_PTR(ret);
	}

	/* Keep SH held across VFS population; released before unlock_new_inode. */
	/* Fill VFS inode from disk data */
	inode->i_mode = le16_to_cpu(di.i_mode);
	set_nlink(inode, le16_to_cpu(di.i_nlink));
	i_uid_write(inode, le32_to_cpu(di.i_uid));
	i_gid_write(inode, le32_to_cpu(di.i_gid));
	inode->i_size = le64_to_cpu(di.i_size);
	inode->i_blocks = le64_to_cpu(di.i_blocks) *
			  (OCSFS_SB(sb)->s_block_size / 512);

	inode_set_atime_to_ts(inode,
		ns_to_timespec64(le64_to_cpu(di.i_atime)));
	inode_set_mtime_to_ts(inode,
		ns_to_timespec64(le64_to_cpu(di.i_mtime)));
	inode_set_ctime_to_ts(inode,
		ns_to_timespec64(le64_to_cpu(di.i_ctime)));

	oi->i_flags = le32_to_cpu(di.i_flags);
	/* Restore VFS immutable/append enforcement from disk */
	if (oi->i_flags & OCSFS_IFLAG_IMMUTABLE)
		inode->i_flags |= S_IMMUTABLE;
	if (oi->i_flags & OCSFS_IFLAG_APPEND)
		inode->i_flags |= S_APPEND;
	oi->i_ag = le32_to_cpu(di.i_ag);
	oi->i_extent_tree_root = le64_to_cpu(di.i_extent_tree_root);

	ocsfs_parse_extents(oi, &di);

	oi->i_dir_btree_root = le64_to_cpu(di.i_dir_btree_root);
	oi->i_dirent_count   = le32_to_cpu(di.i_dirent_count);
	oi->i_xattr_block    = le64_to_cpu(di.i_xattr_block);

	if (oi->i_ag >= sbi->s_ag_count ||
	    (oi->i_extent_tree_root &&
	     oi->i_extent_tree_root >= sbi->s_total_blocks) ||
	    (oi->i_xattr_block && oi->i_xattr_block >= sbi->s_total_blocks)) {
		pr_err_ratelimited("ocsfs: inode %llu: corrupt block pointers\n",
				   ino);
		if (sbi->s_clustered)
			ocsfs_lock_release(sb, &oi->i_lock_res);
		iget_failed(inode);
		return ERR_PTR(-EUCLEAN);
	}

	/* Bound-check i_size and i_dirent_count against impossible values */
	if (inode->i_size > sb->s_maxbytes) {
		pr_err_ratelimited("ocsfs: inode %llu: i_size %llu exceeds maxbytes\n",
				   ino, inode->i_size);
		if (sbi->s_clustered)
			ocsfs_lock_release(sb, &oi->i_lock_res);
		iget_failed(inode);
		return ERR_PTR(-EUCLEAN);
	}
	if (S_ISDIR(inode->i_mode) && inode->i_size > 0 &&
	    oi->i_dirent_count > div64_u64(inode->i_size, OCSFS_DIRENT_SIZE) + 2) {
		pr_err_ratelimited("ocsfs: inode %llu: i_dirent_count %u "
				   "inconsistent with i_size %llu\n",
				   ino, oi->i_dirent_count, inode->i_size);
		if (sbi->s_clustered)
			ocsfs_lock_release(sb, &oi->i_lock_res);
		iget_failed(inode);
		return ERR_PTR(-EUCLEAN);
	}

	/* Set up operations based on file type */
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &ocsfs_file_inode_ops;
		inode->i_fop = &ocsfs_file_fops;
		inode->i_mapping->a_ops = &ocsfs_iomap_aops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &ocsfs_dir_inode_ops;
		inode->i_fop = &ocsfs_dir_fops;
		inode->i_mapping->a_ops = &ocsfs_aops;
	} else if (S_ISLNK(inode->i_mode)) {
		size_t slen = inode->i_size;

		oi->i_symlink = NULL;
		if (slen > 0 && slen <= OCSFS_MAX_INLINE_SYMLINK) {
			oi->i_symlink = kmalloc(slen + 1, GFP_KERNEL);
			if (!oi->i_symlink) {
				if (sbi->s_clustered)
					ocsfs_lock_release(sb, &oi->i_lock_res);
				iget_failed(inode);
				return ERR_PTR(-ENOMEM);
			}
			memcpy(oi->i_symlink, di.i_inline_extents, slen);
			oi->i_symlink[slen] = '\0';
		}
		inode->i_op = &ocsfs_symlink_inode_ops;
	} else {
		inode->i_op = &ocsfs_special_inode_ops;
		init_special_inode(inode, inode->i_mode,
				   inode->i_rdev);
	}

	unlock_new_inode(inode);
	/* Release SH only after unlock_new_inode so no reader sees the inode
	 * before VFS fields are fully initialized (MEDIO-3). */
	if (sbi->s_clustered)
		ocsfs_lock_release(sb, &oi->i_lock_res);
	return inode;
}

/* Re-read inode metadata from disk. Caller holds DLM SH. */
/*
 * Re-read the on-disk inode into the in-core VFS inode.
 *
 * `forced` selects how aggressively we override local state:
 *
 *  - false (ocsfs_inode_refresh): skip whenever the inode is I_DIRTY (any of
 *    SYNC/DATASYNC/TIME/PAGES) or under writeback (I_SYNC).  This is the
 *    conservative default used on read paths and from within an operation: it
 *    protects the in-flight window inside __ocsfs_add_dirent where i_size and
 *    the extent list are grown in-core *before* mark_inode_dirty(), which a
 *    blind re-read would clobber into a corrupt directory.
 *
 *  - true (ocsfs_inode_refresh_forced): skip only on I_DIRTY_INODE / I_SYNC,
 *    i.e. genuinely-newer-in-memory *metadata* (i_nlink from mkdir/rmdir/link,
 *    or an as-yet-unflushed size change) or an in-progress writeback.  A bare
 *    I_DIRTY_PAGES does NOT block it: ocsfs_flush_inode_locked() write-through
 *    clears only I_DIRTY_INODE and leaves I_DIRTY_PAGES set, so after every
 *    committed directory op the inode is metadata-clean-but-pages-dirty.  The
 *    conservative refresh would then refuse forever and the node would keep
 *    growing the directory from its own STALE i_size/extents — under concurrent
 *    multi-node churn that rolls back a peer's block growth and orphans ~one
 *    node's worth of dirents (40/60 visible, i_size stuck).  Callers must only
 *    use the forced form at a known-safe point: holding the inode's DLM EX with
 *    no local mutation in flight (e.g. right after the EX acquire in
 *    ocsfs_add_dirent/ocsfs_del_dirent).
 *
 * In both cases the on-disk inode is read fresh (cache-bypassing) in clustered
 * mode so a peer's flushed changes are picked up — the whole point of
 * refreshing under the DLM lock.  Single-node: in-memory is authoritative.
 */
static int inode_refresh_impl(struct inode *inode, bool forced)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_sb_info *sbi   = OCSFS_SB(inode->i_sb);
	struct ocsfs_disk_inode di;
	unsigned int skip = forced ? (I_DIRTY_INODE | I_SYNC) : (I_DIRTY | I_SYNC);
	int ret;

	if (inode_state_read(inode) & skip)
		return 0;

	/* Cross-node coherence: in clustered mode read the inode block fresh
	 * from disk (bypassing the cache) so we pick up a peer's flushed changes
	 * — the whole point of refreshing under the DLM lock. */
	ret = ocsfs_read_disk_inode(inode->i_sb, oi->i_disk_ino, &di,
				    sbi->s_clustered);
	if (ret)
		return ret;
	mutex_lock(&oi->i_extent_lock);
	/* Refresh all VFS fields so cluster chmod/chown/link are visible
	 * to permission checks on this node (ALTO-V3-3). */
	inode->i_mode   = le16_to_cpu(di.i_mode);
	set_nlink(inode, le16_to_cpu(di.i_nlink));
	i_uid_write(inode, le32_to_cpu(di.i_uid));
	i_gid_write(inode, le32_to_cpu(di.i_gid));
	inode->i_size   = le64_to_cpu(di.i_size);
	inode->i_blocks = le64_to_cpu(di.i_blocks) * (sbi->s_block_size / 512);
	inode_set_atime_to_ts(inode, ns_to_timespec64(le64_to_cpu(di.i_atime)));
	inode_set_mtime_to_ts(inode, ns_to_timespec64(le64_to_cpu(di.i_mtime)));
	inode_set_ctime_to_ts(inode, ns_to_timespec64(le64_to_cpu(di.i_ctime)));
	oi->i_flags            = le32_to_cpu(di.i_flags);
	oi->i_extent_tree_root = le64_to_cpu(di.i_extent_tree_root);
	oi->i_xattr_block      = le64_to_cpu(di.i_xattr_block);
	/* Cross-node directory coherence: pick up dirents a peer added/removed.
	 * Without refreshing these a peer's new files stay invisible (the in-core
	 * btree root / entry count would point at the pre-change directory). */
	oi->i_dir_btree_root   = le64_to_cpu(di.i_dir_btree_root);
	oi->i_dirent_count     = le32_to_cpu(di.i_dirent_count);

	if ((oi->i_extent_tree_root &&
	     oi->i_extent_tree_root >= sbi->s_total_blocks) ||
	    (oi->i_dir_btree_root &&
	     oi->i_dir_btree_root >= sbi->s_total_blocks) ||
	    (oi->i_xattr_block && oi->i_xattr_block >= sbi->s_total_blocks)) {
		pr_err_ratelimited("ocsfs: inode %llu: corrupt block pointers on refresh\n",
				   oi->i_disk_ino);
		mutex_unlock(&oi->i_extent_lock);
		return -EUCLEAN;
	}

	ocsfs_parse_extents(oi, &di);
	mutex_unlock(&oi->i_extent_lock);
	return 0;
}

/* Conservative refresh — see inode_refresh_impl(). */
int ocsfs_inode_refresh(struct inode *inode)
{
	return inode_refresh_impl(inode, false);
}

/* Forced refresh for the directory write paths — bypasses I_DIRTY_PAGES so a
 * node re-reads a peer's committed i_size/extents between directory ops.  ONLY
 * safe under the inode's DLM EX with no local mutation in flight.  See
 * inode_refresh_impl(). */
int ocsfs_inode_refresh_forced(struct inode *inode)
{
	return inode_refresh_impl(inode, true);
}

/* ═══════════════════════════════════════════════════════════════
 * WRITE INODE TO DISK
 * ═══════════════════════════════════════════════════════════════ */

/* Write inode to disk — caller holds DLM EX (or single-node mode).
 * force_sync=true: sync before returning (required before EX release).
 * Lock ordering: DLM EX (caller) → j_lock (acquired here). */
int ocsfs_flush_inode_locked(struct inode *inode, bool force_sync)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_ag_info *ag = &sbi->s_ags[oi->i_ag];
	struct ocsfs_txn *txn;
	struct buffer_head *bh;
	struct ocsfs_disk_inode *di;
	u64 off, block;
	u32 boff;
	u16 i;
	int ret;
	bool fresh = false;

	/*
	 * Cross-node: an inode block packs 8 inodes.  This read-modify-write
	 * writes the WHOLE block, so without serialisation a peer flushing a
	 * co-located inode (read cached → missing our just-written inode → write
	 * back) would clobber it (observed as -EINVAL on the peer's file after a
	 * concurrent two-node write).  Hold the inode's AG EX lock across the RMW
	 * (it also serialises inode allocation/free), and on a real cross-node
	 * acquire re-read the block fresh so the other 7 inodes reflect the peer's
	 * committed state.
	 */
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire_fresh(inode->i_sb, &ag->ag_lock_res,
					       OCSFS_LOCK_EX, &fresh);
		if (ret)
			return ret;
	}

	txn = ocsfs_txn_begin(inode->i_sb);
	if (IS_ERR(txn)) {
		if (sbi->s_clustered)
			ocsfs_lock_release(inode->i_sb, &ag->ag_lock_res);
		return PTR_ERR(txn);
	}

	off   = ocsfs_inode_disk_off(sbi, oi->i_disk_ino);
	block = off / sbi->s_block_size;
	boff  = off % sbi->s_block_size;

	/*
	 * On a real cross-node acquire, force a fresh read so co-located inodes a
	 * peer modified are preserved; otherwise (same-node cache hit, or single
	 * node) a cached read keeps read-your-own-writes for the other inodes.
	 */
	if (fresh) {
		bh = sb_getblk(inode->i_sb, block);
		if (bh) {
			clear_buffer_uptodate(bh);
			if (bh_read(bh, 0) < 0) {
				brelse(bh);
				bh = NULL;
			}
		}
	} else {
		bh = sb_bread(inode->i_sb, block);
	}
	if (!bh) {
		ret = -EIO;
		goto out_abort;
	}

	ret = ocsfs_txn_add_bh(txn, bh);
	if (ret) {
		brelse(bh);
		goto out_abort;
	}

	di = (struct ocsfs_disk_inode *)(bh->b_data + boff);

	di->i_magic            = cpu_to_le32(OCSFS_INODE_MAGIC);
	di->i_ino              = cpu_to_le64(oi->i_disk_ino);
	di->i_mode             = cpu_to_le16(inode->i_mode);
	di->i_nlink            = cpu_to_le16(inode->i_nlink);
	di->i_uid              = cpu_to_le32(i_uid_read(inode));
	di->i_gid              = cpu_to_le32(i_gid_read(inode));
	di->i_size             = cpu_to_le64(inode->i_size);
	di->i_blocks           = cpu_to_le64(inode->i_blocks /
					     (sbi->s_block_size / 512));
	{
		struct timespec64 ts;
		ts = inode_get_atime(inode);
		di->i_atime = cpu_to_le64(timespec64_to_ns(&ts));
		ts = inode_get_mtime(inode);
		di->i_mtime = cpu_to_le64(timespec64_to_ns(&ts));
		ts = inode_get_ctime(inode);
		di->i_ctime = cpu_to_le64(timespec64_to_ns(&ts));
	}
	di->i_flags            = cpu_to_le32(oi->i_flags);
	di->i_ag               = cpu_to_le32(oi->i_ag);
	di->i_extent_count     = cpu_to_le16(oi->i_extent_count);
	di->i_extent_max       = cpu_to_le16(OCSFS_INLINE_EXTENTS);
	di->i_extent_tree_root = cpu_to_le64(oi->i_extent_tree_root);
	di->i_dir_btree_root   = cpu_to_le64(oi->i_dir_btree_root);
	di->i_dirent_count     = cpu_to_le32(oi->i_dirent_count);
	di->i_xattr_block      = cpu_to_le64(oi->i_xattr_block);

	if (S_ISLNK(inode->i_mode) && oi->i_symlink) {
		size_t slen = min_t(size_t, inode->i_size, OCSFS_MAX_INLINE_SYMLINK);

		/* Zero the entire area first so stale extent bytes from a
		 * previously reused inode block cannot leak to disk. */
		memset(di->i_inline_extents, 0,
		       OCSFS_INLINE_EXTENTS * sizeof(struct ocsfs_disk_extent));
		memcpy(di->i_inline_extents, oi->i_symlink, slen);
	} else {
		for (i = 0; i < oi->i_extent_count && i < OCSFS_INLINE_EXTENTS; i++) {
			struct ocsfs_disk_extent *de =
				(struct ocsfs_disk_extent *)
				(di->i_inline_extents + i * sizeof(*de));
			de->e_logical_block  = cpu_to_le64(oi->i_extents[i].logical_block);
			de->e_physical_block = cpu_to_le64(oi->i_extents[i].physical_block);
			de->e_length         = cpu_to_le32(oi->i_extents[i].length);
			de->e_flags          = cpu_to_le16(oi->i_extents[i].flags);
			/* Reuse e_checksum to store physical block count for COMPRESSED extents */
			de->e_checksum = (oi->i_extents[i].flags & OCSFS_EXT_COMPRESSED)
					 ? cpu_to_le16(oi->i_extents[i].phys_length) : 0;
		}
	}

	di->i_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, di, OCSFS_INODE_SIZE - 4));

	ret = ocsfs_txn_commit(txn);
	if (ret == 0 && force_sync)
		sync_dirty_buffer(bh);
	if (ret == 0)
		oi->i_extents_dirty = false;  /* on-disk inode now matches memory */
	if (ret == 0 && force_sync && sbi->s_clustered) {
		/*
		 * Cross-node coherence: this metadata flush is write-through (done
		 * synchronously under DLM EX), so the on-disk inode is now current.
		 * Drop the VFS metadata-dirty flags so a later ocsfs_inode_refresh
		 * — which skips I_DIRTY inodes to protect in-flight local changes —
		 * can pick up a *peer's* changes instead of being stuck on our own
		 * already-flushed (but still dirty-flagged) copy.  This is what makes
		 * a node that has modified a directory still see a peer's later
		 * additions to it.  I_DIRTY_PAGES is left untouched: data-page
		 * writeback is independent of this inode metadata flush.
		 */
		spin_lock(&inode->i_lock);
		inode_state_clear(inode,
				  I_DIRTY_SYNC | I_DIRTY_DATASYNC | I_DIRTY_TIME);
		spin_unlock(&inode->i_lock);
	}
	brelse(bh);
	/* Inode block committed under the AG lock — release it (a peer may now
	 * RMW the block and will see our committed inode). */
	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &ag->ag_lock_res);
	return ret;

out_abort:
	ocsfs_txn_abort(txn);
	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &ag->ag_lock_res);
	return ret;
}

/*
 * Join the inode block to an already-open txn and update both btree root
 * fields atomically.  The caller holds DLM EX on the inode and owns txn.
 * On failure the txn is NOT aborted — caller decides.
 */
int ocsfs_inode_journal_root(struct ocsfs_txn *txn, struct inode *inode)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct buffer_head *bh;
	struct ocsfs_disk_inode *di;
	u64 off   = ocsfs_inode_disk_off(sbi, oi->i_disk_ino);
	u64 block = off / sbi->s_block_size;
	u32 boff  = off % sbi->s_block_size;
	int ret;

	/*
	 * Cross-node: this whole-block inode write would clobber a co-located
	 * inode a peer just committed if it used a stale cached copy.  We cannot
	 * take the AG lock here (held to the caller's commit it deadlocks against
	 * the block allocator's per-AG locks in the same txn), so when clustered
	 * we re-read the block fresh — preserving the peer's co-located inodes as
	 * of read time.  Only the btree-root field (always written from the
	 * current oi value) and the checksum change, so re-reading never loses a
	 * within-txn update.  A residual race (peer commits between this read and
	 * the caller's commit) remains — see the limitation note on
	 * ocsfs_flush_inode_locked.
	 */
	if (sbi->s_clustered) {
		bh = sb_getblk(inode->i_sb, block);
		if (bh) {
			clear_buffer_uptodate(bh);
			if (bh_read(bh, 0) < 0) {
				brelse(bh);
				bh = NULL;
			}
		}
	} else {
		bh = sb_bread(inode->i_sb, block);
	}
	if (!bh)
		return -EIO;

	ret = ocsfs_txn_add_bh(txn, bh);
	if (ret) {
		brelse(bh);
		return ret;
	}

	di = (struct ocsfs_disk_inode *)(bh->b_data + boff);
	/* MEDIO-V3-4: only write i_dir_btree_root for directories.
	 * For regular files the field is always 0; writing the in-memory value
	 * risks clobbering a fresh disk read with a stale cached value. */
	if (S_ISDIR(inode->i_mode))
		di->i_dir_btree_root = cpu_to_le64(oi->i_dir_btree_root);
	di->i_extent_tree_root = cpu_to_le64(oi->i_extent_tree_root);
	di->i_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, di, OCSFS_INODE_SIZE - 4));

	brelse(bh);
	return 0;
}

int ocsfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	int ret;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	ret = ocsfs_flush_inode_locked(inode, wbc->sync_mode == WB_SYNC_ALL);

	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * EVICT INODE — cleanup on last iput / delete
 * ═══════════════════════════════════════════════════════════════ */

void ocsfs_evict_inode(struct inode *inode)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);

	truncate_inode_pages_final(&inode->i_data);

	/*
	 * Free on-disk resources BEFORE clear_inode().  After clear_inode()
	 * the inode is I_FREEING and mark_inode_dirty() becomes a no-op,
	 * so extent_truncate's i_blocks update and inode flush would be silently
	 * dropped — leaving stale extent pointers on disk that could cross-link
	 * with later allocations after the blocks are freed.
	 */
	/*
	 * Skip disk-resource cleanup for inodes that never finished loading
	 * (iget_failed path): make_bad_inode marks them I_BAD, i_nlink is 0
	 * because set_nlink was never called, and freeing the on-disk inode
	 * number would silently delete a live file (NUOV-MEDIO-3).
	 */
	if (!inode->i_nlink && !is_bad_inode(inode) &&
	    oi->i_disk_ino >= OCSFS_FIRST_USER_INO) {
		dquot_initialize(inode);
		int lr = 0;

		if (sbi->s_clustered)
			lr = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
						OCSFS_LOCK_EX);
		if (lr) {
			/* ALTO-N5: cannot free blocks — DLM EX unavailable.
			 * Mark ORPHAN so ocsfs_orphan_scan recovers space on
			 * next mount.  Writing only the flags field (additive,
			 * no extent data modified) is safe without EX. */
			oi->i_flags |= OCSFS_IFLAG_ORPHAN;
			ocsfs_flush_inode_locked(inode, false);
			pr_warn_ratelimited(
				"ocsfs: evict: DLM EX failed (%d), ino %llu "
				"flagged ORPHAN — reclaimed at next mount\n",
				lr, oi->i_disk_ino);
		} else {
			mutex_lock(&oi->i_extent_lock);
			ocsfs_extent_truncate(inode, 0);
			mutex_unlock(&oi->i_extent_lock);
			if (oi->i_xattr_block) {
				struct ocsfs_txn *xt =
					ocsfs_txn_begin(inode->i_sb);

				if (!IS_ERR(xt)) {
					ocsfs_free_blocks_txn(xt,
							      oi->i_xattr_block,
							      1);
					ocsfs_txn_commit(xt);
				} else {
					ocsfs_free_blocks(inode->i_sb,
							  oi->i_xattr_block, 1);
				}
			}
			dquot_free_inode(inode);
			ocsfs_free_inode_num(inode->i_sb, oi->i_disk_ino);
			if (sbi->s_clustered)
				ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
		}
	} else if (sbi->s_clustered &&
		   oi->i_lock_res.lr_mode != OCSFS_LOCK_NL) {
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	}

	dquot_drop(inode);
	clear_inode(inode);

	kfree(oi->i_symlink);
	oi->i_symlink = NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * ALLOCATE NEW INODE
 * ═══════════════════════════════════════════════════════════════ */

struct inode *ocsfs_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct inode *inode;
	struct ocsfs_inode_info *oi;
	u64 ino;
	u32 ag_hint;
	int ret;

	/* Prefer the parent directory's AG for locality */
	ag_hint = OCSFS_I(dir)->i_ag;

	ret = ocsfs_alloc_inode_num(sb, ag_hint, &ino);
	if (ret)
		return ERR_PTR(ret);

	inode = new_inode(sb);
	if (!inode) {
		ocsfs_free_inode_num(sb, ino);
		return ERR_PTR(-ENOMEM);
	}

	oi = OCSFS_I(inode);
	oi->i_disk_ino = ino;
	oi->i_ag = ocsfs_ino_to_ag(sbi, ino);
	oi->i_extent_count = 0;
	oi->i_extent_tree_root = 0;
	oi->i_flags = OCSFS_IFLAG_ORPHAN;
	oi->i_dir_btree_root = 0;
	oi->i_dirent_count   = 0;
	oi->i_xattr_block    = 0;

	/* Initialise per-inode DLM lock and take EX during creation. */
	ocsfs_lock_init(&oi->i_lock_res,
			ocsfs_lock_hash_inode(ino), OCSFS_LOCKRES_INODE);
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(sb, &oi->i_lock_res, OCSFS_LOCK_EX);
		if (ret) {
			iput(inode);
			ocsfs_free_inode_num(sb, ino);
			return ERR_PTR(ret);
		}
	}

	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_ino = ino;
	inode->i_blocks = 0;
	inode->i_size = 0;
	simple_inode_init_ts(inode);

	if (S_ISREG(mode)) {
		inode->i_op = &ocsfs_file_inode_ops;
		inode->i_fop = &ocsfs_file_fops;
		inode->i_mapping->a_ops = &ocsfs_iomap_aops;
	} else if (S_ISDIR(mode)) {
		inode->i_op = &ocsfs_dir_inode_ops;
		inode->i_fop = &ocsfs_dir_fops;
		inode->i_mapping->a_ops = &ocsfs_aops;
		set_nlink(inode, 2);  /* . and .. */
	} else {
		inode->i_op = &ocsfs_special_inode_ops;
	}

	insert_inode_hash(inode);

	ret = dquot_alloc_inode(inode);
	if (ret) {
		inode->i_flags |= S_NOQUOTA;
		if (sbi->s_clustered)
			ocsfs_lock_release(sb, &oi->i_lock_res);
		/* new_inode()/d_instantiate protocol: no I_NEW set, so plain iput
		 * (discard_new_inode would WARN_ON the missing I_NEW). */
		iput(inode);
		return ERR_PTR(ret);
	}

	mark_inode_dirty(inode);

	if (sbi->s_clustered) {
		int fr = ocsfs_flush_inode_locked(inode, true);

		if (fr)
			pr_warn_ratelimited("ocsfs: new_inode flush failed (%d)\n", fr);

		ocsfs_lock_release(sb, &oi->i_lock_res);
	}

	return inode;
}

/* ═══════════════════════════════════════════════════════════════
 * SETATTR / GETATTR
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	int ret;

	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		return ret;

	ret = security_inode_setattr(idmap, dentry, attr);
	if (ret)
		return ret;

	/*
	 * All setattr paths (chmod, chown, utimes, truncate, size expansion)
	 * need DLM EX in cluster mode so every node sees a consistent snapshot
	 * of the inode — the VFS inode_lock does not cross nodes.
	 */
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	if (attr->ia_valid & ATTR_SIZE) {
		if (attr->ia_size < inode->i_size) {
			u64 from_block = (attr->ia_size + sbi->s_block_size - 1) /
					 sbi->s_block_size;

			/*
			 * Zero the partial tail of the last data block ON DISK
			 * before shrinking i_size.  truncate_setsize() only zeroes
			 * the page-cache tail of that block; the folio is then above
			 * the new EOF, so iomap writeback never persists it and the
			 * stale bytes survive on disk.  A later extend (ftruncate up
			 * / fallocate / reflink) re-exposes that block and a
			 * cache-cold read returns the pre-truncate data (caught by
			 * fsx: truncate to a sub-block size then extend → stale tail
			 * read from disk).  iomap_truncate_page() zeroes
			 * [ia_size, end-of-block) through the write path while the
			 * block is still within i_size; flush it so the zeroed tail
			 * reaches disk before truncate_setsize() drops EOF below it.
			 * Block-aligned truncations and UNWRITTEN/hole tails (which
			 * already read zero) are skipped.
			 */
			if (attr->ia_size & (sbi->s_block_size - 1)) {
				ret = iomap_truncate_page(inode, attr->ia_size,
							  NULL, &ocsfs_iomap_ops,
							  NULL, NULL);
				if (ret) {
					if (sbi->s_clustered)
						ocsfs_lock_release(inode->i_sb,
							&oi->i_lock_res);
					return ret;
				}
				filemap_write_and_wait_range(inode->i_mapping,
					attr->ia_size,
					round_up(attr->ia_size,
						 sbi->s_block_size) - 1);
			}

			/*
			 * truncate_setsize: shrinks i_size and invalidates
			 * page cache + mmap mappings before we free blocks.
			 * This prevents mmap faults on freed blocks in the window
			 * between extent_truncate and truncate_setsize.
			 */
			truncate_setsize(inode, attr->ia_size);
			mutex_lock(&oi->i_extent_lock);
			ocsfs_extent_truncate(inode, from_block);
			mutex_unlock(&oi->i_extent_lock);
			/* ALTO-V3-2: record dirty range so the next SH acquirer
			 * performs selective page cache invalidation (ARCH-7). */
			if (sbi->s_clustered) {
				oi->i_lock_res.lr_inv_lo = from_block;
				oi->i_lock_res.lr_inv_hi = U64_MAX;
			}
		} else {
			truncate_setsize(inode, attr->ia_size);
		}
	}

	if (attr->ia_valid & (ATTR_UID | ATTR_GID)) {
		ret = dquot_transfer(idmap, inode, attr);
		if (ret) {
			if (sbi->s_clustered)
				ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
			return ret;
		}
	}

	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);

	if (sbi->s_clustered) {
		int fr = ocsfs_flush_inode_locked(inode, true);

		if (fr)
			pr_warn_ratelimited("ocsfs: setattr flush failed (%d)\n",
					    fr);
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	}

	return 0;
}

int ocsfs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);

	generic_fillattr(idmap, request_mask, inode, stat);
	stat->blksize = OCSFS_SB(inode->i_sb)->s_block_size;
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * FILE ATTRIBUTE FLAGS (chattr / lsattr — FS_IOC_GETFLAGS / SETFLAGS)
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_fileattr_get(struct dentry *dentry, struct file_kattr *fa)
{
	struct ocsfs_inode_info *oi = OCSFS_I(d_inode(dentry));
	u32 flags = 0;

	if (oi->i_flags & OCSFS_IFLAG_IMMUTABLE)
		flags |= FS_IMMUTABLE_FL;
	if (oi->i_flags & OCSFS_IFLAG_APPEND)
		flags |= FS_APPEND_FL;
	if (oi->i_flags & OCSFS_IFLAG_COMPRESSED)
		flags |= FS_COMPR_FL;

	fileattr_fill_flags(fa, flags);
	return 0;
}

int ocsfs_fileattr_set(struct mnt_idmap *idmap, struct dentry *dentry,
		       struct file_kattr *fa)
{
	struct inode *inode = d_inode(dentry);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	const u32 supported = FS_IMMUTABLE_FL | FS_APPEND_FL | FS_COMPR_FL;
	u32 old_fs_flags, new_fs_flags;

	if (fileattr_has_fsx(fa))
		return -EOPNOTSUPP;
	if (fa->flags & ~supported)
		return -EOPNOTSUPP;

	old_fs_flags = 0;
	if (oi->i_flags & OCSFS_IFLAG_IMMUTABLE) old_fs_flags |= FS_IMMUTABLE_FL;
	if (oi->i_flags & OCSFS_IFLAG_APPEND)    old_fs_flags |= FS_APPEND_FL;
	new_fs_flags = fa->flags & supported;

	/* Setting or clearing immutable/append requires CAP_LINUX_IMMUTABLE */
	if ((new_fs_flags ^ old_fs_flags) & (FS_IMMUTABLE_FL | FS_APPEND_FL))
		if (!capable(CAP_LINUX_IMMUTABLE))
			return -EPERM;

	oi->i_flags &= ~(OCSFS_IFLAG_IMMUTABLE | OCSFS_IFLAG_APPEND |
			 OCSFS_IFLAG_COMPRESSED);
	if (new_fs_flags & FS_IMMUTABLE_FL) oi->i_flags |= OCSFS_IFLAG_IMMUTABLE;
	if (new_fs_flags & FS_APPEND_FL)    oi->i_flags |= OCSFS_IFLAG_APPEND;
	if (new_fs_flags & FS_COMPR_FL)     oi->i_flags |= OCSFS_IFLAG_COMPRESSED;

	/* Keep VFS immutable/append in sync so IS_IMMUTABLE/IS_APPEND work */
	inode->i_flags = (inode->i_flags & ~(S_IMMUTABLE | S_APPEND)) |
			 ((new_fs_flags & FS_IMMUTABLE_FL) ? S_IMMUTABLE : 0) |
			 ((new_fs_flags & FS_APPEND_FL)    ? S_APPEND    : 0);

	inode_set_ctime_current(inode);
	mark_inode_dirty(inode);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * INODE OPERATIONS TABLES
 * ═══════════════════════════════════════════════════════════════ */

const struct inode_operations ocsfs_file_inode_ops = {
	.setattr        = ocsfs_setattr,
	.getattr        = ocsfs_getattr,
	.listxattr      = ocsfs_listxattr,
	.fiemap         = ocsfs_fiemap,
	.fileattr_get   = ocsfs_fileattr_get,
	.fileattr_set   = ocsfs_fileattr_set,
	.get_inode_acl  = ocsfs_get_inode_acl,
	.set_acl        = ocsfs_set_acl,
};

const struct inode_operations ocsfs_special_inode_ops = {
	.setattr        = ocsfs_setattr,
	.getattr        = ocsfs_getattr,
	.listxattr      = ocsfs_listxattr,
	.fileattr_get   = ocsfs_fileattr_get,
	.fileattr_set   = ocsfs_fileattr_set,
	.get_inode_acl  = ocsfs_get_inode_acl,
	.set_acl        = ocsfs_set_acl,
};

static const char *ocsfs_get_link(struct dentry *dentry, struct inode *inode,
				   struct delayed_call *done)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);

	if (!oi->i_symlink)
		return ERR_PTR(-ENOLINK);
	return oi->i_symlink;
}

const struct inode_operations ocsfs_symlink_inode_ops = {
	.get_link       = ocsfs_get_link,
	.setattr        = ocsfs_setattr,
	.getattr        = ocsfs_getattr,
	.listxattr      = ocsfs_listxattr,
	.fileattr_get   = ocsfs_fileattr_get,
	.fileattr_set   = ocsfs_fileattr_set,
	.get_inode_acl  = ocsfs_get_inode_acl,
	.set_acl        = ocsfs_set_acl,
};
