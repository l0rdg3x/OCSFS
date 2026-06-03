// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — lease.c
 * L4 ownership leases (single-writer ownership, spec §6.5) and L5 recovery.
 *
 * Each regular file's data is owned EX by at most one node (acquired at
 * open-for-write, released at last close); read-only opens take SH. Ownership
 * lives in the on-disk lease table, indexed by inode number, mutated by a true
 * optimistic CAS over SCSI CAW (read entry -> check conflicts -> CAW with the
 * exact bytes we read; a miscompare means a peer changed it, so we re-evaluate
 * rather than blindly overwrite). A lease is honoured only while its owner is
 * ALIVE with a matching generation (liveness-epoch); a dead owner's lease is
 * reclaimable. Taking EX coherently re-reads the inode (handoff). No-op on a
 * single-node volume.
 */
#include "ocsfs.h"
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

#define LEASE_PROBE_MAX  64
#define LEASE_CAS_TRIES  16

static void le_csum(struct ocsfs2_disk_lease *le)
{
	le->l_checksum = 0;
	le->l_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, le,
			 offsetof(struct ocsfs2_disk_lease, l_checksum)));
}

static bool sh_test(const __le32 *bm, u16 slot) { return le32_to_cpu(bm[slot / 32]) & (1u << (slot % 32)); }
static void sh_set(__le32 *bm, u16 slot)   { bm[slot / 32] = cpu_to_le32(le32_to_cpu(bm[slot / 32]) |  (1u << (slot % 32))); }
static void sh_clr(__le32 *bm, u16 slot)   { bm[slot / 32] = cpu_to_le32(le32_to_cpu(bm[slot / 32]) & ~(1u << (slot % 32))); }

/* any SH holder other than @self currently alive? */
static bool sh_live_other(struct super_block *sb, const struct ocsfs2_disk_lease *le,
			  u16 self)
{
	struct ocsfs2_cluster *c = OCSFS2_SB(sb)->s_cluster;
	u16 s;

	for (s = 0; s < c->max_nodes; s++)
		if (s != self && sh_test(le->l_sh_holders, s) &&
		    ocsfs2_node_alive_any(sb, s))
			return true;
	return false;
}

/* Is this entry's owner (and any SH holder) dead, so the slot is reclaimable
 * by a different resource? */
static bool entry_reclaimable(struct super_block *sb, const struct ocsfs2_disk_lease *le)
{
	u16 owner = le16_to_cpu(le->l_owner_slot);
	u32 ogen = le32_to_cpu(le->l_owner_gen);

	if (owner != OCSFS2_SLOT_NONE && ocsfs2_node_alive(sb, owner, ogen))
		return false;
	return !sh_live_other(sb, le, OCSFS2_SLOT_NONE);
}

/* Core lease table CAS. acquire=true claims @mode for self; acquire=false
 * releases self's hold. Returns 0, -EBUSY (live conflict), or an errno. */
static int lease_modify(struct super_block *sb, u64 resource, int mode, bool acquire)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_cluster *c = sbi->s_cluster;
	unsigned int lbs = bdev_logical_block_size(sb->s_bdev);
	u16 self = c->self_slot;
	u8 *old, *new;
	int ret = -EBUSY, tries;

	old = kmalloc(lbs, GFP_NOFS);
	new = kmalloc(lbs, GFP_NOFS);
	if (!old || !new) { kfree(old); kfree(new); return -ENOMEM; }

	mutex_lock(&c->lease_lock);
	for (tries = 0; tries < LEASE_CAS_TRIES; tries++) {
		int probe;
		bool done = false;

		for (probe = 0; probe < LEASE_PROBE_MAX && !done; probe++) {
			u64 idx = (resource + probe) % sbi->s_lease_count;
			u64 byte = sbi->s_lease_table_off + idx * OCSFS2_LEASE_ENTRY_SIZE;
			u64 blk = byte & ~((u64)lbs - 1);
			unsigned int off = byte - blk;
			struct ocsfs2_disk_lease *le, *ne;
			bool used, mine;
			u16 owner, emode;
			u32 ogen;

			ret = ocsfs2_cl_bio(sb, blk, old, lbs, REQ_OP_READ);
			if (ret)
				goto out;
			le = (struct ocsfs2_disk_lease *)(old + off);
			used = le32_to_cpu(le->l_magic) == OCSFS2_LEASE_MAGIC;
			mine = used && le64_to_cpu(le->l_resource_id) == resource;

			if (used && !mine) {
				/* slot holds a different resource */
				if (acquire && entry_reclaimable(sb, le))
					;            /* steal the slot for us */
				else
					continue;    /* probe next */
			}
			if (!acquire && !mine) {       /* nothing of ours to release */
				ret = 0;
				goto out;
			}

			owner = used ? le16_to_cpu(le->l_owner_slot) : OCSFS2_SLOT_NONE;
			emode = used ? le16_to_cpu(le->l_mode) : OCSFS2_LEASE_NONE;
			ogen  = used ? le32_to_cpu(le->l_owner_gen) : 0;

			if (acquire) {
				/* conflict checks (liveness-epoch) */
				if (emode == OCSFS2_LEASE_EX && owner != self &&
				    owner != OCSFS2_SLOT_NONE &&
				    ocsfs2_node_alive(sb, owner, ogen)) {
					ret = -EBUSY;
					goto out;
				}
				if (mode == OCSFS2_LEASE_EX &&
				    emode == OCSFS2_LEASE_SH &&
				    sh_live_other(sb, le, self)) {
					ret = -EBUSY;
					goto out;
				}
			}

			/* build the new entry in @new (copy of @old, modified) */
			memcpy(new, old, lbs);
			ne = (struct ocsfs2_disk_lease *)(new + off);
			if (acquire) {
				if (!mine) {           /* fresh / stolen slot */
					memset(ne, 0, sizeof(*ne));
					ne->l_magic = cpu_to_le32(OCSFS2_LEASE_MAGIC);
					ne->l_resource_id = cpu_to_le64(resource);
					ne->l_owner_slot = cpu_to_le16(OCSFS2_SLOT_NONE);
				}
				if (mode == OCSFS2_LEASE_EX) {
					ne->l_owner_slot = cpu_to_le16(self);
					ne->l_owner_gen = cpu_to_le32(c->mount_gen);
					ne->l_mode = cpu_to_le16(OCSFS2_LEASE_EX);
				} else {
					ne->l_mode = cpu_to_le16(OCSFS2_LEASE_SH);
					ne->l_owner_slot = cpu_to_le16(OCSFS2_SLOT_NONE);
					sh_set(ne->l_sh_holders, self);
				}
			} else {
				/* release self's hold */
				if (mode == OCSFS2_LEASE_EX) {
					ne->l_mode = cpu_to_le16(OCSFS2_LEASE_NONE);
					ne->l_owner_slot = cpu_to_le16(OCSFS2_SLOT_NONE);
				} else {
					sh_clr(ne->l_sh_holders, self);
					if (le16_to_cpu(ne->l_owner_slot) == OCSFS2_SLOT_NONE) {
						u16 s; bool any = false;
						for (s = 0; s < c->max_nodes; s++)
							if (sh_test(ne->l_sh_holders, s)) { any = true; break; }
						if (!any)
							ne->l_mode = cpu_to_le16(OCSFS2_LEASE_NONE);
					}
				}
			}
			ne->l_seq = cpu_to_le32(le32_to_cpu(le->l_seq) + 1);
			le_csum(ne);

			ret = ocsfs2_scsi_caw(sb, blk / lbs, old, new, lbs);
			if (ret == 0)
				goto out;          /* committed */
			/* miscompare: a peer changed the block — re-evaluate */
			done = true;               /* break the probe loop, retry outer */
		}
		if (!done && probe >= LEASE_PROBE_MAX) {
			ret = acquire ? -ENOSPC : 0;   /* no slot found */
			break;
		}
	}
out:
	mutex_unlock(&c->lease_lock);
	kfree(old);
	kfree(new);
	return ret;
}

int ocsfs2_lease_acquire(struct super_block *sb, u64 resource, int mode)
{
	if (!OCSFS2_SB(sb)->s_cluster)
		return 0;
	return lease_modify(sb, resource, mode, true);
}

void ocsfs2_lease_release(struct super_block *sb, u64 resource, int mode)
{
	if (!OCSFS2_SB(sb)->s_cluster)
		return;
	lease_modify(sb, resource, mode, false);
}

/* ── per-inode lease management (driven by open/release) ── */

int ocsfs2_inode_open_lease(struct inode *inode, bool want_ex)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	int ret = 0;

	if (!OCSFS2_SB(inode->i_sb)->s_cluster)
		return 0;

	/* A WRITE open DEFERS the EX lease to the first actual write
	 * (ocsfs2_inode_ensure_writable). This is what makes online live migration
	 * work: the destination QEMU must open(O_RDWR) the disk during -incoming
	 * while the SOURCE still owns it. Taking EX here would either deny the open
	 * (-EBUSY) or, if it blocked, deadlock the migration (the source only
	 * releases at switchover, which needs the destination already set up). The
	 * single-writer invariant is preserved because the EX is still exclusive —
	 * it's just acquired at first write, by which point the source has released.
	 * Only one node ever holds EX at an instant. */
	if (want_ex) {
		mutex_lock(&oi->i_meta_lock);
		oi->i_lease_count++;
		mutex_unlock(&oi->i_meta_lock);
		return 0;
	}

	/* READ open: take SH so reads are coherent. */
	mutex_lock(&oi->i_meta_lock);
	if (oi->i_lease_count > 0 && oi->i_lease_mode >= OCSFS2_LEASE_SH) {
		oi->i_lease_count++;       /* already hold a sufficient lease */
		mutex_unlock(&oi->i_meta_lock);
		return 0;
	}
	mutex_unlock(&oi->i_meta_lock);

	ret = ocsfs2_lease_acquire(inode->i_sb, oi->i_disk_ino, OCSFS2_LEASE_SH);
	if (ret)
		return ret;
	ocsfs2_inode_refresh_coherent(inode);   /* re-read fresh after a peer */
	mutex_lock(&oi->i_meta_lock);
	if (oi->i_lease_mode == OCSFS2_LEASE_NONE)
		oi->i_lease_mode = OCSFS2_LEASE_SH;
	oi->i_lease_count++;
	mutex_unlock(&oi->i_meta_lock);
	return 0;
}

/* Acquire the EX (write-owner) lease before a write, if not already held — the
 * deferred half of ocsfs2_inode_open_lease. BLOCKS until the lease is free: at a
 * live-migration switchover the source keeps EX until its QEMU closes the disk,
 * so the destination's first write waits for that hand-off (a few ms once the
 * source QEMU exits) rather than failing. ~30 s cap so a wedged peer surfaces.
 * MUST be called by every path that writes a file's data or metadata, or two
 * nodes could write the same file. */
int ocsfs2_inode_ensure_writable(struct inode *inode)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	int ret = 0, tries;

	if (!OCSFS2_SB(inode->i_sb)->s_cluster)
		return 0;

	mutex_lock(&oi->i_meta_lock);
	if (oi->i_lease_mode == OCSFS2_LEASE_EX) {
		mutex_unlock(&oi->i_meta_lock);
		return 0;                  /* already the write owner */
	}
	mutex_unlock(&oi->i_meta_lock);

	for (tries = 0; tries < 6000; tries++) {     /* 6000 * 5 ms = 30 s */
		ret = ocsfs2_lease_acquire(inode->i_sb, oi->i_disk_ino,
					   OCSFS2_LEASE_EX);
		if (ret != -EBUSY)
			break;
		msleep(5);
	}
	if (ret)
		return ret;

	/* Fresh EX: a peer (the migration source) may have written since, so drop
	 * stale caches + re-read the inode before this write lands. */
	ocsfs2_inode_refresh_coherent(inode);
	mutex_lock(&oi->i_meta_lock);
	oi->i_lease_mode = OCSFS2_LEASE_EX;
	mutex_unlock(&oi->i_meta_lock);
	return 0;
}

void ocsfs2_inode_close_lease(struct inode *inode)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	int mode;

	if (!OCSFS2_SB(inode->i_sb)->s_cluster)
		return;

	mutex_lock(&oi->i_meta_lock);
	if (oi->i_lease_count > 0)
		oi->i_lease_count--;
	if (oi->i_lease_count > 0 || oi->i_lease_mode == OCSFS2_LEASE_NONE) {
		mutex_unlock(&oi->i_meta_lock);
		return;
	}
	mode = oi->i_lease_mode;
	oi->i_lease_mode = OCSFS2_LEASE_NONE;
	mutex_unlock(&oi->i_meta_lock);

	/* flush our data + metadata before handing off so the next owner sees it */
	if (mode == OCSFS2_LEASE_EX) {
		filemap_write_and_wait(inode->i_mapping);
		ocsfs2_csum_flush(inode->i_sb);   /* deferred data csums -> disk before
						   * a peer reads + inline-verifies them */
		ocsfs2_write_inode_block(inode);
		/* A9: write the DEFERRED journal metadata (extent btree etc.) to its home
		 * blocks now — it is journal-owned, not on the dirty list, so the
		 * sync_blockdev below would miss it. The next owner then reads current
		 * home blocks coherently. */
		ocsfs2_journal_checkpoint(inode->i_sb);
		sync_blockdev(inode->i_sb->s_bdev);
	}
	ocsfs2_lease_release(inode->i_sb, oi->i_disk_ino, mode);
}

/* ── global metadata lease (L4b: cross-node namespace + allocation) ──
 * Serialises metadata-mutating ops cluster-wide and makes shared metadata
 * (directories, inode table, bitmap) coherent: on acquire we drop stale clean
 * metadata buffers (invalidate_bdev) and re-read the affected directory inode;
 * on release we flush all dirty metadata so the next holder sees it. Coarse but
 * correct — namespace ops are rare in the VM-disk workload. */
void ocsfs2_meta_lock(struct super_block *sb, struct inode *dir, struct inode *dir2)
{
	int ret, tries = 0;

	if (!OCSFS2_SB(sb)->s_cluster)
		return;
	do {
		ret = ocsfs2_lease_acquire(sb, OCSFS2_META_RESOURCE, OCSFS2_LEASE_EX);
		if (ret == 0 || ret != -EBUSY)
			break;
		msleep(5);
	} while (++tries < 2000);          /* wait up to ~10s for a peer to release */
	if (ret)
		pr_warn_ratelimited("ocsfs2: meta lease acquire: %d\n", ret);

	/* Coherence is by fresh reads (ocsfs2_meta_bread) + CAW (inode table,
	 * bitmap), not a whole-device cache flush — invalidate_bdev/sync_blockdev
	 * per op would starve the heartbeat (the v1 cascade). We only re-read the
	 * directory inodes' in-core state here; their data blocks are read fresh. */
	if (dir)
		ocsfs2_inode_refresh_coherent(dir);
	if (dir2 && dir2 != dir)
		ocsfs2_inode_refresh_coherent(dir2);
}

void ocsfs2_meta_unlock(struct super_block *sb)
{
	if (!OCSFS2_SB(sb)->s_cluster)
		return;
	/* A9: the op's journal commit is now DEFERRED (no per-op checkpoint), so
	 * write the deferred metadata (dir / extent btree / xattr) to its home blocks
	 * before releasing the lease — the next holder reads home coherently. The
	 * checkpoint already issues a flush; no extra sync_blockdev (a full device
	 * sync per namespace op would starve the heartbeat, the v1 cascade). */
	ocsfs2_journal_checkpoint(sb);
	ocsfs2_lease_release(sb, OCSFS2_META_RESOURCE, OCSFS2_LEASE_EX);
}

/* L5: eagerly reclaim every lease entry held by a dead instance {slot, gen}.
 * Scans the whole table block-by-block; each block with stale holders is
 * rewritten atomically via CAW (so it races safely with live peers' lazy
 * reclaim). Caller holds the metadata lease + recovery-leader lease. */
static void reclaim_dead_leases(struct super_block *sb, u16 slot, u32 gen)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_cluster *c = sbi->s_cluster;
	unsigned int lbs = bdev_logical_block_size(sb->s_bdev);
	unsigned int per_blk = lbs / OCSFS2_LEASE_ENTRY_SIZE;
	u64 total_blks = (sbi->s_lease_count + per_blk - 1) / per_blk;
	u64 base_blk = sbi->s_lease_table_off / lbs;
	u8 *old, *new;
	u64 b;
	int reclaimed = 0;

	old = kmalloc(lbs, GFP_NOFS);
	new = kmalloc(lbs, GFP_NOFS);
	if (!old || !new) { kfree(old); kfree(new); return; }

	mutex_lock(&c->lease_lock);
	for (b = 0; b < total_blks; b++) {
		u64 blk = base_blk + b;
		int tries;

		for (tries = 0; tries < LEASE_CAS_TRIES; tries++) {
			bool changed = false;
			unsigned int e;

			if (ocsfs2_cl_bio(sb, blk * lbs, old, lbs, REQ_OP_READ))
				break;
			memcpy(new, old, lbs);
			for (e = 0; e < per_blk; e++) {
				struct ocsfs2_disk_lease *le =
					(struct ocsfs2_disk_lease *)(new + e * OCSFS2_LEASE_ENTRY_SIZE);
				bool touched = false;

				if (le32_to_cpu(le->l_magic) != OCSFS2_LEASE_MAGIC)
					continue;
				if (le16_to_cpu(le->l_owner_slot) == slot &&
				    le32_to_cpu(le->l_owner_gen) == gen) {
					le->l_owner_slot = cpu_to_le16(OCSFS2_SLOT_NONE);
					le->l_owner_gen = 0;
					touched = true;
				}
				if (sh_test(le->l_sh_holders, slot)) {
					sh_clr(le->l_sh_holders, slot);
					touched = true;
				}
				if (!touched)
					continue;
				/* recompute the surviving mode */
				if (le16_to_cpu(le->l_owner_slot) != OCSFS2_SLOT_NONE) {
					le->l_mode = cpu_to_le16(OCSFS2_LEASE_EX);
				} else {
					u16 s; bool any = false;
					for (s = 0; s < c->max_nodes; s++)
						if (sh_test(le->l_sh_holders, s)) { any = true; break; }
					le->l_mode = cpu_to_le16(any ? OCSFS2_LEASE_SH
								     : OCSFS2_LEASE_NONE);
				}
				le->l_seq = cpu_to_le32(le32_to_cpu(le->l_seq) + 1);
				le_csum(le);
				changed = true;
			}
			if (!changed)
				break;                  /* nothing stale in this block */
			if (ocsfs2_scsi_caw(sb, blk, old, new, lbs) == 0) {
				reclaimed++;
				break;                  /* committed */
			}
			/* miscompare: a peer mutated the block — re-read and retry */
		}
	}
	mutex_unlock(&c->lease_lock);
	kfree(old);
	kfree(new);
	if (reclaimed)
		pr_info("ocsfs2: recovery: reclaimed leases of dead slot %u gen %u (%d block(s))\n",
			slot, gen, reclaimed);
}

struct ocsfs2_recover_work {
	struct work_struct work;
	struct super_block *sb;
	u16 slot;
	u32 gen;
};

/* Recovery body (runs on the recovery workqueue, off the heartbeat path).
 * 1. become recovery leader for this dead slot (EX lease, lost => peer leads);
 * 2. take the metadata lease so no live peer mutates shared metadata;
 * 3. replay the dead node's journal (redo its committed dir-block txn);
 * 4. reclaim every lease the dead instance held;
 * 5. drop the metadata + recovery leases. */
static void recover_work_fn(struct work_struct *w)
{
	struct ocsfs2_recover_work *rw =
		container_of(w, struct ocsfs2_recover_work, work);
	struct super_block *sb = rw->sb;
	u64 rres = OCSFS2_RECOVERY_RESOURCE(rw->slot);
	int ret;

	ret = ocsfs2_lease_acquire(sb, rres, OCSFS2_LEASE_EX);
	if (ret) {
		/* another live node is (or just finished) recovering this slot */
		pr_info("ocsfs2: recovery of slot %u led by a peer (%d)\n",
			rw->slot, ret);
		goto done;
	}

	pr_info("ocsfs2: recovery leader for dead slot %u gen %u\n",
		rw->slot, rw->gen);

	ocsfs2_meta_lock(sb, NULL, NULL);
	ret = ocsfs2_journal_replay_slot(sb, rw->slot);
	if (ret && ret != -EUCLEAN)
		pr_warn("ocsfs2: recovery: journal replay of slot %u: %d\n",
			rw->slot, ret);
	reclaim_dead_leases(sb, rw->slot, rw->gen);
	ocsfs2_meta_unlock(sb);

	ocsfs2_lease_release(sb, rres, OCSFS2_LEASE_EX);
	pr_info("ocsfs2: recovery of slot %u complete\n", rw->slot);
done:
	kfree(rw);
}

void ocsfs2_recover_node(struct super_block *sb, u16 slot, u32 gen)
{
	struct ocsfs2_cluster *c = OCSFS2_SB(sb)->s_cluster;
	struct ocsfs2_recover_work *rw;

	if (!c)
		return;
	/* Offload to a workqueue: doing CAW loops + journal replay inline in the
	 * heartbeat thread would starve our own heartbeat (the v1 cascade). If we
	 * have no workqueue, dead leases are still reclaimed lazily by acquirers
	 * (entry_reclaimable); only eager replay is skipped. */
	if (!c->recover_wq) {
		pr_warn("ocsfs2: dead slot %u gen %u — lazy reclaim only (no recover_wq)\n",
			slot, gen);
		return;
	}
	rw = kzalloc(sizeof(*rw), GFP_ATOMIC);
	if (!rw) {
		pr_warn("ocsfs2: dead slot %u gen %u — recovery deferred (ENOMEM)\n",
			slot, gen);
		return;
	}
	INIT_WORK(&rw->work, recover_work_fn);
	rw->sb = sb;
	rw->slot = slot;
	rw->gen = gen;
	queue_work(c->recover_wq, &rw->work);
}
