// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — flock.c
 * POSIX distributed file locking: map fcntl(F_SETLK/F_SETLKW) to the
 * on-disk DLM so that byte-range locks are visible across all cluster nodes.
 *
 * Design:
 *   We use inode-granularity locks (not byte-range) because the on-disk DLM
 *   has no range concept.  This is coarser than strict POSIX but is correct
 *   for the primary use case (qcow2 image files opened by QEMU on multiple
 *   Proxmox nodes): each VM holds an EX lock on its image file; concurrent
 *   reads for live migration use SH.
 *
 *   Local lock tracking (posix_lock_file) runs first so that same-node
 *   processes still get proper byte-range semantics.  The DLM step follows
 *   only for cross-node coherence.
 *
 * Lock mapping:
 *   F_RDLCK → DLM SH   (multiple readers, no writers)
 *   F_WRLCK → DLM EX   (exclusive)
 *   F_UNLCK → DLM release (back to NL if no remaining local locks)
 */

#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/filelock.h>
#include "ocsfs.h"

/* Returns the strongest local POSIX lock remaining on this inode for any
 * process on this node.  Called AFTER posix_lock_file() has already applied
 * the current operation, so the list already reflects the new state. */
static unsigned char inode_local_lock_type(struct inode *inode)
{
	struct file_lock_context *flctx = locks_inode_context(inode);
	struct file_lock *fl;
	unsigned char best = F_UNLCK;

	if (!flctx)
		return F_UNLCK;

	spin_lock(&flctx->flc_lock);
	list_for_each_entry(fl, &flctx->flc_posix, c.flc_list) {
		if (fl->c.flc_type == F_WRLCK) {
			best = F_WRLCK;
			break;
		}
		best = F_RDLCK;
	}
	spin_unlock(&flctx->flc_lock);
	return best;
}

int ocsfs_file_lock(struct file *file, int cmd, struct file_lock *fl)
{
	struct inode *inode = file_inode(file);
	struct ocsfs_sb_info *sbi = OCSFS_SB(inode->i_sb);
	struct ocsfs_lock_res *lr = &OCSFS_I(inode)->i_lock_res;
	int ret;
	u16 dlm_mode;
	unsigned char local_type;

	/* Step 1: apply the POSIX lock locally — byte-range semantics */
	ret = posix_lock_file(file, fl, NULL);
	if (ret)
		return ret;

	/* Step 2: single-node mode — no DLM needed */
	if (!sbi->s_clustered)
		return 0;

	/* Step 3: determine required DLM mode from remaining local locks */
	local_type = inode_local_lock_type(inode);

	switch (local_type) {
	case F_WRLCK:
		dlm_mode = OCSFS_LOCK_EX;
		break;
	case F_RDLCK:
		dlm_mode = OCSFS_LOCK_SH;
		break;
	default:
		/* No local locks remain — release the DLM lock if held */
		if (lr->lr_mode != OCSFS_LOCK_NL)
			ocsfs_lock_release(inode->i_sb, lr);
		return 0;
	}

	/* Step 4: upgrade or acquire DLM lock */
	if (lr->lr_mode == dlm_mode)
		return 0;  /* already correct */

	if (lr->lr_mode != OCSFS_LOCK_NL) {
		ret = ocsfs_lock_release(inode->i_sb, lr);
		if (ret)
			return ret;
	}

	ret = ocsfs_lock_acquire(inode->i_sb, lr, dlm_mode);
	if (ret == -ETIMEDOUT)
		return -ENOLCK;   /* POSIX: ENOLCK when lock cannot be granted */
	return ret;
}
