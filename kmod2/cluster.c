// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — cluster.c
 * L3 membership: node-slot claim, per-node heartbeat, and the cluster_ops
 * liveness/fencing provider (spec §4). Coordination blocks (node table,
 * heartbeat) are read via direct bios and written via SCSI CAW so they are
 * coherent across nodes (each node's buffer cache is private — the v1 lesson).
 * Liveness uses the OBSERVER's clock (a peer is alive while we keep seeing its
 * heartbeat sequence advance), avoiding cross-node clock skew.
 *
 * No-op on a single-node volume (s_max_nodes <= 1): s_cluster stays NULL and
 * the data path runs exactly as before.
 */
#include "ocsfs.h"
#include <linux/bio.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/moduleparam.h>

/* DEBUG/TEST: when set, this node stops writing its heartbeat (its sequence
 * stops advancing) so peers declare it dead — a deterministic death-detection
 * test without a real crash. Never set in production. */
static bool ocsfs2_hb_pause;
module_param_named(hb_pause, ocsfs2_hb_pause, bool, 0644);
MODULE_PARM_DESC(hb_pause, "TEST: stop heartbeating to simulate node death");

#define HB_INTERVAL_MS   2000
#define HB_DEATH_FACTOR  4        /* dead after 4 missed intervals */
#define CAW_RETRIES      8

/* ── coherent block I/O for coordination regions (bypasses the buffer cache) ── */
static int cl_bio(struct super_block *sb, u64 byte_off, void *buf,
		  unsigned int len, blk_opf_t op)
{
	struct bio bio;
	struct bio_vec bv;
	struct page *page;
	int ret;

	page = virt_to_page(buf);
	bio_init(&bio, sb->s_bdev, &bv, 1, op);
	bio.bi_iter.bi_sector = byte_off >> 9;
	__bio_add_page(&bio, page, len, offset_in_page(buf));
	ret = submit_bio_wait(&bio);
	bio_uninit(&bio);
	return ret;
}

/* Read-modify-CAW one record inside its logical block, retrying on contention.
 * @cur must already hold our intended record; we re-read the block, splice our
 * record back in, and CAW. Returns 0 on success, -EBUSY if it never settled. */
static int cl_caw_record(struct super_block *sb, u64 byte_off,
			 const void *rec, unsigned int rec_len)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	unsigned int lbs = bdev_logical_block_size(sb->s_bdev);
	u64 blk_off = byte_off & ~((u64)lbs - 1);
	unsigned int off = byte_off - blk_off;
	u8 *old, *new;
	int ret = -EBUSY, i;

	if (!sbi->s_cluster->caw_ok)
		return -ENOTSUPP;
	old = kmalloc(lbs, GFP_NOFS);
	new = kmalloc(lbs, GFP_NOFS);
	if (!old || !new) {
		kfree(old);
		kfree(new);
		return -ENOMEM;
	}
	for (i = 0; i < CAW_RETRIES; i++) {
		ret = cl_bio(sb, blk_off, old, lbs, REQ_OP_READ);
		if (ret)
			break;
		memcpy(new, old, lbs);
		memcpy(new + off, rec, rec_len);
		/* scsi_caw wants the LBA in units of @lbs (device logical blocks) */
		ret = ocsfs2_scsi_caw(sb, blk_off / lbs, old, new, lbs);
		if (ret == 0)
			break;          /* committed */
		ret = -EBUSY;           /* miscompare: peer changed it, retry */
	}
	kfree(old);
	kfree(new);
	return ret;
}

/* ── heartbeat record ── */
static void hb_fill(struct ocsfs2_disk_heartbeat *hb, u16 slot, u32 gen, u64 seq,
		    u8 state)
{
	memset(hb, 0, sizeof(*hb));
	hb->hb_magic = cpu_to_le32(0);   /* magic unused; state/seq carry liveness */
	hb->hb_node_slot = cpu_to_le16(slot);
	hb->hb_state = cpu_to_le16(state);
	hb->hb_sequence = cpu_to_le64(seq);
	hb->hb_mount_gen = cpu_to_le32(gen);
	hb->hb_timestamp = cpu_to_le64(seq);   /* observer-clock: seq is the clock */
	hb->hb_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, hb,
			  offsetof(struct ocsfs2_disk_heartbeat, hb_checksum)));
}

static int hb_write_self(struct super_block *sb)
{
	struct ocsfs2_cluster *c = OCSFS2_SB(sb)->s_cluster;
	u64 off = OCSFS2_SB(sb)->s_heartbeat_off +
		  (u64)c->self_slot * OCSFS2_HEARTBEAT_SIZE;
	struct ocsfs2_disk_heartbeat hb;

	c->hb_seq++;
	hb_fill(&hb, c->self_slot, c->mount_gen, c->hb_seq, OCSFS2_NODE_ACTIVE);
	return cl_caw_record(sb, off, &hb, sizeof(hb));
}

/* Read every peer heartbeat and refresh liveness tracking; declare death after
 * the window. Returns nothing — death triggers on_node_dead. */
static void hb_scan_peers(struct super_block *sb)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_cluster *c = sbi->s_cluster;
	unsigned int lbs = bdev_logical_block_size(sb->s_bdev);
	u8 *buf = kmalloc(lbs, GFP_NOFS);
	u16 slot;

	if (!buf)
		return;
	for (slot = 0; slot < c->max_nodes; slot++) {
		u64 off = sbi->s_heartbeat_off + (u64)slot * OCSFS2_HEARTBEAT_SIZE;
		u64 blk = off & ~((u64)lbs - 1);
		struct ocsfs2_disk_heartbeat *hb;
		struct ocsfs2_peer *p = &c->peers[slot];
		u64 seq;
		u32 gen;
		u8 state;

		if (slot == c->self_slot)
			continue;
		if (cl_bio(sb, blk, buf, lbs, REQ_OP_READ))
			continue;
		hb = (struct ocsfs2_disk_heartbeat *)(buf + (off - blk));
		seq = le64_to_cpu(hb->hb_sequence);
		gen = le32_to_cpu(hb->hb_mount_gen);
		state = le16_to_cpu(hb->hb_state);

		if (state != OCSFS2_NODE_ACTIVE || seq == 0) {
			p->seen = false;
			continue;
		}
		if (!p->seen || seq != p->last_seq || gen != p->gen) {
			/* a changed generation means the old instance died and a new
			 * one joined; either way we re-arm liveness below */
			p->seen = true;
			p->last_seq = seq;
			p->gen = gen;
			p->last_change = jiffies;
			if (p->state != OCSFS2_NODE_ACTIVE) {
				p->state = OCSFS2_NODE_ACTIVE;
				pr_info("ocsfs2: node slot %u (gen %u) is alive\n",
					slot, gen);
			}
		} else if (p->state == OCSFS2_NODE_ACTIVE &&
			   time_after(jiffies, p->last_change + c->death_j)) {
			p->state = OCSFS2_NODE_DEAD;
			pr_warn("ocsfs2: node slot %u (gen %u) DECLARED DEAD\n",
				slot, p->gen);
			if (c->ops->on_node_dead)
				c->ops->on_node_dead(sb, slot, p->gen);
		}
	}
	kfree(buf);
}

static int hb_thread_fn(void *data)
{
	struct super_block *sb = data;
	struct ocsfs2_cluster *c = OCSFS2_SB(sb)->s_cluster;

	while (!kthread_should_stop()) {
		if (!ocsfs2_hb_pause && hb_write_self(sb))
			pr_warn_ratelimited("ocsfs2: heartbeat write failed (slot %u)\n",
					    c->self_slot);
		hb_scan_peers(sb);
		schedule_timeout_interruptible(c->hb_interval_j);
	}
	return 0;
}

/* ── cluster_ops: on-disk provider ── */
bool ocsfs2_node_alive(struct super_block *sb, u16 slot, u32 gen)
{
	struct ocsfs2_cluster *c = OCSFS2_SB(sb)->s_cluster;
	struct ocsfs2_peer *p;

	if (!c || slot >= c->max_nodes)
		return false;
	if (slot == c->self_slot)
		return true;
	p = &c->peers[slot];
	return p->state == OCSFS2_NODE_ACTIVE && p->gen == gen &&
	       !time_after(jiffies, p->last_change + c->death_j);
}

static int prov_node_alive(struct super_block *sb, u16 slot, u32 gen)
{
	return ocsfs2_node_alive(sb, slot, gen);
}

static void prov_on_node_dead(struct super_block *sb, u16 slot, u32 gen)
{
	/* fence the dead node at the fabric (its in-flight I/O is rejected),
	 * then run recovery (replay its journal + reclaim its leases) */
	if (OCSFS2_SB(sb)->s_cluster->caw_ok)
		ocsfs2_pr_preempt_abort(sb, ocsfs2_pr_make_key(
			OCSFS2_SB(sb)->s_ds->s_uuid, gen),
			OCSFS2_PR_TYPE_WRITE_EXCL_REG);
	ocsfs2_recover_node(sb, slot, gen);
}

static int prov_self_liveness_ok(struct super_block *sb)
{
	return 1;   /* on-disk provider: trust our own writes (self-fence is L5) */
}

static const struct ocsfs2_cluster_ops ocsfs2_ondisk_cluster_ops = {
	.node_alive       = prov_node_alive,
	.on_node_dead     = prov_on_node_dead,
	.self_liveness_ok = prov_self_liveness_ok,
};

/* ── node-slot claim ── */
static int claim_node_slot(struct super_block *sb, struct ocsfs2_cluster *c)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	unsigned int lbs = bdev_logical_block_size(sb->s_bdev);
	u8 *buf = kmalloc(lbs, GFP_NOFS);
	u16 slot;
	int ret = -ENOSPC;

	if (!buf)
		return -ENOMEM;
	for (slot = 0; slot < c->max_nodes; slot++) {
		u64 off = sbi->s_node_table_off + (u64)slot * OCSFS2_NODE_SLOT_SIZE;
		u64 blk = off & ~((u64)lbs - 1);
		struct ocsfs2_disk_node_slot *ns;
		u8 state;

		if (cl_bio(sb, blk, buf, lbs, REQ_OP_READ)) {
			ret = -EIO;
			break;
		}
		ns = (struct ocsfs2_disk_node_slot *)(buf + (off - blk));
		state = ns->ns_state;
		if (state == OCSFS2_NODE_ACTIVE)
			continue;          /* taken */

		/* try to claim it via CAW */
		memset(ns, 0, sizeof(*ns));
		ns->ns_magic = cpu_to_le32(OCSFS2_NODE_MAGIC);
		ns->ns_state = OCSFS2_NODE_ACTIVE;
		ns->ns_slot_id = cpu_to_le16(slot);
		memcpy(ns->ns_uuid, sbi->s_ds->s_uuid, 16);
		ns->ns_mount_gen = cpu_to_le32(c->mount_gen);
		ns->ns_pr_key = cpu_to_le64(c->pr_key);
		ns->ns_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, ns,
			offsetof(struct ocsfs2_disk_node_slot, ns_checksum)));
		ret = cl_caw_record(sb, off, ns, sizeof(*ns));
		if (ret == 0) {
			c->self_slot = slot;
			break;
		}
		/* lost the race for this slot: try the next */
		ret = -ENOSPC;
	}
	kfree(buf);
	return ret;
}

/* ── bring-up / teardown ── */
int ocsfs2_cluster_init(struct super_block *sb)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_cluster *c;
	int ret;

	if (!sbi->s_clustered)
		return 0;             /* single-node (no -o cluster): nothing to do */
	if (sbi->s_max_nodes <= 1) {
		pr_err("ocsfs2: -o cluster but volume formatted single-node (mkfs -N)\n");
		return -EINVAL;
	}

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->max_nodes = sbi->s_max_nodes;
	c->peers = kcalloc(c->max_nodes, sizeof(*c->peers), GFP_KERNEL);
	if (!c->peers) {
		kfree(c);
		return -ENOMEM;
	}
	mutex_init(&c->lease_lock);
	c->ops = &ocsfs2_ondisk_cluster_ops;
	c->hb_interval_j = msecs_to_jiffies(HB_INTERVAL_MS);
	c->death_j = msecs_to_jiffies(HB_INTERVAL_MS * HB_DEATH_FACTOR);
	c->mount_gen = (u32)(get_jiffies_64() & 0x7fffffff) | 1;
	c->caw_ok = ocsfs2_scsi_caw_probe(sb);
	if (!c->caw_ok) {
		pr_err("ocsfs2: clustered volume but device lacks SCSI CAW — refusing\n");
		ret = -EOPNOTSUPP;
		goto fail;
	}
	c->pr_key = ocsfs2_pr_make_key(sbi->s_ds->s_uuid, c->mount_gen);
	sbi->s_cluster = c;

	ret = ocsfs2_pr_register(sb, c->pr_key);
	if (ret) {
		pr_err("ocsfs2: PR register failed: %d\n", ret);
		goto fail_clear;
	}
	/* registrants-only write exclusivity: all nodes register, fenced nodes
	 * lose their registration and can no longer write */
	ocsfs2_pr_reserve(sb, OCSFS2_PR_TYPE_WRITE_EXCL_REG);

	ret = claim_node_slot(sb, c);
	if (ret) {
		pr_err("ocsfs2: could not claim a node slot: %d\n", ret);
		goto fail_pr;
	}
	sbi->s_node_slot = c->self_slot;
	sbi->s_mount_gen = c->mount_gen;

	c->hb_thread = kthread_run(hb_thread_fn, sb, "ocsfs2-hb/%s", sb->s_id);
	if (IS_ERR(c->hb_thread)) {
		ret = PTR_ERR(c->hb_thread);
		c->hb_thread = NULL;
		goto fail_slot;
	}
	pr_info("ocsfs2: cluster up — slot %u, gen %u, %u max nodes\n",
		c->self_slot, c->mount_gen, c->max_nodes);
	return 0;

fail_slot:
	/* best-effort release of the claimed slot */
fail_pr:
	ocsfs2_pr_unregister(sb);
fail_clear:
	sbi->s_cluster = NULL;
fail:
	kfree(c->peers);
	kfree(c);
	return ret;
}

void ocsfs2_cluster_exit(struct super_block *sb)
{
	struct ocsfs2_sb_info *sbi = OCSFS2_SB(sb);
	struct ocsfs2_cluster *c = sbi->s_cluster;
	u64 off;
	struct ocsfs2_disk_node_slot ns;

	if (!c)
		return;
	if (c->hb_thread)
		kthread_stop(c->hb_thread);

	/* release our node slot (mark FREE) so peers don't fence us */
	off = sbi->s_node_table_off + (u64)c->self_slot * OCSFS2_NODE_SLOT_SIZE;
	memset(&ns, 0, sizeof(ns));
	ns.ns_magic = cpu_to_le32(OCSFS2_NODE_MAGIC);
	ns.ns_state = OCSFS2_NODE_FREE;
	ns.ns_slot_id = cpu_to_le16(c->self_slot);
	ns.ns_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, &ns,
			 offsetof(struct ocsfs2_disk_node_slot, ns_checksum)));
	cl_caw_record(sb, off, &ns, sizeof(ns));

	ocsfs2_pr_unregister(sb);
	sbi->s_cluster = NULL;
	kfree(c->peers);
	kfree(c);
	pr_info("ocsfs2: cluster down (slot released)\n");
}
