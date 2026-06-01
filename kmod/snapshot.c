// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — snapshot.c
 * Copy-on-Write (CoW) file-level snapshot support.
 *
 * Phase 4: File-level CoW snapshots — superior to VMFS which only
 * supports VMDK-level snapshots.
 *
 * Mechanism:
 *   1. Snapshot creation: increment refcount on all source extents.
 *      The snapshot file gets the same extent map as the original.
 *      Instant — O(1) for small extent counts, O(n) for inline extents.
 *
 *   2. Copy-on-Write: When writing to a shared extent (refcount > 1):
 *      a. Allocate new physical blocks
 *      b. Copy old data to new blocks
 *      c. Update extent map to point to new blocks
 *      d. Decrement refcount on old blocks
 *      e. If refcount reaches 0, free old blocks
 *
 *   3. Snapshot deletion: decrement refcount on all snapshot's extents.
 *      Free blocks whose refcount drops to 0.
 *
 * This integrates with the Proxmox snapshot UI for VM disk snapshots.
 */

#include "ocsfs.h"
#include <linux/pagemap.h>
#include <linux/highmem.h>

/* ═══════════════════════════════════════════════════════════════
 * SNAPSHOT METADATA
 *
 * Snapshot info is stored as a special directory entry with
 * the snapshot name pointing to the snapshot inode. The
 * snapshot inode shares extents with the source via refcounting.
 * ═══════════════════════════════════════════════════════════════ */

/* Callback context for collecting extents from a btree into an array */
struct snap_copy_ctx { struct ocsfs_extent *ext; u32 idx; };

static int snap_copy_extent_cb(u64 logical, u64 physical, u32 length,
				u16 flags, void *ctx_)
{
	struct snap_copy_ctx *c = ctx_;

	c->ext[c->idx++] = (struct ocsfs_extent){
		.logical_block  = logical,
		.physical_block = physical,
		.length         = length,
		.flags          = flags,
	};
	return 0;
}

/*
 * Copy a btree-backed extent map from @src to @snap, incrementing refcounts.
 * Caller holds i_extent_lock on both inodes.
 * On failure, rolls back all refcounts and clears any partial btree state.
 */
static int snapshot_copy_btree_extents(struct inode *src, struct inode *snap)
{
	struct super_block *sb = src->i_sb;
	struct snap_copy_ctx cc;
	u64 count64;
	u32 i;
	int ret;

	ret = ocsfs_extent_btree_count(src, &count64);
	if (ret)
		return ret;

	if (count64 == 0)
		return ocsfs_extent_btree_init_empty(snap);

	if (count64 > UINT_MAX / sizeof(*cc.ext))
		return -EFBIG;

	cc.idx = 0;
	cc.ext = kvmalloc_array((u32)count64, sizeof(*cc.ext), GFP_NOFS);
	if (!cc.ext)
		return -ENOMEM;

	ocsfs_extent_btree_iterate(src, snap_copy_extent_cb, &cc);

	ret = ocsfs_extent_btree_init_empty(snap);
	if (ret)
		goto out;

	i = 0;
	for (; i < cc.idx; i++) {
		struct ocsfs_extent *e = &cc.ext[i];
		bool shared = e->physical_block && !(e->flags & OCSFS_EXT_UNWRITTEN);

		if (shared) {
			ret = ocsfs_refcount_inc(sb, e->physical_block,
						 ocsfs_ext_phys_blocks(e));
			if (ret)
				goto rollback;
		}
		ret = ocsfs_extent_btree_insert(snap, e->logical_block,
						e->physical_block,
						e->length, e->flags);
		if (ret) {
			if (shared)
				ocsfs_refcount_dec(sb, e->physical_block,
						   ocsfs_ext_phys_blocks(e),
						   NULL);
			goto rollback;
		}
	}
	goto out;

rollback:
	ocsfs_extent_btree_clear(snap);
	while (i-- > 0) {
		struct ocsfs_extent *e = &cc.ext[i];

		if (e->physical_block && !(e->flags & OCSFS_EXT_UNWRITTEN))
			ocsfs_refcount_dec(sb, e->physical_block,
					   ocsfs_ext_phys_blocks(e), NULL);
	}
out:
	kvfree(cc.ext);
	return ret;
}

/*
 * ocsfs_snapshot_create() — Create a CoW snapshot of a file.
 *
 * @src:   source inode (the file to snapshot)
 * @dir:   directory to create snapshot entry in
 * @name:  snapshot name
 *
 * Creates a new inode with the same extent map as @src.
 * All shared extents get refcount incremented.
 */
int ocsfs_snapshot_create(struct inode *src, struct inode *dir,
			  const struct qstr *name)
{
	struct super_block *sb = src->i_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_inode_info *src_oi = OCSFS_I(src);
	struct ocsfs_inode_info *snap_oi;
	struct inode *snap;
	u16 i;
	int ret;

	/* Only regular files can be snapshotted */
	if (!S_ISREG(src->i_mode))
		return -EINVAL;

	/* Create the snapshot inode */
	snap = ocsfs_new_inode(dir, src->i_mode);
	if (IS_ERR(snap))
		return PTR_ERR(snap);

	snap_oi = OCSFS_I(snap);

	/* Copy file metadata.
	 * MEDIO-N3: i_blocks = 0 because the snapshot owns NO exclusive blocks
	 * at creation — all extents are shared (refcount > 1).  snapshot_delete
	 * already only decrements i_blocks when should_free == true, so the
	 * value stays accurate as CoW unshares blocks over time. */
	i_size_write(snap, i_size_read(src));
	snap->i_blocks = 0;
	snap_oi->i_flags = src_oi->i_flags;

	/*
	 * Clustered mode: acquire DLM EX on src to serialize refcount
	 * operations and guarantee we read a coherent extent map.
	 * Acquire EX on snap so we can flush it before ocsfs_add_dirent
	 * makes it visible to other nodes. Lock src first (older inode).
	 */
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(sb, &src_oi->i_lock_res, OCSFS_LOCK_EX);
		if (ret) {
			iput(snap);
			return ret;
		}
		ret = ocsfs_lock_acquire(sb, &snap_oi->i_lock_res, OCSFS_LOCK_EX);
		if (ret) {
			ocsfs_lock_release(sb, &src_oi->i_lock_res);
			iput(snap);
			return ret;
		}
	}

	/*
	 * Lock both inodes in inode-number order to prevent ABBA deadlock if
	 * another thread ever takes these locks in the opposite direction.
	 * snap is always freshly allocated so i_disk_ino(src) < i_disk_ino(snap)
	 * in practice, but we make the ordering explicit for safety.
	 */
	if (src_oi->i_disk_ino < snap_oi->i_disk_ino) {
		mutex_lock(&src_oi->i_extent_lock);
		mutex_lock_nested(&snap_oi->i_extent_lock, SINGLE_DEPTH_NESTING);
	} else {
		mutex_lock(&snap_oi->i_extent_lock);
		mutex_lock_nested(&src_oi->i_extent_lock, SINGLE_DEPTH_NESTING);
	}

	/* Copy extent map with refcount increments */
	if (src_oi->i_extent_tree_root) {
		ret = snapshot_copy_btree_extents(src, snap);
	} else {
		/* Inline extents: copy array then bump each refcount */
		snap_oi->i_extent_count = src_oi->i_extent_count;
		memcpy(snap_oi->i_extents, src_oi->i_extents,
		       src_oi->i_extent_count * sizeof(struct ocsfs_extent));

		for (i = 0; i < src_oi->i_extent_count; i++) {
			struct ocsfs_extent *e = &src_oi->i_extents[i];
			u16 j;

			if (e->physical_block == 0 ||
			    (e->flags & OCSFS_EXT_UNWRITTEN))
				continue;

			ret = ocsfs_refcount_inc(sb, e->physical_block,
						 ocsfs_ext_phys_blocks(e));
			if (ret) {
				for (j = 0; j < i; j++) {
					struct ocsfs_extent *p = &src_oi->i_extents[j];

					if (p->physical_block == 0 ||
					    (p->flags & OCSFS_EXT_UNWRITTEN))
						continue;
					ocsfs_refcount_dec(sb, p->physical_block,
							   ocsfs_ext_phys_blocks(p),
							   NULL);
				}
				snap_oi->i_extent_count = 0;
				memset(snap_oi->i_extents, 0, sizeof(snap_oi->i_extents));
				break;
			}
		}
	}

	if (ret) {
		mutex_unlock(&snap_oi->i_extent_lock);
		mutex_unlock(&src_oi->i_extent_lock);
		if (sbi->s_clustered) {
			ocsfs_lock_release(sb, &snap_oi->i_lock_res);
			ocsfs_lock_release(sb, &src_oi->i_lock_res);
		}
		iput(snap);
		return ret;
	}

	mutex_unlock(&snap_oi->i_extent_lock);
	mutex_unlock(&src_oi->i_extent_lock);

	/* Flush to disk before EX release; single-node: async dirty is fine */
	if (sbi->s_clustered) {
		int fr = ocsfs_flush_inode_locked(snap, true);
		if (fr)
			pr_warn_ratelimited("ocsfs: snapshot_create snap flush failed (%d)\n", fr);
		ocsfs_lock_release(sb, &snap_oi->i_lock_res);
		fr = ocsfs_flush_inode_locked(src, true);
		if (fr)
			pr_warn_ratelimited("ocsfs: snapshot_create src flush failed (%d)\n", fr);
		ocsfs_lock_release(sb, &src_oi->i_lock_res);
	} else {
		mark_inode_dirty(src);
		mark_inode_dirty(snap);
	}

	/* Add directory entry for the snapshot */
	ret = ocsfs_add_dirent(dir, name, snap_oi->i_disk_ino,
			       OCSFS_FT_REG_FILE);
	if (ret) {
		/* Clean up on failure */
		ocsfs_snapshot_delete(snap);
		iput(snap);
		return ret;
	}

	snap_oi->i_flags &= ~OCSFS_IFLAG_ORPHAN;
	mark_inode_dirty(snap);
	iput(snap);
	return 0;
}

/* Context for the B+ tree extent iterator used in snapshot_delete */
struct snap_del_ctx {
	struct super_block   *sb;
	struct ocsfs_sb_info *sbi;
	struct inode         *snap;
};

static int snap_del_cb(u64 logical, u64 physical, u32 length, u16 flags,
		       void *ctx)
{
	struct snap_del_ctx *sc = ctx;
	bool should_free = false;
	int ret;

	(void)logical;
	if (!physical || (flags & OCSFS_EXT_UNWRITTEN))
		return 0;
	ret = ocsfs_refcount_dec(sc->sb, physical, length, &should_free);
	if (ret)
		pr_warn("ocsfs: snapshot_delete: refcount_dec failed block %llu\n",
			physical);
	if (should_free) {
		ocsfs_free_blocks(sc->sb, physical, length);
		sc->snap->i_blocks -= (u64)length * (sc->sbi->s_block_size / 512);
	}
	return 0;
}

/*
 * ocsfs_snapshot_delete() — Delete a snapshot, freeing unshared extents.
 *
 * Decrements refcount on all extents. Blocks with refcount 0 are freed.
 */
int ocsfs_snapshot_delete(struct inode *snap)
{
	struct super_block *sb = snap->i_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_inode_info *oi = OCSFS_I(snap);
	bool should_free;
	u16 i;
	int ret;

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(sb, &oi->i_lock_res, OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	mutex_lock(&oi->i_extent_lock);

	if (oi->i_extent_tree_root) {
		struct snap_del_ctx sc = { sb, sbi, snap };

		ocsfs_extent_btree_iterate(snap, snap_del_cb, &sc);
		ocsfs_extent_btree_clear(snap);
	} else {
		for (i = 0; i < oi->i_extent_count; i++) {
			struct ocsfs_extent *e = &oi->i_extents[i];

			if (e->physical_block == 0)
				continue;

			should_free = false;
			{
				u32 phys = ocsfs_ext_phys_blocks(e);

				ret = ocsfs_refcount_dec(sb, e->physical_block,
							 phys, &should_free);
				if (ret)
					pr_warn("ocsfs: snapshot_delete: refcount_dec "
						"failed for block %llu\n",
						e->physical_block);

				if (should_free) {
					ocsfs_free_blocks(sb, e->physical_block,
							  phys);
					snap->i_blocks -= (u64)phys *
							  (sbi->s_block_size / 512);
				}
			}

			e->physical_block = 0;
			e->length = 0;
		}
		oi->i_extent_count = 0;
	}

	i_size_write(snap, 0);
	mark_inode_dirty(snap);

	mutex_unlock(&oi->i_extent_lock);

	if (sbi->s_clustered) {
		int fr = ocsfs_flush_inode_locked(snap, true);
		if (fr)
			pr_warn_ratelimited("ocsfs: snapshot_delete flush failed (%d)\n", fr);
		ocsfs_lock_release(sb, &oi->i_lock_res);
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * COPY-ON-WRITE
 *
 * Called when writing to an extent with refcount > 1.
 * Allocates new blocks, copies data, updates extent map.
 * ═══════════════════════════════════════════════════════════════ */

/*
 * ocsfs_cow_extent() — Perform CoW on a single extent range.
 *
 * @inode:    file being written to
 * @logical:  starting logical block of the write
 * @len:      number of blocks to CoW
 *
 * Returns 0 on success; updates extent map in place.
 * Caller must hold i_extent_lock (and DLM EX in clustered mode).
 * Caller must call ocsfs_flush_inode_locked() before releasing DLM EX.
 */
int ocsfs_cow_extent(struct inode *inode, u64 logical, u32 len)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_extent ext;
	u64 old_phys, new_phys;
	u32 refcount;
	u64 offset_in_ext;
	struct buffer_head *old_bh, *new_bh;
	u32 i;
	int ret;

	/* PERF: CoW remaps logical→new physical; a peer must see the new
	 * mapping.  Force the synchronous cross-node inode flush in write_iter. */
	oi->i_extents_dirty = true;

	/* Look up the current extent */
	ret = ocsfs_extent_lookup(inode, logical, &ext);
	if (ret)
		return ret;

	if (ext.physical_block == 0)
		return 0; /* Hole — nothing to CoW */

	/* Check refcount */
	ret = ocsfs_refcount_get(sb, ext.physical_block, &refcount);
	if (ret)
		return ret;

	if (refcount <= 1)
		return 0; /* Not shared — no CoW needed */

	/* Allocate new blocks */
	offset_in_ext = logical - ext.logical_block;
	old_phys = ext.physical_block + offset_in_ext;

	ret = ocsfs_alloc_blocks(sb, oi->i_ag, len, &new_phys);
	if (ret)
		return ret;

	/* Copy data block by block.
	 *
	 * OCSFS data lives in the iomap PAGE CACHE (folios), NOT in buffer_heads.
	 * A raw sb_bread() of the source block reads a separate buffer_head cache
	 * that iomap never populates, so it can return stale / zero-filled data
	 * and the CoW would silently copy ZEROS — corrupting every reader of the
	 * new block (caught by fsx: a punch on a shared block CoW-copied zeros).
	 * So copy from the uptodate folio when present; fall back to a disk read
	 * only when the block is not cached.  filemap_get_folio() is a pure cache
	 * lookup (no iomap_begin), so it does not deadlock on i_extent_lock. */
	for (i = 0; i < len; i++) {
		loff_t spos = (loff_t)(logical + i) * sbi->s_block_size;
		struct folio *sfolio;
		bool copied = false;

		new_bh = sb_getblk(sb, new_phys + i);
		if (!new_bh) {
			ocsfs_free_blocks(sb, new_phys, len);
			return -ENOMEM;
		}
		lock_buffer(new_bh);

		sfolio = filemap_get_folio(inode->i_mapping, spos >> PAGE_SHIFT);
		if (!IS_ERR_OR_NULL(sfolio)) {
			if (folio_test_uptodate(sfolio)) {
				memcpy_from_folio(new_bh->b_data, sfolio,
						  offset_in_folio(sfolio, spos),
						  sbi->s_block_size);
				copied = true;
			}
			folio_put(sfolio);
		}
		if (!copied) {
			old_bh = sb_bread(sb, old_phys + i);
			if (!old_bh) {
				unlock_buffer(new_bh);
				brelse(new_bh);
				ocsfs_free_blocks(sb, new_phys, len);
				return -EIO;
			}
			memcpy(new_bh->b_data, old_bh->b_data, sbi->s_block_size);
			brelse(old_bh);
		}

		set_buffer_uptodate(new_bh);
		mark_buffer_dirty(new_bh);
		unlock_buffer(new_bh);
		brelse(new_bh);
	}

	/*
	 * Flush CoW data to disk before updating the extent map.
	 * If we crash after the extent update but before writeback,
	 * the file would point to blocks containing stale data.
	 */
	ret = blkdev_issue_flush(sb->s_bdev);
	if (ret) {
		ocsfs_free_blocks(sb, new_phys, len);
		return ret;
	}

	/*
	 * Update the extent map. B+ tree and inline paths are handled
	 * separately: btree uses ocsfs_extent_btree_replace, inline
	 * directly manipulates oi->i_extents[].
	 */
	if (oi->i_extent_tree_root) {
		ret = ocsfs_extent_btree_replace(inode, &ext, offset_in_ext,
						 len, new_phys);
		if (ret) {
			ocsfs_free_blocks(sb, new_phys, len);
			return ret;
		}
	} else if (offset_in_ext == 0 && len == ext.length) {
		/* Full extent replacement — update in place */
		u16 j;

		for (j = 0; j < oi->i_extent_count; j++) {
			struct ocsfs_extent *e = &oi->i_extents[j];

			if (e->logical_block == ext.logical_block &&
			    e->physical_block == ext.physical_block) {
				e->physical_block = new_phys;
				break;
			}
		}
	} else {
		/* Partial CoW — split the existing inline extent */
		u16 j;
		u16 saved_count = oi->i_extent_count;

		for (j = 0; j < oi->i_extent_count; j++) {
			struct ocsfs_extent *e = &oi->i_extents[j];

			if (e->logical_block == ext.logical_block &&
			    e->physical_block == ext.physical_block) {
				if (offset_in_ext > 0) {
					u32 head_len   = (u32)offset_in_ext;
					u32 tail_start = (u32)(offset_in_ext + len);

					e->length = head_len;
					ret = ocsfs_extent_insert(inode, logical,
								  new_phys, len,
								  OCSFS_EXT_WRITTEN);
					if (ret) {
						e->length = ext.length;
						oi->i_extent_count = saved_count;
						ocsfs_free_blocks(sb, new_phys, len);
						return ret;
					}
					if (tail_start < ext.length) {
						ret = ocsfs_extent_insert(inode,
							ext.logical_block + tail_start,
							ext.physical_block + tail_start,
							ext.length - tail_start,
							ext.flags);
						if (ret) {
							e->length = ext.length;
							oi->i_extent_count = saved_count;
							ocsfs_free_blocks(sb, new_phys, len);
							return ret;
						}
					}
				} else {
					u32 remaining = ext.length - len;

					e->physical_block = new_phys;
					e->length = len;
					if (remaining > 0) {
						ret = ocsfs_extent_insert(inode,
							logical + len,
							ext.physical_block + len,
							remaining, ext.flags);
						if (ret) {
							e->physical_block = ext.physical_block;
							e->length         = ext.length;
							oi->i_extent_count = saved_count;
							ocsfs_free_blocks(sb, new_phys, len);
							return ret;
						}
					}
				}
				break;
			}
		}
	}

	/* ARCH-N3: journal inode with new extent map BEFORE decrementing
	 * refcount and freeing old blocks (journal-before-free pattern).
	 * Without this, a crash between free and inode flush leaves the inode
	 * pointing to already-freed (and possibly reallocated) blocks. */
	{
		int fr = ocsfs_flush_inode_locked(inode, false);

		if (fr)
			pr_warn_ratelimited(
				"ocsfs: cow_extent inode flush failed (%d)\n",
				fr);
	}

	/* Decrement refcount on old blocks (safe: inode no longer references them) */
	{
		bool should_free = false;

		ret = ocsfs_refcount_dec(sb, old_phys, len, &should_free);
		if (ret == 0 && should_free)
			ocsfs_free_blocks(sb, old_phys, len);
	}

	mark_inode_dirty(inode);
	return 0;
}

/*
 * ocsfs_needs_cow() — Check if an extent needs Copy-on-Write.
 *
 * Returns true if the physical blocks at @phys_block have refcount > 1.
 */
bool ocsfs_needs_cow(struct super_block *sb, u64 phys_block)
{
	u32 refcount = 0;

	if (ocsfs_refcount_get(sb, phys_block, &refcount) != 0)
		return false;

	return refcount > 1;
}
