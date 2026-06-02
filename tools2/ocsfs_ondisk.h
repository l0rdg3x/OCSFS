/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OCSFS v2 — userspace mirror of the on-disk format.
 *
 * Layout (field offsets and struct sizes) is byte-for-byte identical to
 * kmod2/ocsfs.h. Userspace stores fields little-endian via the htole/letoh
 * helpers from endian.h; struct sizes are independent of byte order. Keep IN
 * SYNC with kmod2/ocsfs.h - the _Static_asserts here guard the sizes.
 */
#ifndef OCSFS2_ONDISK_H
#define OCSFS2_ONDISK_H

#include <stdint.h>
#include <assert.h>

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

/* compat feature: uniform-AG layout that online autogrow needs */
#define OCSFS2_FEAT_COMPAT_AUTOGROW  0x1ULL
#define OCSFS2_FEAT_RO_COMPAT_DATACSUM 0x1ULL   /* A8: per-data-block CRC */

#define OCSFS2_BLOCK_SIZE     4096
#define OCSFS2_INODE_SIZE     512
#define OCSFS2_ROOT_INO       2
#define OCSFS2_FIRST_USER_INO 64
#define OCSFS2_INLINE_EXTENTS 16
#define OCSFS2_MAX_NAME       255
#define OCSFS2_MAX_LABEL      64
/* mkfs default when -N is omitted: format headroom for the cluster, not a
 * runtime cap. 32 = any realistic Proxmox cluster, ~512 MiB reserved journal.
 * Override with -N (1 single-node .. 256 max). Raising it later needs a reformat. */
#define OCSFS2_DEFAULT_MAX_NODES 32

#define OCSFS2_DIRENT_SIZE        512
#define OCSFS2_DIRENTS_PER_BLOCK  (OCSFS2_BLOCK_SIZE / OCSFS2_DIRENT_SIZE)

#define OCSFS2_FT_UNKNOWN 0
#define OCSFS2_FT_REG     1
#define OCSFS2_FT_DIR     2
#define OCSFS2_FT_CHRDEV  3
#define OCSFS2_FT_BLKDEV  4
#define OCSFS2_FT_FIFO    5
#define OCSFS2_FT_SOCK    6
#define OCSFS2_FT_SYMLINK 7

#define OCSFS2_NODE_SLOT_SIZE   256
#define OCSFS2_HEARTBEAT_SIZE   256
#define OCSFS2_LEASE_ENTRY_SIZE 64
#define OCSFS2_DEFAULT_LEASE_COUNT 65536

#define OCSFS2_PACKED __attribute__((packed))

struct ocsfs2_disk_super {
	uint32_t s_magic;
	uint16_t s_major;
	uint16_t s_minor;
	uint8_t  s_uuid[16];
	uint8_t  s_label[OCSFS2_MAX_LABEL];
	uint32_t s_block_size;
	uint32_t s_inode_size;
	uint64_t s_total_blocks;
	uint64_t s_free_blocks;
	uint64_t s_total_inodes;
	uint64_t s_free_inodes;
	uint32_t s_ag_count;
	uint16_t s_max_nodes;
	uint16_t s_pad0;
	uint64_t s_ag_size;
	uint64_t s_ag_blocks;
	uint64_t s_feat_compat;
	uint64_t s_feat_incompat;
	uint64_t s_feat_ro_compat;
	uint64_t s_node_table_off;
	uint64_t s_heartbeat_off;
	uint64_t s_lease_table_off;
	uint64_t s_lease_count;
	uint64_t s_recovery_off;
	uint64_t s_journal_off;
	uint64_t s_journal_size;
	uint64_t s_ag_desc_off;
	uint64_t s_data_off;
	uint64_t s_mkfs_time;
	uint64_t s_mount_count;
	uint64_t s_inodes_per_ag;
	uint8_t  s_reserved[3820];
	uint32_t s_checksum;
} OCSFS2_PACKED;
_Static_assert(sizeof(struct ocsfs2_disk_super) == 4096, "disk_super 4096");

struct ocsfs2_disk_extent {
	uint64_t e_logical;
	uint64_t e_physical;
	uint32_t e_length;
	uint16_t e_flags;
	uint16_t e_pad;
} OCSFS2_PACKED;
_Static_assert(sizeof(struct ocsfs2_disk_extent) == 24, "disk_extent 24");

struct ocsfs2_disk_inode {
	uint32_t i_magic;
	uint32_t i_generation;
	uint64_t i_ino;
	uint16_t i_mode;
	uint16_t i_nlink;
	uint32_t i_uid;
	uint32_t i_gid;
	uint64_t i_size;
	uint64_t i_blocks;
	uint64_t i_atime;
	uint64_t i_mtime;
	uint64_t i_ctime;
	uint32_t i_flags;
	uint16_t i_extent_count;
	uint16_t i_pad2;
	uint64_t i_extent_tree_root;
	uint8_t  i_inline_extents[OCSFS2_INLINE_EXTENTS * 24];
	uint64_t i_dir_btree_root;
	uint32_t i_dirent_count;
	uint32_t i_rdev;
	uint64_t i_xattr_block;
	uint8_t  i_reserved[16];
	uint32_t i_checksum;
} OCSFS2_PACKED;
_Static_assert(sizeof(struct ocsfs2_disk_inode) == 512, "disk_inode 512");

struct ocsfs2_disk_ag {
	uint32_t ag_magic;
	uint32_t ag_number;
	uint64_t ag_block_start;
	uint64_t ag_block_count;
	uint64_t ag_free_blocks;
	uint64_t ag_free_inodes;
	uint64_t ag_bitmap_off;
	uint64_t ag_bitmap_blocks;
	uint64_t ag_inode_table_off;
	uint64_t ag_inodes_per_ag;
	uint64_t ag_data_off;
	uint64_t ag_data_blocks;
	uint64_t ag_rc_btree_root;
	uint64_t ag_csum_off;        /* A8: data-checksum region byte offset (0=none) */
	uint64_t ag_csum_blocks;
	uint8_t  ag_reserved[3980];
	uint32_t ag_checksum;
} OCSFS2_PACKED;
_Static_assert(sizeof(struct ocsfs2_disk_ag) == 4096, "disk_ag 4096");

struct ocsfs2_disk_dirent {
	uint32_t de_magic;
	uint32_t de_checksum;
	uint64_t de_ino;
	uint64_t de_name_hash;
	uint8_t  de_file_type;
	uint8_t  de_name_len;
	uint16_t de_pad;
	uint8_t  de_name[OCSFS2_MAX_NAME + 1];
	uint8_t  de_reserved[228];
} OCSFS2_PACKED;
_Static_assert(sizeof(struct ocsfs2_disk_dirent) == OCSFS2_DIRENT_SIZE,
	       "disk_dirent 512");

struct ocsfs2_disk_node_slot {
	uint32_t ns_magic;
	uint8_t  ns_state;
	uint8_t  ns_pad;
	uint16_t ns_slot_id;
	uint8_t  ns_uuid[16];
	uint8_t  ns_name[64];
	uint32_t ns_mount_gen;
	uint64_t ns_mount_time;
	uint64_t ns_pr_key;
	uint8_t  ns_reserved[144];
	uint32_t ns_checksum;
} OCSFS2_PACKED;
_Static_assert(sizeof(struct ocsfs2_disk_node_slot) == 256, "node_slot 256");

struct ocsfs2_disk_heartbeat {
	uint32_t hb_magic;
	uint16_t hb_node_slot;
	uint16_t hb_state;
	uint64_t hb_timestamp;
	uint64_t hb_sequence;
	uint32_t hb_mount_gen;
	uint8_t  hb_reserved[224];
	uint32_t hb_checksum;
} OCSFS2_PACKED;
_Static_assert(sizeof(struct ocsfs2_disk_heartbeat) == 256, "heartbeat 256");

struct ocsfs2_disk_lease {
	uint32_t l_magic;
	uint64_t l_resource_id;
	uint16_t l_owner_slot;
	uint16_t l_mode;
	uint32_t l_owner_gen;
	uint32_t l_sh_holders[8];
	uint16_t l_want_slot;
	uint16_t l_pad;
	uint32_t l_seq;
	uint32_t l_checksum;
} OCSFS2_PACKED;
_Static_assert(sizeof(struct ocsfs2_disk_lease) == 64, "lease 64");

struct ocsfs2_disk_journal_hdr {
	uint32_t jh_magic;
	uint16_t jh_node_slot;
	uint16_t jh_flags;
	uint64_t jh_head;
	uint64_t jh_tail;
	uint64_t jh_sequence;
	uint64_t jh_size;
	uint8_t  jh_reserved[4052];
	uint32_t jh_checksum;
} OCSFS2_PACKED;
_Static_assert(sizeof(struct ocsfs2_disk_journal_hdr) == 4096, "journal_hdr 4096");

/* refcount B+tree leaf node (reflink/snapshot, Plan 4) */
#define OCSFS2_RC_MAX_RECS  254
struct ocsfs2_disk_rc_rec {
	uint64_t rr_phys;
	uint32_t rr_len;
	uint32_t rr_refcount;
} OCSFS2_PACKED;
_Static_assert(sizeof(struct ocsfs2_disk_rc_rec) == 16, "rc_rec 16");

struct ocsfs2_disk_rc_node {
	uint32_t rn_magic;
	uint16_t rn_level;
	uint16_t rn_nr;
	uint32_t rn_ag;
	uint32_t rn_pad;
	struct ocsfs2_disk_rc_rec rn_recs[OCSFS2_RC_MAX_RECS];
	uint8_t  rn_reserved[12];
	uint32_t rn_checksum;
} OCSFS2_PACKED;
_Static_assert(sizeof(struct ocsfs2_disk_rc_node) == 4096, "rc_node 4096");

#endif /* OCSFS2_ONDISK_H */
