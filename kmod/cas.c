// SPDX-License-Identifier: GPL-2.0-only
/*
 * cas.c — Atomic Compare-And-Swap engine per OCSFS cluster.
 *
 * Backend PR-lease: per ogni target block si acquisisce un "lease entry"
 * on-disk (write → flush → reread), si fa il RMW, si rilascia il lease.
 * SCSI CAW è un fast-path opzionale (CAS_BACKEND_SCSI_CAW) non ancora usato.
 *
 * Il lease area è layout-compatibile con blocksize 4096: ogni blocco ospita
 * 128 entry da 32 byte. Con OCSFS_CAS_LEASE_ENTRIES=256, servono 2 blocchi.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/crc32c.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include "ocsfs.h"

/* entry per blocco (blocksize 4096 / 32 byte per entry) */
#define CAS_ENTRIES_PER_BLOCK   128U

/* ------------------------------------------------------------------ helpers */

static u32 cas_lease_crc(const struct ocsfs_disk_cas_lease *cl)
{
	return crc32c(0, cl, offsetof(struct ocsfs_disk_cas_lease, cl_checksum));
}

/* Blocco logico che ospita il lease per `block` */
static u64 cas_lease_block(struct super_block *sb, u64 block)
{
	u32 idx = (u32)(block % OCSFS_CAS_LEASE_ENTRIES);
	u64 base = OCSFS_CAS_LEASE_OFF / sb->s_blocksize;

	return base + idx / CAS_ENTRIES_PER_BLOCK;
}

/* Indice dell'entry all'interno del blocco */
static u32 cas_lease_idx(u64 block)
{
	u32 idx = (u32)(block % OCSFS_CAS_LEASE_ENTRIES);

	return idx % CAS_ENTRIES_PER_BLOCK;
}

/* ------------------------------------------------------------------ probe */

int ocsfs_cas_probe(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	if (!sbi->s_clustered) {
		sbi->s_cas_backend = CAS_BACKEND_NONE;
		return 0;
	}

	if (sbi->s_caw_supported) {
		sbi->s_cas_backend = CAS_BACKEND_SCSI_CAW;
		pr_info("ocsfs: CAS backend: SCSI CAW\n");
		return 0;
	}

	sbi->s_cas_backend = CAS_BACKEND_PR_LEASE;
	pr_info("ocsfs: CAS backend: PR-lease (software)\n");
	return 0;
}

/* ------------------------------------------------------------------ PR-lease */

/*
 * cas_acquire_lease — acquisisce il lease per `block`.
 *
 * 1. Forced-read del blocco lease (invalida cache per coerenza cluster).
 * 2. Se l'entry è libera o scaduta: scrivi owner=my_slot, deadline=now+2s.
 * 3. sync_dirty_buffer (flush).
 * 4. Reread: se owner==my_slot → OK; altrimenti → -EAGAIN (race persa).
 *
 * *bh_out è trattenuto con get_bh(). Il chiamante deve brelse() dopo uso.
 */
static int cas_acquire_lease(struct super_block *sb, u64 block,
			     struct buffer_head **bh_out, u32 *eidx_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	u64 lblock = cas_lease_block(sb, block);
	u32 eidx   = cas_lease_idx(block);
	struct buffer_head *bh;
	struct ocsfs_disk_cas_lease *cl;
	u64 now_ns;
	int ret;

	bh = sb_getblk(sb, lblock);
	if (!bh)
		return -ENOMEM;

	clear_buffer_uptodate(bh);
	ret = bh_read(bh, 0);
	if (ret < 0) {
		brelse(bh);
		return ret;
	}

	cl     = (struct ocsfs_disk_cas_lease *)bh->b_data + eidx;
	now_ns = ktime_get_real_ns();

	if (le32_to_cpu(cl->cl_magic) == OCSFS_CAS_LEASE_MAGIC) {
		u16 owner    = le16_to_cpu(cl->cl_owner_slot);
		u64 deadline = le64_to_cpu(cl->cl_deadline_ns);

		if (owner != 0xFFFF && now_ns < deadline) {
			brelse(bh);
			return -EAGAIN;
		}
	}

	lock_buffer(bh);
	cl->cl_magic      = cpu_to_le32(OCSFS_CAS_LEASE_MAGIC);
	cl->cl_owner_slot = cpu_to_le16((u16)sbi->s_node_slot);
	cl->cl_reserved   = 0;
	cl->cl_deadline_ns = cpu_to_le64(now_ns + CAS_LEASE_TIMEOUT_NS);
	cl->cl_checksum   = cpu_to_le32(cas_lease_crc(cl));
	cl->cl_pad        = 0;
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);

	ret = sync_dirty_buffer(bh);
	if (ret) {
		brelse(bh);
		return ret;
	}

	/* Reread per verificare la vincita della race */
	clear_buffer_uptodate(bh);
	ret = bh_read(bh, 0);
	if (ret < 0) {
		brelse(bh);
		return ret;
	}

	cl = (struct ocsfs_disk_cas_lease *)bh->b_data + eidx;
	if (le16_to_cpu(cl->cl_owner_slot) != (u16)sbi->s_node_slot) {
		brelse(bh);
		return -EAGAIN;
	}

	*bh_out   = bh;
	*eidx_out = eidx;
	return 0;
}

static void cas_release_lease(struct buffer_head *bh, u32 eidx)
{
	struct ocsfs_disk_cas_lease *cl;

	lock_buffer(bh);
	cl = (struct ocsfs_disk_cas_lease *)bh->b_data + eidx;
	cl->cl_owner_slot  = cpu_to_le16(0xFFFF);
	cl->cl_deadline_ns = 0;
	cl->cl_checksum    = cpu_to_le32(cas_lease_crc(cl));
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);

	sync_dirty_buffer(bh);
	brelse(bh);
}

/* ------------------------------------------------------------------ single-node CAS */

static int cas_single_node(struct super_block *sb, u64 block, u32 boff,
			   u32 len, const void *expected, const void *new_data)
{
	struct buffer_head *bh;
	int ret;

	bh = sb_getblk(sb, block);
	if (!bh)
		return -ENOMEM;

	clear_buffer_uptodate(bh);
	ret = bh_read(bh, 0);
	if (ret < 0) {
		brelse(bh);
		return ret;
	}

	if (memcmp(bh->b_data + boff, expected, len) != 0) {
		brelse(bh);
		return -EAGAIN;
	}

	lock_buffer(bh);
	memcpy(bh->b_data + boff, new_data, len);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);

	ret = sync_dirty_buffer(bh);
	brelse(bh);
	return ret;
}

/* ------------------------------------------------------------------ public API */

/*
 * ocsfs_atomic_cas — Compare-And-Swap su [block, boff .. boff+len).
 *
 * Confronta `expected` con il contenuto on-disk; se uguale sovrascrive con
 * `new_data` e ritorna 0.  Se diverso ritorna -EAGAIN (MISCOMPARE).
 *
 * Single-node (CAS_BACKEND_NONE): RMW diretto senza lease.
 * PR-lease: acquisisce lease, verifica, scrive, rilascia.
 * SCSI CAW: non ancora implementato, fallback su PR-lease.
 */
int ocsfs_atomic_cas(struct super_block *sb, u64 block, u32 boff,
		     u32 len, const void *expected, const void *new_data)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct buffer_head *lease_bh = NULL;
	u32 lease_eidx = 0;
	struct buffer_head *data_bh;
	int attempt, ret;

	if (WARN_ON(boff + len > (u32)sb->s_blocksize))
		return -EINVAL;

	if (sbi->s_cas_backend == CAS_BACKEND_NONE)
		return cas_single_node(sb, block, boff, len, expected, new_data);

	/* PR-lease (e SCSI CAW non ancora disponibile: usa stesso path) */
	for (attempt = 0; attempt < CAS_MAX_ATTEMPTS; attempt++) {
		ret = cas_acquire_lease(sb, block, &lease_bh, &lease_eidx);
		if (ret == -EAGAIN) {
			udelay(1 << min(attempt, 8));
			continue;
		}
		if (ret < 0)
			return ret;

		/* Leggi block target (forced-read dentro il lease) */
		data_bh = sb_getblk(sb, block);
		if (!data_bh) {
			cas_release_lease(lease_bh, lease_eidx);
			return -ENOMEM;
		}
		clear_buffer_uptodate(data_bh);
		ret = bh_read(data_bh, 0);
		if (ret < 0) {
			brelse(data_bh);
			cas_release_lease(lease_bh, lease_eidx);
			return ret;
		}

		if (memcmp(data_bh->b_data + boff, expected, len) != 0) {
			brelse(data_bh);
			cas_release_lease(lease_bh, lease_eidx);
			return -EAGAIN;
		}

		lock_buffer(data_bh);
		memcpy(data_bh->b_data + boff, new_data, len);
		set_buffer_uptodate(data_bh);
		mark_buffer_dirty(data_bh);
		unlock_buffer(data_bh);
		ret = sync_dirty_buffer(data_bh);
		brelse(data_bh);

		cas_release_lease(lease_bh, lease_eidx);
		return ret;
	}

	return -EBUSY;
}
