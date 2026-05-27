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
 */

#ifdef CONFIG_FS_ENCRYPTION

#include <linux/fscrypt.h>
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
	return ocsfs_xattr_set_internal(inode, OCSFS_XATTR_NS_SECURITY, "c",
					ctx, len, 0);
}

/* ─── empty_dir check ───────────────────────────────────────────────────── */

static bool ocsfs_fscrypt_empty_dir(struct inode *inode)
{
	/* i_dirent_count tracks live entries; 0 means the directory is empty.
	 * fscrypt calls this before allowing a policy change on a directory. */
	return OCSFS_I(inode)->i_dirent_count == 0;
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

#endif /* CONFIG_FS_ENCRYPTION */
