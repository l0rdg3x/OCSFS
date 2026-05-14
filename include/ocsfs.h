/*
 * OCSFS — Open Cluster Shared FileSystem
 * On-disk structures and constants
 *
 * Copyright (C) 2026 OCSFS Project Contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * This header defines the on-disk format. It is shared between:
 *   - mkfs.ocsfs (formatting tool)
 *   - ocsfs-tool (administration CLI)
 *   - FUSE prototype (userspace filesystem)
 *   - Linux kernel module (future)
 *
 * All multi-byte integers are little-endian on disk.
 * All offsets are in bytes unless noted.
 * All timestamps are nanoseconds since Unix epoch.
 */

#ifndef OCSFS_H
#define OCSFS_H

#include <stdint.h>
#include <uuid/uuid.h>

/* ═══════════════════════════════════════════════════════════════
 * MAGIC NUMBERS & VERSION
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_MAGIC             0x4F435346  /* 'OCSF' */
#define OCSFS_INODE_MAGIC       0x494E4F44  /* 'INOD' */
#define OCSFS_AG_MAGIC          0x41474850  /* 'AGHD' */
#define OCSFS_JOURNAL_MAGIC     0x4A524E4C  /* 'JRNL' */
#define OCSFS_LOCK_MAGIC        0x4C4F434B  /* 'LOCK' */
#define OCSFS_HEARTBEAT_MAGIC   0x48425454  /* 'HBTT' */
#define OCSFS_DIRENT_MAGIC      0x44495245  /* 'DIRE' */
#define OCSFS_EXTENT_TREE_MAGIC 0x45585442  /* 'EXTB' */

#define OCSFS_VERSION_MAJOR     0
#define OCSFS_VERSION_MINOR     1

/* ═══════════════════════════════════════════════════════════════
 * SIZE LIMITS & DEFAULTS
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_SUPERBLOCK_SIZE       4096
#define OCSFS_SUPERBLOCK_OFFSET     0
#define OCSFS_SUPERBLOCK_MIRROR     4096

#define OCSFS_NODE_SLOT_TABLE_OFF   8192        /* 8 KB */
#define OCSFS_NODE_SLOT_TABLE_SIZE  65536       /* 64 KB */

#define OCSFS_HEARTBEAT_OFF         73728       /* 72 KB */
#define OCSFS_HEARTBEAT_SIZE        262144      /* 256 KB */
#define OCSFS_HEARTBEAT_ENTRY_SIZE  1024        /* 1 KB per node */

#define OCSFS_LOCK_TABLE_OFF        335872      /* 328 KB */
#define OCSFS_LOCK_TABLE_SIZE       1048576     /* 1 MB */
#define OCSFS_LOCK_ENTRY_SIZE       256
#define OCSFS_LOCK_ENTRY_COUNT      (OCSFS_LOCK_TABLE_SIZE / OCSFS_LOCK_ENTRY_SIZE)

#define OCSFS_MAX_NODES             256
#define OCSFS_DEFAULT_MAX_NODES     64
#define OCSFS_MAX_LABEL             64

#define OCSFS_DEFAULT_BLOCK_SIZE    4096        /* 4 KB */
#define OCSFS_DEFAULT_EXTENT_SIZE   (1 << 20)   /* 1 MB */
#define OCSFS_MIN_EXTENT_SIZE       (64 << 10)  /* 64 KB */
#define OCSFS_MAX_EXTENT_SIZE       (64 << 20)  /* 64 MB */

#define OCSFS_DEFAULT_AG_SIZE       (1ULL << 30) /* 1 GB */
#define OCSFS_MIN_AG_SIZE           (256ULL << 20) /* 256 MB */
#define OCSFS_MAX_AG_SIZE           (64ULL << 30)  /* 64 GB */

#define OCSFS_DEFAULT_JOURNAL_SIZE  (32 << 20)  /* 32 MB per node */

#define OCSFS_HEARTBEAT_INTERVAL_MS 5000        /* 5 seconds */
#define OCSFS_HEARTBEAT_TIMEOUT_MS  15000       /* 15 seconds */

#define OCSFS_INODE_SIZE            512
#define OCSFS_INLINE_EXTENTS        16          /* 16 × 24 = 384 bytes, fits in 512-byte inode */
#define OCSFS_MAX_NAME_LEN          255

#define OCSFS_ROOT_INO              2           /* root directory inode */
#define OCSFS_FIRST_USER_INO        64          /* inodes below are reserved */

/* ═══════════════════════════════════════════════════════════════
 * FEATURE FLAGS (superblock s_feature_flags)
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_FEAT_THIN_PROV    (1ULL << 0)
#define OCSFS_FEAT_COMPRESSION  (1ULL << 1)
#define OCSFS_FEAT_ENCRYPTION   (1ULL << 2)
#define OCSFS_FEAT_SNAPSHOTS    (1ULL << 3)
#define OCSFS_FEAT_DEDUP        (1ULL << 4)
#define OCSFS_FEAT_MULTI_LUN    (1ULL << 5)

/* ═══════════════════════════════════════════════════════════════
 * NODE SLOT STATES
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_NODE_FREE         0x00
#define OCSFS_NODE_ACTIVE       0x01
#define OCSFS_NODE_EVICTING     0x02
#define OCSFS_NODE_DEAD         0xFF

/* ═══════════════════════════════════════════════════════════════
 * LOCK TYPES
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_LOCK_NL           0   /* Null — advisory */
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

/* ═══════════════════════════════════════════════════════════════
 * EXTENT FLAGS
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_EXT_WRITTEN       0x0000  /* normal extent with data */
#define OCSFS_EXT_UNWRITTEN     0x0001  /* allocated but not written (thin/prealloc) */
#define OCSFS_EXT_COMPRESSED    0x0002  /* inline compressed data */
#define OCSFS_EXT_SHARED        0x0004  /* CoW snapshot shared extent */

/* ═══════════════════════════════════════════════════════════════
 * INODE FLAGS
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_IFLAG_THIN        0x0001  /* thin-provisioned file */
#define OCSFS_IFLAG_COMPRESSED  0x0002  /* compression enabled */
#define OCSFS_IFLAG_ENCRYPTED   0x0004  /* encrypted file */
#define OCSFS_IFLAG_IMMUTABLE   0x0008  /* immutable (no writes) */
#define OCSFS_IFLAG_APPEND      0x0010  /* append-only */
#define OCSFS_IFLAG_NOSNAP      0x0020  /* exclude from snapshots */

/* ═══════════════════════════════════════════════════════════════
 * INODE MODE (matches Linux stat.h for POSIX compat)
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_FT_UNKNOWN        0
#define OCSFS_FT_REG_FILE       1
#define OCSFS_FT_DIR            2
#define OCSFS_FT_CHRDEV         3
#define OCSFS_FT_BLKDEV         4
#define OCSFS_FT_FIFO           5
#define OCSFS_FT_SOCK           6
#define OCSFS_FT_SYMLINK        7

/* ═══════════════════════════════════════════════════════════════
 * JOURNAL TRANSACTION TYPES
 * ═══════════════════════════════════════════════════════════════ */

#define OCSFS_JTYPE_BEGIN       1
#define OCSFS_JTYPE_METADATA    2   /* metadata block before/after image */
#define OCSFS_JTYPE_COMMIT      3
#define OCSFS_JTYPE_ABORT       4
#define OCSFS_JTYPE_CHECKPOINT  5

/* ═══════════════════════════════════════════════════════════════
 * ON-DISK STRUCTURES
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Superblock — always at offset 0 and mirrored at offset 4096.
 * Total size: 4096 bytes (padded).
 */
struct ocsfs_superblock {
    uint32_t    s_magic;            /* OCSFS_MAGIC */
    uint16_t    s_version_major;
    uint16_t    s_version_minor;
    uint8_t     s_uuid[16];         /* volume UUID */
    char        s_label[OCSFS_MAX_LABEL]; /* UTF-8 volume label */
    uint32_t    s_block_size;       /* bytes, power of 2 */
    uint32_t    s_extent_size;      /* bytes, multiple of block_size */
    uint64_t    s_total_blocks;     /* total blocks in volume */
    uint64_t    s_free_blocks;      /* free blocks (approximate, per-AG is authoritative) */
    uint32_t    s_ag_count;         /* number of allocation groups */
    uint64_t    s_ag_size;          /* blocks per AG */
    uint16_t    s_max_nodes;        /* max concurrent nodes */
    uint64_t    s_feature_flags;    /* OCSFS_FEAT_* */
    uint32_t    s_heartbeat_interval; /* ms */
    uint32_t    s_heartbeat_timeout;  /* ms */
    uint32_t    s_journal_size;     /* bytes per node journal */
    uint64_t    s_lock_table_off;   /* byte offset to lock table */
    uint64_t    s_journal_off;      /* byte offset to first journal */
    uint64_t    s_ag_desc_off;      /* byte offset to AG descriptor array */
    uint64_t    s_data_off;         /* byte offset to first AG data region */
    uint64_t    s_mkfs_time;        /* creation timestamp (ns) */
    uint64_t    s_mount_count;      /* total mount count */
    uint64_t    s_last_mount_time;  /* last mount timestamp (ns) */
    uint8_t     s_reserved[3890];   /* pad to 4096 bytes */
    uint32_t    s_checksum;         /* CRC32C of bytes 0..4091 */
} __attribute__((packed));

_Static_assert(sizeof(struct ocsfs_superblock) == OCSFS_SUPERBLOCK_SIZE,
               "superblock must be exactly 4096 bytes");

/*
 * Node Slot — 256 bytes, up to 256 nodes.
 */
struct ocsfs_node_slot {
    uint8_t     ns_uuid[16];        /* node machine UUID */
    char        ns_name[64];        /* hostname (informational) */
    uint8_t     ns_state;           /* OCSFS_NODE_* */
    uint8_t     ns_reserved1;
    uint16_t    ns_slot_id;         /* slot number (0..max_nodes-1) */
    uint32_t    ns_mount_gen;       /* mount generation counter */
    uint64_t    ns_mount_time;      /* when this node mounted (ns) */
    uint64_t    ns_last_heartbeat;  /* last heartbeat timestamp (ns) */
    uint64_t    ns_pr_key;          /* SCSI PR registration key */
    uint8_t     ns_reserved2[140];  /* pad to 256 bytes */
    uint32_t    ns_checksum;        /* CRC32C */
} __attribute__((packed));

_Static_assert(sizeof(struct ocsfs_node_slot) == 256,
               "node slot must be exactly 256 bytes");

/*
 * Heartbeat Entry — 1024 bytes per node.
 */
struct ocsfs_heartbeat {
    uint32_t    hb_magic;           /* OCSFS_HEARTBEAT_MAGIC */
    uint16_t    hb_node_slot;
    uint16_t    hb_state;           /* mirrors ns_state */
    uint64_t    hb_timestamp;       /* nanoseconds since epoch */
    uint64_t    hb_sequence;        /* monotonic counter */
    uint32_t    hb_mount_gen;       /* must match node slot */
    uint8_t     hb_reserved[992];   /* pad to 1024 bytes */
    uint32_t    hb_checksum;        /* CRC32C */
} __attribute__((packed));

_Static_assert(sizeof(struct ocsfs_heartbeat) == OCSFS_HEARTBEAT_ENTRY_SIZE,
               "heartbeat entry must be exactly 1024 bytes");

/*
 * Lock Table Entry — 256 bytes, 4096 entries in the lock table.
 */
struct ocsfs_lock_entry {
    uint32_t    le_magic;           /* OCSFS_LOCK_MAGIC */
    uint64_t    le_resource_id;     /* hash of resource identifier */
    uint32_t    le_resource_type;   /* OCSFS_LOCKRES_* */
    uint16_t    le_mode;            /* OCSFS_LOCK_* current grant mode */
    uint16_t    le_holder_slot;     /* node slot of current EX holder */
    uint32_t    le_holder_gen;      /* mount generation of holder */
    uint64_t    le_grant_time;      /* when lock was granted (ns) */
    uint32_t    le_sh_holders;      /* bitmask: nodes holding SH (up to 32 fast path) */
    uint8_t     le_sh_holders_ext[32]; /* extended bitmask for nodes 32..255 */
    uint8_t     le_waiters[32];     /* bitmask: waiting node slots */
    uint8_t     le_waiter_modes[64]; /* requested mode per waiter (packed 2 bits each) */
    uint32_t    le_version;         /* CAS version for atomicity */
    uint8_t     le_reserved[84];    /* pad to 256 bytes */
    uint32_t    le_checksum;        /* CRC32C */
} __attribute__((packed));

_Static_assert(sizeof(struct ocsfs_lock_entry) == OCSFS_LOCK_ENTRY_SIZE,
               "lock entry must be exactly 256 bytes");

/*
 * Extent — 24 bytes. Describes a contiguous range of blocks for a file.
 */
struct ocsfs_extent {
    uint64_t    e_logical_block;    /* file-relative block offset */
    uint64_t    e_physical_block;   /* volume-absolute block offset */
    uint32_t    e_length;           /* extent length in blocks */
    uint16_t    e_flags;            /* OCSFS_EXT_* */
    uint16_t    e_checksum;         /* CRC16 of this entry */
} __attribute__((packed));

_Static_assert(sizeof(struct ocsfs_extent) == 24,
               "extent must be exactly 24 bytes");

/*
 * Inode — 512 bytes. Holds file metadata and inline extent list.
 *
 * Inline extents: up to 28 extents (28 × 24 = 672 bytes).
 * When more extents are needed, i_extent_tree_root points to a B+ tree.
 */
struct ocsfs_inode {
    uint32_t    i_magic;            /* OCSFS_INODE_MAGIC */
    uint64_t    i_ino;              /* inode number */
    uint16_t    i_mode;             /* file type (upper 4 bits) + permissions */
    uint16_t    i_nlink;            /* hard link count */
    uint32_t    i_uid;
    uint32_t    i_gid;
    uint64_t    i_size;             /* file size in bytes */
    uint64_t    i_blocks;           /* allocated blocks (block_size units) */
    uint64_t    i_atime;            /* access time (ns) */
    uint64_t    i_mtime;            /* modification time (ns) */
    uint64_t    i_ctime;            /* change time (ns) */
    uint32_t    i_flags;            /* OCSFS_IFLAG_* */
    uint16_t    i_extent_count;     /* number of valid inline extents */
    uint16_t    i_extent_max;       /* max inline (OCSFS_INLINE_EXTENTS) */
    uint64_t    i_extent_tree_root; /* B+ tree root block (0 if inline only) */
    uint64_t    i_thin_allocated;   /* actual bytes written (thin prov.) */
    uint32_t    i_ag;               /* home allocation group */
    uint8_t     i_inline_extents[OCSFS_INLINE_EXTENTS * sizeof(struct ocsfs_extent)];
    /* 16 × 24 = 384 bytes; metadata = 92 bytes; checksum = 4; reserved = 32 → total 512 */
    uint8_t     i_reserved[32];
    uint32_t    i_checksum;         /* CRC32C of bytes 0..507 */
} __attribute__((packed));

_Static_assert(sizeof(struct ocsfs_inode) == OCSFS_INODE_SIZE,
               "inode must be exactly 512 bytes");

/*
 * Allocation Group Descriptor — 4096 bytes.
 */
struct ocsfs_ag_desc {
    uint32_t    ag_magic;           /* OCSFS_AG_MAGIC */
    uint32_t    ag_number;          /* AG index (0..ag_count-1) */
    uint64_t    ag_block_start;     /* first block in this AG (volume-absolute) */
    uint64_t    ag_block_count;     /* total blocks in this AG */
    uint64_t    ag_free_blocks;     /* free blocks in this AG */
    uint64_t    ag_free_extents;    /* number of free extents */
    uint64_t    ag_bitmap_off;      /* byte offset to block bitmap (relative to AG start) */
    uint64_t    ag_bitmap_size;     /* bitmap size in bytes */
    uint64_t    ag_inode_table_off; /* byte offset to inode table */
    uint64_t    ag_inode_count;     /* total inodes allocated in this AG */
    uint64_t    ag_free_inodes;     /* free inode slots */
    uint64_t    ag_extent_tree_off; /* byte offset to free extent B+ tree root */
    uint64_t    ag_inode_btree_off; /* byte offset to inode B+ tree root */
    uint16_t    ag_owner_node;      /* preferred (home) node for this AG */
    uint16_t    ag_flags;
    uint8_t     ag_reserved[3992];  /* pad to 4096 bytes */
    uint32_t    ag_checksum;        /* CRC32C */
} __attribute__((packed));

_Static_assert(sizeof(struct ocsfs_ag_desc) == 4096,
               "AG descriptor must be exactly 4096 bytes");

/*
 * Directory Entry — variable size, stored in B+ tree or inline.
 */
struct ocsfs_dirent {
    uint32_t    de_magic;           /* OCSFS_DIRENT_MAGIC */
    uint64_t    de_ino;             /* target inode number */
    uint64_t    de_name_hash;       /* XXH3-64 of filename */
    uint8_t     de_file_type;       /* OCSFS_FT_* */
    uint8_t     de_name_len;        /* filename length (1..255) */
    char        de_name[OCSFS_MAX_NAME_LEN + 1]; /* null-terminated filename */
    uint16_t    de_rec_len;         /* total record length (for padding/alignment) */
    uint16_t    de_checksum;
} __attribute__((packed));

/*
 * Journal Header — at the start of each per-node journal region.
 */
struct ocsfs_journal_header {
    uint32_t    jh_magic;           /* OCSFS_JOURNAL_MAGIC */
    uint16_t    jh_node_slot;       /* owning node */
    uint16_t    jh_flags;
    uint64_t    jh_head;            /* byte offset of journal head (next write) */
    uint64_t    jh_tail;            /* byte offset of journal tail (oldest live txn) */
    uint64_t    jh_sequence;        /* next transaction ID */
    uint64_t    jh_size;            /* journal region size in bytes */
    uint8_t     jh_reserved[4052];  /* pad to 4096 */
    uint32_t    jh_checksum;
} __attribute__((packed));

_Static_assert(sizeof(struct ocsfs_journal_header) == 4096,
               "journal header must be exactly 4096 bytes");

/*
 * Journal Transaction Record — variable size.
 */
struct ocsfs_journal_txn {
    uint32_t    jt_type;            /* OCSFS_JTYPE_* */
    uint64_t    jt_id;              /* transaction ID */
    uint64_t    jt_timestamp;       /* ns */
    uint16_t    jt_node_slot;
    uint16_t    jt_block_count;     /* number of metadata blocks in this record */
    uint32_t    jt_data_len;        /* total bytes of block data following */
    uint32_t    jt_checksum;        /* CRC32C of this header + all block data */
} __attribute__((packed));

/*
 * Journal Block Reference — precedes each metadata block in the journal.
 */
struct ocsfs_journal_block_ref {
    uint64_t    jbr_block_num;      /* volume-absolute block number */
    uint32_t    jbr_flags;          /* BEFORE_IMAGE, AFTER_IMAGE */
    uint32_t    jbr_checksum;       /* CRC32C of the block data */
} __attribute__((packed));

#define OCSFS_JBR_BEFORE    0x01
#define OCSFS_JBR_AFTER     0x02

/* ═══════════════════════════════════════════════════════════════
 * UTILITY FUNCTIONS (shared between userspace tools and FUSE)
 * ═══════════════════════════════════════════════════════════════ */

/* CRC32C — we use the same polynomial as the Linux kernel */
uint32_t ocsfs_crc32c(uint32_t crc, const void *data, size_t len);

/* Volume geometry calculations */
static inline uint64_t ocsfs_journal_region_size(uint16_t max_nodes, uint32_t journal_size) {
    return (uint64_t)max_nodes * journal_size;
}

static inline uint64_t ocsfs_journal_offset(void) {
    return OCSFS_LOCK_TABLE_OFF + OCSFS_LOCK_TABLE_SIZE;
}

static inline uint64_t ocsfs_ag_desc_offset(uint16_t max_nodes, uint32_t journal_size) {
    return ocsfs_journal_offset() + ocsfs_journal_region_size(max_nodes, journal_size);
}

static inline uint64_t ocsfs_data_offset(uint16_t max_nodes, uint32_t journal_size,
                                          uint32_t ag_count) {
    return ocsfs_ag_desc_offset(max_nodes, journal_size) +
           (uint64_t)ag_count * sizeof(struct ocsfs_ag_desc);
}

/* Lock resource ID hashing */
static inline uint64_t ocsfs_lock_hash_inode(uint64_t ino) {
    /* Simple but effective: FNV-1a inspired mixing */
    uint64_t h = 0xcbf29ce484222325ULL;
    h ^= ino;
    h *= 0x100000001b3ULL;
    h ^= (ino >> 32);
    h *= 0x100000001b3ULL;
    return h;
}

static inline uint64_t ocsfs_lock_hash_ag(uint32_t ag_num) {
    return ocsfs_lock_hash_inode((uint64_t)ag_num | 0xA600000000000000ULL);
}

static inline uint32_t ocsfs_lock_slot(uint64_t resource_id) {
    return (uint32_t)(resource_id % OCSFS_LOCK_ENTRY_COUNT);
}

#endif /* OCSFS_H */
