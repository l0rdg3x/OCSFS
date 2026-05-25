// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — acl.c
 * POSIX Access Control List support.
 *
 * ACLs are stored as xattrs in the OCSFS_XATTR_NS_SYSTEM namespace:
 *   "posix_acl_access"  (ACL_TYPE_ACCESS)
 *   "posix_acl_default" (ACL_TYPE_DEFAULT, directories only)
 *
 * posix_acl_to_xattr() in Linux 7.x allocates its output buffer and
 * returns void * (or ERR_PTR on error); caller must kfree it.
 */

#include "ocsfs.h"
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>

/* Xattr name suffix (after "system.") for each ACL type. */
static const char *acl_suffix(int type)
{
	return (type == ACL_TYPE_ACCESS) ? "posix_acl_access" : "posix_acl_default";
}

/*
 * ocsfs_get_inode_acl — Read a POSIX ACL from xattr storage.
 *
 * Called by the VFS for permission checks and getxattr.
 * Returns ERR_PTR(-ECHILD) when rcu=true (we need I/O).
 */
struct posix_acl *ocsfs_get_inode_acl(struct inode *inode, int type, bool rcu)
{
	const char *name = acl_suffix(type);
	struct posix_acl *acl;
	void *value;
	int ret;

	if (rcu)
		return ERR_PTR(-ECHILD);

	/* Size probe */
	ret = ocsfs_xattr_get_internal(inode, OCSFS_XATTR_NS_SYSTEM,
				       name, NULL, 0);
	if (ret == -ENODATA)
		return NULL;
	if (ret < 0)
		return ERR_PTR(ret);

	value = kmalloc(ret, GFP_KERNEL);
	if (!value)
		return ERR_PTR(-ENOMEM);

	ret = ocsfs_xattr_get_internal(inode, OCSFS_XATTR_NS_SYSTEM,
				       name, value, ret);
	if (ret < 0) {
		acl = ERR_PTR(ret);
		goto out;
	}

	acl = posix_acl_from_xattr(&init_user_ns, value, ret);
out:
	kfree(value);
	return acl;
}

/*
 * ocsfs_set_acl — Write or remove a POSIX ACL via xattr storage.
 *
 * For ACL_TYPE_ACCESS, updates inode->i_mode via posix_acl_update_mode.
 * acl=NULL removes the ACL entry.
 */
int ocsfs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct posix_acl *acl, int type)
{
	struct inode *inode = d_inode(dentry);
	const char *name = acl_suffix(type);
	void *xattr_val = NULL;
	size_t xattr_size = 0;
	int ret;

	if (type == ACL_TYPE_ACCESS && acl) {
		ret = posix_acl_update_mode(idmap, inode, &inode->i_mode, &acl);
		if (ret)
			return ret;
	}

	if (acl) {
		/* posix_acl_to_xattr allocates the buffer; returns ERR_PTR on error */
		xattr_val = posix_acl_to_xattr(&init_user_ns, acl,
					       &xattr_size, GFP_KERNEL);
		if (IS_ERR(xattr_val))
			return PTR_ERR(xattr_val);
	}

	ret = ocsfs_xattr_set_internal(inode, OCSFS_XATTR_NS_SYSTEM,
				       name, xattr_val, xattr_size, 0);
	kfree(xattr_val);
	if (ret == -ENODATA && !acl)
		ret = 0; /* removing a non-existent ACL is a no-op */
	if (!ret)
		mark_inode_dirty(inode);
	return ret;
}

/*
 * ocsfs_init_acl — Inherit default ACL from parent directory on inode creation.
 *
 * Call this after ocsfs_new_inode() and before d_instantiate().
 * posix_acl_create() updates inode->i_mode (umask + default ACL masking)
 * and returns the ACLs to store on the new inode.
 */
int ocsfs_init_acl(struct mnt_idmap *idmap, struct inode *inode,
		   struct inode *dir)
{
	struct posix_acl *default_acl = NULL, *acl = NULL;
	void *xattr_val;
	size_t xattr_size;
	int ret;

	ret = posix_acl_create(dir, &inode->i_mode, &default_acl, &acl);
	if (ret)
		return ret;

	if (default_acl) {
		xattr_val = posix_acl_to_xattr(&init_user_ns, default_acl,
					       &xattr_size, GFP_KERNEL);
		if (IS_ERR(xattr_val)) {
			ret = PTR_ERR(xattr_val);
			goto out;
		}
		ret = ocsfs_xattr_set_internal(inode, OCSFS_XATTR_NS_SYSTEM,
					       "posix_acl_default",
					       xattr_val, xattr_size, 0);
		kfree(xattr_val);
		if (ret)
			goto out;
	}

	if (acl) {
		xattr_val = posix_acl_to_xattr(&init_user_ns, acl,
					       &xattr_size, GFP_KERNEL);
		if (IS_ERR(xattr_val)) {
			ret = PTR_ERR(xattr_val);
			goto out;
		}
		ret = ocsfs_xattr_set_internal(inode, OCSFS_XATTR_NS_SYSTEM,
					       "posix_acl_access",
					       xattr_val, xattr_size, 0);
		kfree(xattr_val);
	}
out:
	posix_acl_release(default_acl);
	posix_acl_release(acl);
	return (ret < 0) ? ret : 0;
}
