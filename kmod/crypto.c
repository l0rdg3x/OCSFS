// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — crypto.c
 * fscrypt integration: optional per-directory/file encryption policies.
 *
 * Encryption is entirely optional and per-directory: a directory is
 * encrypted by setting a policy via FS_IOC_SET_ENCRYPTION_POLICY.
 * All files and sub-directories created inside inherit the policy.
 * Volumes without any encrypted inodes work on kernels compiled without
 * CONFIG_FS_ENCRYPTION without any restriction.
 *
 * Context storage: the fscrypt context (cipher suite + nonce) is kept in
 * the security xattr "c" (OCSFS_XATTR_NS_SECURITY / name "c"), consistent
 * with ext4 and f2fs.
 *
 * Data encryption: bounce-page path (needs_bounce_pages=1).
 *   Reads:  ocsfs_enc_read_folio() in iomap.c — synchronous sb_bread +
 *           fscrypt_decrypt_pagecache_blocks().
 *   Writes: ocsfs_enc_writepages() in iomap.c — fscrypt_encrypt_pagecache_blocks()
 *           + synchronous bio submission per folio.
 *
 * Filename encryption: handled automatically by fscrypt when a key is loaded
 * and the parent directory has an encryption policy.  Lookups in encrypted
 * directories use the ciphertext name stored on disk.
 *
 * ARCH-V3-1 — Cluster key store:
 *   fscrypt keys are node-local by design.  In cluster mode, any node that
 *   hasn't had a key added via FS_IOC_ADD_ENCRYPTION_KEY cannot open
 *   encrypted files.  This file provides a shared on-disk key store so that
 *   administrators can distribute keys across cluster nodes:
 *
 *   1. Node A calls FS_IOC_ADD_ENCRYPTION_KEY → ocsfs intercepts and writes
 *      the key (encrypted with ChaCha20-Poly1305 / cluster secret) to the
 *      shared key store on the LUN, then proceeds with the standard fscrypt
 *      path.
 *   2. On mount, every node logs the identifiers of keys present in the
 *      shared store (ocsfs_key_store_notify_mount).
 *   3. The admin runs `ocsfs-tool keys restore <dev>` on each node, which
 *      calls OCSFS_IOC_KEY_LIST + OCSFS_IOC_KEY_FETCH and then issues
 *      FS_IOC_ADD_ENCRYPTION_KEY for each key — no manual key entry needed.
 *
 *   Security: raw key material is never stored in plaintext.  Retrieval via
 *   OCSFS_IOC_KEY_FETCH requires CAP_SYS_ADMIN.  The cluster secret is a
 *   32-byte value supplied at mount time and never leaves kernel memory.
 */

#ifdef CONFIG_FS_ENCRYPTION

#include <linux/fscrypt.h>
#include <linux/random.h>
#include <crypto/chacha20poly1305.h>
#include <crypto/poly1305.h>
#include "ocsfs.h"

/* ─── fscrypt context storage ──────────────────────────────────────────── */

static int ocsfs_fscrypt_get_context(struct inode *inode, void *ctx, size_t len)
{
	return ocsfs_xattr_get_internal(inode, OCSFS_XATTR_NS_SECURITY, "c",
					ctx, len);
}

static int ocsfs_fscrypt_set_context(struct inode *inode, const void *ctx,
				      size_t len, void *fs_data)
{
	int r = ocsfs_xattr_set_internal(inode, OCSFS_XATTR_NS_SECURITY, "c",
					 ctx, len, 0);
	if (r)
		return r;
	/*
	 * Mark the inode encrypted right now (in-memory S_ENCRYPTED) AND persist
	 * OCSFS_IFLAG_ENCRYPTED.  Without this the inode/directory we just gave a
	 * policy to still reads IS_ENCRYPTED == false, so a child created in this
	 * directory does not inherit encryption (fscrypt_prepare_new_inode sees an
	 * "unencrypted" parent) and its data is written in PLAINTEXT.  This is the
	 * ext4 set_context pattern.
	 */
	OCSFS_I(inode)->i_flags |= OCSFS_IFLAG_ENCRYPTED;
	inode->i_flags |= S_ENCRYPTED;
	mark_inode_dirty(inode);
	return 0;
}

/* ─── empty_dir check ───────────────────────────────────────────────────── */

static bool ocsfs_fscrypt_empty_dir(struct inode *inode)
{
	/* MEDIO-V3-9: i_dirent_count is cached and may be stale in cluster mode.
	 * Use ocsfs_empty_dir() which does a real disk scan with DLM SH + refresh
	 * so a policy change is never allowed on a non-empty directory. */
	return ocsfs_empty_dir(inode) != 0;
}

/* ─── fscrypt_operations ─────────────────────────────────────────────────
 *
 * inode_info_offs: offset from &ocsfs_inode_info.vfs_inode to
 *   &ocsfs_inode_info.i_crypt_info.  Negative because i_crypt_info
 *   comes before vfs_inode in the struct layout.
 */
const struct fscrypt_operations ocsfs_fscrypt_ops = {
	.needs_bounce_pages = 1,
	.get_context         = ocsfs_fscrypt_get_context,
	.set_context         = ocsfs_fscrypt_set_context,
	.empty_dir           = ocsfs_fscrypt_empty_dir,
	.inode_info_offs     = (ptrdiff_t)offsetof(struct ocsfs_inode_info,
						    i_crypt_info) -
				(ptrdiff_t)offsetof(struct ocsfs_inode_info,
						    vfs_inode),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * ARCH-V3-1: shared encrypted key store
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * On-disk layout: OCSFS_KEY_STORE_OFF, one block (4096 bytes),
 * OCSFS_KEY_STORE_MAX_ENTRIES (32) × 128-byte entries.
 *
 * Encryption: ChaCha20-Poly1305 with s_cluster_secret[32] as key and a
 * per-entry random 64-bit nonce.  The 16-byte Poly1305 tag immediately
 * follows the ciphertext inside kse_ct[].
 *
 * Only volumes with OCSFS_FEATURE_INCOMPAT_KEY_STORE use this area. */

/* Read all 32 entries from the shared key store block into caller's buffer. */
static int key_store_read(struct super_block *sb,
			   struct ocsfs_disk_key_store_entry *buf)
{
	u64 blk = OCSFS_KEY_STORE_OFF >> sb->s_blocksize_bits;
	struct buffer_head *bh;

	bh = __bread(sb->s_bdev, blk, sb->s_blocksize);
	if (!bh)
		return -EIO;
	memcpy(buf, bh->b_data, OCSFS_KEY_STORE_SIZE);
	brelse(bh);
	return 0;
}

/* Write all 32 entries back to the shared key store block. */
static int key_store_write(struct super_block *sb,
			    const struct ocsfs_disk_key_store_entry *buf)
{
	u64 blk = OCSFS_KEY_STORE_OFF >> sb->s_blocksize_bits;
	struct buffer_head *bh;

	bh = __getblk(sb->s_bdev, blk, sb->s_blocksize);
	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memcpy(bh->b_data, buf, OCSFS_KEY_STORE_SIZE);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

/* Build the canonical 16-byte key identifier from a fscrypt_key_specifier.
 * DESCRIPTOR keys are 8 bytes, padded with zeros to 16.
 * IDENTIFIER keys are exactly 16 bytes. */
static void key_store_make_id(const struct fscrypt_key_specifier *spec,
			       u8 id[16])
{
	memset(id, 0, 16);
	if (spec->type == FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR)
		memcpy(id, spec->u.descriptor, FSCRYPT_KEY_DESCRIPTOR_SIZE);
	else
		memcpy(id, spec->u.identifier, FSCRYPT_KEY_IDENTIFIER_SIZE);
}

/**
 * ocsfs_key_store_add - persist an fscrypt key in the shared encrypted store.
 * @sb:       mounted superblock
 * @spec:     fscrypt key specifier (type + identifier/descriptor)
 * @raw_key:  raw key material (kernel buffer, already copied from user)
 * @key_size: length of raw_key in bytes
 *
 * Encrypts raw_key with ChaCha20-Poly1305 / s_cluster_secret and writes the
 * entry to the shared key store block on the LUN.  Idempotent: calling again
 * with the same key identifier is a no-op (returns 0 immediately).
 *
 * Only active when OCSFS_FEATURE_INCOMPAT_KEY_STORE is set on the volume.
 * Errors are non-fatal from the caller's perspective — the fscrypt add-key
 * path proceeds regardless.
 */
int ocsfs_key_store_add(struct super_block *sb,
			 const struct fscrypt_key_specifier *spec,
			 const u8 *raw_key, u16 key_size)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_key_store_entry *store;
	struct ocsfs_disk_key_store_entry *e;
	u8 key_id[16];
	u8 ct[FSCRYPT_MAX_KEY_SIZE + POLY1305_DIGEST_SIZE];
	u64 nonce;
	u32 crc;
	int i, free_slot = -1, ret = 0;
	bool locked = false;

	if (!(sbi->s_feature_incompat & OCSFS_FEATURE_INCOMPAT_KEY_STORE))
		return 0;
	/* KS-2: never persist a key when there is no real cluster secret — it would
	 * be "encrypted" under an all-zero key and trivially recoverable from the LUN.
	 * The key store is only meaningful alongside cluster_secret= (s_auth_required). */
	if (!sbi->s_auth_required) {
		pr_warn_once("ocsfs: key_store: cluster_secret= not set — refusing to persist key (would use zero key)\n");
		return 0;
	}
	if (key_size == 0 || key_size > FSCRYPT_MAX_KEY_SIZE)
		return -EINVAL;

	store = kmalloc(OCSFS_KEY_STORE_SIZE, GFP_NOFS);
	if (!store)
		return -ENOMEM;

	key_store_make_id(spec, key_id);

	/* KS-1: serialize the read-modify-write against other nodes/threads so a
	 * concurrent add cannot lose this entry (last-writer-wins on the 4K block). */
	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(sb, &sbi->s_keystore_lock_res, OCSFS_LOCK_EX);
		if (ret)
			goto out;
		locked = true;
	}

	if (key_store_read(sb, store)) {
		ret = -EIO;
		goto out;
	}

	for (i = 0; i < OCSFS_KEY_STORE_MAX_ENTRIES; i++) {
		e = &store[i];
		if (le32_to_cpu(e->kse_magic) != OCSFS_KEY_STORE_ENTRY_MAGIC) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		/* idempotent: same key id → already stored */
		if (!memcmp(e->kse_id, key_id, 16))
			goto out;
	}
	if (free_slot < 0) {
		pr_warn_ratelimited("ocsfs: key_store: no free slot (max %d keys)\n",
				    OCSFS_KEY_STORE_MAX_ENTRIES);
		ret = -ENOSPC;
		goto out;
	}

	get_random_bytes(&nonce, sizeof(nonce));

	/* encrypt: ct = ChaCha20Poly1305(key=cluster_secret, nonce, pt=raw_key)
	 * output is key_size bytes ciphertext + 16-byte Poly1305 tag */
	chacha20poly1305_encrypt(ct, raw_key, key_size,
				 NULL, 0, nonce, sbi->s_cluster_secret);

	e = &store[free_slot];
	memset(e, 0, sizeof(*e));
	e->kse_magic     = cpu_to_le32(OCSFS_KEY_STORE_ENTRY_MAGIC);
	e->kse_key_size  = cpu_to_le16(key_size);
	e->kse_spec_type = cpu_to_le16(spec->type);
	memcpy(e->kse_id, key_id, 16);
	e->kse_nonce     = cpu_to_le64(nonce);
	memcpy(e->kse_ct, ct, key_size + POLY1305_DIGEST_SIZE);
	crc = crc32c(~0U, e, offsetof(struct ocsfs_disk_key_store_entry,
				       kse_checksum));
	e->kse_checksum = cpu_to_le32(crc);

	if (key_store_write(sb, store)) {
		pr_err_ratelimited("ocsfs: key_store: write failed (slot %d)\n",
				   free_slot);
		ret = -EIO;
		goto out;
	}

	pr_info("ocsfs: key_store: stored key id=%*phN size=%u — "
		"run 'ocsfs-tool keys restore <dev>' on other nodes\n",
		16, key_id, (unsigned int)key_size);
out:
	if (locked)
		ocsfs_lock_release(sb, &sbi->s_keystore_lock_res);
	kfree(store);
	return ret;
}

/**
 * ocsfs_key_store_list - enumerate key identifiers stored in the shared store.
 * @sb:          mounted superblock
 * @out:         caller-allocated array of at least max_entries elements
 * @max_entries: capacity of @out
 * @out_count:   number of valid entries written to @out
 */
int ocsfs_key_store_list(struct super_block *sb,
			  struct ocsfs_key_list_entry *out,
			  u32 max_entries, u32 *out_count)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_key_store_entry *store;
	u32 n = 0;
	int i, ret = 0;
	bool locked = false;

	if (!(sbi->s_feature_incompat & OCSFS_FEATURE_INCOMPAT_KEY_STORE)) {
		*out_count = 0;
		return 0;
	}

	store = kmalloc(OCSFS_KEY_STORE_SIZE, GFP_NOFS);
	if (!store)
		return -ENOMEM;

	/* KS-1: SH lock — consistent snapshot against a concurrent EX add. */
	if (sbi->s_clustered) {
		int lret = ocsfs_lock_acquire(sb, &sbi->s_keystore_lock_res,
					      OCSFS_LOCK_SH);
		if (lret) {
			kfree(store);
			return lret;
		}
		locked = true;
	}

	if (key_store_read(sb, store)) {
		ret = -EIO;
		goto out;
	}

	for (i = 0; i < OCSFS_KEY_STORE_MAX_ENTRIES && n < max_entries; i++) {
		struct ocsfs_disk_key_store_entry *e = &store[i];

		if (le32_to_cpu(e->kse_magic) != OCSFS_KEY_STORE_ENTRY_MAGIC)
			continue;
		memcpy(out[n].kle_id, e->kse_id, 16);
		out[n].kle_spec_type = le16_to_cpu(e->kse_spec_type);
		out[n].kle_key_size  = le16_to_cpu(e->kse_key_size);
		out[n].kle_pad       = 0;
		n++;
	}
	*out_count = n;
out:
	if (locked)
		ocsfs_lock_release(sb, &sbi->s_keystore_lock_res);
	kfree(store);
	return ret;
}

/**
 * ocsfs_key_store_fetch - decrypt and return a stored fscrypt key.
 * @sb:       mounted superblock
 * @key_id:   16-byte canonical key identifier
 * @out_key:  caller buffer, must be at least FSCRYPT_MAX_KEY_SIZE bytes
 * @out_size: actual key size written to @out_key
 *
 * Returns 0 on success, -ENOKEY if not found, -EBADMSG on auth failure.
 * Caller must zero @out_key after use (memzero_explicit).
 */
int ocsfs_key_store_fetch(struct super_block *sb,
			   const u8 *key_id, u8 *out_key, u16 *out_size)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_key_store_entry *store;
	struct ocsfs_disk_key_store_entry *e;
	u8 pt[FSCRYPT_MAX_KEY_SIZE];
	u16 key_size;
	u64 nonce;
	u32 crc;
	int i, ret = -ENOKEY;
	bool locked = false;

	if (!(sbi->s_feature_incompat & OCSFS_FEATURE_INCOMPAT_KEY_STORE))
		return -EOPNOTSUPP;
	/* KS-2: without a real cluster secret the stored ciphertext is meaningless. */
	if (!sbi->s_auth_required)
		return -EACCES;

	store = kmalloc(OCSFS_KEY_STORE_SIZE, GFP_NOFS);
	if (!store)
		return -ENOMEM;

	/* KS-1: SH lock — a concurrent EX add must not produce a torn read. */
	if (sbi->s_clustered) {
		int lret = ocsfs_lock_acquire(sb, &sbi->s_keystore_lock_res,
					      OCSFS_LOCK_SH);
		if (lret) {
			kfree(store);
			return lret;
		}
		locked = true;
	}

	if (key_store_read(sb, store)) {
		ret = -EIO;
		goto out;
	}

	for (i = 0; i < OCSFS_KEY_STORE_MAX_ENTRIES; i++) {
		e = &store[i];
		if (le32_to_cpu(e->kse_magic) != OCSFS_KEY_STORE_ENTRY_MAGIC)
			continue;
		if (memcmp(e->kse_id, key_id, 16))
			continue;

		/* verify CRC before using any field */
		crc = crc32c(~0U, e, offsetof(struct ocsfs_disk_key_store_entry,
					       kse_checksum));
		if (crc != le32_to_cpu(e->kse_checksum)) {
			ret = -EBADMSG;
			break;
		}

		key_size = le16_to_cpu(e->kse_key_size);
		if (key_size == 0 || key_size > FSCRYPT_MAX_KEY_SIZE) {
			ret = -EBADMSG;
			break;
		}

		nonce = le64_to_cpu(e->kse_nonce);

		/* decrypt: verify Poly1305 tag and recover plaintext */
		if (!chacha20poly1305_decrypt(pt, e->kse_ct,
					      (size_t)key_size + POLY1305_DIGEST_SIZE,
					      NULL, 0, nonce,
					      sbi->s_cluster_secret)) {
			memzero_explicit(pt, sizeof(pt));
			pr_warn_ratelimited(
				"ocsfs: key_store: auth failed for id=%*phN — "
				"wrong cluster secret?\n", 16, key_id);
			ret = -EBADMSG;
			break;
		}

		memcpy(out_key, pt, key_size);
		memzero_explicit(pt, sizeof(pt));
		*out_size = key_size;
		ret = 0;
		break;
	}
out:
	if (locked)
		ocsfs_lock_release(sb, &sbi->s_keystore_lock_res);
	kfree(store);
	return ret;
}

/**
 * ocsfs_key_store_notify_mount - log stored key identifiers at mount time.
 *
 * Called from fill_super after cluster join completes.  Prints the ID and
 * size of each stored key so administrators know which keys need to be
 * added on this node.  Fails silently on I/O error.
 */
void ocsfs_key_store_notify_mount(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_disk_key_store_entry *store;
	int i, n = 0;

	if (!sbi->s_clustered)
		return;
	if (!(sbi->s_feature_incompat & OCSFS_FEATURE_INCOMPAT_KEY_STORE))
		return;

	store = kmalloc(OCSFS_KEY_STORE_SIZE, GFP_NOFS);
	if (!store)
		return;

	if (key_store_read(sb, store)) {
		kfree(store);
		return;
	}

	for (i = 0; i < OCSFS_KEY_STORE_MAX_ENTRIES; i++) {
		struct ocsfs_disk_key_store_entry *e = &store[i];

		if (le32_to_cpu(e->kse_magic) != OCSFS_KEY_STORE_ENTRY_MAGIC)
			continue;
		pr_info("ocsfs: key_store[%d] id=%*phN size=%u bytes — "
			"FS_IOC_ADD_ENCRYPTION_KEY if not yet added\n",
			n, 16, e->kse_id, le16_to_cpu(e->kse_key_size));
		n++;
	}
	if (n > 0)
		pr_info("ocsfs: %d encryption key(s) in shared store; "
			"run 'ocsfs-tool keys restore <dev>' to install on this node\n",
			n);
	kfree(store);
}

#endif /* CONFIG_FS_ENCRYPTION */
