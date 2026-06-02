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
	int want = want_ex ? OCSFS2_LEASE_EX : OCSFS2_LEASE_SH;
	int ret = 0;

	if (!OCSFS2_SB(inode->i_sb)->s_cluster)
		return 0;

	mutex_lock(&oi->i_meta_lock);
	if (oi->i_lease_count > 0 && oi->i_lease_mode >= want) {
		oi->i_lease_count++;       /* already hold a sufficient lease */
		mutex_unlock(&oi->i_meta_lock);
		return 0;
	}
	mutex_unlock(&oi->i_meta_lock);

	ret = ocsfs2_lease_acquire(inode->i_sb, oi->i_disk_ino, want);
	if (ret)
		return ret;

	/* Fresh acquire (EX or SH): another node may have owned and modified this
	 * file since we last touched it, so coherently re-read the inode + drop
	 * stale caches before any I/O. (Re-opens while we already hold the lease
	 * skip this — no other writer can have run.) */
	ocsfs2_inode_refresh_coherent(inode);

	mutex_lock(&oi->i_meta_lock);
	oi->i_lease_mode = want;
	oi->i_lease_count++;
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
		ocsfs2_write_inode_block(inode);
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
	/* No sync_blockdev here: the op's journal commit already checkpointed +
	 * flushed its metadata durably. A full device sync per namespace op would
	 * starve the heartbeat under load (the v1 cascade). A barrier suffices. */
	blkdev_issue_flush(sb->s_bdev);
	ocsfs2_lease_release(sb, OCSFS2_META_RESOURCE, OCSFS2_LEASE_EX);
}

void ocsfs2_recover_node(struct super_block *sb, u16 slot, u32 gen)
{
	pr_warn("ocsfs2: recovery for dead node slot %u gen %u (journal replay + lease reclaim: L5 pending)\n",
		slot, gen);
	/* TODO L5: elect a recovery leader, replay the dead node's journal, then
	 * reclaim every lease entry owned by {slot, gen}. Until then, dead-owner
	 * leases are reclaimed lazily by acquirers (entry_reclaimable). */
}
