/*
 * OCSFS — On-Disk B+ Tree
 *
 * Generic B+ tree implementation used for:
 *   - Directory entries (key: XXH3-64 name hash)
 *   - Extent overflow (key: logical block number)
 *   - Refcount table (key: physical block number, future)
 *
 * Each node occupies one filesystem block (4 KB default).
 * Internal nodes hold keys + child block pointers.
 * Leaf nodes hold keys + values, linked for sequential scan.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef OCSFS_BTREE_H
#define OCSFS_BTREE_H

#include <stdint.h>
#include <stddef.h>

/* B+ tree node magic numbers */
#define OCSFS_BTREE_INTERNAL_MAGIC  0x42544E49  /* 'BTNI' */
#define OCSFS_BTREE_LEAF_MAGIC      0x42544E4C  /* 'BTNL' */

/* B+ tree node flags */
#define OCSFS_BTREE_NODE_ROOT       0x01
#define OCSFS_BTREE_NODE_LEAF       0x02

/*
 * On-disk B+ tree node header — at the start of every node block.
 * The rest of the block is filled with key/value or key/pointer pairs.
 */
struct ocsfs_btree_node_hdr {
    uint32_t    bn_magic;           /* INTERNAL or LEAF magic */
    uint16_t    bn_flags;
    uint16_t    bn_count;           /* number of keys in this node */
    uint16_t    bn_level;           /* 0 = leaf, >0 = internal */
    uint16_t    bn_reserved;
    uint32_t    bn_block_size;      /* block size (for portability) */
    uint64_t    bn_block_num;       /* this node's block number */
    uint64_t    bn_parent;          /* parent block (0 if root) */
    uint64_t    bn_left_sibling;    /* leaf: prev leaf, internal: 0 */
    uint64_t    bn_right_sibling;   /* leaf: next leaf, internal: 0 */
    uint32_t    bn_checksum;        /* CRC32C of entire block */
    uint32_t    bn_padding;
} __attribute__((packed));

#define OCSFS_BTREE_HDR_SIZE    sizeof(struct ocsfs_btree_node_hdr)

/*
 * B+ tree key-value pair (for leaves).
 * Key is a 64-bit integer (hash or block number).
 * Value is a 64-bit integer (inode number, physical block, etc.)
 */
struct ocsfs_btree_entry {
    uint64_t    key;
    uint64_t    value;
} __attribute__((packed));

/*
 * B+ tree internal pointer.
 * key[i] is the minimum key in child[i+1].
 * child[0] has keys < key[0].
 */
struct ocsfs_btree_ptr {
    uint64_t    key;
    uint64_t    child;  /* block number of child node */
} __attribute__((packed));

/*
 * Calculate order (max entries) for a given block size.
 * Leaf: (block_size - hdr) / sizeof(entry)
 * Internal: (block_size - hdr - 8) / sizeof(ptr)
 *   (internal has one extra child pointer beyond the key count)
 */
static inline uint32_t ocsfs_btree_leaf_order(uint32_t block_size) {
    return (block_size - OCSFS_BTREE_HDR_SIZE) / sizeof(struct ocsfs_btree_entry);
}

static inline uint32_t ocsfs_btree_internal_order(uint32_t block_size) {
    /* Internal node: N keys + (N+1) child pointers
     * Storage: first_child (8 bytes) + N * (key 8 + child 8)
     * Available space = block_size - header
     */
    return (block_size - OCSFS_BTREE_HDR_SIZE - sizeof(uint64_t)) /
           sizeof(struct ocsfs_btree_ptr);
}

/* ─── In-memory B+ tree context ─────────────────────────────── */

/*
 * Block allocator callback — the B+ tree needs to allocate
 * new blocks for splits and free blocks on merge.
 */
typedef int (*ocsfs_btree_alloc_fn)(void *ctx, uint64_t *out_block);
typedef int (*ocsfs_btree_free_fn)(void *ctx, uint64_t block);

/*
 * Block I/O callbacks — read/write a single block.
 */
typedef int (*ocsfs_btree_read_fn)(void *ctx, uint64_t block, void *buf, uint32_t size);
typedef int (*ocsfs_btree_write_fn)(void *ctx, uint64_t block, const void *buf, uint32_t size);

struct ocsfs_btree {
    uint64_t    root_block;     /* block number of root node */
    uint32_t    block_size;
    uint32_t    leaf_order;     /* max entries per leaf */
    uint32_t    internal_order; /* max keys per internal */
    uint32_t    height;         /* tree height (1 = root is leaf) */
    uint64_t    entry_count;    /* total entries in tree */

    /* I/O callbacks */
    ocsfs_btree_read_fn   read_block;
    ocsfs_btree_write_fn  write_block;
    ocsfs_btree_alloc_fn  alloc_block;
    ocsfs_btree_free_fn   free_block;
    void *io_ctx;
};

/* ─── API ───────────────────────────────────────────────────── */

/*
 * Create a new empty B+ tree.
 * Allocates a root leaf node.
 */
int ocsfs_btree_create(struct ocsfs_btree *bt, uint32_t block_size,
                        ocsfs_btree_read_fn read_fn,
                        ocsfs_btree_write_fn write_fn,
                        ocsfs_btree_alloc_fn alloc_fn,
                        ocsfs_btree_free_fn free_fn,
                        void *io_ctx);

/*
 * Open an existing B+ tree from its root block.
 */
int ocsfs_btree_open(struct ocsfs_btree *bt, uint64_t root_block,
                      uint32_t block_size,
                      ocsfs_btree_read_fn read_fn,
                      ocsfs_btree_write_fn write_fn,
                      ocsfs_btree_alloc_fn alloc_fn,
                      ocsfs_btree_free_fn free_fn,
                      void *io_ctx);

/*
 * Search for a key. Returns 0 if found, -ENOENT if not.
 * If found, *out_value is set to the value.
 */
int ocsfs_btree_search(struct ocsfs_btree *bt, uint64_t key, uint64_t *out_value);

/*
 * Insert a key-value pair. Returns 0 on success.
 * If the key already exists, the value is updated.
 */
int ocsfs_btree_insert(struct ocsfs_btree *bt, uint64_t key, uint64_t value);

/*
 * Delete a key. Returns 0 on success, -ENOENT if not found.
 */
int ocsfs_btree_delete(struct ocsfs_btree *bt, uint64_t key);

/*
 * Range scan callback. Called for each entry in [start_key, end_key].
 * Return 0 to continue, non-zero to stop.
 */
typedef int (*ocsfs_btree_scan_fn)(uint64_t key, uint64_t value, void *ctx);

/*
 * Scan entries in range [start_key, end_key].
 * Calls callback for each entry. Returns number of entries scanned.
 */
int ocsfs_btree_range_scan(struct ocsfs_btree *bt,
                            uint64_t start_key, uint64_t end_key,
                            ocsfs_btree_scan_fn callback, void *ctx);

/*
 * Get the number of entries in the tree.
 */
static inline uint64_t ocsfs_btree_count(const struct ocsfs_btree *bt) {
    return bt->entry_count;
}

#endif /* OCSFS_BTREE_H */
