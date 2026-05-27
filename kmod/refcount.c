// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — refcount.c
 * Extent reference counting for Copy-on-Write snapshots.
 *
 * Phase 4: Per-AG refcount tree stored on disk.
 *
 * Design:
 *   Each AG has a refcount area on disk (after the block bitmap).
 *   The refcount area is a flat array of __le32 entries, one per
 *   physical block in the AG. Only blocks with refcount > 1 need
 *   an entry (refcount 1 is the default for allocated blocks).
 *
 *   To save space, we use a sparse representation: a B+ tree
 *   mapping (physical_block → refcount). Only blocks with
 *   refcount > 1 have entries. For Phase 4 MVP, we use a simpler
 *   approach: a per-AG hash table stored in dedicated blocks.
 *
 * Refcount lifecycle:
 *   alloc_blocks: refcount = 1 (implicit, no entry needed)
 *   snapshot:     refcount++ (entry created if refcount goes to 2)
 *   write (CoW):  refcount-- on old blocks, alloc new with refcount 1
 *   free:         if refcount > 1, refcount--; else actually free
 *
 * The refcount table uses a simple on-disk hash map with chaining.
 * Each bucket is a linked list of (block, refcount) pairs.
 */

#include "ocsfs.h"

/* ═══════════════════════════════════════════════════════════════
 * REFCOUNT TABLE LAYOUT
 *
 * The refcount table occupies OCSFS_REFCOUNT_BLOCKS_PER_AG blocks
 * at the end of each AG's metadata area. Each block contains a
 * flat array of ocsfs_disk_refcount entries.
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_REFCOUNT_BLOCKS_PER_AG	16
#define OCSFS_REFCOUNT_MAGIC		0x52454643  /* 'REFC' */

/*
 * On-disk refcount entry — 16 bytes each.
 * Stored in sorted arrays within each refcount block.
 */
struct ocsfs_disk_refcount {
	__le64	rc_block;	/* physical block number (0 = unused slot) */
	__le32	rc_count;	/* reference count (0 = free entry) */
	__le32	rc_checksum;
} __packed;

/* Entries per block = block_size / 16 */
static inline u32 ocsfs_rc_entries_per_block(struct ocsfs_sb_info *sbi)
{
	return sbi->s_block_size / sizeof(struct ocsfs_disk_refcount);
}

/* Hash a physical block number to a refcount block index */
static inline u32 ocsfs_rc_hash(u64 phys_block, u32 nr_blocks)
{
	u64 h = phys_block * 0x9e3779b97f4a7c15ULL;
	return (u32)(h % nr_blocks);
}

/*
 * Find the AG and local block offset for a physical block.
 */
static struct ocsfs_ag_info *ocsfs_block_to_ag(struct ocsfs_sb_info *sbi,
					       u64 phys_block,
					       u64 *local_block)
{
	u32 ag_no;

	for (ag_no = 0; ag_no < sbi->s_ag_count; ag_no++) {
		struct ocsfs_ag_info *ag = &sbi->s_ags[ag_no];

		if (phys_block >= ag->block_start &&
		    phys_block < ag->block_start + ag->block_count) {
			*local_block = phys_block - ag->block_start;
			return ag;
		}
	}

	return NULL;
}

/*
 * Get the disk block number for a refcount table block in an AG.
 * The refcount table is stored after the bitmap area.
 */
static u64 ocsfs_rc_table_block(struct ocsfs_sb_info *sbi,
				struct ocsfs_ag_info *ag, u32 bucket)
{
	u64 inode_table_blocks = (ag->inode_count * OCSFS_INODE_SIZE +
				  sbi->s_block_size - 1) / sbi->s_block_size;
	u64 rc_start = (ag->inode_table_off / sbi->s_block_size) +
		       inode_table_blocks;

	return rc_start + bucket;
}

/* refcount operations */

/*
 * ocsfs_refcount_get() — legge il refcount per un blocco fisico.
 * Ritorna 1 (default) se non esiste entry esplicita.
 */
int ocsfs_refcount_get(struct super_block *sb, u64 phys_block,
		       u32 *refcount_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_ag_info *ag;
	struct buffer_head *bh;
	struct ocsfs_disk_refcount *entries;
	u64 local, rc_block;
	u32 bucket, nr_entries, i;

	ag = ocsfs_block_to_ag(sbi, phys_block, &local);
	if (!ag) {
		*refcount_out = 0;
		return -EINVAL;
	}

	bucket   = ocsfs_rc_hash(phys_block, OCSFS_REFCOUNT_BLOCKS_PER_AG);
	rc_block = ocsfs_rc_table_block(sbi, ag, bucket);

	bh = sb_getblk(sb, rc_block);
	if (!bh) { *refcount_out = 0; return -EIO; }
	clear_buffer_uptodate(bh);
	if (bh_read(bh, 0) < 0) {
		brelse(bh);
		*refcount_out = 0;
		return -EIO;
	}

	entries    = (struct ocsfs_disk_refcount *)bh->b_data;
	nr_entries = ocsfs_rc_entries_per_block(sbi);

	for (i = 0; i < nr_entries; i++) {
		if (le64_to_cpu(entries[i].rc_block) == phys_block &&
		    le32_to_cpu(entries[i].rc_count) > 0) {
			u32 csum = ocsfs_crc32c(0, &entries[i],
						sizeof(*entries) - 4);

			if (le32_to_cpu(entries[i].rc_checksum) != csum) {
				pr_warn_ratelimited("ocsfs: refcount csum mismatch block %llu\n",
						    phys_block);
				brelse(bh);
				return -EIO;
			}
			*refcount_out = le32_to_cpu(entries[i].rc_count);
			brelse(bh);
			return 0;
		}
	}

	brelse(bh);
	*refcount_out = 1;  /* default: blocco allocato non condiviso */
	return 0;
}

/*
 * ocsfs_refcount_apply_delta — RMW atomico via CAS su un singolo blocco.
 *
 * Algoritmo (CRIT-3 fix):
 *   1. Forced-read del bucket block (4096 byte) per coerenza cluster.
 *   2. Modifica in-place l'entry per phys_block nel buffer new_buf.
 *   3. ocsfs_atomic_cas confronta l'intero bucket: se -EAGAIN riprova.
 *
 * Il CAS garantisce atomicità cross-node: nessun TOCTOU tra get e set.
 * Il DLM ag_rc_lock_res non è più necessario per correttezza (rimane
 * come ottimizzazione locale opzionale per ridurre contesa CAS).
 *
 * result_count_out: refcount risultante (0 = blocco da liberare).
 */
static int ocsfs_refcount_apply_delta(struct super_block *sb, u64 phys_block,
				      int delta, u32 *result_count_out)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_ag_info *ag;
	struct ocsfs_disk_refcount *entries;
	u64 local, rc_block;
	u32 bucket, nr_entries, i, new_count;
	u8 *expected_buf, *new_buf;
	int attempt, ret, found_idx, free_slot;

	ag = ocsfs_block_to_ag(sbi, phys_block, &local);
	if (!ag)
		return -EINVAL;

	bucket   = ocsfs_rc_hash(phys_block, OCSFS_REFCOUNT_BLOCKS_PER_AG);
	rc_block = ocsfs_rc_table_block(sbi, ag, bucket);
	nr_entries = ocsfs_rc_entries_per_block(sbi);

	expected_buf = mempool_alloc(sbi->s_rc_buf_pool, GFP_KERNEL);
	new_buf      = mempool_alloc(sbi->s_rc_buf_pool, GFP_KERNEL);
	if (!expected_buf || !new_buf) {
		ret = -ENOMEM;
		goto out_free;
	}

	for (attempt = 0; attempt < CAS_MAX_ATTEMPTS; attempt++) {
		struct buffer_head *bh;

		bh = sb_getblk(sb, rc_block);
		if (!bh) { ret = -EIO; break; }
		clear_buffer_uptodate(bh);
		ret = bh_read(bh, 0);
		if (ret < 0) { brelse(bh); break; }

		memcpy(expected_buf, bh->b_data, sbi->s_block_size);
		brelse(bh);
		memcpy(new_buf, expected_buf, sbi->s_block_size);

		entries   = (struct ocsfs_disk_refcount *)new_buf;
		found_idx = -1;
		free_slot = -1;

		for (i = 0; i < nr_entries; i++) {
			u64 blk = le64_to_cpu(entries[i].rc_block);
			u32 cnt = le32_to_cpu(entries[i].rc_count);

			if (blk == phys_block && cnt > 0) {
				found_idx = (int)i;
				break;
			}
			if (free_slot < 0 && (blk == 0 || cnt == 0))
				free_slot = (int)i;
		}

		if (found_idx >= 0) {
			u32 old = le32_to_cpu(entries[found_idx].rc_count);

			if (delta >= 0)
				new_count = old + (u32)delta;
			else
				new_count = (old > (u32)(-delta)) ?
					    old - (u32)(-delta) : 0;

			if (new_count <= 1) {
				entries[found_idx].rc_block    = 0;
				entries[found_idx].rc_count    = 0;
				entries[found_idx].rc_checksum = 0;
			} else {
				entries[found_idx].rc_count = cpu_to_le32(new_count);
				entries[found_idx].rc_checksum = cpu_to_le32(
					ocsfs_crc32c(0, &entries[found_idx],
						     sizeof(*entries) - 4));
			}
		} else if (delta > 0) {
			/* Blocco era a refcount implicito 1; inc → 2+ */
			if (free_slot < 0) {
				pr_warn("ocsfs: refcount table full AG %u\n",
					ag->ag_no);
				ret = -ENOSPC;
				break;
			}
			new_count = 1 + (u32)delta;
			entries[free_slot].rc_block = cpu_to_le64(phys_block);
			entries[free_slot].rc_count = cpu_to_le32(new_count);
			entries[free_slot].rc_checksum = cpu_to_le32(
				ocsfs_crc32c(0, &entries[free_slot],
					     sizeof(*entries) - 4));
		} else {
			/* Dec su blocco a refcount implicito 1 → 0 (da liberare) */
			new_count = 0;
			if (result_count_out)
				*result_count_out = 0;
			ret = 0;
			goto out_free;
		}

		ret = ocsfs_atomic_cas(sb, rc_block, 0, sbi->s_block_size,
				       expected_buf, new_buf);
		if (ret == 0) {
			if (result_count_out)
				*result_count_out = new_count;
			break;
		}
		if (ret != -EAGAIN)
			break;
		usleep_range(1U << min(attempt, 8),
			     2U << min(attempt, 8));
	}

	if (attempt == CAS_MAX_ATTEMPTS)
		ret = -EBUSY;

out_free:
	mempool_free(expected_buf, sbi->s_rc_buf_pool);
	mempool_free(new_buf, sbi->s_rc_buf_pool);
	return ret;
}

/*
 * ocsfs_refcount_inc — incrementa refcount su un range di blocchi.
 *
 * Usa apply_delta (CAS atomico) invece del vecchio get+set TOCTOU.
 */
int ocsfs_refcount_inc(struct super_block *sb, u64 phys_block, u32 len)
{
	u32 i;

	for (i = 0; i < len; i++) {
		int ret = ocsfs_refcount_apply_delta(sb, phys_block + i, +1, NULL);

		if (ret)
			return ret;
	}
	return 0;
}

/*
 * ocsfs_refcount_dec — decrementa refcount su un range di blocchi.
 *
 * Usa apply_delta (CAS atomico) invece del vecchio get+set TOCTOU.
 * should_free viene settato a true se TUTTI i blocchi scendono a 0.
 */
int ocsfs_refcount_dec(struct super_block *sb, u64 phys_block, u32 len,
		       bool *should_free)
{
	bool all_free = true;
	u32 i;

	for (i = 0; i < len; i++) {
		u32 result_count = 1;
		int ret = ocsfs_refcount_apply_delta(sb, phys_block + i, -1,
						     &result_count);

		if (ret)
			return ret;
		if (result_count > 0)
			all_free = false;
	}

	if (should_free)
		*should_free = all_free;
	return 0;
}

/*
 * ocsfs_refcount_init_ag() — Initialize the refcount table for an AG.
 *
 * Called during mkfs or when first enabling snapshots.
 * Zeroes the refcount blocks.
 */
int ocsfs_refcount_init_ag(struct super_block *sb, u32 ag_no)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_ag_info *ag = &sbi->s_ags[ag_no];
	u32 i;

	for (i = 0; i < OCSFS_REFCOUNT_BLOCKS_PER_AG; i++) {
		u64 rc_block = ocsfs_rc_table_block(sbi, ag, i);
		struct buffer_head *bh;

		bh = sb_getblk(sb, rc_block);
		if (!bh)
			return -ENOMEM;

		lock_buffer(bh);
		memset(bh->b_data, 0, sbi->s_block_size);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		brelse(bh);
	}

	return 0;
}
