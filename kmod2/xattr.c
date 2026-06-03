// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — xattr.c
 * Extended attributes and POSIX ACLs. All of an inode's xattrs (user / trusted
 * / security namespaces and the ACL "system.posix_acl_*" attrs) live in one
 * 4 KiB block (i_xattr_block). Entries are packed [le16 name_len][le16 val_len]
 * [name][value], 4-byte aligned, with the full prefixed name stored. Mutations
 * join the caller's journal transaction when one is active (so ACL inheritance
 * is atomic with create), else run in their own. Single-node.
 */
#include "ocsfs.h"
#include <linux/xattr.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/unaligned.h>
#include <linux/slab.h>

#define XH_SZ  ((u32)sizeof(struct ocsfs2_disk_xattr_header))

/* crc over the block, skipping the 4-byte checksum field at offset 12 */
static u32 xattr_crc(const void *block, u32 bs)
{
	u32 off = offsetof(struct ocsfs2_disk_xattr_header, xh_checksum);
	u32 c = ocsfs2_crc32c(~0U, block, off);

	return ocsfs2_crc32c(c, (const u8 *)block + off + 4, bs - off - 4);
}

static bool xattr_block_valid(const void *block, u32 bs)
{
	const struct ocsfs2_disk_xattr_header *h = block;

	return le32_to_cpu(h->xh_magic) == OCSFS2_XATTR_MAGIC &&
	       xattr_crc(block, bs) == le32_to_cpu(h->xh_checksum);
}

/* ── lookup ── */

int ocsfs2_xattr_get(struct inode *inode, const char *name, void *buf,
		     size_t size)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	struct buffer_head *bh;
	u32 count, o, i, bs = sb->s_blocksize;
	size_t namelen = strlen(name);
	int ret = -ENODATA;

	mutex_lock(&oi->i_meta_lock);
	if (!oi->i_xattr_block)
		goto out;
	bh = sb_bread(sb, oi->i_xattr_block);
	if (!bh) { ret = -EIO; goto out; }
	if (!xattr_block_valid(bh->b_data, bs)) { ret = -EIO; goto rel; }

	count = le32_to_cpu(((struct ocsfs2_disk_xattr_header *)bh->b_data)->xh_count);
	o = XH_SZ;
	for (i = 0; i < count && o + 4 <= bs; i++) {
		u8 *e = (u8 *)bh->b_data + o;
		u16 nl = get_unaligned_le16(e), vl = get_unaligned_le16(e + 2);

		if (o + 4 + nl + vl > bs)
			break;
		if (nl == namelen && !memcmp(e + 4, name, nl)) {
			if (!buf)
				ret = vl;
			else if (size < vl)
				ret = -ERANGE;
			else {
				memcpy(buf, e + 4 + nl, vl);
				ret = vl;
			}
			break;
		}
		o += ALIGN(4 + nl + vl, 4);
	}
rel:
	brelse(bh);
out:
	mutex_unlock(&oi->i_meta_lock);
	return ret;
}

/* ── mutate ── */

int ocsfs2_xattr_set(struct inode *inode, const char *name, const void *value,
		     size_t size, int flags)
{
	struct super_block *sb = inode->i_sb;
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	u32 bs = sb->s_blocksize, o_new = XH_SZ, count = 0;
	size_t namelen = strlen(name);
	struct ocsfs2_disk_xattr_header *nh;
	struct buffer_head *obh = NULL, *bh;
	bool own_txn = !ocsfs2_current_txn();
	struct ocsfs2_txn *txn = NULL;
	bool found = false;
	u8 *nb;
	int ret = 0;

	/* A standalone xattr/ACL write mutates an existing file -> EX lease first.
	 * When called inside a create txn (own_txn==false) the inode is brand-new and
	 * owned by this node already, so skip it (and avoid leaking a lease). */
	if (own_txn) {
		ret = ocsfs2_inode_ensure_writable(inode);
		if (ret)
			return ret;
	}

	if (namelen == 0 || namelen > OCSFS2_MAX_NAME)
		return -EINVAL;
	if (size > OCSFS2_XATTR_SPACE)
		return -E2BIG;

	nb = kzalloc(bs, GFP_NOFS);
	if (!nb)
		return -ENOMEM;
	nh = (struct ocsfs2_disk_xattr_header *)nb;

	if (own_txn) {
		txn = ocsfs2_txn_begin(sb);
		if (!txn) { kfree(nb); return -ENOMEM; }
	}
	mutex_lock(&oi->i_meta_lock);

	if (oi->i_xattr_block) {
		struct ocsfs2_disk_xattr_header *oh;
		u32 oc, oo = XH_SZ, i;

		obh = sb_bread(sb, oi->i_xattr_block);
		if (!obh) { ret = -EIO; goto out; }
		if (!xattr_block_valid(obh->b_data, bs)) { ret = -EIO; goto out; }
		oh = (struct ocsfs2_disk_xattr_header *)obh->b_data;
		oc = le32_to_cpu(oh->xh_count);
		for (i = 0; i < oc && oo + 4 <= bs; i++) {
			u8 *e = (u8 *)obh->b_data + oo;
			u16 nl = get_unaligned_le16(e), vl = get_unaligned_le16(e + 2);
			u32 esz = ALIGN(4 + nl + vl, 4);

			if (oo + esz > bs)
				break;
			if (nl == namelen && !memcmp(e + 4, name, nl)) {
				found = true;          /* drop: replace or remove */
			} else {
				memcpy(nb + o_new, e, esz);
				o_new += esz;
				count++;
			}
			oo += esz;
		}
	}

	if ((flags & XATTR_CREATE) && found) { ret = -EEXIST; goto out; }
	if ((flags & XATTR_REPLACE) && !found) { ret = -ENODATA; goto out; }
	if (!value && !found) { ret = -ENODATA; goto out; }

	if (value) {
		u32 esz = ALIGN(4 + namelen + size, 4);
		u8 *e = nb + o_new;

		if (o_new + esz > bs) { ret = -ENOSPC; goto out; }
		put_unaligned_le16(namelen, e);
		put_unaligned_le16(size, e + 2);
		memcpy(e + 4, name, namelen);
		memcpy(e + 4 + namelen, value, size);
		o_new += esz;
		count++;
	}

	if (count == 0) {
		if (oi->i_xattr_block) {
			u64 old = oi->i_xattr_block;

			oi->i_xattr_block = 0;
			ocsfs2_free_blocks(sb, old, 1);   /* journaled if in txn */
		}
	} else {
		u64 blk = oi->i_xattr_block;

		if (!blk) {
			ret = ocsfs2_alloc_blocks(sb, oi->i_ag, 1, &blk);
			if (ret)
				goto out;
			oi->i_xattr_block = blk;
		}
		nh->xh_magic = cpu_to_le32(OCSFS2_XATTR_MAGIC);
		nh->xh_count = cpu_to_le32(count);
		nh->xh_used = cpu_to_le32(o_new - XH_SZ);
		nh->xh_checksum = cpu_to_le32(xattr_crc(nb, bs));
		bh = sb_bread(sb, blk);
		if (!bh) { ret = -EIO; goto out; }
		ocsfs2_jbuf(bh);
		memcpy(bh->b_data, nb, bs);
		if (!ocsfs2_current_txn()) {   /* in a txn the journal owns writeback */
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
		}
		brelse(bh);
	}
	ret = ocsfs2_write_inode_block(inode);

out:
	mutex_unlock(&oi->i_meta_lock);
	if (obh)
		brelse(obh);
	kfree(nb);
	if (own_txn) {
		if (ret)
			ocsfs2_txn_abort(txn);
		else
			ret = ocsfs2_txn_commit(txn);
	}
	return ret;
}

/* Free the xattr block when the inode is deleted. Caller (evict) is single
 * reference; no txn (matches the bitmap free in evict). */
void ocsfs2_xattr_free(struct inode *inode)
{
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);

	if (oi->i_xattr_block) {
		ocsfs2_free_blocks(inode->i_sb, oi->i_xattr_block, 1);
		oi->i_xattr_block = 0;
	}
}

/* ── listing ── */

static const struct xattr_handler *find_handler(const char *name)
{
	const struct xattr_handler * const *hp;

	for (hp = ocsfs2_xattr_handlers; *hp; hp++)
		if ((*hp)->prefix && !strncmp(name, (*hp)->prefix,
					      strlen((*hp)->prefix)))
			return *hp;
	return NULL;
}

ssize_t ocsfs2_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct ocsfs2_inode_info *oi = OCSFS2_I(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	u32 count, o, i, bs = sb->s_blocksize;
	ssize_t total = 0;

	mutex_lock(&oi->i_meta_lock);
	if (!oi->i_xattr_block)
		goto out;
	bh = sb_bread(sb, oi->i_xattr_block);
	if (!bh)
		goto out;
	if (!xattr_block_valid(bh->b_data, bs))
		goto rel;

	count = le32_to_cpu(((struct ocsfs2_disk_xattr_header *)bh->b_data)->xh_count);
	o = XH_SZ;
	for (i = 0; i < count && o + 4 <= bs; i++) {
		u8 *e = (u8 *)bh->b_data + o;
		u16 nl = get_unaligned_le16(e), vl = get_unaligned_le16(e + 2);
		char nm[OCSFS2_MAX_NAME + 1];
		const struct xattr_handler *h;

		if (o + 4 + nl > bs || nl > OCSFS2_MAX_NAME)
			break;
		memcpy(nm, e + 4, nl);
		nm[nl] = '\0';
		h = find_handler(nm);
		if (!(h && h->list && !h->list(dentry))) {  /* listable */
			if (buffer) {
				if (total + nl + 1 > size) { total = -ERANGE; goto rel; }
				memcpy(buffer + total, nm, nl);
				buffer[total + nl] = '\0';
			}
			total += nl + 1;
		}
		o += ALIGN(4 + nl + vl, 4);
	}
rel:
	brelse(bh);
out:
	mutex_unlock(&oi->i_meta_lock);
	return total;
}

/* ── namespace handlers (user / trusted / security) ── */

static int ocsfs2_xattr_h_get(const struct xattr_handler *handler,
			      struct dentry *unused, struct inode *inode,
			      const char *name, void *buffer, size_t size)
{
	return ocsfs2_xattr_get(inode, xattr_full_name(handler, name),
				buffer, size);
}

static int ocsfs2_xattr_h_set(const struct xattr_handler *handler,
			      struct mnt_idmap *idmap, struct dentry *unused,
			      struct inode *inode, const char *name,
			      const void *value, size_t size, int flags)
{
	return ocsfs2_xattr_set(inode, xattr_full_name(handler, name),
				value, size, flags);
}

static bool ocsfs2_xattr_trusted_list(struct dentry *dentry)
{
	return capable(CAP_SYS_ADMIN);
}

static const struct xattr_handler ocsfs2_xattr_user_handler = {
	.prefix = XATTR_USER_PREFIX,
	.get    = ocsfs2_xattr_h_get,
	.set    = ocsfs2_xattr_h_set,
};
static const struct xattr_handler ocsfs2_xattr_trusted_handler = {
	.prefix = XATTR_TRUSTED_PREFIX,
	.list   = ocsfs2_xattr_trusted_list,
	.get    = ocsfs2_xattr_h_get,
	.set    = ocsfs2_xattr_h_set,
};
static const struct xattr_handler ocsfs2_xattr_security_handler = {
	.prefix = XATTR_SECURITY_PREFIX,
	.get    = ocsfs2_xattr_h_get,
	.set    = ocsfs2_xattr_h_set,
};

const struct xattr_handler * const ocsfs2_xattr_handlers[] = {
	&ocsfs2_xattr_user_handler,
	&ocsfs2_xattr_trusted_handler,
	&ocsfs2_xattr_security_handler,
	NULL,
};

/* ── POSIX ACL ── */
#ifdef CONFIG_FS_POSIX_ACL

static const char *acl_xattr_name(int type)
{
	return type == ACL_TYPE_ACCESS ? XATTR_NAME_POSIX_ACL_ACCESS
				       : XATTR_NAME_POSIX_ACL_DEFAULT;
}

struct posix_acl *ocsfs2_get_acl(struct inode *inode, int type, bool rcu)
{
	const char *name = acl_xattr_name(type);
	struct posix_acl *acl;
	void *value;
	int len;

	if (rcu)
		return ERR_PTR(-ECHILD);

	len = ocsfs2_xattr_get(inode, name, NULL, 0);
	if (len == -ENODATA)
		return NULL;
	if (len < 0)
		return ERR_PTR(len);
	value = kmalloc(len, GFP_NOFS);
	if (!value)
		return ERR_PTR(-ENOMEM);
	len = ocsfs2_xattr_get(inode, name, value, len);
	if (len < 0)
		acl = ERR_PTR(len);
	else
		acl = posix_acl_from_xattr(&init_user_ns, value, len);
	kfree(value);
	return acl;
}

static int __ocsfs2_set_acl(struct inode *inode, struct posix_acl *acl, int type)
{
	const char *name = acl_xattr_name(type);
	void *value = NULL;
	size_t size = 0;
	int ret;

	if (acl) {
		/* this kernel's posix_acl_to_xattr allocates and returns the
		 * buffer, with the length in *sizep */
		value = posix_acl_to_xattr(&init_user_ns, acl, &size, GFP_NOFS);
		if (IS_ERR(value))
			return PTR_ERR(value);
	}
	ret = ocsfs2_xattr_set(inode, name, value, size, 0);
	kfree(value);
	return ret;
}

int ocsfs2_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		   struct posix_acl *acl, int type)
{
	struct inode *inode = d_inode(dentry);
	umode_t mode = inode->i_mode;
	int ret;

	if (type == ACL_TYPE_ACCESS && acl) {
		ret = posix_acl_update_mode(idmap, inode, &mode, &acl);
		if (ret)
			return ret;
		if (mode != inode->i_mode) {
			inode->i_mode = mode;
			inode_set_ctime_current(inode);
		}
	}
	ret = __ocsfs2_set_acl(inode, acl, type);
	if (!ret) {
		set_cached_acl(inode, type, acl);   /* keep the VFS cache coherent */
		mark_inode_dirty(inode);
	}
	return ret;
}

/* Inherit the parent's default ACL on create, applying umask otherwise. Runs
 * inside the create transaction (ocsfs2_xattr_set joins it). */
int ocsfs2_init_acl(struct inode *inode, struct inode *dir)
{
	struct posix_acl *default_acl = NULL, *acl = NULL;
	int ret;

	ret = posix_acl_create(dir, &inode->i_mode, &default_acl, &acl);
	if (ret)
		return ret;
	if (default_acl) {
		ret = __ocsfs2_set_acl(inode, default_acl, ACL_TYPE_DEFAULT);
		if (!ret)
			set_cached_acl(inode, ACL_TYPE_DEFAULT, default_acl);
		posix_acl_release(default_acl);
	}
	if (!ret && acl) {
		ret = __ocsfs2_set_acl(inode, acl, ACL_TYPE_ACCESS);
		if (!ret)
			set_cached_acl(inode, ACL_TYPE_ACCESS, acl);
		posix_acl_release(acl);
	} else if (acl) {
		posix_acl_release(acl);
	}
	return ret;
}

#else  /* !CONFIG_FS_POSIX_ACL */

int ocsfs2_init_acl(struct inode *inode, struct inode *dir) { return 0; }

#endif
