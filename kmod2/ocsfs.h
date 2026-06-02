/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OCSFS v2 — private kernel header.
 *
 * Single-writer ownership clustered filesystem. This header freezes the v2
 * on-disk format (the contract every other component depends on) and declares
 * the in-memory state and the inter-file API.
 *
 * On-disk layout (byte order: little-endian):
 *   SB | SB-mirror | node-table | heartbeat | lease-table | recovery |
 *   journal[max_nodes] | AG[0..ag_count)
 * Each AG: ag-header-block | block-bitmap | inode-table | data-blocks.
 *
 * mkfs computes every region offset and stores it in the superblock; the
 * kernel validates non-overlap at mount. The cluster regions (node/heartbeat/
 * lease/recovery, per-node journals) are reserved on disk by mkfs but NOT used
 * by the single-node code in Plan 1.
 */
#ifndef _OCSFS2_KMOD_H
#define _OCSFS2_KMOD_H

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/crc32c.h>
#include <linux/mutex.h>
#include <linux/uuid.h>
#include <linux/time64.h>
#include <linux/dma-direction.h>
#include <linux/sched.h>

/* ═══════════════════════ on-disk constants ═══════════════════════ */

#define OCSFS2_MAGIC          0x4F435332u   /* 'OCS2' */
#define OCSFS2_INODE_MAGIC    0x494E4F32u   /* 'INO2' */
#define OCSFS2_AG_MAGIC       0x41474732u   /* 'AGG2' */
#define OCSFS2_DIRENT_MAGIC   0x44495232u   /* 'DIR2' */
#define OCSFS2_JOURNAL_MAGIC  0x4A524C32u   /* 'JRL2' */
#define OCSFS2_LEASE_MAGIC    0x4C455332u   /* 'LES2' */
#define OCSFS2_NODE_MAGIC     0x4E4F4432u   /* 'NOD2' */
#define OCSFS2_RC_NODE_MAGIC  0x52434E32u   /* 'RCN2' — refcount btree node */

#define OCSFS2_VERSION_MAJOR  2
#define OCSFS2_VERSION_MINOR  0

#define OCSFS2_BLOCK_SIZE     4096
#define OCSFS2_INODE_SIZE     512
#define OCSFS2_ROOT_INO       2
#define OCSFS2_FIRST_USER_INO 64
#define OCSFS2_INLINE_EXTENTS 16
/* A fast symlink stores its target in the inode's inline-extent byte area. */
#define OCSFS2_SYMLINK_INLINE_MAX  (OCSFS2_INLINE_EXTENTS * 24)   /* 384 */
#define OCSFS2_MAX_NAME       255
#define OCSFS2_MAX_LABEL      64
#define OCSFS2_DEFAULT_MAX_NODES 8

/* Fixed-stride directory entries: 8 per 4096-byte block. Simple and correct
 * (no block-straddle, no rec_len juggling); space cost is negligible for the
 * VM-disk workload (small directories). */
#define OCSFS2_DIRENT_SIZE        512
#define OCSFS2_DIRENTS_PER_BLOCK  (OCSFS2_BLOCK_SIZE / OCSFS2_DIRENT_SIZE)

/* file types (de_file_type / inode helpers) */
#define OCSFS2_FT_UNKNOWN 0
#define OCSFS2_FT_REG     1
#define OCSFS2_FT_DIR     2
#define OCSFS2_FT_CHRDEV  3
#define OCSFS2_FT_BLKDEV  4
#define OCSFS2_FT_FIFO    5
#define OCSFS2_FT_SOCK    6
#define OCSFS2_FT_SYMLINK 7

/* extent flags */
#define OCSFS2_EXT_WRITTEN    0x0000
#define OCSFS2_EXT_UNWRITTEN  0x0001
#define OCSFS2_EXT_SHARED     0x0002   /* reflink/snapshot (Plan 3) */

/* inode flags */
#define OCSFS2_IFLAG_IMMUTABLE 0x0001
#define OCSFS2_IFLAG_APPEND    0x0002

/* feature bitmasks (none required by Plan 1) */
#define OCSFS2_FEATURE_INCOMPAT_SUPP   0ULL
#define OCSFS2_FEATURE_RO_COMPAT_SUPP  0ULL
#define OCSFS2_FEATURE_COMPAT_SUPP     0ULL

/* ═══════════════════════ on-disk structures ═══════════════════════ */

struct ocsfs2_disk_super {
	__le32  s_magic;
	__le16  s_major;
	__le16  s_minor;
	__u8    s_uuid[16];
	__u8    s_label[OCSFS2_MAX_LABEL];
	__le32  s_block_size;
	__le32  s_inode_size;
	__le64  s_total_blocks;
	__le64  s_free_blocks;
	__le64  s_total_inodes;
	__le64  s_free_inodes;
	__le32  s_ag_count;
	__le16  s_max_nodes;
	__le16  s_pad0;
	__le64  s_ag_size;        /* inode-number span per AG (== inodes_per_ag) */
	__le64  s_ag_blocks;      /* block span per AG */
	__le64  s_feat_compat;
	__le64  s_feat_incompat;
	__le64  s_feat_ro_compat;
	/* region offsets (absolute byte offsets) — computed by mkfs */
	__le64  s_node_table_off;
	__le64  s_heartbeat_off;
	__le64  s_lease_table_off;
	__le64  s_lease_count;
	__le64  s_recovery_off;
	__le64  s_journal_off;
	__le64  s_journal_size;   /* per-node journal size */
	__le64  s_ag_desc_off;    /* first AG header byte offset */
	__le64  s_data_off;       /* informational: first data byte of AG0 */
	__le64  s_mkfs_time;
	__le64  s_mount_count;
	__le64  s_inodes_per_ag;
	__u8    s_reserved[3820];
	__le32  s_checksum;       /* crc32c(~0, [0..4091]) */
} __packed;
static_assert(sizeof(struct ocsfs2_disk_super) == 4096, "disk_super must be 4096");

struct ocsfs2_disk_extent {
	__le64  e_logical;
	__le64  e_physical;
	__le32  e_length;
	__le16  e_flags;
	__le16  e_pad;
} __packed;
static_assert(sizeof(struct ocsfs2_disk_extent) == 24, "disk_extent must be 24");

struct ocsfs2_disk_inode {
	__le32  i_magic;
	__le32  i_generation;
	__le64  i_ino;
	__le16  i_mode;
	__le16  i_nlink;
	__le32  i_uid;
	__le32  i_gid;
	__le64  i_size;
	__le64  i_blocks;         /* 512-byte sectors, like VFS i_blocks */
	__le64  i_atime;
	__le64  i_mtime;
	__le64  i_ctime;
	__le32  i_flags;
	__le16  i_extent_count;
	__le16  i_pad2;
	__le64  i_extent_tree_root;
	__u8    i_inline_extents[OCSFS2_INLINE_EXTENTS * 24]; /* 384 */
	__le64  i_dir_btree_root;
	__le32  i_dirent_count;
	__le32  i_rdev;
	__le64  i_xattr_block;
	__u8    i_reserved[16];
	__le32  i_checksum;       /* crc32c(~0, [0..507]) */
} __packed;
static_assert(sizeof(struct ocsfs2_disk_inode) == 512, "disk_inode must be 512");

struct ocsfs2_disk_ag {
	__le32  ag_magic;
	__le32  ag_number;
	__le64  ag_block_start;
	__le64  ag_block_count;
	__le64  ag_free_blocks;
	__le64  ag_free_inodes;
	__le64  ag_bitmap_off;       /* absolute byte offset */
	__le64  ag_bitmap_blocks;
	__le64  ag_inode_table_off;  /* absolute byte offset */
	__le64  ag_inodes_per_ag;
	__le64  ag_data_off;         /* absolute byte offset of first data block */
	__le64  ag_data_blocks;
	__le64  ag_rc_btree_root;    /* reserved (reflink/snapshot, Plan 3) */
	__u8    ag_reserved[3996];
	__le32  ag_checksum;         /* crc32c(~0, [0..4091]) */
} __packed;
static_assert(sizeof(struct ocsfs2_disk_ag) == 4096, "disk_ag must be 4096");

struct ocsfs2_disk_dirent {
	__le32  de_magic;        /* OCSFS2_DIRENT_MAGIC when used, 0 = free slot */
	__le32  de_checksum;     /* crc32c(~0, [0..511]) with de_checksum zeroed */
	__le64  de_ino;
	__le64  de_name_hash;
	__u8    de_file_type;
	__u8    de_name_len;
	__le16  de_pad;
	__u8    de_name[OCSFS2_MAX_NAME + 1];   /* 256 */
	__u8    de_reserved[228];
} __packed;
static_assert(sizeof(struct ocsfs2_disk_dirent) == OCSFS2_DIRENT_SIZE,
	      "disk_dirent must be 512");

/* ── reserved cluster structures (defined now, used in Plan 2+) ── */

struct ocsfs2_disk_node_slot {       /* 256 bytes */
	__le32  ns_magic;
	__u8    ns_state;
	__u8    ns_pad;
	__le16  ns_slot_id;
	__u8    ns_uuid[16];
	__u8    ns_name[64];
	__le32  ns_mount_gen;
	__le64  ns_mount_time;
	__le64  ns_pr_key;
	__u8    ns_reserved[144];
	__le32  ns_checksum;
} __packed;
static_assert(sizeof(struct ocsfs2_disk_node_slot) == 256, "node_slot 256");

struct ocsfs2_disk_heartbeat {       /* 256 bytes */
	__le32  hb_magic;
	__le16  hb_node_slot;
	__le16  hb_state;
	__le64  hb_timestamp;
	__le64  hb_sequence;
	__le32  hb_mount_gen;
	__u8    hb_reserved[224];
	__le32  hb_checksum;
} __packed;
static_assert(sizeof(struct ocsfs2_disk_heartbeat) == 256, "heartbeat 256");

struct ocsfs2_disk_lease {           /* 64 bytes */
	__le32  l_magic;
	__le64  l_resource_id;
	__le16  l_owner_slot;
	__le16  l_mode;
	__le32  l_owner_gen;
	__le32  l_sh_holders[8];
	__le16  l_want_slot;
	__le16  l_pad;
	__le32  l_seq;
	__le32  l_checksum;
} __packed;
static_assert(sizeof(struct ocsfs2_disk_lease) == 64, "lease 64");

struct ocsfs2_disk_journal_hdr {     /* one block */
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
static_assert(sizeof(struct ocsfs2_disk_journal_hdr) == 4096, "journal_hdr 4096");

/* Journal record block types (one 4 KiB block each). */
#define OCSFS2_JREC_DESC    1
#define OCSFS2_JREC_COMMIT  2
#define OCSFS2_JTXN_MAX_BLOCKS  254   /* fits in one descriptor block */

struct ocsfs2_jent {
	__le64  je_home;     /* home block number of this after-image */
	__le32  je_crc;      /* crc32c of the after-image block content */
	__le32  je_pad;
} __packed;
static_assert(sizeof(struct ocsfs2_jent) == 16, "jent 16");

struct ocsfs2_disk_jdesc {           /* descriptor block */
	__le32  jd_magic;
	__le32  jd_type;     /* OCSFS2_JREC_DESC */
	__le64  jd_seq;
	__le32  jd_nr;       /* number of after-image blocks following */
	__le32  jd_pad;
	struct ocsfs2_jent jd_ent[OCSFS2_JTXN_MAX_BLOCKS];   /* 254 * 16 = 4064 */
	__u8    jd_reserved[4];
	__le32  jd_checksum; /* crc32c([0..4091]) */
} __packed;
static_assert(sizeof(struct ocsfs2_disk_jdesc) == 4096, "jdesc 4096");

struct ocsfs2_disk_jcommit {         /* commit block */
	__le32  jc_magic;
	__le32  jc_type;     /* OCSFS2_JREC_COMMIT */
	__le64  jc_seq;
	__le32  jc_checksum; /* crc32c([0..15]) */
	__u8    jc_pad[4076];
} __packed;
static_assert(sizeof(struct ocsfs2_disk_jcommit) == 4096, "jcommit 4096");

/* ── refcount B+tree (reflink/snapshot, Plan 4) ──
 * A per-AG tree maps a physical block range -> reference count. Only SHARED
 * ranges (refcount >= 2) get a record; a sole-owner block has no record and an
 * implicit refcount of 1. The root block number lives in ag_rc_btree_root
 * (0 = empty tree). Plan 4 uses a single leaf node (level 0); growth to an
 * internal level is a bounded future extension (records cap at one node). */
struct ocsfs2_disk_rc_rec {          /* 16 bytes — one refcounted phys range */
	__le64  rr_phys;             /* first physical block of the range */
	__le32  rr_len;              /* contiguous block count */
	__le32  rr_refcount;         /* reference count (>= 2 when stored) */
} __packed;
static_assert(sizeof(struct ocsfs2_disk_rc_rec) == 16, "rc_rec 16");

#define OCSFS2_RC_MAX_RECS  254
struct ocsfs2_disk_rc_node {         /* one 4096-byte metadata block */
	__le32  rn_magic;            /* OCSFS2_RC_NODE_MAGIC */
	__le16  rn_level;            /* 0 = leaf (only level in Plan 4) */
	__le16  rn_nr;               /* number of valid records */
	__le32  rn_ag;               /* owning AG number (sanity) */
	__le32  rn_pad;
	struct ocsfs2_disk_rc_rec rn_recs[OCSFS2_RC_MAX_RECS];  /* 254*16 = 4064 */
	__u8    rn_reserved[12];
	__le32  rn_checksum;         /* crc32c(~0, [0..4091]) */
} __packed;
static_assert(sizeof(struct ocsfs2_disk_rc_node) == 4096, "rc_node 4096");

/* ── reflink/snapshot ioctl ──
 * OCSFS_IOC_SNAP_CREATE: called on a regular-file fd; arg points at a
 * NUL-terminated name (<= OCSFS2_MAX_NAME) for a point-in-time reflink copy
 * created in the source's parent directory. */
#define OCSFS_IOC_SNAP_CREATE  _IOW('O', 0x01, char[OCSFS2_MAX_NAME + 1])

/* reservation unit sizes used by mkfs to size the cluster regions */
#define OCSFS2_NODE_SLOT_SIZE   256
#define OCSFS2_HEARTBEAT_SIZE   256
#define OCSFS2_LEASE_ENTRY_SIZE 64
#define OCSFS2_DEFAULT_LEASE_COUNT 65536

/* ═══════════════════════ in-memory structures ═══════════════════════ */

struct ocsfs2_extent {
	u64  logical;
	u64  physical;
	u32  length;
	u16  flags;
};

struct ocsfs2_pr_info {              /* used by transport/scsi_pr.c */
	u64   pr_key;
	bool  pr_registered;
};

struct ocsfs2_ag_info {
	u32   ag_no;
	u64   block_start;
	u64   block_count;
	u64   free_blocks;
	u64   free_inodes;
	u64   bitmap_off;        /* absolute byte offset */
	u64   bitmap_blocks;
	u64   inode_table_off;   /* absolute byte offset */
	u64   inodes_per_ag;
	u64   data_off;          /* absolute byte offset of first data block */
	u64   data_blocks;
	u64   rc_btree_root;     /* physical block of the refcount tree root, 0=empty */
	u64   next_blk_hint;     /* AG-relative search start for block alloc */
	u64   next_ino_hint;     /* AG-local search start for inode alloc */
	struct mutex ag_lock;    /* protects this AG's bitmap + inode table */
	struct mutex rc_lock;    /* protects this AG's refcount tree (ordered before ag_lock) */
};

/* In-memory journal state (single-node: this node's slot-0 WAL). */
struct ocsfs2_journal {
	u64    j_off;        /* journal region byte offset */
	u64    j_first_blk;  /* j_off / blocksize */
	u64    j_blocks;     /* total blocks in the region */
	u64    j_ring_len;   /* j_blocks - 1 (block 0 is the header) */
	u64    j_head;       /* monotonic record index — next free slot */
	u64    j_tail;       /* monotonic record index — oldest live record */
	u64    j_seq;        /* next transaction sequence */
	struct mutex j_lock;
	struct buffer_head *j_hdr_bh;
	struct super_block *j_sb;
	bool   j_active;
};

/* A metadata buffer enrolled in a transaction (with its before-image). */
struct ocsfs2_txn_buf {
	struct list_head  link;
	struct buffer_head *bh;
	u64    home_block;
	u8    *before;       /* snapshot taken at txn_get (for abort rollback) */
};

struct ocsfs2_txn {
	struct super_block *t_sb;
	u64    t_seq;
	struct list_head t_bufs;
	unsigned int t_nr;
	bool   t_failed;
};

struct ocsfs2_sb_info {
	struct super_block  *s_sb;
	struct buffer_head  *s_sbh;      /* superblock buffer (block 0 or mirror) */
	struct ocsfs2_disk_super *s_ds;  /* points into s_sbh->b_data */

	/* cached geometry */
	u32  s_block_size;
	u32  s_inode_size;
	u64  s_total_blocks;
	u64  s_free_blocks;
	u64  s_total_inodes;
	u64  s_free_inodes;
	u32  s_ag_count;
	u64  s_ag_size;          /* inode span per AG */
	u64  s_ag_blocks;
	u64  s_inodes_per_ag;
	u16  s_max_nodes;
	u64  s_feat_compat;
	u64  s_feat_incompat;
	u64  s_feat_ro_compat;

	/* region offsets */
	u64  s_node_table_off;
	u64  s_heartbeat_off;
	u64  s_lease_table_off;
	u64  s_lease_count;
	u64  s_recovery_off;
	u64  s_journal_off;
	u64  s_journal_size;
	u64  s_ag_desc_off;
	u64  s_data_off;

	struct ocsfs2_ag_info *s_ags;    /* [0..s_ag_count) */

	spinlock_t  s_free_lock;         /* protects s_free_blocks/s_free_inodes */
	struct mutex s_super_lock;       /* serialises superblock writeback */

	struct ocsfs2_journal s_journal; /* WAL redo log (single-node slot 0) */

	/* cluster identity — reserved, unused in Plan 1 */
	struct ocsfs2_pr_info s_pr;
	u16   s_node_slot;
	u32   s_mount_gen;
	bool  s_clustered;
};

struct ocsfs2_inode_info {
	u64   i_disk_ino;
	u32   i_ag;
	u32   i_flags;
	u32   i_generation;
	u16   i_extent_count;
	struct ocsfs2_extent i_extents[OCSFS2_INLINE_EXTENTS];
	u64   i_extent_tree_root;
	u64   i_dir_btree_root;
	u32   i_dirent_count;
	struct mutex i_meta_lock;        /* protects extent map + dir metadata */
	struct inode vfs_inode;          /* must be last */
};

static inline struct ocsfs2_sb_info *OCSFS2_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct ocsfs2_inode_info *OCSFS2_I(struct inode *inode)
{
	return container_of(inode, struct ocsfs2_inode_info, vfs_inode);
}

/* ═══════════════════════ helpers ═══════════════════════ */

static inline u32 ocsfs2_crc32c(u32 crc, const void *data, size_t len)
{
	return crc32c(crc, data, len);
}

static inline u64 ocsfs2_block_to_byte(struct ocsfs2_sb_info *sbi, u64 block)
{
	return block * sbi->s_block_size;
}

static inline u64 ocsfs2_byte_to_block(struct ocsfs2_sb_info *sbi, u64 byte)
{
	return byte / sbi->s_block_size;
}

static inline u32 ocsfs2_ino_to_ag(struct ocsfs2_sb_info *sbi, u64 ino)
{
	return sbi->s_ag_size ? (u32)(ino / sbi->s_ag_size) : 0;
}

/* Byte offset of an inode's 512-byte on-disk slot. 0 on out-of-range. */
static inline u64 ocsfs2_inode_disk_off(struct ocsfs2_sb_info *sbi, u64 ino)
{
	u32 ag = ocsfs2_ino_to_ag(sbi, ino);
	u64 local;

	if (!sbi->s_ag_size || ag >= sbi->s_ag_count)
		return 0;
	local = ino % sbi->s_ag_size;
	if (local >= sbi->s_ags[ag].inodes_per_ag)
		return 0;
	return sbi->s_ags[ag].inode_table_off + local * OCSFS2_INODE_SIZE;
}

static inline u8 ocsfs2_mode_to_ft(umode_t mode)
{
	if (S_ISREG(mode))  return OCSFS2_FT_REG;
	if (S_ISDIR(mode))  return OCSFS2_FT_DIR;
	if (S_ISCHR(mode))  return OCSFS2_FT_CHRDEV;
	if (S_ISBLK(mode))  return OCSFS2_FT_BLKDEV;
	if (S_ISFIFO(mode)) return OCSFS2_FT_FIFO;
	if (S_ISSOCK(mode)) return OCSFS2_FT_SOCK;
	if (S_ISLNK(mode))  return OCSFS2_FT_SYMLINK;
	return OCSFS2_FT_UNKNOWN;
}

static inline unsigned char ocsfs2_ft_to_dt(u8 ft)
{
	static const unsigned char t[] = {
		[OCSFS2_FT_UNKNOWN] = DT_UNKNOWN, [OCSFS2_FT_REG] = DT_REG,
		[OCSFS2_FT_DIR] = DT_DIR, [OCSFS2_FT_CHRDEV] = DT_CHR,
		[OCSFS2_FT_BLKDEV] = DT_BLK, [OCSFS2_FT_FIFO] = DT_FIFO,
		[OCSFS2_FT_SOCK] = DT_SOCK, [OCSFS2_FT_SYMLINK] = DT_LNK,
	};
	return ft <= OCSFS2_FT_SYMLINK ? t[ft] : DT_UNKNOWN;
}

/* FNV-1a name hash for dirents (stable across nodes). */
static inline u64 ocsfs2_name_hash(const char *name, int len)
{
	u64 h = 0xcbf29ce484222325ULL;
	int i;

	for (i = 0; i < len; i++) {
		h ^= (u8)name[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static inline void ocsfs2_dirent_set_csum(struct ocsfs2_disk_dirent *de)
{
	de->de_checksum = 0;
	de->de_checksum = cpu_to_le32(ocsfs2_crc32c(~0U, de,
						    OCSFS2_DIRENT_SIZE));
}

static inline bool ocsfs2_dirent_csum_ok(const struct ocsfs2_disk_dirent *de)
{
	struct ocsfs2_disk_dirent tmp = *de;
	u32 stored = le32_to_cpu(de->de_checksum);

	tmp.de_checksum = 0;
	return ocsfs2_crc32c(~0U, &tmp, OCSFS2_DIRENT_SIZE) == stored;
}

/* ═══════════════════════ inter-file API ═══════════════════════ */

/* super.c */
int  ocsfs2_statfs(struct dentry *dentry, struct kstatfs *buf);

/* inode.c */
extern struct kmem_cache *ocsfs2_inode_cachep;
extern const struct inode_operations ocsfs2_file_iops;
extern const struct inode_operations ocsfs2_special_iops;
extern const struct inode_operations ocsfs2_symlink_iops;
extern const struct file_operations ocsfs2_file_fops;
struct inode *ocsfs2_iget(struct super_block *sb, u64 ino);
int  ocsfs2_write_inode_block(struct inode *inode);
int  ocsfs2_write_inode(struct inode *inode, struct writeback_control *wbc);
void ocsfs2_evict_inode(struct inode *inode);
struct inode *ocsfs2_new_inode(struct mnt_idmap *idmap, struct inode *dir,
			       umode_t mode, dev_t rdev);
int  ocsfs2_alloc_inode_num(struct super_block *sb, u32 ag_hint, u64 *ino_out);
void ocsfs2_free_inode_num(struct super_block *sb, u64 ino);
struct inode *ocsfs2_alloc_inode(struct super_block *sb);
void ocsfs2_free_in_core_inode(struct inode *inode);
int  ocsfs2_getattr(struct mnt_idmap *idmap, const struct path *path,
		    struct kstat *stat, u32 request_mask, unsigned int flags);
int  ocsfs2_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		    struct iattr *attr);
/* extent map (Plan 1/2: inline extents only — caller holds i_meta_lock) */
int  ocsfs2_bmap(struct inode *inode, u64 logical_block, u64 *phys_out);
int  ocsfs2_inode_append_block(struct inode *inode, u64 *phys_out);
int  ocsfs2_extent_find(struct inode *inode, u64 lblk,
			struct ocsfs2_extent *cover, u64 *next_logical);
int  ocsfs2_extent_insert(struct inode *inode, u64 logical, u64 phys,
			  u32 len, u16 flags);
int  ocsfs2_extent_update_phys(struct inode *inode, u64 logical, u32 len,
			       u64 new_phys, u16 flags);
int  ocsfs2_extent_remap_range(struct inode *inode, u64 logical, u32 len,
			       u64 new_phys, u16 new_flags);
/* Free (refcount-aware) and remove every extent or part within [lblk, end).
 * Caller holds i_meta_lock. -ENOSPC if a mid-extent split needs more slots. */
int  ocsfs2_extent_punch_range(struct inode *inode, u64 lblk, u64 end);
void ocsfs2_extent_truncate_from(struct inode *inode, u64 from_block);
/* Discard in-core extent map / size and re-read it from the on-disk inode
 * (used to roll back after a failed reflink whose journal txn was aborted). */
void ocsfs2_reload_extents(struct inode *inode);

/* iomap.c — file data path */
extern const struct address_space_operations ocsfs2_file_aops;
extern const struct iomap_ops ocsfs2_iomap_ops;
ssize_t ocsfs2_file_read_iter(struct kiocb *iocb, struct iov_iter *to);
ssize_t ocsfs2_file_write_iter(struct kiocb *iocb, struct iov_iter *from);

/* Cap a single block allocation (8 MiB at 4 KiB blocks) — shared by the write
 * path and fallocate so one mapping call never scans/claims too much. */
#define OCSFS2_ALLOC_CAP_BLOCKS  2048u

/* file.c — fallocate, fiemap, SEEK_HOLE/DATA (L2 completeness) */
long ocsfs2_fallocate(struct file *file, int mode, loff_t offset, loff_t len);
int  ocsfs2_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		   u64 start, u64 len);
loff_t ocsfs2_llseek(struct file *file, loff_t offset, int whence);

/* bitmap.c */
int  ocsfs2_alloc_blocks(struct super_block *sb, u32 ag_hint, u32 count,
			 u64 *block_out);
void ocsfs2_free_blocks(struct super_block *sb, u64 block, u32 count);

/* refcount.c — per-AG reflink/snapshot refcount tree (Plan 4) */
u32  ocsfs2_refcount_get(struct super_block *sb, u64 phys);
int  ocsfs2_refcount_inc(struct super_block *sb, u64 phys, u32 len);
/* Decrement the refcount of [phys, phys+len); release to the bitmap any block
 * that drops to refcount 0 (was sole-owned). The refcount-aware free used by
 * truncate / evict / CoW in place of ocsfs2_free_blocks. */
void ocsfs2_free_blocks_rc(struct super_block *sb, u64 phys, u32 len);

/* True when a write to @phys must copy-on-write (it is shared, refcount > 1). */
static inline bool ocsfs2_needs_cow(struct super_block *sb, u64 phys)
{
	return ocsfs2_refcount_get(sb, phys) > 1;
}

/* reflink.c — FICLONE/clone + snapshot ioctl (Plan 4) */
loff_t ocsfs2_remap_file_range(struct file *src_file, loff_t src_off,
			       struct file *dst_file, loff_t dst_off,
			       loff_t len, unsigned int remap_flags);
long ocsfs2_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
/* Share src's extents [soff,soff+len) into dst at doff (callers hold the two
 * inode locks). Used by remap_file_range and the snapshot ioctl. Returns the
 * number of bytes cloned, or a negative errno. */
loff_t ocsfs2_reflink_range(struct file *src_file, loff_t soff,
			    struct file *dst_file, loff_t doff,
			    loff_t len, unsigned int remap_flags);

/* dir.c */
extern const struct inode_operations ocsfs2_dir_iops;
extern const struct file_operations ocsfs2_dir_fops;
int  ocsfs2_add_dirent(struct inode *dir, const struct qstr *name,
		       u64 ino, u8 ft);
int  ocsfs2_del_dirent(struct inode *dir, const struct qstr *name);
u64  ocsfs2_find_dirent(struct inode *dir, const struct qstr *name, u8 *ft_out);
int  ocsfs2_empty_dir(struct inode *dir);
int  ocsfs2_init_empty_dir(struct inode *dir, struct inode *parent);

/* journal.c — WAL redo log + crash recovery */
int  ocsfs2_journal_init(struct super_block *sb);
void ocsfs2_journal_exit(struct super_block *sb);
int  ocsfs2_journal_replay(struct super_block *sb);
struct ocsfs2_txn *ocsfs2_txn_begin(struct super_block *sb);
int  ocsfs2_txn_get(struct ocsfs2_txn *txn, struct buffer_head *bh);
int  ocsfs2_txn_commit(struct ocsfs2_txn *txn);
void ocsfs2_txn_abort(struct ocsfs2_txn *txn);

/* The transaction the current task is building (jbd2-style), or NULL. Metadata
 * write helpers enrol their buffers here so the op commits atomically. */
static inline struct ocsfs2_txn *ocsfs2_current_txn(void)
{
	return current->journal_info;
}

/* Enrol a metadata buffer in the current txn (snapshotting its before-image)
 * BEFORE modifying it. No-op (returns 0) outside a transaction. */
static inline int ocsfs2_jbuf(struct buffer_head *bh)
{
	struct ocsfs2_txn *txn = ocsfs2_current_txn();

	return txn ? ocsfs2_txn_get(txn, bh) : 0;
}

/* rename.c */
int  ocsfs2_rename(struct mnt_idmap *idmap, struct inode *old_dir,
		   struct dentry *old_dentry, struct inode *new_dir,
		   struct dentry *new_dentry, unsigned int flags);

/* transport/scsi_pr.c — salvaged, dormant in Plan 1 (compiles, not exercised).
 * SCSI-3 Persistent Reservations + Compare-And-Write. Used by the cluster
 * layer (Plan 2+) for fencing and lease-table CAS. */
#define OCSFS2_PR_TYPE_WRITE_EXCL       0x01
#define OCSFS2_PR_TYPE_EXCL_ACCESS      0x03
#define OCSFS2_PR_TYPE_WRITE_EXCL_REG   0x05
#define OCSFS2_PR_TYPE_EXCL_ACCESS_REG  0x06

int  ocsfs2_scsi_pool_init(void);
void ocsfs2_scsi_pool_destroy(void);
int  ocsfs2_pr_register(struct super_block *sb, u64 key);
int  ocsfs2_pr_unregister(struct super_block *sb);
int  ocsfs2_pr_reserve(struct super_block *sb, u8 type);
int  ocsfs2_pr_release(struct super_block *sb, u8 type);
int  ocsfs2_pr_preempt(struct super_block *sb, u64 victim_key, u8 type);
int  ocsfs2_pr_preempt_abort(struct super_block *sb, u64 victim_key, u8 type);
bool ocsfs2_pr_probe(struct super_block *sb);
u64  ocsfs2_pr_make_key(const u8 *uuid, u32 mount_gen);
void ocsfs2_build_caw_cdb(u8 cdb[16], u64 lba, u32 num_blocks);
bool ocsfs2_scsi_caw_probe(struct super_block *sb);
int  ocsfs2_scsi_caw(struct super_block *sb, u64 lba, const void *expected,
		     const void *new_data, unsigned int lbs);
int  ocsfs2_bsg_execute_cdb(struct super_block *sb, const u8 cdb[16],
			    void *buf, unsigned int buf_len,
			    enum dma_data_direction data_dir);

#endif /* _OCSFS2_KMOD_H */
