/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OCSFS — lock_internal.h
 * Private declarations shared between lock.c and lock_io.c.
 */

#ifndef OCSFS_LOCK_INTERNAL_H
#define OCSFS_LOCK_INTERNAL_H

/* lock_io.c — disk I/O helpers */
int  lock_read_entry(struct super_block *sb, u32 slot,
		     struct ocsfs_disk_lock *out,
		     struct buffer_head **bh_out);
int  lock_write_entry(struct super_block *sb, u32 slot,
		      struct ocsfs_disk_lock *entry,
		      struct buffer_head *bh);
int  lock_probe_slot(struct super_block *sb, struct ocsfs_lock_res *lr);

/* lock_io.c — bitmask helpers */
void set_waiter_bit(struct ocsfs_disk_lock *dl, u16 slot);
void clear_waiter_bit(struct ocsfs_disk_lock *dl, u16 slot);
bool is_sh_holder(struct ocsfs_disk_lock *dl, u16 slot);
void add_sh_holder(struct ocsfs_disk_lock *dl, u16 slot);
void remove_sh_holder(struct ocsfs_disk_lock *dl, u16 slot);
bool has_sh_holders(struct ocsfs_disk_lock *dl);
bool has_waiters(struct ocsfs_disk_lock *dl);

#endif /* OCSFS_LOCK_INTERNAL_H */
