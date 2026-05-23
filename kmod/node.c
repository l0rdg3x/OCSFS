// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — node.c
 * Node slot table management.
 *
 * Each mounting node must claim a slot in the on-disk Node Slot Table.
 * Slot claiming uses SCSI PR to ensure atomicity: a node registers its
 * PR key, then performs a compare-and-write to claim the slot.
 *
 * Node states: FREE → ACTIVE → (EVICTING → DEAD) → FREE
 */

#include <linux/utsname.h>
#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * READ NODE TABLE — load all node slots into memory
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_node_read_table(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct buffer_head *bh;
	u64 table_off = OCSFS_NODE_SLOT_TABLE_OFF;
	u16 i;
	u32 slots_per_block;

	slots_per_block = sbi->s_block_size / sizeof(struct ocsfs_disk_node_slot);

	/*
	 * sb_bread can sleep (I/O); must NOT hold s_node_lock across it.
	 * Read each slot into a stack copy, then update in-memory state
	 * under the lock in a separate step.
	 */
	for (i = 0; i < sbi->s_max_nodes; i++) {
		struct ocsfs_disk_node_slot dns;
		struct ocsfs_disk_node_slot *pdns;
		struct ocsfs_node_info *ni;
		u64 off = table_off + (u64)i * sizeof(struct ocsfs_disk_node_slot);
		u64 block = off / sbi->s_block_size;
		u32 boff = off % sbi->s_block_size;

		/*
		 * Force a fresh read: another node may have written to this
		 * block (e.g., claiming a slot) since we last read it.
		 */
		bh = sb_getblk(sb, block);
		if (!bh)
			return -EIO;
		clear_buffer_uptodate(bh);
		if (bh_read(bh, 0) < 0) {
			brelse(bh);
			return -EIO;
		}
		pdns = (struct ocsfs_disk_node_slot *)(bh->b_data + boff);
		memcpy(&dns, pdns, sizeof(dns));
		brelse(bh);

		spin_lock(&sbi->s_node_lock);
		ni             = &sbi->s_nodes[i];
		ni->ni_slot    = i;
		ni->ni_state   = dns.ns_state;
		ni->ni_mount_gen = le32_to_cpu(dns.ns_mount_gen);
		ni->ni_pr_key  = le64_to_cpu(dns.ns_pr_key);
		ni->ni_last_hb = le64_to_cpu(dns.ns_last_heartbeat);
		memcpy(ni->ni_uuid, dns.ns_uuid, 16);
		memcpy(ni->ni_name, dns.ns_name, 64);

		if (ni->ni_state == OCSFS_NODE_ACTIVE &&
		    ocsfs_node_verify_auth(sb, &dns) < 0)
			ni->ni_state = OCSFS_NODE_SUSPECTED;
		spin_unlock(&sbi->s_node_lock);
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * WRITE NODE SLOT — persist a single slot to disk
 * ═══════════════════════════════════════════════════════════════ */

static int ocsfs_node_write_slot(struct super_block *sb, u16 slot)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_node_info *ni = &sbi->s_nodes[slot];
	struct buffer_head *bh;
	struct ocsfs_disk_node_slot *dns;
	u64 off = OCSFS_NODE_SLOT_TABLE_OFF +
		  (u64)slot * sizeof(struct ocsfs_disk_node_slot);
	u64 block = off / sbi->s_block_size;
	u32 boff = off % sbi->s_block_size;

	/*
	 * Force a fresh read so we get the current state of all slots in
	 * this block before overwriting our slot — stale cached state for
	 * other slots would be written back, losing their recent changes.
	 */
	bh = sb_getblk(sb, block);
	if (!bh)
		return -EIO;
	clear_buffer_uptodate(bh);
	if (bh_read(bh, 0) < 0) {
		brelse(bh);
		return -EIO;
	}

	dns = (struct ocsfs_disk_node_slot *)(bh->b_data + boff);

	memcpy(dns->ns_uuid, ni->ni_uuid, 16);
	memcpy(dns->ns_name, ni->ni_name, 64);
	dns->ns_state = ni->ni_state;
	dns->ns_slot_id = cpu_to_le16(slot);
	dns->ns_mount_gen = cpu_to_le32(ni->ni_mount_gen);
	dns->ns_mount_time = cpu_to_le64(ktime_get_real_ns());
	dns->ns_last_heartbeat = cpu_to_le64(ni->ni_last_hb);
	dns->ns_pr_key = cpu_to_le64(ni->ni_pr_key);

	if (sbi->s_auth_required) {
		__le32 tok = cpu_to_le32(ocsfs_crc32c(0, sbi->s_cluster_secret, 32));
		memcpy(dns->ns_auth_token, &tok, sizeof(tok));
		memset(dns->ns_auth_token + sizeof(tok), 0,
		       sizeof(dns->ns_auth_token) - sizeof(tok));
	} else {
		memset(dns->ns_auth_token, 0, sizeof(dns->ns_auth_token));
	}

	/* Compute checksum */
	dns->ns_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, dns,
			     sizeof(*dns) - sizeof(__le32)));

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * AUTH VERIFICATION — compare a slot's token against our secret
 * Protects against stray nodes joining the wrong cluster.
 * Token is CRC32C(secret,32) stored as LE32 in ns_auth_token[0..3].
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_node_verify_auth(struct super_block *sb,
			    const struct ocsfs_disk_node_slot *dns)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	__le32 expected, got;

	if (!sbi->s_auth_required)
		return 0;

	expected = cpu_to_le32(ocsfs_crc32c(0, sbi->s_cluster_secret, 32));
	memcpy(&got, dns->ns_auth_token, sizeof(got));
	if (got != expected) {
		pr_warn("ocsfs: node slot %u auth mismatch — wrong cluster secret?\n",
			le16_to_cpu(dns->ns_slot_id));
		return -EACCES;
	}
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * CLAIM SLOT — find a free slot and claim it for this node
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_node_claim_slot(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_node_info *ni;
	u16 i;
	int ret;

	/* Read current table state */
	ret = ocsfs_node_read_table(sb);
	if (ret)
		return ret;

	spin_lock(&sbi->s_node_lock);

	/* First: look for a DEAD slot from a previous mount of this node */
	for (i = 0; i < sbi->s_max_nodes; i++) {
		ni = &sbi->s_nodes[i];
		if (ni->ni_state == OCSFS_NODE_DEAD &&
		    memcmp(ni->ni_uuid, sbi->s_node_uuid, 16) == 0) {
			goto claim;
		}
	}

	/* Second: look for any FREE slot */
	for (i = 0; i < sbi->s_max_nodes; i++) {
		ni = &sbi->s_nodes[i];
		if (ni->ni_state == OCSFS_NODE_FREE)
			goto claim;
	}

	/*
	 * Third: reclaim any DEAD slot whose node has already been fully
	 * recovered (journal replayed, locks released, slot marked DEAD).
	 * Without this, UUID rotation on each mount causes DEAD slots to
	 * accumulate — each crash cycle consumes one slot permanently,
	 * leading to slot exhaustion after max_nodes crash cycles.
	 */
	for (i = 0; i < sbi->s_max_nodes; i++) {
		ni = &sbi->s_nodes[i];
		if (ni->ni_state == OCSFS_NODE_DEAD)
			goto claim;
	}

	spin_unlock(&sbi->s_node_lock);
	pr_err("ocsfs: no free node slots (max=%u)\n", sbi->s_max_nodes);
	return -ENOSPC;

claim:
	/* Fill in our info */
	sbi->s_node_slot = i;
	ni->ni_state = OCSFS_NODE_ACTIVE;
	ni->ni_mount_gen++;
	sbi->s_mount_gen = ni->ni_mount_gen;
	memcpy(ni->ni_uuid, sbi->s_node_uuid, 16);
	memcpy(ni->ni_name, sbi->s_node_name, 64);
	ni->ni_pr_key = ocsfs_pr_make_key(sbi->s_node_uuid,
					    sbi->s_mount_gen);
	ni->ni_last_hb = ktime_get_real_ns();

	spin_unlock(&sbi->s_node_lock);

	/* Persist to disk */
	ret = ocsfs_node_write_slot(sb, i);
	if (ret) {
		pr_err("ocsfs: failed to write node slot %u\n", i);
		return ret;
	}

	pr_info("ocsfs: claimed node slot %u (gen=%u)\n",
		i, sbi->s_mount_gen);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * RELEASE SLOT — mark our slot as FREE on unmount
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_node_release_slot(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_node_info *ni;
	int ret;

	spin_lock(&sbi->s_node_lock);
	ni = &sbi->s_nodes[sbi->s_node_slot];
	ni->ni_state = OCSFS_NODE_FREE;
	spin_unlock(&sbi->s_node_lock);

	ret = ocsfs_node_write_slot(sb, sbi->s_node_slot);
	if (ret)
		pr_err("ocsfs: failed to release node slot %u on disk: %d\n",
		       sbi->s_node_slot, ret);

	pr_info("ocsfs: released node slot %u\n", sbi->s_node_slot);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * MARK DEAD — used during recovery to mark a failed node
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_node_mark_dead(struct super_block *sb, u16 slot)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_node_info *ni;

	if (slot >= sbi->s_max_nodes)
		return -EINVAL;

	spin_lock(&sbi->s_node_lock);
	ni = &sbi->s_nodes[slot];
	ni->ni_state = OCSFS_NODE_DEAD;
	spin_unlock(&sbi->s_node_lock);

	return ocsfs_node_write_slot(sb, slot);
}

/* ═══════════════════════════════════════════════════════════════
 * INIT / EXIT
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_node_init(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	char hostname[64];
	int ret;

	spin_lock_init(&sbi->s_node_lock);

	/* Get machine identity */
	memset(sbi->s_node_uuid, 0, 16);
	generate_random_uuid(sbi->s_node_uuid);

	memset(hostname, 0, sizeof(hostname));
	/* Use the init_uts_ns for hostname */
	memcpy(sbi->s_node_name, init_uts_ns.name.nodename,
	       min(sizeof(sbi->s_node_name),
		   sizeof(init_uts_ns.name.nodename)));

	/* Claim a slot */
	ret = ocsfs_node_claim_slot(sb);
	if (ret)
		return ret;

	/* Register PR key */
	sbi->s_pr.pr_key = ocsfs_pr_make_key(sbi->s_node_uuid,
					       sbi->s_mount_gen);
	ret = ocsfs_pr_register(sb, sbi->s_pr.pr_key);
	if (ret) {
		ocsfs_node_release_slot(sb);
		return ret;
	}

	return 0;
}

void ocsfs_node_exit(struct super_block *sb)
{
	ocsfs_pr_unregister(sb);
	ocsfs_node_release_slot(sb);
}

/* ═══════════════════════════════════════════════════════════════
 * CLUSTER INIT / EXIT — bring up / tear down all Phase 2 subsystems
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_cluster_init(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	int ret;

	sbi->s_clustered = (sbi->s_max_nodes > 1);

	if (!sbi->s_clustered) {
		pr_info("ocsfs: single-node mode (max_nodes=1)\n");
		sbi->s_node_slot = 0;
		sbi->s_mount_gen = 1;
		ocsfs_dlm_init(sb);
		ocsfs_recovery_init(sb);
		return 0;
	}

	if (sbi->s_auth_required)
		pr_info("ocsfs: cluster auth enabled\n");

	ret = ocsfs_recovery_init(sb);
	if (ret)
		return ret;

	ret = ocsfs_dlm_init(sb);
	if (ret)
		goto fail_recovery;

	ret = ocsfs_node_init(sb);
	if (ret)
		goto fail_dlm;

	ret = ocsfs_heartbeat_start(sb);
	if (ret)
		goto fail_node;

	pr_info("ocsfs: cluster mode active (slot %u, gen %u, "
		"PR key 0x%016llx)\n",
		sbi->s_node_slot, sbi->s_mount_gen, sbi->s_pr.pr_key);
	return 0;

fail_node:
	ocsfs_node_exit(sb);
fail_dlm:
	ocsfs_dlm_exit(sb);
fail_recovery:
	ocsfs_recovery_exit(sb);
	return ret;
}

void ocsfs_cluster_exit(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	if (sbi->s_clustered) {
		ocsfs_heartbeat_stop(sb);
		/*
		 * Cancel recovery work before tearing down DLM and node state.
		 * The work function accesses both — if it races with dlm_exit
		 * or node_exit, it can dereference freed/zeroed structures.
		 */
		ocsfs_recovery_exit(sb);
		ocsfs_dlm_exit(sb);
		ocsfs_node_exit(sb);
	} else {
		ocsfs_recovery_exit(sb);
	}
}
