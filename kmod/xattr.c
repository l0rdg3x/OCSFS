// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — xattr.c
 * Extended attribute support.
 *
 * One xattr block (4096 bytes) per inode, allocated lazily on first set.
 * The block holds a sequence of packed entries:
 *   { u8 ns, u8 name_len, __le16 value_len, name[name_len], value[value_len] }
 * The block is never freed within set_internal (only in evict_inode).
 */

#include <linux/xattr.h>
#include "ocsfs.h"

struct ocsfs_xattr_entry {
	u8    xe_ns;
	u8    xe_name_len;
	__le16 xe_value_len;
	/* u8 xe_name[xe_name_len]; u8 xe_value[xe_value_len]; */
} __packed;

static inline u32 xe_total(const struct ocsfs_xattr_entry *xe)
{
	return sizeof(*xe) + xe->xe_name_len + le16_to_cpu(xe->xe_value_len);
}

/* Scan xb->xb_data for a matching entry. Returns pointer or NULL. */
static struct ocsfs_xattr_entry *
xattr_find(struct ocsfs_disk_xattr_block *xb, u8 ns, const char *name,
	   u8 name_len, u32 *out_off)
{
	u32 data_len = le16_to_cpu(xb->xb_data_len);
	u32 off = 0;

	while (off + sizeof(struct ocsfs_xattr_entry) <= data_len) {
		struct ocsfs_xattr_entry *xe =
			(struct ocsfs_xattr_entry *)(xb->xb_data + off);
		u32 total = xe_total(xe);

		if (off + total > data_len)
			break;

		if (xe->xe_ns == ns && xe->xe_name_len == name_len &&
		    memcmp(xb->xb_data + off + sizeof(*xe), name, name_len) == 0) {
			if (out_off)
				*out_off = off;
			return xe;
		}
		off += total;
	}
	return NULL;
}

/* Force-read the xattr block; caller must brelse() on success. */
static struct buffer_head *xattr_read_bh(struct inode *inode)
{
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_disk_xattr_block *xb;
	struct buffer_head *bh;

	if (OCSFS_SB(inode->i_sb)->s_clustered) {
		bh = sb_getblk(inode->i_sb, oi->i_xattr_block);
		if (!bh)
			return ERR_PTR(-EIO);
		clear_buffer_uptodate(bh);
		if (bh_read(bh, 0) < 0) {
			brelse(bh);
			return ERR_PTR(-EIO);
		}
	} else {
		bh = sb_bread(inode->i_sb, oi->i_xattr_block);
		if (!bh)
			return ERR_PTR(-EIO);
	}

	xb = (struct ocsfs_disk_xattr_block *)bh->b_data;
	if (le32_to_cpu(xb->xb_magic) != OCSFS_XATTR_MAGIC) {
		pr_err_ratelimited("ocsfs: xattr block bad magic ino %llu\n",
				   oi->i_disk_ino);
		brelse(bh);
		return ERR_PTR(-EIO);
	}
	{
		u32 csum = ocsfs_crc32c(~0U, xb,
				OCSFS_DEFAULT_BLOCK_SIZE - sizeof(__le32));
		if (csum != le32_to_cpu(xb->xb_checksum)) {
			pr_err_ratelimited("ocsfs: xattr block checksum mismatch ino %llu\n",
					   oi->i_disk_ino);
			brelse(bh);
			return ERR_PTR(-EIO);
		}
	}
	if (le16_to_cpu(xb->xb_data_len) > OCSFS_XATTR_DATA_SIZE) {
		pr_err_ratelimited("ocsfs: xattr block corrupt data_len %u ino %llu\n",
				   le16_to_cpu(xb->xb_data_len), oi->i_disk_ino);
		brelse(bh);
		return ERR_PTR(-EUCLEAN);
	}
	return bh;
}

/* ═══════════════════════════════════════════════════════════════
 * PUBLIC GET
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_xattr_get_internal(struct inode *inode, u8 ns, const char *name,
			     void *buffer, size_t size)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_disk_xattr_block *xb;
	struct ocsfs_xattr_entry *xe;
	struct buffer_head *bh;
	u8 name_len;
	u16 value_len;

	name_len = strlen(name);
	if (!name_len || name_len > 255)
		return -ERANGE;

	if (sbi->s_clustered) {
		int r = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					   OCSFS_LOCK_SH);
		if (r)
			return r;
		r = ocsfs_inode_refresh(inode);
		if (r) {
			ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
			return r;
		}
	}

	if (!oi->i_xattr_block) {
		if (sbi->s_clustered)
			ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
		return -ENODATA;
	}

	bh = xattr_read_bh(inode);
	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	xb = (struct ocsfs_disk_xattr_block *)bh->b_data;
	if (!le32_to_cpu(xb->xb_count)) {
		brelse(bh);
		return -ENODATA;
	}

	xe = xattr_find(xb, ns, name, name_len, NULL);
	if (!xe) {
		brelse(bh);
		return -ENODATA;
	}

	value_len = le16_to_cpu(xe->xe_value_len);
	if (!buffer) {
		brelse(bh);
		return value_len;
	}
	if (size < value_len) {
		brelse(bh);
		return -ERANGE;
	}
	memcpy(buffer, (u8 *)xe + sizeof(*xe) + name_len, value_len);
	brelse(bh);
	return value_len;
}

/* ═══════════════════════════════════════════════════════════════
 * PUBLIC SET / REMOVE
 * ═══════════════════════════════════════════════════════════════ */

int ocsfs_xattr_set_internal(struct inode *inode, u8 ns, const char *name,
			     const void *value, size_t size, int flags)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_disk_xattr_block *xb;
	struct ocsfs_xattr_entry *xe;
	struct buffer_head *bh = NULL;
	struct ocsfs_txn *txn;
	bool remove = (value == NULL);
	bool need_alloc;
	u8  name_len;
	u32 new_sz;
	u32 data_len;
	u32 found_off;
	int ret;

	name_len = strlen(name);
	if (!name_len || name_len > 255)
		return -ERANGE;

	if (!remove) {
		new_sz = sizeof(struct ocsfs_xattr_entry) + name_len + size;
		if (new_sz > OCSFS_XATTR_DATA_SIZE)
			return -E2BIG;
	} else {
		new_sz = 0;
	}

	if (sbi->s_clustered) {
		ret = ocsfs_lock_acquire(inode->i_sb, &oi->i_lock_res,
					 OCSFS_LOCK_EX);
		if (ret)
			return ret;
	}

	need_alloc = (oi->i_xattr_block == 0);

	/* Pre-flight: check existence/flags before starting txn */
	if (need_alloc) {
		if (remove || (flags & XATTR_REPLACE)) {
			ret = -ENODATA;
			goto out_release;
		}
	} else {
		struct buffer_head *peek = xattr_read_bh(inode);

		if (IS_ERR(peek)) {
			ret = PTR_ERR(peek);
			goto out_release;
		}
		xb = (struct ocsfs_disk_xattr_block *)peek->b_data;
		xe = xattr_find(xb, ns, name, name_len, NULL);
		brelse(peek);

		if (xe && (flags & XATTR_CREATE)) {
			ret = -EEXIST;
			goto out_release;
		}
		if (!xe && ((flags & XATTR_REPLACE) || remove)) {
			ret = -ENODATA;
			goto out_release;
		}
	}

	txn = ocsfs_txn_begin(inode->i_sb);
	if (IS_ERR(txn)) {
		ret = PTR_ERR(txn);
		goto out_release;
	}

	if (need_alloc) {
		u64 new_block;

		ret = ocsfs_alloc_blocks_txn(txn, inode->i_sb, oi->i_ag, 1,
					     &new_block);
		if (ret)
			goto out_abort;

		bh = sb_getblk(inode->i_sb, new_block);
		if (!bh) {
			ret = -EIO;
			goto out_abort;
		}
		lock_buffer(bh);
		memset(bh->b_data, 0, OCSFS_DEFAULT_BLOCK_SIZE);
		((struct ocsfs_disk_xattr_block *)bh->b_data)->xb_magic =
			cpu_to_le32(OCSFS_XATTR_MAGIC);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		oi->i_xattr_block = new_block;

		ret = ocsfs_txn_add_bh(txn, bh);
		if (ret) {
			brelse(bh);
			oi->i_xattr_block = 0;
			goto out_abort;
		}
	} else {
		if (sbi->s_clustered) {
			bh = sb_getblk(inode->i_sb, oi->i_xattr_block);
			if (!bh) { ret = -EIO; goto out_abort; }
			clear_buffer_uptodate(bh);
			if (bh_read(bh, 0) < 0) {
				brelse(bh);
				ret = -EIO;
				goto out_abort;
			}
		} else {
			bh = sb_bread(inode->i_sb, oi->i_xattr_block);
			if (!bh) { ret = -EIO; goto out_abort; }
		}

		ret = ocsfs_txn_add_bh(txn, bh);
		if (ret) {
			brelse(bh);
			goto out_abort;
		}
	}

	xb = (struct ocsfs_disk_xattr_block *)bh->b_data;
	data_len = le16_to_cpu(xb->xb_data_len);
	xe = xattr_find(xb, ns, name, name_len, &found_off);

	if (xe) {
		/* Remove old entry by compacting */
		u32 old_sz  = xe_total(xe);
		u32 tail    = data_len - found_off - old_sz;

		memmove(xb->xb_data + found_off,
			xb->xb_data + found_off + old_sz, tail);
		data_len -= old_sz;
		le32_add_cpu(&xb->xb_count, -1);
	}

	if (!remove) {
		if (data_len + new_sz > OCSFS_XATTR_DATA_SIZE) {
			brelse(bh);
			ret = -ENOSPC;
			goto out_abort;
		}
		xe = (struct ocsfs_xattr_entry *)(xb->xb_data + data_len);
		xe->xe_ns        = ns;
		xe->xe_name_len  = name_len;
		xe->xe_value_len = cpu_to_le16(size);
		memcpy((u8 *)xe + sizeof(*xe), name, name_len);
		memcpy((u8 *)xe + sizeof(*xe) + name_len, value, size);
		data_len += new_sz;
		le32_add_cpu(&xb->xb_count, 1);
	}

	xb->xb_data_len = cpu_to_le16(data_len);
	xb->xb_checksum = cpu_to_le32(
		ocsfs_crc32c(~0U, xb,
			     OCSFS_DEFAULT_BLOCK_SIZE - sizeof(__le32)));

	ret = ocsfs_txn_commit(txn);
	if (ret == 0) {
		mark_inode_dirty(inode);
		if (sbi->s_clustered)
			ocsfs_flush_inode_locked(inode, true);
	}
	brelse(bh);

out_release:
	if (sbi->s_clustered)
		ocsfs_lock_release(inode->i_sb, &oi->i_lock_res);
	return ret;

out_abort:
	ocsfs_txn_abort(txn);
	goto out_release;
}

/* ═══════════════════════════════════════════════════════════════
 * PUBLIC LISTXATTR
 * ═══════════════════════════════════════════════════════════════ */

static const char * const ns_pfx[4] = {
	XATTR_USER_PREFIX, XATTR_TRUSTED_PREFIX,
	XATTR_SECURITY_PREFIX, XATTR_SYSTEM_PREFIX,
};

ssize_t ocsfs_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct ocsfs_inode_info *oi = OCSFS_I(inode);
	struct ocsfs_disk_xattr_block *xb;
	struct buffer_head *bh;
	u32 data_len, off;
	ssize_t total = 0;

	if (!oi->i_xattr_block)
		return 0;

	bh = xattr_read_bh(inode);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	xb       = (struct ocsfs_disk_xattr_block *)bh->b_data;
	data_len = le16_to_cpu(xb->xb_data_len);
	off      = 0;

	while (off + sizeof(struct ocsfs_xattr_entry) <= data_len) {
		struct ocsfs_xattr_entry *xe =
			(struct ocsfs_xattr_entry *)(xb->xb_data + off);
		u32 total_sz = xe_total(xe);
		size_t plen;
		ssize_t entry_sz;

		if (off + total_sz > data_len)
			break;

		if (xe->xe_ns >= ARRAY_SIZE(ns_pfx))
			goto next;

		plen     = strlen(ns_pfx[xe->xe_ns]);
		entry_sz = plen + xe->xe_name_len + 1; /* +1 NUL */
		total   += entry_sz;

		if (buffer) {
			if (total > (ssize_t)size) {
				brelse(bh);
				return -ERANGE;
			}
			memcpy(buffer, ns_pfx[xe->xe_ns], plen);
			buffer += plen;
			memcpy(buffer, xb->xb_data + off + sizeof(*xe),
			       xe->xe_name_len);
			buffer += xe->xe_name_len;
			*buffer++ = '\0';
		}
next:
		off += total_sz;
	}

	brelse(bh);
	return total;
}

/* ═══════════════════════════════════════════════════════════════
 * XATTR HANDLERS
 * ═══════════════════════════════════════════════════════════════ */

static int handler_get(const struct xattr_handler *handler,
		       struct dentry *dentry, struct inode *inode,
		       const char *name, void *buffer, size_t size)
{
	return ocsfs_xattr_get_internal(inode,
					(u8)handler->flags,
					name, buffer, size);
}

static int handler_set(const struct xattr_handler *handler,
		       struct mnt_idmap *idmap,
		       struct dentry *dentry, struct inode *inode,
		       const char *name, const void *value,
		       size_t size, int flags)
{
	return ocsfs_xattr_set_internal(inode,
					(u8)handler->flags,
					name, value, size, flags);
}

static const struct xattr_handler ocsfs_xattr_user_handler = {
	.prefix = XATTR_USER_PREFIX,
	.flags  = OCSFS_XATTR_NS_USER,
	.get    = handler_get,
	.set    = handler_set,
};

static const struct xattr_handler ocsfs_xattr_trusted_handler = {
	.prefix = XATTR_TRUSTED_PREFIX,
	.flags  = OCSFS_XATTR_NS_TRUSTED,
	.get    = handler_get,
	.set    = handler_set,
};

static const struct xattr_handler ocsfs_xattr_security_handler = {
	.prefix = XATTR_SECURITY_PREFIX,
	.flags  = OCSFS_XATTR_NS_SECURITY,
	.get    = handler_get,
	.set    = handler_set,
};

const struct xattr_handler * const ocsfs_xattr_handlers[] = {
	&ocsfs_xattr_user_handler,
	&ocsfs_xattr_trusted_handler,
	&ocsfs_xattr_security_handler,
	NULL,
};
