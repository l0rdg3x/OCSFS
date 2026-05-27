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
#include <linux/fileattr.h>
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
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/timekeeping.h>
#include <linux/mempool.h>

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

/* CAS lease area — immediatamente dopo la lock table */
#define OCSFS_CAS_LEASE_OFF         1384448ULL   /* = LOCK_TABLE_OFF + LOCK_TABLE_SIZE */
#define OCSFS_CAS_LEASE_ENTRIES     1024
#define OCSFS_CAS_LEASE_MAGIC       0x4F43414CU  /* "OCAL" */
#define OCSFS_CAS_LEASE_SIZE        32768ULL     /* 1024 × 32 byte */
#define CAS_MAX_ATTEMPTS            128
#define CAS_MAX_BACKOFF_US          32000 /* 32 ms — covers SAN RTTs up to ~10 ms */
#define CAS_LEASE_TIMEOUT_NS        (10ULL * NSEC_PER_SEC)

/* Recovery leader election block — dopo l'area CAS lease */
#define OCSFS_RECOVERY_LEADER_OFF   (OCSFS_CAS_LEASE_OFF + OCSFS_CAS_LEASE_SIZE)
#define OCSFS_RECOVERY_LEADER_MAGIC 0x52454C44U  /* "RELD" */
#define OCSFS_RL_SLOT_FREE          0xFFFFU
#define RECOVERY_LEADER_TIMEOUT_NS  (60ULL * NSEC_PER_SEC)
/* High bit of rl_epoch: set while the leader is replaying the failed journal.
 * Survivor nodes see this via the heartbeat-driven leader-block probe and
 * defer EX acquisitions, providing cross-node quiescence during replay. */
#define OCSFS_RL_REPLAY_ACTIVE      (1U << 31)

struct ocsfs_disk_recovery_leader {
	__le32  rl_magic;
	__le16  rl_leader_slot;   /* 0xFFFF = nessun leader attivo */
	__le16  rl_target_slot;   /* slot del nodo in recovery */
	__le32  rl_leader_gen;    /* mount gen del leader (anti-zombie) */
	__le32  rl_epoch;         /* monotonic — incrementato ad ogni elezione */
	__le64  rl_deadline_ns;   /* scadenza leadership (ktime_get_real_ns) */
	__le32  rl_checksum;      /* crc32c dei primi 24 byte */
} __packed;

/* 32 byte per entry; 128 entry per blocco da 4096 byte */
struct ocsfs_disk_cas_lease {
	__le32  cl_magic;
	__le16  cl_owner_slot;    /* 0xFFFF = libero */
	__le16  cl_reserved;
	__le64  cl_deadline_ns;
	__le32  cl_checksum;      /* crc32c dei primi 28 byte */
	__le32  cl_pad;
} __packed;

enum ocsfs_cas_backend {
	CAS_BACKEND_NONE = 0,
	CAS_BACKEND_PR_LEASE,
	CAS_BACKEND_SCSI_CAW,     /* fast-path hardware opzionale */
};

#define OCSFS_MAX_NODES             256
/* ARCH-7: sentinel for i_last_writer_slot — "no local write yet" */
#define OCSFS_INVALID_WRITER_SLOT   ((u16)OCSFS_MAX_NODES)
#define OCSFS_DEFAULT_MAX_NODES     64
#define OCSFS_MAX_LABEL             64

#define OCSFS_DEFAULT_BLOCK_SIZE    4096
#define OCSFS_DEFAULT_EXTENT_SIZE   (1 << 20)
#define OCSFS_DEFAULT_AG_SIZE       (1ULL << 30)
#define OCSFS_DEFAULT_JOURNAL_SIZE  (32 << 20)

#define OCSFS_INODE_SIZE            512
#define OCSFS_INLINE_EXTENTS        16
#define OCSFS_MAX_NAME_LEN          255  /* must equal NAME_MAX (255 on Linux) */
#define OCSFS_XATTR_MAGIC           0x4F435841  /* "OCXA" */
#define OCSFS_XATTR_DATA_SIZE       (OCSFS_DEFAULT_BLOCK_SIZE - 16)
#define OCSFS_DIR_BTREE_THRESHOLD   64u  /* entries before building dir index */
#define OCSFS_MIN_PREALLOC_BLOCKS   8u   /* min blocks per iomap write alloc */
#define OCSFS_DIRENT_SIZE           sizeof(struct ocsfs_disk_dirent)

#define OCSFS_ROOT_INO              2
#define OCSFS_FIRST_USER_INO        64

/* Feature flags */
#define OCSFS_FEAT_THIN_PROV    (1ULL << 0)
#define OCSFS_FEAT_COMPRESSION  (1ULL << 1)

/* Compression algorithm IDs (also in compress.c — keep in sync) */
#define OCSFS_COMPRESS_NONE	0
#define OCSFS_COMPRESS_LZ4	1
#define OCSFS_COMPRESS_ZSTD	2
#define OCSFS_FEAT_ENCRYPTION   (1ULL << 2)
#define OCSFS_FEAT_SNAPSHOTS    (1ULL << 3)
#define OCSFS_FEAT_DEDUP        (1ULL << 4)
#define OCSFS_FEAT_MULTI_LUN    (1ULL << 5)
#define OCSFS_FEAT_AUTH         (1ULL << 6)  /* cluster membership auth */

/* ── ARCH-1: compat / incompat / ro_compat feature bits ─────────────────
 *
 * INCOMPAT: mount fails if this kernel doesn't understand the feature.
 * RO_COMPAT: mount proceeds read-only if this kernel doesn't understand it.
 * COMPAT: safely ignored by older kernels.
 *
 * Enforcement is skipped for legacy volumes (s_revision_level == 0).
 *
 * These bits are set by mkfs / ocsfs-tool when a feature is first enabled
 * and cleared on downgrade.  The supported masks below must be updated
 * each time a new feature is implemented in this kernel.
 */

/* INCOMPAT bits — layout-breaking features */
#define OCSFS_FEATURE_INCOMPAT_LOCK_TABLE_V2    (1ULL << 0)  /* ARCH-2 */
#define OCSFS_FEATURE_INCOMPAT_RC_BTREE_PER_AG  (1ULL << 1)  /* ARCH-5 */

/* RO_COMPAT bits — read-write-semantic features */
#define OCSFS_FEATURE_RO_COMPAT_SELECTIVE_INV   (1ULL << 0)  /* ARCH-7 */
#define OCSFS_FEATURE_RO_COMPAT_HB_SUMMARY      (1ULL << 1)  /* ARCH-3 */
#define OCSFS_FEATURE_RO_COMPAT_DEDUP_SCRUB     (1ULL << 2)  /* ARCH-6 */

/* Masks of features this build understands.  Update as features land. */
#define OCSFS_FEATURE_INCOMPAT_SUPP     (OCSFS_FEATURE_INCOMPAT_LOCK_TABLE_V2 | \
					 OCSFS_FEATURE_INCOMPAT_RC_BTREE_PER_AG)
#define OCSFS_FEATURE_RO_COMPAT_SUPP    0ULL   /* ARCH-3/6/7 not yet implemented */
#define OCSFS_FEATURE_COMPAT_SUPP       0ULL

/* Inode flags */
#define OCSFS_IFLAG_THIN        0x0001
#define OCSFS_IFLAG_COMPRESSED  0x0002
#define OCSFS_IFLAG_ENCRYPTED   0x0004
#define OCSFS_IFLAG_IMMUTABLE   0x0008
#define OCSFS_IFLAG_APPEND      0x0010
#define OCSFS_IFLAG_ORPHAN      0x0020  /* set at create, cleared after dirent commit */

/* Extent flags */
#define OCSFS_EXT_WRITTEN        0x0000
#define OCSFS_EXT_UNWRITTEN      0x0001
#define OCSFS_EXT_COMPRESSED     0x0004  /* data is LZ4/ZSTD compressed */
#define OCSFS_EXT_COMP_ALGO_MASK  0x0018 /* bits 3-4: algorithm ID */
#define OCSFS_EXT_COMP_ALGO_SHIFT 3
#define OCSFS_EXT_ENCRYPTED      0x0020  /* data is encrypted — skip dedup/content inspection */

static inline u8 ocsfs_ext_comp_algo(u16 flags)
{
	return (flags & OCSFS_EXT_COMP_ALGO_MASK) >> OCSFS_EXT_COMP_ALGO_SHIFT;
}

static inline u16 ocsfs_ext_set_comp_algo(u16 flags, u8 algo)
{
	flags &= ~OCSFS_EXT_COMP_ALGO_MASK;
	flags |= ((u16)algo << OCSFS_EXT_COMP_ALGO_SHIFT);
	return flags;
}

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

/* Node slot states */
#define OCSFS_NODE_FREE         0x00
#define OCSFS_NODE_ACTIVE       0x01
#define OCSFS_NODE_EVICTING     0x02
#define OCSFS_NODE_SUSPECTED    0x03  /* heartbeat stale, not yet confirmed dead */
#define OCSFS_NODE_DEAD         0xFF

/* Lock modes */
#define OCSFS_LOCK_NL           0   /* Null */
#define OCSFS_LOCK_SH           1   /* Shared (read) */
#define OCSFS_LOCK_EX           2   /* Exclusive (write) */
#define OCSFS_LOCK_CW           3   /* Concurrent Write */

/* Lock resource types */
#define OCSFS_LOCKRES_INODE     1
#define OCSFS_LOCKRES_AG        2
#define OCSFS_LOCKRES_JOURNAL   3
#define OCSFS_LOCKRES_RENAME    4
#define OCSFS_LOCKRES_RECOVERY  5
#define OCSFS_LOCKRES_SUPER     6
#define OCSFS_LOCKRES_REFCOUNT  7

/* Heartbeat constants */
#define OCSFS_HB_INTERVAL_MS    5000   /* write every 5s */
#define OCSFS_HB_TIMEOUT_MS     15000  /* 3 missed → suspected */
#define OCSFS_HB_CONFIRM_MS     10000  /* 2 more missed → confirmed dead */
/* Total dead-node detection window: suspected + confirmation period */
#define OCSFS_HB_DEAD_MS        (OCSFS_HB_TIMEOUT_MS + OCSFS_HB_CONFIRM_MS)
#define OCSFS_HB_CHECK_MS       2000   /* check peers every 2s */
#define OCSFS_HB_IO_TIMEOUT_MS  3000   /* heartbeat write I/O deadline */

/* Lock acquisition retry */
#define OCSFS_LOCK_RETRY_MIN_US 1000   /* 1 ms */
#define OCSFS_LOCK_RETRY_MAX_US 100000 /* 100 ms */
#define OCSFS_LOCK_MAX_RETRIES  200

/* Open-addressing probe limit to resolve slot collisions */
#define OCSFS_LOCK_PROBE_MAX    64

/* Lock caching — serve re-acquires from local cache for this window */
#define OCSFS_LOCK_CACHE_MS     500ULL
#define OCSFS_LOCK_CACHE_NS     (OCSFS_LOCK_CACHE_MS * 1000000ULL)

/* Recovery backoff for transient errors in the recovery work function */
#define OCSFS_RECOVERY_BACKOFF_MS   60000U  /* 60s before re-arming a failed recovery */

/* Recovery phases */
#define OCSFS_RECOVERY_ELECT    1
#define OCSFS_RECOVERY_FENCE    2
#define OCSFS_RECOVERY_JOURNAL  3
#define OCSFS_RECOVERY_LOCKS    4
#define OCSFS_RECOVERY_CLEANUP  5

/* SCSI PR service action codes */
#define OCSFS_PR_REGISTER                0x00
#define OCSFS_PR_RESERVE                 0x01
#define OCSFS_PR_RELEASE                 0x02
#define OCSFS_PR_CLEAR                   0x03
#define OCSFS_PR_PREEMPT                 0x04
#define OCSFS_PR_PREEMPT_AND_ABORT       0x05
#define OCSFS_PR_REGISTER_AND_IGNORE     0x06

/* SCSI PR types */
#define OCSFS_PR_TYPE_WRITE_EXCL         0x01
#define OCSFS_PR_TYPE_EXCL_ACCESS        0x03
#define OCSFS_PR_TYPE_WRITE_EXCL_REG     0x05
#define OCSFS_PR_TYPE_EXCL_ACCESS_REG    0x06

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
	/* ARCH-1: on-disk format versioning — carved from s_reserved (28 bytes).
	 * s_revision_level == 0 means legacy FS; compat enforcement is skipped. */
	__le32  s_revision_level;      /* incremented on each on-disk format change */
	__le64  s_feature_compat;      /* backward-compatible features (safely ignored) */
	__le64  s_feature_incompat;    /* incompatible features — mount fails if unknown */
	__le64  s_feature_ro_compat;   /* read-only-compat features — forces SB_RDONLY */
	/* ARCH-2: dynamic lock table — carved from s_reserved (4 bytes).
	 * 0 means legacy (OCSFS_LOCK_ENTRY_COUNT entries); new FS default 65536. */
	__le32  s_lock_primary_count;
	__u8    s_reserved[3858];   /* was 3890; reduced by 32 total (ARCH-1: 28, ARCH-2: 4) */
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
	__le64  i_dir_btree_root;   /* dir B+ tree root block, 0 = flat list */
	__le32  i_dirent_count;     /* number of directory entries */
	__le64  i_xattr_block;      /* xattr block number, 0 = no xattrs */
	__u8    i_reserved[12];
	__le32  i_checksum;
} __packed;

static_assert(sizeof(struct ocsfs_disk_inode) == OCSFS_INODE_SIZE,
	      "ocsfs_disk_inode must be exactly 512 bytes — fix i_reserved[]");

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
	__le64  ag_rc_btree_root;   /* ARCH-5: B+ tree root block for refcount (0 = empty) */
	__u8    ag_reserved[3984];
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

/* Xattr block — one per inode, allocated lazily, exactly one 4096-byte block */
struct ocsfs_disk_xattr_block {
	__le32  xb_magic;                       /* OCSFS_XATTR_MAGIC */
	__le32  xb_count;                       /* number of live entries */
	__le16  xb_data_len;                    /* bytes used in xb_data[] */
	__le16  xb_reserved;
	__u8    xb_data[OCSFS_XATTR_DATA_SIZE]; /* packed entries */
	__le32  xb_checksum;                    /* CRC32C([0..4091]) */
} __packed;

static_assert(sizeof(struct ocsfs_disk_xattr_block) == OCSFS_DEFAULT_BLOCK_SIZE,
	      "ocsfs_disk_xattr_block must be exactly 4096 bytes");

/* Node Slot — 256 bytes, up to 256 nodes */
struct ocsfs_disk_node_slot {
	__u8    ns_uuid[16];
	__u8    ns_name[64];
	__u8    ns_state;
	__u8    ns_reserved1;
	__le16  ns_slot_id;
	__le32  ns_mount_gen;
	__le64  ns_mount_time;
	__le64  ns_last_heartbeat;
	__le64  ns_pr_key;
	__u8    ns_auth_token[32];  /* HMAC-SHA256(secret,"ocsfs-v1"); 0 if no auth */
	__le32  ns_version;       /* CAS version per slot claim race-free */
	__u8    ns_reserved2[104];
	__le32  ns_checksum;
} __packed;

/* Heartbeat Entry — 1024 bytes per node */
struct ocsfs_disk_heartbeat {
	__le32  hb_magic;
	__le16  hb_node_slot;
	__le16  hb_state;
	__le64  hb_timestamp;
	__le64  hb_sequence;
	__le32  hb_mount_gen;
	__u8    hb_reserved[992];
	__le32  hb_checksum;
} __packed;

/* Lock Table Entry — 256 bytes */
struct ocsfs_disk_lock {
	__le32  le_magic;
	__le64  le_resource_id;
	__le32  le_resource_type;
	__le16  le_mode;
	__le16  le_holder_slot;
	__le32  le_holder_gen;
	__le64  le_grant_time;
	__le32  le_sh_holders;           /* bitmask: nodes 0-31 */
	__u8    le_sh_holders_ext[32];   /* nodes 32-255 */
	__u8    le_waiters[32];          /* waiting node bitmask */
	__u8    le_waiter_modes[64];     /* 2 bits per waiter */
	__le32  le_version;              /* CAS version */
	/* ARCH-7: dirty range written by EX holder at release; read by SH acquirer */
	__le64  le_inv_lo;      /* dirty range start (bytes) */
	__le64  le_inv_hi;      /* dirty range end (bytes); 0 == lo → full invalidation */
	__le32  le_inv_epoch;   /* bumped on every EX release; wrap-around accepted */
	/* ARCH-2: overflow chain — 0 = no overflow; non-zero = phys block address */
	__le64  le_overflow_block;
	__u8    le_reserved[56]; /* was 84; reduced by 28 (ARCH-7: 20, ARCH-2: 8) */
	__le32  le_checksum;
} __packed;

/* ═══════════════════════════════════════════════════════════════
 * IN-MEMORY STRUCTURES
 * ═══════════════════════════════════════════════════════════════ */

/* Per-extent in-memory representation */
struct ocsfs_extent {
	u64     logical_block;
	u64     physical_block;
	u32     length;         /* logical block count (range coverage) */
	u16     phys_length;    /* compressed physical blocks; 0 = same as length */
	u16     flags;
};

/* Physical block count for an extent (compressed or not). */
static inline u32 ocsfs_ext_phys_blocks(const struct ocsfs_extent *e)
{
	return (e->flags & OCSFS_EXT_COMPRESSED && e->phys_length)
	       ? (u32)e->phys_length : e->length;
}

/* In-memory lock resource — must be defined before ocsfs_ag_info and
 * ocsfs_inode_info which embed it. */
struct ocsfs_lock_res {
	u64             lr_resource_id;
	u32             lr_resource_type;
	u16             lr_mode;         /* currently held mode */
	u32             lr_slot;         /* lock table slot index (u16→u32 for ARCH-2) */
	bool            lr_dynamic;      /* allocated via kzalloc; safe to kfree */
	struct mutex    lr_mutex;        /* local serialization */
	struct list_head lr_list;        /* link in sb's active lock list */
	/* ARCH-7: dirty range captured at last SH acquire; used by read path */
	u64             lr_inv_lo;
	u64             lr_inv_hi;
	u32             lr_inv_epoch;
	/* ARCH-2: overflow chain — if non-zero, entry lives in this overflow block */
	u64             lr_overflow_addr; /* absolute byte addr on disk; 0 = primary table */
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
	u64             rc_btree_root;  /* ARCH-5: B+ tree root block (0 = empty) */
	struct mutex    ag_lock;        /* protects bitmap + inode table (local) */
	struct ocsfs_lock_res ag_lock_res; /* cross-node DLM lock for this AG */
	struct ocsfs_lock_res ag_rc_lock_res; /* cross-node DLM refcount lock */
};

/* Journal in-memory state */
struct ocsfs_journal {
	u64             disk_off;       /* byte offset on block device */
	u64             size;           /* journal region size */
	u64             head;           /* write position — protected by j_lock */
	u64             tail;           /* oldest live txn — updated under j_lock */
	u64             sequence;       /* next txn ID — protected by j_lock */
	u16             j_node_slot;    /* which node's journal this is */
	struct mutex    j_lock;
	struct buffer_head *j_header_bh;
	struct super_block *j_sb;       /* owning superblock */

	/*
	 * Ordered-checkpoint ticket system.
	 *
	 * j_lock is released immediately after the COMMIT record is durable,
	 * allowing concurrent txns to begin journaling while the checkpoint
	 * (parallel write_dirty_buffer + SAN flush) runs lock-free.
	 *
	 * Tickets enforce FIFO checkpoint order so j->tail only advances
	 * monotonically and recovery correctness is preserved.
	 */
	atomic64_t      j_ckpt_ticket;  /* next ticket to hand out (incremented under j_lock) */
	atomic64_t      j_ckpt_now;     /* ticket that currently holds the checkpoint turn */
	wait_queue_head_t j_ckpt_waitq;
};

/* Active journal transaction */
struct ocsfs_txn {
	u64                     t_id;
	struct ocsfs_journal    *t_journal;
	struct list_head        t_buffers;      /* list of journaled BHs */
	unsigned int            t_nr_blocks;
	bool                    t_started;
	/* O(1) idempotency check: hash by block_num & 63 */
	struct hlist_head       t_block_hash[64];
};

/* Buffer in a transaction */
struct ocsfs_txn_buf {
	struct list_head        list;
	struct hlist_node       hash_node;      /* link in t_block_hash[] */
	struct buffer_head      *bh;
	u64                     block_num;
	u8                      *before_buf;    /* snapshot for rollback on abort */
	u8                      *after_buf;     /* shadow copy for AFTER-image */
};

/* Node info — in-memory representation of a peer node */
struct ocsfs_node_info {
	u16             ni_slot;
	u8              ni_state;        /* OCSFS_NODE_* */
	u32             ni_mount_gen;
	u64             ni_pr_key;
	u64             ni_last_hb;      /* last heartbeat timestamp we saw */
	u64             ni_hb_sequence;
	u64             ni_suspect_time; /* when we first suspected this node */
	u8              ni_uuid[16];
	char            ni_name[64];
};

/* Heartbeat thread state */
struct ocsfs_heartbeat_info {
	struct task_struct      *hb_thread;
	wait_queue_head_t        hb_waitq;
	bool                    hb_running;
	atomic64_t              hb_sequence;     /* our monotonic counter */
};

/* SCSI PR state */
struct ocsfs_pr_info {
	u64             pr_key;          /* our registration key */
	bool            pr_registered;
};

/* Superblock in-memory info — stored in sb->s_fs_info */
struct ocsfs_sb_info {
	struct super_block      *s_sb;          /* back-link to VFS superblock */
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
	/* ARCH-1: format versioning */
	u32             s_revision_level;
	u64             s_feature_compat;
	u64             s_feature_incompat;
	u64             s_feature_ro_compat;
	/* ARCH-2: dynamic lock table */
	u64             s_lock_table_off_cached; /* from ds->s_lock_table_off */
	u32             s_lock_primary_count;    /* 0 = legacy (OCSFS_LOCK_ENTRY_COUNT) */
	u64             s_data_off;             /* first data byte */
	u64             s_ag_desc_off;

	/* Allocation groups */
	struct ocsfs_ag_info    *s_ags;         /* array [s_ag_count] */

	/* Journal */
	struct ocsfs_journal    s_journal;

	/* Inode cache */
	struct kmem_cache       *s_inode_cache;

	/* Locks */
	struct rw_semaphore     s_global_lock;  /* global metadata lock */
	spinlock_t              s_free_lock;    /* protects s_free_blocks */

	/* === Phase 2: Clustering === */

	/* This node's identity */
	u16             s_node_slot;            /* our slot in node table */
	u32             s_mount_gen;            /* our mount generation */
	u8              s_node_uuid[16];        /* machine UUID */
	char            s_node_name[64];        /* hostname */
	bool            s_clustered;            /* multi-node mode active */

	/* Node table */
	struct ocsfs_node_info  s_nodes[OCSFS_MAX_NODES];
	spinlock_t              s_node_lock;

	/* Heartbeat */
	struct ocsfs_heartbeat_info s_hb;

	/* SCSI PR */
	struct ocsfs_pr_info    s_pr;

	/* Distributed lock manager */
	struct list_head        s_lock_list;    /* active lock_res list */
	spinlock_t              s_lock_list_lock;
	atomic_t                s_lock_epoch;  /* incremented by lock_recover_node */

	/* Recovery */
	struct work_struct      s_recovery_work;
	DECLARE_BITMAP(s_recovery_pending, OCSFS_MAX_NODES); /* one bit per failed slot */
	bool                    s_recovery_in_progress;
	struct mutex            s_recovery_lock;
	atomic_t                s_recovery_barrier;        /* non-zero during Phase 3 replay (local); EX acquires must wait */
	atomic_t                s_remote_recovery_barrier; /* non-zero when a peer leader has OCSFS_RL_REPLAY_ACTIVE set */

	/* Cluster auth */
	u8              s_cluster_secret[32];   /* raw secret from mount option */
	bool            s_auth_required;        /* OCSFS_FEAT_AUTH or secret given */

	/* SCSI Compare-And-Write capability (probed at mount time) */
	bool            s_caw_supported;
	bool            s_pr_capable;   /* device has working SCSI PR (pr_register + pr_preempt) */
	bool            s_degraded;     /* mount option: allow cluster without fencing */
	enum ocsfs_cas_backend s_cas_backend;

	/* ZSTD decompression workspace — lazy-allocated on first ZSTD read */
	struct mutex    s_decompress_lock;
	void           *s_decompress_wksp;
	size_t          s_decompress_wksp_sz;

	/* Refcount CAS buffer pool — avoids per-call kmalloc in hot path */
	mempool_t      *s_rc_buf_pool;
};

/*
 * Maximum symlink target length that fits inline in the disk inode's
 * i_inline_extents area (OCSFS_INLINE_EXTENTS * 24 bytes - 1 for NUL).
 */
#define OCSFS_MAX_INLINE_SYMLINK  (OCSFS_INLINE_EXTENTS * 24 - 1)

/* Per-inode in-memory info — wraps struct inode */
struct ocsfs_inode_info {
	u64                     i_disk_ino;     /* on-disk inode number */
	u32                     i_ag;           /* home AG */
	u32                     i_flags;        /* OCSFS_IFLAG_* */
	u16                     i_extent_count;
	struct ocsfs_extent     i_extents[OCSFS_INLINE_EXTENTS];
	u64                     i_extent_tree_root;
	struct mutex            i_extent_lock;
	struct ocsfs_lock_res   i_lock_res;     /* cross-node DLM inode lock */
	u16                     i_last_writer_slot; /* ARCH-7: OCSFS_INVALID_WRITER_SLOT if none */
	/* directory B+ tree index */
	u64                     i_dir_btree_root; /* 0 = flat-list dir */
	u32                     i_dirent_count;   /* live entry count */
	/* symlink target (NULL unless S_ISLNK; freed on evict) */
	char                   *i_symlink;
	/* xattr block (0 = no xattr block allocated yet) */
	u64                     i_xattr_block;
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

/*
 * Assert that the caller holds DLM EX on this inode in cluster mode.
 * Fires WARN_ON when a btree write function is entered without the required
 * exclusive lock — catches missing lock acquisitions during development.
 * Silent in single-node mode (DLM not used).
 */
#define OCSFS_WARN_NO_EX(inode) \
	WARN_ON(OCSFS_SB((inode)->i_sb)->s_clustered && \
		OCSFS_I(inode)->i_lock_res.lr_mode != OCSFS_LOCK_EX)

/* ═══════════════════════════════════════════════════════════════
 * UTILITY HELPERS
 * ═══════════════════════════════════════════════════════════════ */

static inline u32 ocsfs_crc32c(u32 crc, const void *data, size_t len)
{
	return crc32c(crc, data, len);
}

/*
 * ocsfs_ino_to_ag — mappa un numero di inode al suo AG.
 *
 * Contratto:
 *   - Richiede s_ag_size > 0; ritorna 0 se zero (guard anti-div-by-zero).
 *   - Non valida il risultato contro s_ag_count: il caller deve controllare
 *     che il valore ritornato sia < s_ag_count prima di accedere a s_ags[].
 *   - Valori speciali: ino < OCSFS_FIRST_USER_INO appartengono ad AG 0.
 */
static inline u32 ocsfs_ino_to_ag(struct ocsfs_sb_info *sbi, u64 ino)
{
	u64 ag_size = sbi->s_ag_size;

	if (unlikely(!ag_size))
		return 0;
	return (u32)(ino / ag_size);
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
	u64 local;

	if (unlikely(ag >= sbi->s_ag_count || !sbi->s_ag_size))
		return 0;
	local = ino % sbi->s_ag_size;
	return sbi->s_ags[ag].inode_table_off + local * OCSFS_INODE_SIZE;
}

/* ═══════════════════════════════════════════════════════════════
 * FUNCTION DECLARATIONS
 * ═══════════════════════════════════════════════════════════════ */

/* super.c */
extern const struct super_operations ocsfs_sops;
int ocsfs_fill_super(struct super_block *sb, struct fs_context *fc);
void ocsfs_put_super(struct super_block *sb);
int ocsfs_statfs(struct dentry *dentry, struct kstatfs *buf);
int ocsfs_sync_fs(struct super_block *sb, int wait);

/* inode.c */
extern const struct inode_operations ocsfs_file_inode_ops;
extern const struct inode_operations ocsfs_special_inode_ops;
extern const struct inode_operations ocsfs_symlink_inode_ops;
/* xattr namespace identifiers (shared between xattr.c and dir_rename.c) */
#define OCSFS_XATTR_NS_USER     0
#define OCSFS_XATTR_NS_TRUSTED  1
#define OCSFS_XATTR_NS_SECURITY 2
#define OCSFS_XATTR_NS_SYSTEM   3

extern const struct xattr_handler * const ocsfs_xattr_handlers[];
int ocsfs_xattr_get_internal(struct inode *inode, u8 ns, const char *name,
			     void *buffer, size_t size);
int ocsfs_xattr_set_internal(struct inode *inode, u8 ns, const char *name,
			     const void *value, size_t size, int flags);
ssize_t ocsfs_listxattr(struct dentry *dentry, char *buffer, size_t size);

/* acl.c */
struct posix_acl *ocsfs_get_inode_acl(struct inode *inode, int type, bool rcu);
int ocsfs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct posix_acl *acl, int type);
int ocsfs_init_acl(struct mnt_idmap *idmap, struct inode *inode,
		   struct inode *dir);

struct inode *ocsfs_iget(struct super_block *sb, u64 ino);
int ocsfs_flush_inode_locked(struct inode *inode, bool force_sync);
int ocsfs_inode_journal_root(struct ocsfs_txn *txn, struct inode *inode);
int ocsfs_inode_refresh(struct inode *inode);
int ocsfs_orphan_scan(struct super_block *sb);
int ocsfs_write_inode(struct inode *inode, struct writeback_control *wbc);
void ocsfs_evict_inode(struct inode *inode);
struct inode *ocsfs_new_inode(struct inode *dir, umode_t mode);
int ocsfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct iattr *attr);
int ocsfs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 request_mask, unsigned int flags);

/* dir.c */
int ocsfs_add_dirent(struct inode *dir, const struct qstr *name,
		     u64 ino, u8 file_type);
int ocsfs_del_dirent(struct inode *dir, const struct qstr *name);
u64 ocsfs_find_dirent(struct inode *dir, const struct qstr *name, u8 *ft_out);
int ocsfs_empty_dir(struct inode *dir);
/* dir.c internal — used by dir_rename.c */
struct buffer_head *ocsfs_dir_bread(struct inode *dir, u64 logical_block);
int __ocsfs_add_dirent(struct inode *dir, const struct qstr *name,
		       u64 ino, u8 file_type);
int __ocsfs_del_dirent(struct inode *dir, const struct qstr *name);
int __ocsfs_update_dirent_ino(struct inode *dir, const struct qstr *name,
			      u64 new_ino, u8 new_ft);
int __ocsfs_empty_dir(struct inode *dir);
struct dentry *ocsfs_lookup(struct inode *dir, struct dentry *dentry,
			    unsigned int flags);
int ocsfs_create(struct mnt_idmap *idmap, struct inode *dir,
		 struct dentry *dentry, umode_t mode, bool excl);
struct dentry *ocsfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			   struct dentry *dentry, umode_t mode);

/* dir_rename.c */
extern const struct inode_operations ocsfs_dir_inode_ops;
extern const struct file_operations ocsfs_dir_fops;

/* dir_btree.c */
u64  ocsfs_dir_btree_lookup(struct inode *dir, const struct qstr *name,
			    u8 *ft_out);
int  ocsfs_dir_btree_locate(struct inode *dir, const struct qstr *name,
			    u64 *phys_block, u32 *phys_off);
int  ocsfs_dir_btree_insert(struct inode *dir, const struct qstr *name,
			    u64 phys_block, u32 offset);
int  ocsfs_dir_btree_delete(struct inode *dir, const struct qstr *name);
int  ocsfs_dir_btree_migrate(struct inode *dir);
bool ocsfs_dir_btree_should_build(struct inode *dir);

/* file.c */
extern const struct file_operations ocsfs_file_fops;
extern const struct address_space_operations ocsfs_aops;
int ocsfs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		 u64 start, u64 len);
int ocsfs_fileattr_get(struct dentry *dentry, struct file_kattr *fa);
int ocsfs_fileattr_set(struct mnt_idmap *idmap, struct dentry *dentry,
		       struct file_kattr *fa);

/* extent.c */
int ocsfs_extent_lookup(struct inode *inode, u64 logical_block,
			struct ocsfs_extent *ext_out);
int ocsfs_extent_insert(struct inode *inode, u64 logical, u64 physical,
			u32 len, u16 flags);
int ocsfs_extent_truncate(struct inode *inode, u64 from_block);
int ocsfs_extent_count_blocks(struct inode *inode, u64 *count);
int ocsfs_extent_convert_unwritten(struct inode *inode, u64 logical_block,
				   u32 len);

/* extent_btree.c */
int ocsfs_extent_btree_lookup(struct inode *inode, u64 logical,
			      struct ocsfs_extent *out);
int ocsfs_extent_btree_insert(struct inode *inode, u64 logical, u64 physical,
			      u32 len, u16 flags);
int ocsfs_extent_btree_migrate(struct inode *inode);
int ocsfs_extent_btree_truncate(struct inode *inode, u64 from_block);
int ocsfs_extent_btree_convert_unwritten(struct inode *inode, u64 logical,
					 u32 len);
int ocsfs_extent_btree_count(struct inode *inode, u64 *count);
u64 ocsfs_extent_btree_goal_block(struct inode *inode);
typedef int (*ocsfs_extent_iter_fn)(u64 logical, u64 physical, u32 length,
				    u16 flags, void *ctx);
int ocsfs_extent_btree_iterate(struct inode *inode, ocsfs_extent_iter_fn fn,
			       void *ctx);
int ocsfs_extent_btree_replace(struct inode *inode,
				const struct ocsfs_extent *orig,
				u64 offset_in_ext, u32 cow_len, u64 new_phys);
int ocsfs_extent_btree_clear(struct inode *inode);
int ocsfs_extent_btree_init_empty(struct inode *inode);
int ocsfs_extent_btree_punch_hole(struct inode *inode,
				  u64 start_block, u64 end_block);
int ocsfs_extent_btree_zero_range(struct inode *inode,
				  u64 start_block, u64 end_block);
int ocsfs_extent_btree_compress_one(struct inode *inode,
				    const struct ocsfs_extent *old_ext,
				    u64 new_phys, u32 new_len, u16 new_flags);

/* bitmap.c */
int ocsfs_alloc_blocks(struct super_block *sb, u32 ag_hint, u32 count,
		       u64 *block_out);
int ocsfs_alloc_blocks_txn(struct ocsfs_txn *txn, struct super_block *sb,
			    u32 ag_hint, u32 count, u64 *block_out);
int  ocsfs_free_blocks_txn(struct ocsfs_txn *txn, u64 block, u32 count);
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
int ocsfs_journal_replay_node(struct super_block *sb, u16 node_slot);

/* scsi_pr.c — SCSI-3 Persistent Reservations + Compare-And-Write */
int ocsfs_pr_register(struct super_block *sb, u64 key);
int ocsfs_pr_unregister(struct super_block *sb);
int ocsfs_pr_reserve(struct super_block *sb, u8 type);
int ocsfs_pr_release(struct super_block *sb, u8 type);
int ocsfs_pr_preempt(struct super_block *sb, u64 victim_key, u8 type);
int ocsfs_pr_preempt_abort(struct super_block *sb, u64 victim_key, u8 type);
bool ocsfs_pr_probe(struct super_block *sb);
u64 ocsfs_pr_make_key(const u8 *uuid, u32 mount_gen);
/* CAW — atomic lock-table write via SCSI Compare-And-Write (opcode 0x89) */
void ocsfs_build_caw_cdb(u8 cdb[16], u64 lba, u32 num_blocks);
bool ocsfs_scsi_caw_probe(struct super_block *sb);
int  ocsfs_scsi_caw(struct super_block *sb, u64 lba,
		    const void *expected, const void *new_data,
		    unsigned int lbs);
int  ocsfs_bsg_execute_cdb(struct super_block *sb,
			    const u8 cdb[16], void *buf, unsigned int buf_len,
			    enum dma_data_direction data_dir);
int  ocsfs_scsi_pool_init(void);
void ocsfs_scsi_pool_destroy(void);

/* lock.c — Distributed on-disk lock manager */
bool lock_modes_compatible(u16 held, u16 requested);
int ocsfs_dlm_init(struct super_block *sb);
void ocsfs_dlm_exit(struct super_block *sb);
void ocsfs_lock_init(struct ocsfs_lock_res *lr, u64 resource_id,
		     u32 resource_type);
struct ocsfs_lock_res *ocsfs_lock_alloc(struct super_block *sb,
					u64 resource_id, u32 resource_type);
void ocsfs_lock_free(struct ocsfs_lock_res *lr);
int ocsfs_lock_acquire(struct super_block *sb, struct ocsfs_lock_res *lr,
		       u16 mode);
int ocsfs_lock_release(struct super_block *sb, struct ocsfs_lock_res *lr);
int ocsfs_lock_downgrade(struct super_block *sb, struct ocsfs_lock_res *lr,
			 u16 new_mode);
int ocsfs_lock_recover_node(struct super_block *sb, u16 node_slot,
			    u32 mount_gen);

/* Resource ID hashing */
static inline u64 ocsfs_lock_hash_inode(u64 ino)
{
	u64 h = 0xcbf29ce484222325ULL;
	h ^= ino;
	h *= 0x100000001b3ULL;
	h ^= (ino >> 32);
	h *= 0x100000001b3ULL;
	return h;
}

static inline u64 ocsfs_lock_hash_ag(u32 ag_num)
{
	return ocsfs_lock_hash_inode((u64)ag_num | 0xA600000000000000ULL);
}

static inline u64 ocsfs_lock_hash_rc(u32 ag_num)
{
	return ocsfs_lock_hash_inode((u64)ag_num | 0xAC00000000000000ULL);
}

/* ARCH-2: use runtime count (0 = legacy fallback to OCSFS_LOCK_ENTRY_COUNT). */
static inline u32 ocsfs_lock_primary_count(struct ocsfs_sb_info *sbi)
{
	return sbi->s_lock_primary_count ? sbi->s_lock_primary_count
					 : OCSFS_LOCK_ENTRY_COUNT;
}

static inline u64 ocsfs_lock_table_base(struct ocsfs_sb_info *sbi)
{
	return sbi->s_lock_table_off_cached ? sbi->s_lock_table_off_cached
					    : OCSFS_LOCK_TABLE_OFF;
}

static inline u32 ocsfs_lock_table_slot(u64 resource_id, u32 count)
{
	return (u32)(resource_id % count);
}

/* heartbeat.c — Storage-path heartbeat */
int ocsfs_heartbeat_start(struct super_block *sb);
void ocsfs_heartbeat_stop(struct super_block *sb);
int ocsfs_heartbeat_write(struct super_block *sb);
int ocsfs_heartbeat_check_peers(struct super_block *sb);
bool ocsfs_node_is_alive(struct super_block *sb, u16 slot);

/* node.c — Node slot table management */
int ocsfs_node_init(struct super_block *sb);
void ocsfs_node_exit(struct super_block *sb);
int ocsfs_node_claim_slot(struct super_block *sb);
int ocsfs_node_release_slot(struct super_block *sb);
int ocsfs_node_read_table(struct super_block *sb);
int ocsfs_node_mark_dead(struct super_block *sb, u16 slot);
int ocsfs_node_verify_auth(struct super_block *sb,
			    const struct ocsfs_disk_node_slot *slot);

/* recovery.c — Multi-phase crash recovery */
int ocsfs_recovery_init(struct super_block *sb);
void ocsfs_recovery_exit(struct super_block *sb);
void ocsfs_recovery_trigger(struct super_block *sb, u16 failed_slot);
int ocsfs_recovery_run(struct super_block *sb, u16 failed_slot);

/* cluster init/exit (called from super.c) */
int ocsfs_cluster_init(struct super_block *sb);
void ocsfs_cluster_exit(struct super_block *sb);

/* === Phase 3: Performance Optimization === */

/* iomap.c — iomap-based I/O */
extern const struct iomap_ops ocsfs_iomap_ops;
extern const struct iomap_ops ocsfs_dio_iomap_ops;
extern const struct address_space_operations ocsfs_iomap_aops;
ssize_t ocsfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to);
ssize_t ocsfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from);

/* alloc.c — Smart block allocation with preallocation */
int ocsfs_alloc_extent(struct inode *inode, u64 logical_block,
		       u32 requested, u32 *allocated, u64 *phys_out,
		       u16 flags);
int ocsfs_prealloc_blocks(struct inode *inode, u64 offset, u64 len);

/* thin.c — Thin provisioning, fallocate, DISCARD */
long ocsfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len);
int ocsfs_punch_hole(struct inode *inode, loff_t offset, loff_t len);
int ocsfs_zero_range(struct inode *inode, loff_t offset, loff_t len);
int ocsfs_discard_blocks(struct super_block *sb, u64 block, u32 count);
void ocsfs_thin_stats(struct inode *inode, u64 *written, u64 *unwritten);

/* === Phase 4: Advanced Features + Proxmox Integration === */

/* ioctl interface for snapshot management (OCSFS_IOC_SNAP_CREATE/DELETE) */
#define OCSFS_SNAP_NAME_MAX  255

struct ocsfs_snap_arg {
	__u64 dir_ino;                     /* inode of the target directory */
	char  name[OCSFS_SNAP_NAME_MAX + 1];
};

#define OCSFS_IOC_SNAP_CREATE  _IOW('O', 1, struct ocsfs_snap_arg)
#define OCSFS_IOC_SNAP_DELETE  _IO ('O', 2)  /* invoked on the snap inode */

/* snapshot.c — CoW file-level snapshots */
int ocsfs_snapshot_create(struct inode *src, struct inode *dir,
			  const struct qstr *name);
int ocsfs_snapshot_delete(struct inode *snap);
int ocsfs_cow_extent(struct inode *inode, u64 logical, u32 len);
bool ocsfs_needs_cow(struct super_block *sb, u64 phys_block);

/* refcount.c — Extent reference counting for CoW */
int ocsfs_refcount_get(struct super_block *sb, u64 phys_block,
		       u32 *refcount_out);
int ocsfs_refcount_inc(struct super_block *sb, u64 phys_block, u32 len);
int ocsfs_refcount_dec(struct super_block *sb, u64 phys_block, u32 len,
		       bool *should_free);
int ocsfs_refcount_init_ag(struct super_block *sb, u32 ag_no);

/* compress.c — Inline LZ4/ZSTD compression */
int ocsfs_compress_data(u8 algo, const void *src, unsigned int src_len,
			void *dst, unsigned int *dst_len);
int ocsfs_decompress_data(struct super_block *sb, u8 algo,
			  const void *src, unsigned int src_len,
			  void *dst, unsigned int dst_len);
int ocsfs_compress_extent_read(struct inode *inode,
			       struct ocsfs_extent *ext,
			       struct page **pages, unsigned int nr_pages);
u8 ocsfs_get_compression_algo(struct inode *inode);
int ocsfs_set_compression(struct inode *inode, u8 algo);
int ocsfs_compress_file(struct inode *inode);
int ocsfs_extent_decompress_for_write(struct inode *inode, u64 logical_block);
void ocsfs_compress_stats(struct inode *inode, u64 *disk_size,
			  u64 *logical_size);

/* cas.c */
int ocsfs_cas_probe(struct super_block *sb);
int ocsfs_atomic_cas(struct super_block *sb, u64 block, u32 boff,
		     u32 len, const void *expected, const void *new_data);

/* dedup.c — content-based block deduplication using refcount infrastructure */
struct ocsfs_dedup_result {
	__u64 bytes_deduped;
};
#define OCSFS_IOC_DEDUP  _IOR('O', 3, struct ocsfs_dedup_result)

int ocsfs_dedup_file(struct inode *inode, u64 *bytes_deduped);

#endif /* _OCSFS_KMOD_H */
