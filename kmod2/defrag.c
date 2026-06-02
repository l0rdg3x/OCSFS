// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — defrag.c
 * Online defragmentation: relocate a file's data into contiguous runs so its
 * extent map shrinks. Random VM-disk writes, snapshots and discard fragment a
 * file over time; coalescing the extents restores sequential-read throughput.
 *
 * Method (whole file, in three passes):
 *   1. scan: refuse files that contain SHARED (reflink/snapshot/dedup) or
 *      UNWRITTEN extents — relocating a shared block would break sharing, so we
 *      leave such files untouched. Collect the maximal logically-contiguous
 *      runs of written data.
 *   2. copy: for each run, allocate fresh CONTIGUOUS blocks (in <= cap chunks)
 *      and copy the current data into them (bio-based, coherent — same path as
 *      CoW). No journal txn is held during the copy.
 *   3. swap: in ONE journal txn, tear the old extent map down completely
 *      (ocsfs2_extent_drop_all — frees the old data + any B+tree nodes and
 *      reverts to the empty inline map) and rebuild it from the new runs via the
 *      normal insert path (which re-spills to a freshly-built, consistent
 *      B+tree if needed). Holes between runs are preserved.
 *
 * Rebuilding from scratch (rather than punching the live tree) sidesteps the
 * B+tree's lack of delete-time collapse: a partial punch of a multi-leaf tree
 * would leave stale internal routing. Online: holds the inode lock (no
 * concurrent writers) and, in a cluster, runs on the node that owns the file.
 */
#include "ocsfs.h"
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/minmax.h>

/* Bound a single online defrag so the copy time and the swap transaction stay
 * reasonable; larger or very sparse files are skipped (reported, not failed). */
#define OCSFS2_DEFRAG_MAX_BLOCKS   (256u * 1024u)   /* 1 GiB at 4 KiB */
#define OCSFS2_DEFRAG_MAX_RUNS     4096
#define OCSFS2_DEFRAG_CHUNK        4096u            /* contiguous alloc unit */

/* a logically-contiguous run of written data to relocate */
struct defrag_run { u64 logical; u32 len; };
/* a freshly-allocated contiguous piece backing part of a run */
struct defrag_new { u64 logical, phys; u32 len; };

/* Drop the whole extent map: free every data extent (refcount-aware) and any
 * B+tree nodes, leaving an empty inline map. Caller holds i_meta_lock + txn.
 * Mirrors what truncate-to-zero does, without changing i_size. */
static void defrag_drop_all(struct inode *inode)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	struct super_block *sb = inode->i_sb;
	u16 i;

	if (oi->i_extent_tree_root) {
		ocsfs2_ext_tree_free_all(inode);   /* frees data + nodes, root=0 */
	} else {
		for (i = 0; i < oi->i_extent_count; i++)
			ocsfs2_free_blocks_rc(sb, oi->i_extents[i].physical,
					      oi->i_extents[i].length);
		oi->i_extent_count = 0;
	}
	inode->i_blocks = 0;
}

int ocsfs2_defrag_file(struct inode *inode, struct ocsfs2_defrag_result *res)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	u32 bs = sb->s_blocksize, spb = bs / 512;
	u64 isize_blk, lblk, total = 0, before = 0;
	struct defrag_run *runs = NULL;
	struct defrag_new *news = NULL;
	int nrun = 0, nnew = 0, i, ret = 0;
	struct ocsfs2_txn *txn;

	memset(res, 0, sizeof(*res));
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	inode_lock(inode);
	ret = filemap_write_and_wait(inode->i_mapping);
	if (ret)
		goto out;
	isize_blk = (i_size_read(inode) + bs - 1) / bs;
	if (isize_blk == 0)
		goto out;

	runs = kmalloc_array(OCSFS2_DEFRAG_MAX_RUNS, sizeof(*runs), GFP_NOFS);
	if (!runs) { ret = -ENOMEM; goto out; }

	/* ── pass 1: scan, validate, collect contiguous written runs ── */
	mutex_lock(&oi->i_meta_lock);
	lblk = 0;
	while (lblk < isize_blk) {
		struct ocsfs2_extent e;
		u64 next;

		if (ocsfs2_extent_find(inode, lblk, &e, &next)) {   /* hole */
			if (next == U64_MAX || next >= isize_blk)
				break;
			lblk = next;
			continue;
		}
		before++;
		if (e.flags != OCSFS2_EXT_WRITTEN) {
			/* shared/unwritten: cannot safely relocate — skip file */
			mutex_unlock(&oi->i_meta_lock);
			res->extents_before = before;     /* (partial count) */
			res->extents_after = before;
			goto out;                          /* no-op, success */
		}
		/* extend the current run if this extent is logically adjacent */
		if (nrun > 0 &&
		    runs[nrun - 1].logical + runs[nrun - 1].len == e.logical) {
			runs[nrun - 1].len += e.length;
		} else {
			if (nrun >= OCSFS2_DEFRAG_MAX_RUNS) {
				mutex_unlock(&oi->i_meta_lock);
				goto out;                  /* too sparse: skip */
			}
			runs[nrun].logical = e.logical;
			runs[nrun].len = e.length;
			nrun++;
		}
		total += e.length;
		lblk = e.logical + e.length;
	}
	res->extents_before = before;
	mutex_unlock(&oi->i_meta_lock);

	if (before <= 1 || total == 0 || total > OCSFS2_DEFRAG_MAX_BLOCKS) {
		res->extents_after = before;
		goto out;                              /* nothing to do / too big */
	}

	/* ── pass 2: allocate contiguous destinations and copy the data ── */
	news = kmalloc_array(nrun + total / OCSFS2_DEFRAG_CHUNK + nrun + 1,
			     sizeof(*news), GFP_NOFS);
	if (!news) { ret = -ENOMEM; goto out; }

	for (i = 0; i < nrun; i++) {
		u64 off = 0;            /* offset within run i, in blocks */

		while (off < runs[i].len) {
			u32 want = (u32)min((u64)OCSFS2_DEFRAG_CHUNK,
					    runs[i].len - off);
			u64 newphys, b, cl = runs[i].logical + off;

			ret = ocsfs2_alloc_blocks(sb, oi->i_ag, want, &newphys);
			if (ret == -ENOSPC && want > 1) {
				/* no run that big: fall back to smaller chunks */
				want = 1;
				ret = ocsfs2_alloc_blocks(sb, oi->i_ag, 1, &newphys);
			}
			if (ret)
				goto free_new;     /* give up: free, leave file as-is */

			/* copy [cl, cl+want) from the live (old) map to newphys */
			mutex_lock(&oi->i_meta_lock);
			for (b = cl; b < cl + want; ) {
				struct ocsfs2_extent e;
				u64 nx, sphys;
				u32 piece;

				ret = ocsfs2_extent_find(inode, b, &e, &nx);
				if (ret) { ret = -EIO; break; }
				piece = (u32)(min(e.logical + e.length,
						  cl + (u64)want) - b);
				sphys = e.physical + (b - e.logical);
				ret = ocsfs2_copy_blocks(sb, sphys,
							 newphys + (b - cl), piece);
				if (ret)
					break;
				b += piece;
			}
			mutex_unlock(&oi->i_meta_lock);
			if (ret) {
				ocsfs2_free_blocks(sb, newphys, want);
				goto free_new;
			}
			news[nnew].logical = cl;
			news[nnew].phys = newphys;
			news[nnew].len = want;
			nnew++;
			off += want;
		}
	}

	/* ── pass 3: swap the map atomically — drop old, rebuild from news ── */
	txn = ocsfs2_txn_begin(sb);
	if (!txn) { ret = -ENOMEM; goto free_new; }
	mutex_lock(&oi->i_meta_lock);
	defrag_drop_all(inode);                    /* frees old data + tree */
	for (i = 0; i < nnew; i++) {
		ret = ocsfs2_extent_insert(inode, news[i].logical, news[i].phys,
					   news[i].len, OCSFS2_EXT_WRITTEN);
		if (ret)
			break;
		inode->i_blocks += (u64)news[i].len * spb;
	}
	if (!ret)
		ret = ocsfs2_write_inode_block(inode);
	mutex_unlock(&oi->i_meta_lock);
	if (ret) {
		ocsfs2_txn_abort(txn);
		ocsfs2_reload_extents(inode);      /* restore old map from disk */
		goto free_new;                     /* new runs leak -> fsck reclaims */
	}
	ret = ocsfs2_txn_commit(txn);
	if (ret) {
		ocsfs2_reload_extents(inode);
		goto out;
	}

	/* drop now stale-mapped clean folios so reads resolve to the new extents */
	truncate_inode_pages_range(inode->i_mapping, 0, isize_blk * bs - 1);

	res->runs_relocated = nnew;
	res->blocks_relocated = total;
	mutex_lock(&oi->i_meta_lock);
	res->extents_after = 0;
	lblk = 0;
	while (lblk < isize_blk) {              /* recount for the report */
		struct ocsfs2_extent e;
		u64 next;

		if (ocsfs2_extent_find(inode, lblk, &e, &next)) {
			if (next == U64_MAX || next >= isize_blk)
				break;
			lblk = next;
			continue;
		}
		res->extents_after++;
		lblk = e.logical + e.length;
	}
	mutex_unlock(&oi->i_meta_lock);
	kfree(runs);
	kfree(news);
	inode_unlock(inode);
	return 0;

free_new:
	for (i = 0; i < nnew; i++)
		ocsfs2_free_blocks(sb, news[i].phys, news[i].len);
out:
	kfree(runs);
	kfree(news);
	inode_unlock(inode);
	return ret;
}
