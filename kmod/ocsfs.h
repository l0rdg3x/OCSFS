/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OCSFS — Open Cluster Shared FileSystem
 * Internal kernel header
 *
 * Copyright (C) 2026 OCSFS Project Contributors
 *
 * This header is private to the kernel module. The on-disk format
 * constants are duplicated here (not #include'd from the userspace
 * ocsfs.h) because kernel code must use __le16/__le32/__le64 types
 * and kernel-only APIs.
 */

#ifndef _OCSFS_KMOD_H
#define _OCSFS_KMOD_H

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/crc32c.h>
#include <linux/uuid.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/mpage.h>
#include <linux/time64.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/parser.h>

/* ═══════════════════════════════════════════════════════════════
 * ON-DISK CONSTANTS (mirrored from userspace ocsfs.h)
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_MAGIC             0x4F435346  /* 'OCSF' */
#define OCSFS_INODE_MAGIC       0x494E4F44  /* 'INOD' */
#define OCSFS_AG_MAGIC          0x41474850  /* 'AGHD' */
#define OCSFS_JOURNAL_MAGIC     0x4A524E4C  /* 'JRNL' */
#define OCSFS_LOCK_MAGIC        0x4C4F434B  /* 'LOCK' */
#define OCSFS_HEARTBEAT_MAGIC   0x48425454  /* 'HBTT' */
#define OCSFS_DIRENT_MAGIC      0x44495245  /* 'DIRE' */

#define OCSFS_VERSION_MAJOR     0
#define OCSFS_VERSION_MINOR     1

#define OCSFS_SUPERBLOCK_SIZE       4096
#define OCSFS_SUPERBLOCK_OFFSET     0
#define OCSFS_SUPERBLOCK_MIRROR     4096

#define OCSFS_NODE_SLOT_TABLE_OFF   8192
#define OCSFS_NODE_SLOT_TABLE_SIZE  65536
#define OCSFS_HEARTBEAT_OFF         73728
#define OCSFS_HEARTBEAT_SIZE        262144
#define OCSFS_HEARTBEAT_ENTRY_SIZE  1024
#define OCSFS_LOCK_TABLE_OFF        335872
#define OCSFS_LOCK_TABLE_SIZE       1048576
#define OCSFS_LOCK_ENTRY_SIZE       256
#define OCSFS_LOCK_ENTRY_COUNT      (OCSFS_LOCK_TABLE_SIZE / OCSFS_LOCK_ENTRY_SIZE)

#define OCSFS_MAX_NODES             256
#define OCSFS_DEFAULT_MAX_NODES     64
#define OCSFS_MAX_LABEL             64

#define OCSFS_DEFAULT_BLOCK_SIZE    4096
#define OCSFS_DEFAULT_EXTENT_SIZE   (1 << 20)
#define OCSFS_DEFAULT_AG_SIZE       (1ULL << 30)
#define OCSFS_DEFAULT_JOURNAL_SIZE  (32 << 20)

#define OCSFS_INODE_SIZE            512
#define OCSFS_INLINE_EXTENTS        16
#define OCSFS_MAX_NAME_LEN          255

#define OCSFS_ROOT_INO              2
#define OCSFS_FIRST_USER_INO        64

/* Feature flags */
#define OCSFS_FEAT_THIN_PROV    (1ULL << 0)
#define OCSFS_FEAT_COMPRESSION  (1ULL << 1)
#define OCSFS_FEAT_ENCRYPTION   (1ULL << 2)
#define OCSFS_FEAT_SNAPSHOTS    (1ULL << 3)
#define OCSFS_FEAT_DEDUP        (1ULL << 4)
#define OCSFS_FEAT_MULTI_LUN    (1ULL << 5)

/* Inode flags */
#define OCSFS_IFLAG_THIN        0x0001
#define OCSFS_IFLAG_COMPRESSED  0x0002
#define OCSFS_IFLAG_ENCRYPTED   0x0004
#define OCSFS_IFLAG_IMMUTABLE   0x0008
#define OCSFS_IFLAG_APPEND      0x0010

/* Extent flags */
#define OCSFS_EXT_WRITTEN       0x0000
#define OCSFS_EXT_UNWRITTEN     0x0001

/* File types */
#define OCSFS_FT_UNKNOWN        0
#define OCSFS_FT_REG_FILE       1
#define OCSFS_FT_DIR            2
#define OCSFS_FT_CHRDEV         3
#define OCSFS_FT_BLKDEV         4
#define OCSFS_FT_FIFO           5
#define OCSFS_FT_SOCK           6
#define OCSFS_FT_SYMLINK        7

/* Journal transaction types */
#define OCSFS_JTYPE_BEGIN       1
#define OCSFS_JTYPE_METADATA    2
#define OCSFS_JTYPE_COMMIT      3
#define OCSFS_JTYPE_ABORT       4
#define OCSFS_JTYPE_CHECKPOINT  5

#define OCSFS_JBR_BEFORE        0x01
#define OCSFS_JBR_AFTER         0x02

/* ═══════════════════════════════════════════════════════════════
 * ON-DISK STRUCTURES (little-endian on disk)
 * ═══════════════════════════════════════════════════════════════ */

struct ocsfs_disk_super {
	__le32  s_magic;
	__le16  s_version_major;
	__le16  s_version_minor;
	__u8    s_uuid[16];
	__u8    s_label[OCSFS_MAX_LABEL];
	__le32  s_block_size;
	__le32  s_extent_size;
	__le64  s_total_blocks;
	__le64  s_free_blocks;
	__le32  s_ag_count;
	__le64  s_ag_size;
	__le16  s_max_nodes;
	__le64  s_feature_flags;
	__le32  s_heartbeat_interval;
	__le32  s_heartbeat_timeout;
	__le32  s_journal_size;
	__le64  s_lock_table_off;
	__le64  s_journal_off;
	__le64  s_ag_desc_off;
	__le64  s_data_off;
	__le64  s_mkfs_time;
	__le64  s_mount_count;
	__le64  s_last_mount_time;
	__u8    s_reserved[3890];
	__le32  s_checksum;
} __packed;

struct ocsfs_disk_inode {
	__le32  i_magic;
	__le64  i_ino;
	__le16  i_mode;
	__le16  i_nlink;
	__le32  i_uid;
	__le32  i_gid;
	__le64  i_size;
	__le64  i_blocks;
	__le64  i_atime;
	__le64  i_mtime;
	__le64  i_ctime;
	__le32  i_flags;
	__le16  i_extent_count;
	__le16  i_extent_max;
	__le64  i_extent_tree_root;
	__le64  i_thin_allocated;
	__le32  i_ag;
	__u8    i_inline_extents[OCSFS_INLINE_EXTENTS * 24];
	__u8    i_reserved[12];
	__le32  i_checksum;
} __packed;

struct ocsfs_disk_extent {
	__le64  e_logical_block;
	__le64  e_physical_block;
	__le32  e_length;
	__le16  e_flags;
	__le16  e_checksum;
} __packed;

struct ocsfs_disk_ag {
	__le32  ag_magic;
	__le32  ag_number;
	__le64  ag_block_start;
	__le64  ag_block_count;
	__le64  ag_free_blocks;
	__le64  ag_free_extents;
	__le64  ag_bitmap_off;
	__le64  ag_bitmap_size;
	__le64  ag_inode_table_off;
	__le64  ag_inode_count;
	__le64  ag_free_inodes;
	__le64  ag_extent_tree_off;
	__le64  ag_inode_btree_off;
	__le16  ag_owner_node;
	__le16  ag_flags;
	__u8    ag_reserved[3992];
	__le32  ag_checksum;
} __packed;

struct ocsfs_disk_dirent {
	__le32  de_magic;
	__le64  de_ino;
	__le64  de_name_hash;
	__u8    de_file_type;
	__u8    de_name_len;
	__u8    de_name[OCSFS_MAX_NAME_LEN + 1];
	__le16  de_rec_len;
	__le16  de_checksum;
} __packed;

struct ocsfs_disk_journal_hdr {
	__le32  jh_magic;
	__le16  jh_node_slot;
	__le16  jh_flags;
	__le64  jh_head;
	__le64  jh_tail;
	__le64  jh_sequence;
	__le64  jh_size;
	__u8    jh_reserved[4052];
	__le32  jh_checksum;
} __packed;

struct ocsfs_disk_journal_txn {
	__le32  jt_type;
	__le64  jt_id;
	__le64  jt_timestamp;
	__le16  jt_node_slot;
	__le16  jt_block_count;
	__le32  jt_data_len;
	__le32  jt_checksum;
} __packed;

struct ocsfs_disk_journal_bref {
	__le64  jbr_block_num;
	__le32  jbr_flags;
	__le32  jbr_checksum;
} __packed;

/* ═══════════════════════════════════════════════════════════════
 * IN-MEMORY STRUCTURES
 * ═══════════════════════════════════════════════════════════════ */

/* Per-extent in-memory representation */
struct ocsfs_extent {
	u64     logical_block;
	u64     physical_block;
	u32     length;         /* blocks */
	u16     flags;
};

/* Per-AG in-memory state */
struct ocsfs_ag_info {
	u32             ag_no;
	u64             block_start;    /* first block (absolute) */
	u64             block_count;
	u64             free_blocks;
	u64             bitmap_off;     /* byte offset on disk */
	u64             bitmap_size;
	u64             inode_table_off;
	u64             inode_count;
	u64             free_inodes;
	struct mutex    ag_lock;        /* protects bitmap + inode table */
};

/* Journal in-memory state */
struct ocsfs_journal {
	u64             disk_off;       /* byte offset on block device */
	u64             size;           /* journal region size */
	u64             head;           /* write position */
	u64             tail;           /* oldest live txn */
	u64             sequence;       /* next txn ID */
	struct mutex    j_lock;
	struct buffer_head *j_header_bh;
};

/* Active journal transaction */
struct ocsfs_txn {
	u64                     t_id;
	struct ocsfs_journal    *t_journal;
	struct list_head        t_buffers;      /* list of journaled BHs */
	unsigned int            t_nr_blocks;
	bool                    t_started;
};

/* Buffer in a transaction */
struct ocsfs_txn_buf {
	struct list_head        list;
	struct buffer_head      *bh;
	u64                     block_num;
};

/* Superblock in-memory info — stored in sb->s_fs_info */
struct ocsfs_sb_info {
	struct ocsfs_disk_super *s_ds;          /* raw superblock copy */
	struct buffer_head      *s_sbh;         /* superblock buffer_head */

	/* Cached superblock fields */
	u32             s_block_size;
	u32             s_extent_size;
	u64             s_total_blocks;
	u64             s_free_blocks;
	u32             s_ag_count;
	u64             s_ag_size;
	u16             s_max_nodes;
	u64             s_feature_flags;
	u64             s_data_off;             /* first data byte */
	u64             s_ag_desc_off;

	/* Allocation groups */
	struct ocsfs_ag_info    *s_ags;         /* array [s_ag_count] */

	/* Journal (single-node: node slot 0) */
	struct ocsfs_journal    s_journal;

	/* Inode cache */
	struct kmem_cache       *s_inode_cache;

	/* Locks */
	struct rw_semaphore     s_global_lock;  /* global metadata lock */
	spinlock_t              s_free_lock;    /* protects s_free_blocks */
};

/* Per-inode in-memory info — wraps struct inode */
struct ocsfs_inode_info {
	u64                     i_disk_ino;     /* on-disk inode number */
	u32                     i_ag;           /* home AG */
	u32                     i_flags;        /* OCSFS_IFLAG_* */
	u16                     i_extent_count;
	struct ocsfs_extent     i_extents[OCSFS_INLINE_EXTENTS];
	u64                     i_extent_tree_root;
	struct mutex            i_extent_lock;
	struct inode            vfs_inode;      /* must be last */
};

static inline struct ocsfs_sb_info *OCSFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct ocsfs_inode_info *OCSFS_I(struct inode *inode)
{
	return container_of(inode, struct ocsfs_inode_info, vfs_inode);
}

/* ═══════════════════════════════════════════════════════════════
 * UTILITY HELPERS
 * ═══════════════════════════════════════════════════════════════ */

static inline u32 ocsfs_crc32c(u32 crc, const void *data, size_t len)
{
	return crc32c(crc, data, len);
}

/* Convert on-disk inode number to AG + offset within AG */
static inline u32 ocsfs_ino_to_ag(struct ocsfs_sb_info *sbi, u64 ino)
{
	u64 inodes_per_ag = sbi->s_ag_size;  /* 1 inode slot per block as max */
	return (u32)(ino / inodes_per_ag);
}

/* Convert OCSFS file type to DT_* type for readdir */
static inline unsigned char ocsfs_type_to_dt(u8 ft)
{
	static const unsigned char table[] = {
		[OCSFS_FT_UNKNOWN]  = DT_UNKNOWN,
		[OCSFS_FT_REG_FILE] = DT_REG,
		[OCSFS_FT_DIR]      = DT_DIR,
		[OCSFS_FT_CHRDEV]   = DT_CHR,
		[OCSFS_FT_BLKDEV]   = DT_BLK,
		[OCSFS_FT_FIFO]     = DT_FIFO,
		[OCSFS_FT_SOCK]     = DT_SOCK,
		[OCSFS_FT_SYMLINK]  = DT_LNK,
	};
	if (ft > OCSFS_FT_SYMLINK)
		return DT_UNKNOWN;
	return table[ft];
}

/* Convert Linux mode to OCSFS file type */
static inline u8 ocsfs_mode_to_ft(umode_t mode)
{
	if (S_ISREG(mode))  return OCSFS_FT_REG_FILE;
	if (S_ISDIR(mode))  return OCSFS_FT_DIR;
	if (S_ISCHR(mode))  return OCSFS_FT_CHRDEV;
	if (S_ISBLK(mode))  return OCSFS_FT_BLKDEV;
	if (S_ISFIFO(mode)) return OCSFS_FT_FIFO;
	if (S_ISSOCK(mode)) return OCSFS_FT_SOCK;
	if (S_ISLNK(mode))  return OCSFS_FT_SYMLINK;
	return OCSFS_FT_UNKNOWN;
}

/* Block number ↔ byte offset on disk */
static inline u64 ocsfs_block_to_byte(struct ocsfs_sb_info *sbi, u64 block)
{
	return block * sbi->s_block_size;
}

static inline u64 ocsfs_byte_to_block(struct ocsfs_sb_info *sbi, u64 byte)
{
	return byte / sbi->s_block_size;
}

/* Compute disk offset for an inode within its AG inode table */
static inline u64 ocsfs_inode_disk_off(struct ocsfs_sb_info *sbi, u64 ino)
{
	u32 ag = ocsfs_ino_to_ag(sbi, ino);
	u64 local = ino % sbi->s_ag_size;
	return sbi->s_ags[ag].inode_table_off + local * OCSFS_INODE_SIZE;
}

/* ═══════════════════════════════════════════════════════════════
 * FUNCTION DECLARATIONS
 * ═══════════════════════════════════════════════════════════════ */

/* super.c */
extern const struct super_operations ocsfs_sops;
int ocsfs_fill_super(struct super_block *sb, void *data, int silent);
void ocsfs_put_super(struct super_block *sb);
int ocsfs_statfs(struct dentry *dentry, struct kstatfs *buf);
int ocsfs_sync_fs(struct super_block *sb, int wait);

/* inode.c */
extern const struct inode_operations ocsfs_file_inode_ops;
extern const struct inode_operations ocsfs_dir_inode_ops;
extern const struct inode_operations ocsfs_special_inode_ops;
struct inode *ocsfs_iget(struct super_block *sb, u64 ino);
int ocsfs_write_inode(struct inode *inode, struct writeback_control *wbc);
void ocsfs_evict_inode(struct inode *inode);
struct inode *ocsfs_new_inode(struct inode *dir, umode_t mode);
int ocsfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct iattr *attr);
int ocsfs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 request_mask, unsigned int flags);

/* dir.c */
extern const struct file_operations ocsfs_dir_fops;
int ocsfs_add_dirent(struct inode *dir, const struct qstr *name,
		     u64 ino, u8 file_type);
int ocsfs_del_dirent(struct inode *dir, const struct qstr *name);
u64 ocsfs_find_dirent(struct inode *dir, const struct qstr *name, u8 *ft_out);
int ocsfs_empty_dir(struct inode *dir);

/* file.c */
extern const struct file_operations ocsfs_file_fops;
extern const struct address_space_operations ocsfs_aops;

/* extent.c */
int ocsfs_extent_lookup(struct inode *inode, u64 logical_block,
			struct ocsfs_extent *ext_out);
int ocsfs_extent_insert(struct inode *inode, u64 logical, u64 physical,
			u32 len, u16 flags);
int ocsfs_extent_truncate(struct inode *inode, u64 from_block);
int ocsfs_extent_count_blocks(struct inode *inode, u64 *count);

/* bitmap.c */
int ocsfs_alloc_blocks(struct super_block *sb, u32 ag_hint, u32 count,
		       u64 *block_out);
void ocsfs_free_blocks(struct super_block *sb, u64 block, u32 count);
int ocsfs_alloc_inode_num(struct super_block *sb, u32 ag_hint, u64 *ino_out);
void ocsfs_free_inode_num(struct super_block *sb, u64 ino);

/* journal.c */
int ocsfs_journal_init(struct super_block *sb);
void ocsfs_journal_exit(struct super_block *sb);
struct ocsfs_txn *ocsfs_txn_begin(struct super_block *sb);
int ocsfs_txn_add_bh(struct ocsfs_txn *txn, struct buffer_head *bh);
int ocsfs_txn_commit(struct ocsfs_txn *txn);
void ocsfs_txn_abort(struct ocsfs_txn *txn);
int ocsfs_journal_replay(struct super_block *sb);

#endif /* _OCSFS_KMOD_H */
