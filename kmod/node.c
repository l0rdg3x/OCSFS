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
#include <linux/crypto.h>
#include <crypto/algapi.h>
#include <crypto/hash.h>
#include <crypto/sha2.h>
#include "ocsfs.h"

static int ocsfs_hmac_sha256(const u8 *key, const u8 *msg, size_t msg_len,
			      u8 *out)
{
	struct crypto_shash *tfm;
	SHASH_DESC_ON_STACK(desc, tfm);
	int ret;

	tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	ret = crypto_shash_setkey(tfm, key, 32);
	if (!ret) {
		desc->tfm = tfm;
		ret = crypto_shash_digest(desc, msg, msg_len, out);
	}
	crypto_free_shash(tfm);
	return ret;
}

/* Token: HMAC-SHA256(secret, cluster_uuid||node_uuid||mount_gen_be32) */
static void ocsfs_build_auth_msg(const struct ocsfs_sb_info *sbi,
				 const u8 *node_uuid, u32 mount_gen,
				 u8 msg[36])
{
	__be32 gen_be = cpu_to_be32(mount_gen);

	memcpy(msg,      sbi->s_ds->s_uuid, 16);
	memcpy(msg + 16, node_uuid,          16);
	memcpy(msg + 32, &gen_be,             4);
}

/* ═══════════════════════════════════════════════════════════════
 * READ NODE TABLE — load all node slots into memory
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_node_read_table(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct buffer_head *bh;
	u64 table_off = OCSFS_NODE_SLOT_TABLE_OFF;
	u16 i;

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

/*
 * ocsfs_build_new_slot — riempie `new_dns` con lo stato che vogliamo scrivere.
 * `expected_dns` è lo stato letto da disco; `ns_version` viene incrementato.
 */
static int ocsfs_build_new_slot(struct super_block *sb, u16 slot,
				const struct ocsfs_disk_node_slot *expected_dns,
				struct ocsfs_disk_node_slot *new_dns)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_node_info *ni = &sbi->s_nodes[slot];
	int ret = 0;

	memcpy(new_dns, expected_dns, sizeof(*new_dns));

	memcpy(new_dns->ns_uuid, ni->ni_uuid, 16);
	memcpy(new_dns->ns_name, ni->ni_name, 64);
	new_dns->ns_state          = ni->ni_state;
	new_dns->ns_slot_id        = cpu_to_le16(slot);
	new_dns->ns_mount_gen      = cpu_to_le32(ni->ni_mount_gen);
	new_dns->ns_mount_time     = cpu_to_le64(ktime_get_real_ns());
	new_dns->ns_last_heartbeat = cpu_to_le64(ni->ni_last_hb);
	new_dns->ns_pr_key         = cpu_to_le64(ni->ni_pr_key);
	new_dns->ns_version        = cpu_to_le32(
		le32_to_cpu(expected_dns->ns_version) + 1);

	if (sbi->s_auth_required) {
		u8 msg[36];

		ocsfs_build_auth_msg(sbi, ni->ni_uuid, ni->ni_mount_gen, msg);
		ret = ocsfs_hmac_sha256(sbi->s_cluster_secret, msg, sizeof(msg),
					new_dns->ns_auth_token);
		if (ret)
			return ret;
	} else {
		memset(new_dns->ns_auth_token, 0, sizeof(new_dns->ns_auth_token));
	}

	new_dns->ns_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, new_dns, sizeof(*new_dns) - sizeof(__le32)));
	return 0;
}

/*
 * ocsfs_node_write_slot — scrive lo slot via CAS atomico (CRIT-2 fix).
 *
 * Legge lo stato corrente on-disk, costruisce il nuovo, chiama ocsfs_atomic_cas.
 * Ritorna -EAGAIN se un altro nodo ha scritto nel frattempo (race persa).
 * Il chiamante (ocsfs_node_claim_slot) ri-scansiona la tabella su -EAGAIN.
 */
static int ocsfs_node_write_slot(struct super_block *sb, u16 slot)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 off   = OCSFS_NODE_SLOT_TABLE_OFF +
		    (u64)slot * sizeof(struct ocsfs_disk_node_slot);
	u64 block = off / sbi->s_block_size;
	u32 boff  = off % sbi->s_block_size;
	struct buffer_head *bh;
	struct ocsfs_disk_node_slot expected_dns, new_dns;
	int ret;

	bh = sb_getblk(sb, block);
	if (!bh)
		return -EIO;
	clear_buffer_uptodate(bh);
	if (bh_read(bh, 0) < 0) {
		brelse(bh);
		return -EIO;
	}

	memcpy(&expected_dns, bh->b_data + boff, sizeof(expected_dns));
	brelse(bh);

	ret = ocsfs_build_new_slot(sb, slot, &expected_dns, &new_dns);
	if (ret)
		return ret;

	ret = ocsfs_atomic_cas(sb, block, boff,
			       sizeof(struct ocsfs_disk_node_slot),
			       &expected_dns, &new_dns);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * AUTH VERIFICATION — compare a slot's token against our secret
 * Protects against stray nodes joining the wrong cluster.
 *
 * Token is HMAC-SHA256(secret, cluster_uuid||node_uuid||mount_gen_be32).
 * Including cluster_uuid and mount_gen makes tokens cluster-specific and
 * mount-specific, preventing replay attacks across clusters sharing the
 * same secret or across reboots.
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_node_verify_auth(struct super_block *sb,
			    const struct ocsfs_disk_node_slot *dns)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u8 msg[36], expected[32];
	int ret;

	if (!sbi->s_auth_required)
		return 0;

	ocsfs_build_auth_msg(sbi, dns->ns_uuid,
			     le32_to_cpu(dns->ns_mount_gen), msg);
	ret = ocsfs_hmac_sha256(sbi->s_cluster_secret, msg, sizeof(msg), expected);
	if (ret)
		return ret;

	if (crypto_memneq(expected, dns->ns_auth_token, 32)) {
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
	int attempt, ret;

	for (attempt = 0; attempt < CAS_MAX_ATTEMPTS; attempt++) {
		/* Fresh read della tabella da disco ad ogni tentativo */
		ret = ocsfs_node_read_table(sb);
		if (ret)
			return ret;

		u64 now     = ktime_get_real_ns();
		u64 dead_ns = (u64)OCSFS_HB_DEAD_MS * 1000000ULL;
		bool crash_reclaim = false;

		spin_lock(&sbi->s_node_lock);
		i = sbi->s_max_nodes;  /* sentinel: nessuno slot trovato */

		/* Prima: il NOSTRO slot (stesso UUID) lasciato da un mount
		 * precedente — DEAD (release pulito) oppure ancora ACTIVE ma con
		 * heartbeat scaduto (la nostra incarnazione è crashata senza
		 * rilasciarlo). Recuperarlo evita di consumare uno slot nuovo ad
		 * ogni remount post-crash: senza questo, dopo s_max_nodes crash
		 * non recuperati il claim fallisce con -ENOSPC e fsck segnala slot
		 * ACTIVE orfani. L'UUID è derivato in modo stabile dall'host
		 * (ocsfs_node_derive_uuid), quindi uno slot col nostro UUID è
		 * sempre una nostra incarnazione precedente, mai un altro nodo. */
		for (u16 k = 0; k < sbi->s_max_nodes; k++) {
			ni = &sbi->s_nodes[k];
			if (memcmp(ni->ni_uuid, sbi->s_node_uuid, 16) != 0)
				continue;
			if (ni->ni_state == OCSFS_NODE_DEAD ||
			    (ni->ni_state == OCSFS_NODE_ACTIVE &&
			     (now - ni->ni_last_hb) > dead_ns)) {
				/* ACTIVE = our previous incarnation crashed without
				 * releasing its DLM locks; flag it so the mount path
				 * runs lock recovery for the dead generation. */
				crash_reclaim = (ni->ni_state == OCSFS_NODE_ACTIVE);
				i = k;
				break;
			}
		}

		/* Seconda: slot FREE */
		if (i == sbi->s_max_nodes) {
			for (u16 k = 0; k < sbi->s_max_nodes; k++) {
				ni = &sbi->s_nodes[k];
				if (ni->ni_state == OCSFS_NODE_FREE) {
					i = k;
					break;
				}
			}
		}

		/* Terza: qualsiasi slot DEAD */
		if (i == sbi->s_max_nodes) {
			for (u16 k = 0; k < sbi->s_max_nodes; k++) {
				ni = &sbi->s_nodes[k];
				if (ni->ni_state == OCSFS_NODE_DEAD) {
					i = k;
					break;
				}
			}
		}

		if (i == sbi->s_max_nodes) {
			spin_unlock(&sbi->s_node_lock);
			pr_err("ocsfs: no free node slots (max=%u)\n",
			       sbi->s_max_nodes);
			return -ENOSPC;
		}

		/* Prepara il nuovo stato in memoria */
		ni               = &sbi->s_nodes[i];
		sbi->s_node_slot = i;
		/* Reclaiming our own crash-orphaned slot: remember the dead
		 * incarnation's generation so the mount path can release the DLM
		 * locks it never unlocked (otherwise we deadlock against our own
		 * stale EX locks).  Reset to 0 on a clean FREE/DEAD claim. */
		sbi->s_self_recover_gen = crash_reclaim ? ni->ni_mount_gen : 0;
		ni->ni_state     = OCSFS_NODE_ACTIVE;
		ni->ni_mount_gen++;
		sbi->s_mount_gen = ni->ni_mount_gen;
		memcpy(ni->ni_uuid, sbi->s_node_uuid, 16);
		memcpy(ni->ni_name, sbi->s_node_name, 64);
		ni->ni_pr_key  = ocsfs_pr_make_key(sbi->s_node_uuid,
						   sbi->s_mount_gen);
		ni->ni_last_hb = ktime_get_real_ns();

		spin_unlock(&sbi->s_node_lock);

		/* CAS atomico: se -EAGAIN un altro nodo ha preso lo slot */
		ret = ocsfs_node_write_slot(sb, i);
		if (ret == 0) {
			/* SEC-V3-1: re-read and verify auth on our own slot.
			 * Catches wrong-cluster joins where our secret doesn't
			 * match what the existing cluster expects, or CAS
			 * implementation bugs in the storage array. */
			if (sbi->s_auth_required) {
				struct ocsfs_disk_node_slot verify_dns;
				u64 voff = OCSFS_NODE_SLOT_TABLE_OFF +
					   (u64)i * sizeof(verify_dns);
				u64 vblk = voff / sbi->s_block_size;
				u32 vboff = voff % sbi->s_block_size;
				struct buffer_head *vbh = sb_getblk(sb, vblk);

				if (vbh) {
					clear_buffer_uptodate(vbh);
					if (bh_read(vbh, 0) >= 0) {
						memcpy(&verify_dns,
						       vbh->b_data + vboff,
						       sizeof(verify_dns));
						brelse(vbh);
						if (ocsfs_node_verify_auth(sb, &verify_dns) < 0) {
							ocsfs_node_release_slot(sb);
							pr_err("ocsfs: slot %u auth failed after CAS — wrong cluster secret?\n",
							       i);
							return -EACCES;
						}
					} else {
						brelse(vbh);
					}
				}
			}
			pr_info("ocsfs: claimed node slot %u (gen=%u)\n",
				i, sbi->s_mount_gen);
			return 0;
		}
		if (ret != -EAGAIN) {
			pr_err("ocsfs: failed to write node slot %u: %d\n",
			       i, ret);
			return ret;
		}

		/* -EAGAIN: race con un altro nodo — rileggi la tabella e riprova */
		usleep_range(1U << min(attempt, 8),
			     2U << min(attempt, 8));
	}

	pr_err("ocsfs: node slot claim timeout after %d attempts\n",
	       CAS_MAX_ATTEMPTS);
	return -EBUSY;
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

/*
 * Derive a stable 16-byte node UUID from the system hostname.
 * Unlike generate_random_uuid(), this produces the same UUID across mounts
 * so that after a crash the recovering node can recognise its own orphan slot.
 */
static void ocsfs_node_derive_uuid(u8 *uuid)
{
	static const u8 label[] = "ocsfs-node-uuid-v1";
	u8 input[sizeof(init_uts_ns.name.nodename) + sizeof(label)];
	u8 digest[SHA256_DIGEST_SIZE];
	size_t hlen;

	hlen = strnlen(init_uts_ns.name.nodename,
		       sizeof(init_uts_ns.name.nodename));
	memcpy(input, init_uts_ns.name.nodename, hlen);
	memcpy(input + hlen, label, sizeof(label));
	sha256(input, hlen + sizeof(label), digest);
	memcpy(uuid, digest, 16);
	/* RFC 4122 variant bits: version 5 (SHA-1 namespace, repurposed) */
	uuid[6] = (uuid[6] & 0x0f) | 0x50;
	uuid[8] = (uuid[8] & 0x3f) | 0x80;
}

int ocsfs_node_init(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	int ret;

	spin_lock_init(&sbi->s_node_lock);

	/* Derive stable UUID from hostname (same across mounts/reboots) */
	memset(sbi->s_node_uuid, 0, 16);
	ocsfs_node_derive_uuid(sbi->s_node_uuid);

	memset(sbi->s_node_name, 0, sizeof(sbi->s_node_name));
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
		ocsfs_dlm_exit(sb);
	}
}
