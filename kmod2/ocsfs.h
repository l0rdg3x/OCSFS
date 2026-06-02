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
#define OCSFS2_XATTR_MAGIC    0x58415432u   /* 'XAT2' — extended attr block */
#define OCSFS2_EXT_NODE_MAGIC 0x45584E32u   /* 'EXN2' — extent btree node */

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

/* cluster node slot states (ns_state) */
#define OCSFS2_NODE_FREE    0
#define OCSFS2_NODE_ACTIVE  1
#define OCSFS2_NODE_DEAD    2

/* lease modes (l_mode) */
#define OCSFS2_LEASE_NONE   0
#define OCSFS2_LEASE_SH     1
#define OCSFS2_LEASE_EX     2
#define OCSFS2_SLOT_NONE    0xFFFF
/* reserved lease resource id (never an inode number) for the global metadata
 * lease that serialises cross-node namespace + allocation (L4b). */
#define OCSFS2_META_RESOURCE  1ULL
/* recovery-leader lease resource per dead slot (L5). High bit keeps it clear of
 * real inode numbers (which start at 2 and never reach 2^48). */
#define OCSFS2_RECOVERY_RESOURCE(slot)  ((2ULL << 48) | (u64)(slot))

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

/* compat features (safe to ignore if unknown): AUTOGROW guarantees the
 * uniform-AG layout that online grow needs (all AGs == s_ag_blocks). */
#define OCSFS2_FEAT_COMPAT_AUTOGROW    0x1ULL
/* A8: per-data-block CRC32c checksums in a per-AG region (silent-corruption
 * detection on any SAN). ro_compat: an unaware build may mount read-only (reads
 * don't touch checksums) but not read-write (writes would leave them stale). */
#define OCSFS2_FEAT_RO_COMPAT_DATACSUM 0x1ULL

/* feature bitmasks */
#define OCSFS2_FEATURE_INCOMPAT_SUPP   0ULL
#define OCSFS2_FEATURE_RO_COMPAT_SUPP  (OCSFS2_FEAT_RO_COMPAT_DATACSUM)
#define OCSFS2_FEATURE_COMPAT_SUPP     (OCSFS2_FEAT_COMPAT_AUTOGROW)

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
	__le64  ag_csum_off;         /* A8: byte offset of the data-checksum region (0 = none) */
	__le64  ag_csum_blocks;      /* A8: blocks in the data-checksum region */
	__u8    ag_reserved[3980];
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

/* ── extended attributes (Plan 5b) ──
 * All of an inode's xattrs (including POSIX ACLs) live in a single 4 KiB block
 * pointed at by i_xattr_block (0 = none). Entries are packed after the header:
 * [le16 name_len][le16 value_len][name][value], 4-byte aligned. The full
 * prefixed name ("user.foo", "system.posix_acl_access", ...) is stored. */
struct ocsfs2_disk_xattr_header {
	__le32  xh_magic;            /* OCSFS2_XATTR_MAGIC */
	__le32  xh_count;            /* number of entries */
	__le32  xh_used;             /* bytes of entries after this header */
	__le32  xh_checksum;         /* crc32c(~0, whole block, this field 0) */
} __packed;
static_assert(sizeof(struct ocsfs2_disk_xattr_header) == 16, "xattr_header 16");
#define OCSFS2_XATTR_SPACE  (OCSFS2_BLOCK_SIZE - sizeof(struct ocsfs2_disk_xattr_header))

/* ── extent B+tree (large/fragmented files, Plan 2b) ──
 * When an inode's inline extents (16) overflow, the whole map spills to a
 * per-inode B+tree rooted at i_extent_tree_root. Leaf nodes (level 0) hold
 * sorted extent records; internal nodes hold {min_logical, child} pointers.
 * Each node is one 4 KiB CoW-journaled metadata block. */
struct ocsfs2_disk_ext_rec {         /* 24 — a leaf extent record */
	__le64  er_logical;
	__le64  er_physical;
	__le32  er_length;
	__le16  er_flags;
	__le16  er_pad;
} __packed;
static_assert(sizeof(struct ocsfs2_disk_ext_rec) == 24, "ext_rec 24");

struct ocsfs2_disk_ext_ptr {         /* 16 — an internal child pointer */
	__le64  ep_logical;          /* lowest logical block in the child */
	__le64  ep_child;            /* child node block number */
} __packed;
static_assert(sizeof(struct ocsfs2_disk_ext_ptr) == 16, "ext_ptr 16");

#define OCSFS2_EXT_LEAF_MAX  169     /* 169 * 24 = 4056 <= 4076 */
#define OCSFS2_EXT_INT_MAX   254     /* 254 * 16 = 4064 <= 4076 */
struct ocsfs2_disk_ext_node {        /* one 4096-byte metadata block */
	__le32  en_magic;            /* OCSFS2_EXT_NODE_MAGIC */
	__le16  en_level;            /* 0 = leaf, >0 = internal */
	__le16  en_nr;               /* number of records / pointers */
	__le64  en_next;             /* leaf chain: next leaf block (0 = last) */
	__u8    en_body[4076];       /* ext_rec[] (leaf) or ext_ptr[] (internal) */
	__le32  en_checksum;         /* crc32c(~0, [0..4091]) */
} __packed;
static_assert(sizeof(struct ocsfs2_disk_ext_node) == 4096, "ext_node 4096");

/* ── reflink/snapshot ioctl ──
 * OCSFS_IOC_SNAP_CREATE: called on a regular-file fd; arg points at a
 * NUL-terminated name (<= OCSFS2_MAX_NAME) for a point-in-time reflink copy
 * created in the source's parent directory. */
#define OCSFS_IOC_SNAP_CREATE  _IOW('O', 0x01, char[OCSFS2_MAX_NAME + 1])

/* OCSFS_IOC_GROWFS: called on any fd; force an autogrow check now (admin/udev
 * after a SAN LUN resize). Same work the autonomous grow thread does. */
#define OCSFS_IOC_GROWFS       _IO('O', 0x02)

/* OCSFS_IOC_SCRUB: online metadata scrub — verify every on-disk checksum
 * (super, AG headers, used inodes, extent/refcount B+tree nodes, xattr blocks)
 * across the live filesystem, reading coherently. Read-only; reports counts. */
struct ocsfs2_scrub_result {
	__u64 checked;     /* structures verified */
	__u64 errors;      /* checksum / magic failures */
	__u64 inodes;      /* used inodes scanned */
	__u32 ag_count;
	__u32 flags;       /* reserved (0) */
};
#define OCSFS_IOC_SCRUB        _IOWR('O', 0x03, struct ocsfs2_scrub_result)

/* OCSFS_IOC_DEFRAG: called on a regular-file fd; relocate the file's private
 * (non-shared, written) data into contiguous runs, shrinking its extent count.
 * Online, journaled, leaves shared (reflink/snapshot/dedup) extents untouched. */
struct ocsfs2_defrag_result {
	__u64 extents_before;
	__u64 extents_after;
	__u64 blocks_relocated;
	__u64 runs_relocated;
};
#define OCSFS_IOC_DEFRAG       _IOWR('O', 0x04, struct ocsfs2_defrag_result)

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
	u64   csum_off;          /* A8: byte offset of the data-checksum region (0=none) */
	u64   csum_blocks;       /* A8: blocks in the checksum region */
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

/* ═══════════════════════ cluster (L3-L5) ═══════════════════════ */

/* L3 membership/fencing provider interface (spec §4). The v1 provider is the
 * on-disk heartbeat; a corosync provider can replace it without touching L4/L5. */
struct ocsfs2_cluster_ops {
	int  (*node_alive)(struct super_block *sb, u16 slot, u32 gen);
	void (*on_node_dead)(struct super_block *sb, u16 slot, u32 gen);
	int  (*self_liveness_ok)(struct super_block *sb);
};

/* Per-peer liveness tracking (observer-clock based, avoids cross-node clocks). */
struct ocsfs2_peer {
	u64           last_seq;       /* last heartbeat sequence we observed */
	unsigned long last_change;    /* jiffies when last_seq last advanced */
	u32           gen;            /* observed mount generation */
	u8            state;          /* OCSFS2_NODE_* as last read */
	bool          seen;
};

struct ocsfs2_cluster {
	bool   active;
	bool   caw_ok;
	u16    self_slot;
	u32    mount_gen;
	u64    pr_key;
	u16    max_nodes;
	struct task_struct *hb_thread;
	u64    hb_seq;                /* our monotonic heartbeat sequence */
	struct ocsfs2_peer *peers;   /* [max_nodes] */
	const struct ocsfs2_cluster_ops *ops;
	struct mutex lease_lock;     /* serialises lease-table CAW read-modify-CAS */
	unsigned long hb_interval_j; /* heartbeat period (jiffies) */
	unsigned long death_j;       /* death window (jiffies) */
	struct workqueue_struct *recover_wq; /* L5: off-heartbeat recovery */
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

	struct ocsfs2_ag_info *s_ags;    /* [0..s_ag_count), allocated [0..s_ag_capacity) */
	u32   s_ag_capacity;             /* slots in s_ags (autogrow headroom) */
	bool  s_growable;                /* COMPAT_AUTOGROW: uniform AGs, online-grow ok */
	bool  s_datacsum;                /* A8: RO_COMPAT_DATACSUM — per-data-block CRC */
	struct task_struct *s_grow_thread;
	struct mutex s_grow_lock;        /* serialises online grow + geometry refresh */

	spinlock_t  s_free_lock;         /* protects s_free_blocks/s_free_inodes */
	struct mutex s_super_lock;       /* serialises superblock writeback */

	struct ocsfs2_journal s_journal; /* WAL redo log (single-node slot 0) */

	/* cluster identity */
	struct ocsfs2_pr_info s_pr;
	u16   s_node_slot;
	u32   s_mount_gen;
	bool  s_clustered;
	struct ocsfs2_cluster *s_cluster;   /* L3-L5 state, NULL if single-node */
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
	u64   i_xattr_block;             /* xattr/ACL block, 0 = none */
	u32   i_dirent_count;
	u8    i_lease_mode;              /* OCSFS2_LEASE_* currently held (cluster) */
	u32   i_lease_count;            /* opens needing the lease */
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
/* Coherent re-read of the inode from disk + page-cache drop (cluster handoff). */
void ocsfs2_inode_refresh_coherent(struct inode *inode);

/* extent_btree.c — spilled extent map for large/fragmented files (Plan 2b).
 * Active when i_extent_tree_root != 0; the inode.c extent ops dispatch here. */
int  ocsfs2_ext_tree_find(struct inode *inode, u64 lblk,
			  struct ocsfs2_extent *cover, u64 *next_logical);
int  ocsfs2_ext_tree_insert(struct inode *inode, u64 logical, u64 phys,
			    u32 len, u16 flags);
int  ocsfs2_ext_tree_update_phys(struct inode *inode, u64 logical, u32 len,
				 u64 new_phys, u16 flags);
int  ocsfs2_ext_tree_remap_range(struct inode *inode, u64 logical, u32 len,
				 u64 new_phys, u16 new_flags);
int  ocsfs2_ext_tree_punch_range(struct inode *inode, u64 lblk, u64 end);
int  ocsfs2_ext_tree_truncate_from(struct inode *inode, u64 from_block);
void ocsfs2_ext_tree_free_all(struct inode *inode);   /* evict: free data + nodes */
int  ocsfs2_extent_spill(struct inode *inode, u64 logical, u64 phys, u32 len,
			 u16 flags);   /* migrate inline -> tree, then insert */
int  ocsfs2_extent_spill_only(struct inode *inode); /* migrate inline -> tree */

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
u64  ocsfs2_recompute_free(struct super_block *sb);   /* A4: true free from bitmap */

/* A8 — per-data-block CRC32c checksums (csum.c); no-ops unless s_datacsum */
void ocsfs2_csum_set(struct super_block *sb, u64 phys, u32 crc);  /* store */
u32  ocsfs2_csum_read(struct super_block *sb, u64 phys);          /* 0 = unset */
struct folio;
struct iomap;
struct bio;
void ocsfs2_csum_folio_range(struct super_block *sb, struct folio *folio,
			     u64 pos, unsigned int len, const struct iomap *iomap);
void ocsfs2_csum_bio(struct super_block *sb, struct bio *bio);  /* O_DIRECT write */
static inline u32 ocsfs2_data_crc(struct super_block *sb, const void *data)
{
	return ocsfs2_crc32c(~0U, data, sb->s_blocksize);
}
int  ocsfs2_fitrim(struct super_block *sb, struct fstrim_range *range);  /* D4 */

/* D2 autonomous online autogrow */
int  ocsfs2_grow_check(struct super_block *sb, bool force);
int  ocsfs2_grow_start(struct super_block *sb);
void ocsfs2_grow_stop(struct super_block *sb);

/* D5 online metadata scrub */
int  ocsfs2_scrub(struct super_block *sb, struct ocsfs2_scrub_result *res);

/* online defragmentation (extent compaction) */
int  ocsfs2_defrag_file(struct inode *inode, struct ocsfs2_defrag_result *res);

/* coherent block-range copy (bio-based), shared by CoW and defrag */
int  ocsfs2_copy_blocks(struct super_block *sb, u64 oldphys, u64 newphys, u32 n);

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
int  ocsfs2_journal_replay_slot(struct super_block *sb, u16 slot);  /* L5 recovery */
void ocsfs2_txn_forget(struct super_block *sb, u64 start, u32 count); /* revoke on free */

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

/* xattr.c — extended attributes + POSIX ACL */
extern const struct xattr_handler * const ocsfs2_xattr_handlers[];
int  ocsfs2_xattr_get(struct inode *inode, const char *name, void *buf,
		      size_t size);
int  ocsfs2_xattr_set(struct inode *inode, const char *name, const void *value,
		      size_t size, int flags);
ssize_t ocsfs2_listxattr(struct dentry *dentry, char *buffer, size_t size);
void ocsfs2_xattr_free(struct inode *inode);   /* free the xattr block on evict */
struct posix_acl *ocsfs2_get_acl(struct inode *inode, int type, bool rcu);
int  ocsfs2_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		    struct posix_acl *acl, int type);
int  ocsfs2_init_acl(struct inode *inode, struct inode *dir);   /* inherit on create */

/* rename.c */
int  ocsfs2_rename(struct mnt_idmap *idmap, struct inode *old_dir,
		   struct dentry *old_dentry, struct inode *new_dir,
		   struct dentry *new_dentry, unsigned int flags);

/* cluster.c — L3 membership: node-slot claim, heartbeat, cluster_ops.
 * No-op (returns 0, leaves s_cluster NULL) when the volume is single-node. */
int  ocsfs2_cluster_init(struct super_block *sb);   /* join: claim slot, PR (no hb) */
int  ocsfs2_cluster_start(struct super_block *sb);  /* start the heartbeat */
void ocsfs2_cluster_exit(struct super_block *sb);
/* True iff the peer owning @slot at @gen is currently alive (per L3). */
bool ocsfs2_node_alive(struct super_block *sb, u16 slot, u32 gen);
/* coherent coordination-block I/O (bypass the per-node buffer cache) */
int  ocsfs2_cl_bio(struct super_block *sb, u64 byte_off, void *buf,
		   unsigned int len, blk_opf_t op);
int  ocsfs2_cl_caw_record(struct super_block *sb, u64 byte_off,
			  const void *rec, unsigned int rec_len);
/* fresh metadata read (clustered: bio-coherent; single-node: sb_bread) */
struct buffer_head *ocsfs2_meta_bread(struct super_block *sb, u64 blk);

/* lease.c — L4 ownership leases (single-writer) + L5 recovery.
 * acquire/release are no-ops on a single-node volume. */
int  ocsfs2_lease_acquire(struct super_block *sb, u64 resource, int mode);
void ocsfs2_lease_release(struct super_block *sb, u64 resource, int mode);
void ocsfs2_recover_node(struct super_block *sb, u16 slot, u32 gen);
/* per-inode lease management driven by open()/release() (cluster only). */
int  ocsfs2_inode_open_lease(struct inode *inode, bool want_ex);
void ocsfs2_inode_close_lease(struct inode *inode);
/* global metadata lease around cross-node namespace ops: serialises + makes
 * shared metadata (dirs, inode table, bitmap) coherent. No-op single-node.
 * meta_lock refreshes @dir coherently if non-NULL. */
void ocsfs2_meta_lock(struct super_block *sb, struct inode *dir,
		      struct inode *dir2);
void ocsfs2_meta_unlock(struct super_block *sb);
/* True if @slot is currently alive at any generation (for stale SH bits). */
bool ocsfs2_node_alive_any(struct super_block *sb, u16 slot);

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
