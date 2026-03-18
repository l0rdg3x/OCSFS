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

/* ═══════════════════════════════════════════════════════════════
 * SNAPSHOT METADATA
 *
 * Snapshot info is stored as a special directory entry with
 * the snapshot name pointing to the snapshot inode. The
 * snapshot inode shares extents with the source via refcounting.
 * ═══════════════════════════════════════════════════════════════ */

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

	/* Copy file metadata */
	i_size_write(snap, i_size_read(src));
	snap->i_blocks = src->i_blocks;
	snap_oi->i_flags = src_oi->i_flags;

	/* Lock both inodes for extent manipulation */
	mutex_lock(&src_oi->i_extent_lock);
	mutex_lock_nested(&snap_oi->i_extent_lock, SINGLE_DEPTH_NESTING);

	/* Copy the extent map */
	snap_oi->i_extent_count = src_oi->i_extent_count;
	memcpy(snap_oi->i_extents, src_oi->i_extents,
	       src_oi->i_extent_count * sizeof(struct ocsfs_extent));

	/* Increment refcount on all shared extents */
	for (i = 0; i < src_oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &src_oi->i_extents[i];

		if (e->physical_block == 0 ||
		    (e->flags & OCSFS_EXT_UNWRITTEN))
			continue;

		ret = ocsfs_refcount_inc(sb, e->physical_block, e->length);
		if (ret) {
			/* Rollback: decrement already-incremented extents */
			u16 j;

			for (j = 0; j < i; j++) {
				struct ocsfs_extent *prev = &src_oi->i_extents[j];

				if (prev->physical_block == 0 ||
				    (prev->flags & OCSFS_EXT_UNWRITTEN))
					continue;
				ocsfs_refcount_dec(sb, prev->physical_block,
						   prev->length, NULL);
			}

			mutex_unlock(&snap_oi->i_extent_lock);
			mutex_unlock(&src_oi->i_extent_lock);
			iput(snap);
			return ret;
		}
	}

	mutex_unlock(&snap_oi->i_extent_lock);
	mutex_unlock(&src_oi->i_extent_lock);

	/* Mark both inodes dirty */
	mark_inode_dirty(src);
	mark_inode_dirty(snap);

	/* Add directory entry for the snapshot */
	ret = ocsfs_add_dirent(dir, name, snap_oi->i_disk_ino,
			       OCSFS_FT_REG_FILE);
	if (ret) {
		/* Clean up on failure */
		ocsfs_snapshot_delete(snap);
		iput(snap);
		return ret;
	}

	iput(snap);
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

	mutex_lock(&oi->i_extent_lock);

	for (i = 0; i < oi->i_extent_count; i++) {
		struct ocsfs_extent *e = &oi->i_extents[i];

		if (e->physical_block == 0)
			continue;

		should_free = false;
		ret = ocsfs_refcount_dec(sb, e->physical_block, e->length,
					 &should_free);
		if (ret)
			pr_warn("ocsfs: snapshot_delete: refcount_dec failed "
				"for block %llu\n", e->physical_block);

		if (should_free) {
			ocsfs_free_blocks(sb, e->physical_block, e->length);
			snap->i_blocks -= (u64)e->length *
					  (sbi->s_block_size / 512);
		}

		e->physical_block = 0;
		e->length = 0;
	}

	oi->i_extent_count = 0;
	i_size_write(snap, 0);
	mark_inode_dirty(snap);

	mutex_unlock(&oi->i_extent_lock);
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
 * Returns 0 on success, the new physical block in the extent map.
 * Caller must hold i_extent_lock.
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

	/* Copy data block by block */
	for (i = 0; i < len; i++) {
		old_bh = sb_bread(sb, old_phys + i);
		if (!old_bh) {
			ocsfs_free_blocks(sb, new_phys, len);
			return -EIO;
		}

		new_bh = sb_getblk(sb, new_phys + i);
		if (!new_bh) {
			brelse(old_bh);
			ocsfs_free_blocks(sb, new_phys, len);
			return -ENOMEM;
		}

		lock_buffer(new_bh);
		memcpy(new_bh->b_data, old_bh->b_data, sbi->s_block_size);
		set_buffer_uptodate(new_bh);
		mark_buffer_dirty(new_bh);
		unlock_buffer(new_bh);

		brelse(old_bh);
		brelse(new_bh);
	}

	/*
	 * Update the extent map. We need to split the existing extent
	 * if the CoW region doesn't cover the entire extent.
	 *
	 * For simplicity, we remove and re-insert the affected range.
	 * The extent insert/merge logic handles the split.
	 */
	if (offset_in_ext == 0 && len == ext.length) {
		/* Full extent replacement — find and update in place */
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
		/*
		 * Partial CoW: we need to split the extent.
		 * The original extent keeps the unmodified parts,
		 * the new allocation covers the modified range.
		 */
		u16 j;

		for (j = 0; j < oi->i_extent_count; j++) {
			struct ocsfs_extent *e = &oi->i_extents[j];

			if (e->logical_block == ext.logical_block &&
			    e->physical_block == ext.physical_block) {
				if (offset_in_ext > 0) {
					/* Keep head, split off */
					u32 head_len = (u32)offset_in_ext;
					u32 tail_start = (u32)(offset_in_ext + len);

					/* Shrink to head */
					e->length = head_len;

					/* Insert new CoW'd region */
					ocsfs_extent_insert(inode, logical,
							    new_phys, len,
							    OCSFS_EXT_WRITTEN);

					/* Insert tail if exists */
					if (tail_start < ext.length) {
						ocsfs_extent_insert(inode,
							ext.logical_block + tail_start,
							ext.physical_block + tail_start,
							ext.length - tail_start,
							ext.flags);
					}
				} else {
					/* CoW starts at extent start */
					u32 remaining = ext.length - len;

					e->physical_block = new_phys;
					e->length = len;

					if (remaining > 0) {
						ocsfs_extent_insert(inode,
							logical + len,
							ext.physical_block + len,
							remaining,
							ext.flags);
					}
				}
				break;
			}
		}
	}

	/* Decrement refcount on old blocks */
	{
		bool should_free = false;

		ret = ocsfs_refcount_dec(sb, old_phys, len, &should_free);
		if (ret == 0 && should_free) {
			ocsfs_free_blocks(sb, old_phys, len);
		}
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
