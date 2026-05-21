/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OCSFS — On-Disk B+ Tree
 *
 * Generic B+ tree for directory entries, extent overflow, refcount tables.
 * Each node is one filesystem block. Leaves are linked for range scans.
 */

#ifndef OCSFS_BTREE_H
#define OCSFS_BTREE_H

#include <linux/types.h>

/* Node magic */
#define OCSFS_BTREE_INTERNAL_MAGIC  0x42544E49u  /* 'BTNI' */
#define OCSFS_BTREE_LEAF_MAGIC      0x42544E4Cu  /* 'BTNL' */

/* Node flags */
#define OCSFS_BTREE_NODE_ROOT   0x01
#define OCSFS_BTREE_NODE_LEAF   0x02

struct ocsfs_btree_node_hdr {
	__le32  bn_magic;
	__le16  bn_flags;
	__le16  bn_count;       /* number of keys */
	__le16  bn_level;       /* 0 = leaf, >0 = internal */
	__le16  bn_reserved;
	__le32  bn_block_size;
	__le64  bn_block_num;
	__le64  bn_parent;
	__le64  bn_left_sibling;
	__le64  bn_right_sibling;
	__le32  bn_checksum;
	__le32  bn_padding;
} __packed;

#define OCSFS_BTREE_HDR_SIZE  sizeof(struct ocsfs_btree_node_hdr)

struct ocsfs_btree_entry {
	__le64  key;
	__le64  value;
} __packed;

struct ocsfs_btree_ptr {
	__le64  key;
	__le64  child;
} __packed;

static inline u32 ocsfs_btree_leaf_order(u32 block_size)
{
	return (block_size - OCSFS_BTREE_HDR_SIZE) /
	       sizeof(struct ocsfs_btree_entry);
}

static inline u32 ocsfs_btree_internal_order(u32 block_size)
{
	return (block_size - OCSFS_BTREE_HDR_SIZE - sizeof(u64)) /
	       sizeof(struct ocsfs_btree_ptr);
}

/* I/O and allocator callbacks */
typedef int (*ocsfs_btree_alloc_fn)(void *ctx, u64 *out_block);
typedef int (*ocsfs_btree_free_fn)(void *ctx, u64 block);
typedef int (*ocsfs_btree_read_fn)(void *ctx, u64 block, void *buf, u32 size);
typedef int (*ocsfs_btree_write_fn)(void *ctx, u64 block, const void *buf, u32 size);

struct ocsfs_btree {
	u64     root_block;
	u32     block_size;
	u32     leaf_order;
	u32     internal_order;
	u32     height;
	u64     entry_count;

	ocsfs_btree_read_fn   read_block;
	ocsfs_btree_write_fn  write_block;
	ocsfs_btree_alloc_fn  alloc_block;
	ocsfs_btree_free_fn   free_block;
	void *io_ctx;
};

/* ── node accessor inlines (shared by btree.c and btree_mod.c) ── */

static inline struct ocsfs_btree_node_hdr *node_hdr(void *buf)
{
	return (struct ocsfs_btree_node_hdr *)buf;
}

static inline int node_is_leaf(void *buf)
{
	return le16_to_cpu(node_hdr(buf)->bn_level) == 0;
}

static inline struct ocsfs_btree_entry *leaf_entries(void *buf)
{
	return (struct ocsfs_btree_entry *)((u8 *)buf + OCSFS_BTREE_HDR_SIZE);
}

static inline u64 *internal_first_child(void *buf)
{
	return (u64 *)((u8 *)buf + OCSFS_BTREE_HDR_SIZE);
}

static inline struct ocsfs_btree_ptr *internal_ptrs(void *buf)
{
	return (struct ocsfs_btree_ptr *)
	       ((u8 *)buf + OCSFS_BTREE_HDR_SIZE + sizeof(u64));
}

/* ── public API ── */

int ocsfs_btree_create(struct ocsfs_btree *bt, u32 block_size,
		       ocsfs_btree_read_fn, ocsfs_btree_write_fn,
		       ocsfs_btree_alloc_fn, ocsfs_btree_free_fn, void *ctx);

int ocsfs_btree_open(struct ocsfs_btree *bt, u64 root_block, u32 block_size,
		     ocsfs_btree_read_fn, ocsfs_btree_write_fn,
		     ocsfs_btree_alloc_fn, ocsfs_btree_free_fn, void *ctx);

int ocsfs_btree_search(struct ocsfs_btree *bt, u64 key, u64 *out_value);
int ocsfs_btree_insert(struct ocsfs_btree *bt, u64 key, u64 value);
int ocsfs_btree_delete(struct ocsfs_btree *bt, u64 key);

typedef int (*ocsfs_btree_scan_fn)(u64 key, u64 value, void *ctx);
int ocsfs_btree_range_scan(struct ocsfs_btree *bt, u64 start, u64 end,
			   ocsfs_btree_scan_fn cb, void *ctx);

static inline u64 ocsfs_btree_count(const struct ocsfs_btree *bt)
{
	return bt->entry_count;
}

/* internal — used by btree_mod.c */
int  ocsfs_btree_find_leaf(struct ocsfs_btree *bt, u64 key, void *buf,
			   u64 *path, int *path_len, int max_path);
void ocsfs_btree_node_update_csum(void *buf, u32 block_size);
void ocsfs_btree_init_leaf(void *buf, u32 bsz, u64 block_num);
void ocsfs_btree_init_internal(void *buf, u32 bsz, u64 block_num, u16 level);

#endif /* OCSFS_BTREE_H */
