/*
 * OCSFS — On-Disk B+ Tree Implementation
 *
 * Generic B+ tree for directory entries, extent overflow, and refcount tables.
 * Each node is a single filesystem block. Leaves are linked for range scans.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "ocsfs.h"
#include "ocsfs_btree.h"

/* ─── Node I/O helpers ──────────────────────────────────────── */

static int read_node(struct ocsfs_btree *bt, uint64_t block, void *buf)
{
    return bt->read_block(bt->io_ctx, block, buf, bt->block_size);
}

static int write_node(struct ocsfs_btree *bt, uint64_t block, const void *buf)
{
    return bt->write_block(bt->io_ctx, block, buf, bt->block_size);
}

static int alloc_node(struct ocsfs_btree *bt, uint64_t *out)
{
    return bt->alloc_block(bt->io_ctx, out);
}

static int free_node(struct ocsfs_btree *bt, uint64_t block)
{
    return bt->free_block(bt->io_ctx, block);
}

/* ─── Node accessor helpers ─────────────────────────────────── */

static struct ocsfs_btree_node_hdr *node_hdr(void *buf)
{
    return (struct ocsfs_btree_node_hdr *)buf;
}

static int node_is_leaf(void *buf)
{
    return node_hdr(buf)->bn_level == 0;
}

/* Leaf entries start after the header */
static struct ocsfs_btree_entry *leaf_entries(void *buf)
{
    return (struct ocsfs_btree_entry *)((uint8_t *)buf + OCSFS_BTREE_HDR_SIZE);
}

/* Internal node: first child pointer, then key/child pairs */
static uint64_t *internal_first_child(void *buf)
{
    return (uint64_t *)((uint8_t *)buf + OCSFS_BTREE_HDR_SIZE);
}

static struct ocsfs_btree_ptr *internal_ptrs(void *buf)
{
    return (struct ocsfs_btree_ptr *)((uint8_t *)buf + OCSFS_BTREE_HDR_SIZE +
                                      sizeof(uint64_t));
}

static void node_update_checksum(void *buf, uint32_t block_size)
{
    struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);
    hdr->bn_checksum = 0;
    hdr->bn_checksum = ocsfs_crc32c(0, buf, block_size);
}

/* ─── Node initialization ──────────────────────────────────── */

static void init_leaf_node(void *buf, uint32_t block_size, uint64_t block_num)
{
    memset(buf, 0, block_size);
    struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);
    hdr->bn_magic = OCSFS_BTREE_LEAF_MAGIC;
    hdr->bn_flags = OCSFS_BTREE_NODE_LEAF;
    hdr->bn_count = 0;
    hdr->bn_level = 0;
    hdr->bn_block_size = block_size;
    hdr->bn_block_num = block_num;
    hdr->bn_parent = 0;
    hdr->bn_left_sibling = 0;
    hdr->bn_right_sibling = 0;
}

static void init_internal_node(void *buf, uint32_t block_size,
                                uint64_t block_num, uint16_t level)
{
    memset(buf, 0, block_size);
    struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);
    hdr->bn_magic = OCSFS_BTREE_INTERNAL_MAGIC;
    hdr->bn_flags = 0;
    hdr->bn_count = 0;
    hdr->bn_level = level;
    hdr->bn_block_size = block_size;
    hdr->bn_block_num = block_num;
    hdr->bn_parent = 0;
}

/* ─── B+ Tree Create / Open ─────────────────────────────────── */

int ocsfs_btree_create(struct ocsfs_btree *bt, uint32_t block_size,
                        ocsfs_btree_read_fn read_fn,
                        ocsfs_btree_write_fn write_fn,
                        ocsfs_btree_alloc_fn alloc_fn,
                        ocsfs_btree_free_fn free_fn,
                        void *io_ctx)
{
    memset(bt, 0, sizeof(*bt));
    bt->block_size = block_size;
    bt->leaf_order = ocsfs_btree_leaf_order(block_size);
    bt->internal_order = ocsfs_btree_internal_order(block_size);
    bt->read_block = read_fn;
    bt->write_block = write_fn;
    bt->alloc_block = alloc_fn;
    bt->free_block = free_fn;
    bt->io_ctx = io_ctx;

    /* Allocate root leaf */
    uint64_t root;
    int ret = alloc_node(bt, &root);
    if (ret < 0) return ret;

    void *buf = calloc(1, block_size);
    if (!buf) return -ENOMEM;

    init_leaf_node(buf, block_size, root);
    node_hdr(buf)->bn_flags |= OCSFS_BTREE_NODE_ROOT;
    node_update_checksum(buf, block_size);

    ret = write_node(bt, root, buf);
    free(buf);
    if (ret < 0) return ret;

    bt->root_block = root;
    bt->height = 1;
    bt->entry_count = 0;

    return 0;
}

int ocsfs_btree_open(struct ocsfs_btree *bt, uint64_t root_block,
                      uint32_t block_size,
                      ocsfs_btree_read_fn read_fn,
                      ocsfs_btree_write_fn write_fn,
                      ocsfs_btree_alloc_fn alloc_fn,
                      ocsfs_btree_free_fn free_fn,
                      void *io_ctx)
{
    memset(bt, 0, sizeof(*bt));
    bt->root_block = root_block;
    bt->block_size = block_size;
    bt->leaf_order = ocsfs_btree_leaf_order(block_size);
    bt->internal_order = ocsfs_btree_internal_order(block_size);
    bt->read_block = read_fn;
    bt->write_block = write_fn;
    bt->alloc_block = alloc_fn;
    bt->free_block = free_fn;
    bt->io_ctx = io_ctx;

    /* Read root to determine height */
    void *buf = calloc(1, block_size);
    if (!buf) return -ENOMEM;

    int ret = read_node(bt, root_block, buf);
    if (ret < 0) { free(buf); return ret; }

    bt->height = node_hdr(buf)->bn_level + 1;

    /* Count entries by scanning leaves */
    bt->entry_count = 0;
    /* Walk to leftmost leaf */
    void *cur = calloc(1, block_size);
    if (!cur) { free(buf); return -ENOMEM; }
    memcpy(cur, buf, block_size);

    while (!node_is_leaf(cur)) {
        uint64_t child = *internal_first_child(cur);
        ret = read_node(bt, child, cur);
        if (ret < 0) { free(buf); free(cur); return ret; }
    }

    /* Scan all leaves via right sibling links */
    while (1) {
        bt->entry_count += node_hdr(cur)->bn_count;
        uint64_t right = node_hdr(cur)->bn_right_sibling;
        if (right == 0) break;
        ret = read_node(bt, right, cur);
        if (ret < 0) break;
    }

    free(buf);
    free(cur);
    return 0;
}

/* ─── Search ────────────────────────────────────────────────── */

/*
 * Find the leaf node containing (or that would contain) the given key.
 * Returns the leaf in buf. path/path_len track the traversal for insert.
 */
static int find_leaf(struct ocsfs_btree *bt, uint64_t key, void *buf,
                      uint64_t *path, int *path_len, int max_path)
{
    int ret = read_node(bt, bt->root_block, buf);
    if (ret < 0) return ret;

    if (path) {
        path[0] = bt->root_block;
        *path_len = 1;
    }

    while (!node_is_leaf(buf)) {
        struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);
        struct ocsfs_btree_ptr *ptrs = internal_ptrs(buf);
        uint64_t *first_child = internal_first_child(buf);

        /* Binary search for the child to descend into */
        uint64_t next_block = *first_child; /* default: leftmost child */
        for (int i = 0; i < hdr->bn_count; i++) {
            if (key >= ptrs[i].key) {
                next_block = ptrs[i].child;
            } else {
                break;
            }
        }

        ret = read_node(bt, next_block, buf);
        if (ret < 0) return ret;

        if (path && *path_len < max_path) {
            path[*path_len] = next_block;
            (*path_len)++;
        }
    }

    return 0;
}

int ocsfs_btree_search(struct ocsfs_btree *bt, uint64_t key, uint64_t *out_value)
{
    void *buf = calloc(1, bt->block_size);
    if (!buf) return -ENOMEM;

    int ret = find_leaf(bt, key, buf, NULL, NULL, 0);
    if (ret < 0) { free(buf); return ret; }

    struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);
    struct ocsfs_btree_entry *entries = leaf_entries(buf);

    /* Binary search in leaf */
    int lo = 0, hi = hdr->bn_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (entries[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (lo < hdr->bn_count && entries[lo].key == key) {
        if (out_value) *out_value = entries[lo].value;
        free(buf);
        return 0;
    }

    free(buf);
    return -ENOENT;
}

/* ─── Insert ────────────────────────────────────────────────── */

/*
 * Insert a key/child pointer into an internal node at position pos.
 */
static void internal_insert_at(void *buf, int pos, uint64_t key, uint64_t child)
{
    struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);
    struct ocsfs_btree_ptr *ptrs = internal_ptrs(buf);

    /* Shift entries right */
    if (pos < hdr->bn_count) {
        memmove(&ptrs[pos + 1], &ptrs[pos],
                (hdr->bn_count - pos) * sizeof(struct ocsfs_btree_ptr));
    }
    ptrs[pos].key = key;
    ptrs[pos].child = child;
    hdr->bn_count++;
}

int ocsfs_btree_insert(struct ocsfs_btree *bt, uint64_t key, uint64_t value)
{
    void *buf = calloc(1, bt->block_size);
    if (!buf) return -ENOMEM;

    /* Track path from root to leaf for split propagation */
    uint64_t path[64];
    int path_len = 0;

    int ret = find_leaf(bt, key, buf, path, &path_len, 64);
    if (ret < 0) { free(buf); return ret; }

    struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);
    struct ocsfs_btree_entry *entries = leaf_entries(buf);

    /* Find insertion position (binary search) */
    int lo = 0, hi = hdr->bn_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (entries[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }

    /* Key already exists? Update value */
    if (lo < hdr->bn_count && entries[lo].key == key) {
        entries[lo].value = value;
        node_update_checksum(buf, bt->block_size);
        ret = write_node(bt, hdr->bn_block_num, buf);
        free(buf);
        return ret;
    }

    /* Check if leaf has room */
    if ((uint32_t)hdr->bn_count < bt->leaf_order) {
        /* Insert in place */
        if (lo < hdr->bn_count) {
            memmove(&entries[lo + 1], &entries[lo],
                    (hdr->bn_count - lo) * sizeof(struct ocsfs_btree_entry));
        }
        entries[lo].key = key;
        entries[lo].value = value;
        hdr->bn_count++;
        bt->entry_count++;

        node_update_checksum(buf, bt->block_size);
        ret = write_node(bt, hdr->bn_block_num, buf);
        free(buf);
        return ret;
    }

    /* Leaf is full — need to split.
     * Insert the entry first into a temporary expanded array, then split.
     */
    int total = hdr->bn_count + 1;
    struct ocsfs_btree_entry *tmp = calloc(total, sizeof(struct ocsfs_btree_entry));
    if (!tmp) { free(buf); return -ENOMEM; }

    /* Copy with insertion */
    memcpy(tmp, entries, lo * sizeof(struct ocsfs_btree_entry));
    tmp[lo].key = key;
    tmp[lo].value = value;
    memcpy(&tmp[lo + 1], &entries[lo],
           (hdr->bn_count - lo) * sizeof(struct ocsfs_btree_entry));

    /* Split point */
    int split = total / 2;

    /* Left half stays in current leaf */
    memcpy(entries, tmp, split * sizeof(struct ocsfs_btree_entry));
    hdr->bn_count = split;

    /* Right half goes to new leaf */
    uint64_t new_block;
    uint64_t split_key;

    /* We need to create the new leaf and copy entries */
    void *new_buf = calloc(1, bt->block_size);
    if (!new_buf) { free(tmp); free(buf); return -ENOMEM; }

    ret = alloc_node(bt, &new_block);
    if (ret < 0) { free(new_buf); free(tmp); free(buf); return ret; }

    init_leaf_node(new_buf, bt->block_size, new_block);
    struct ocsfs_btree_node_hdr *new_hdr = node_hdr(new_buf);
    struct ocsfs_btree_entry *new_entries = leaf_entries(new_buf);

    int right_count = total - split;
    memcpy(new_entries, &tmp[split], right_count * sizeof(struct ocsfs_btree_entry));
    new_hdr->bn_count = right_count;
    new_hdr->bn_parent = hdr->bn_parent;
    split_key = new_entries[0].key;

    /* Update sibling links */
    new_hdr->bn_right_sibling = hdr->bn_right_sibling;
    new_hdr->bn_left_sibling = hdr->bn_block_num;
    hdr->bn_right_sibling = new_block;

    if (new_hdr->bn_right_sibling != 0) {
        void *right_buf = calloc(1, bt->block_size);
        if (right_buf) {
            if (read_node(bt, new_hdr->bn_right_sibling, right_buf) == 0) {
                node_hdr(right_buf)->bn_left_sibling = new_block;
                node_update_checksum(right_buf, bt->block_size);
                write_node(bt, new_hdr->bn_right_sibling, right_buf);
            }
            free(right_buf);
        }
    }

    node_update_checksum(buf, bt->block_size);
    write_node(bt, hdr->bn_block_num, buf);
    node_update_checksum(new_buf, bt->block_size);
    write_node(bt, new_block, new_buf);

    free(new_buf);
    free(tmp);
    bt->entry_count++;

    /* Propagate split up the tree */
    uint64_t promote_key = split_key;
    uint64_t promote_child = new_block;

    /* Walk up the path, inserting the promoted key */
    for (int level = path_len - 2; level >= 0; level--) {
        ret = read_node(bt, path[level], buf);
        if (ret < 0) { free(buf); return ret; }

        hdr = node_hdr(buf);
        struct ocsfs_btree_ptr *ptrs = internal_ptrs(buf);

        /* Find insertion position */
        int pos = 0;
        for (pos = 0; pos < hdr->bn_count; pos++) {
            if (ptrs[pos].key > promote_key)
                break;
        }

        if ((uint32_t)hdr->bn_count < bt->internal_order) {
            /* Room in this internal node */
            internal_insert_at(buf, pos, promote_key, promote_child);
            node_update_checksum(buf, bt->block_size);
            write_node(bt, hdr->bn_block_num, buf);
            free(buf);
            return 0;
        }

        /* Internal node is full — split it */
        /* Insert into temp expanded array */
        int old_count = hdr->bn_count;
        struct ocsfs_btree_ptr *tmp_ptrs = calloc(old_count + 1,
                                                    sizeof(struct ocsfs_btree_ptr));
        if (!tmp_ptrs) { free(buf); return -ENOMEM; }

        memcpy(tmp_ptrs, ptrs, pos * sizeof(struct ocsfs_btree_ptr));
        tmp_ptrs[pos].key = promote_key;
        tmp_ptrs[pos].child = promote_child;
        memcpy(&tmp_ptrs[pos + 1], &ptrs[pos],
               (old_count - pos) * sizeof(struct ocsfs_btree_ptr));

        int new_total = old_count + 1;
        int isplit = new_total / 2;

        /* Promoted key from internal split */
        uint64_t new_promote_key = tmp_ptrs[isplit].key;

        /* Left stays */
        memcpy(ptrs, tmp_ptrs, isplit * sizeof(struct ocsfs_btree_ptr));
        hdr->bn_count = isplit;

        /* Right goes to new internal node */
        uint64_t new_internal;
        ret = alloc_node(bt, &new_internal);
        if (ret < 0) { free(tmp_ptrs); free(buf); return ret; }

        void *ni_buf = calloc(1, bt->block_size);
        if (!ni_buf) { free(tmp_ptrs); free(buf); return -ENOMEM; }

        init_internal_node(ni_buf, bt->block_size, new_internal, hdr->bn_level);
        struct ocsfs_btree_node_hdr *ni_hdr = node_hdr(ni_buf);
        uint64_t *ni_first = internal_first_child(ni_buf);
        struct ocsfs_btree_ptr *ni_ptrs = internal_ptrs(ni_buf);

        *ni_first = tmp_ptrs[isplit].child;
        int ni_count = new_total - isplit - 1;
        if (ni_count > 0) {
            memcpy(ni_ptrs, &tmp_ptrs[isplit + 1],
                   ni_count * sizeof(struct ocsfs_btree_ptr));
        }
        ni_hdr->bn_count = ni_count;
        ni_hdr->bn_parent = hdr->bn_parent;

        node_update_checksum(buf, bt->block_size);
        write_node(bt, hdr->bn_block_num, buf);
        node_update_checksum(ni_buf, bt->block_size);
        write_node(bt, new_internal, ni_buf);

        free(ni_buf);
        free(tmp_ptrs);

        promote_key = new_promote_key;
        promote_child = new_internal;
    }

    /* If we get here, we need a new root */
    uint64_t new_root;
    ret = alloc_node(bt, &new_root);
    if (ret < 0) { free(buf); return ret; }

    init_internal_node(buf, bt->block_size, new_root, bt->height);
    node_hdr(buf)->bn_flags |= OCSFS_BTREE_NODE_ROOT;
    *internal_first_child(buf) = bt->root_block;
    internal_ptrs(buf)[0].key = promote_key;
    internal_ptrs(buf)[0].child = promote_child;
    node_hdr(buf)->bn_count = 1;

    node_update_checksum(buf, bt->block_size);
    ret = write_node(bt, new_root, buf);
    if (ret < 0) { free(buf); return ret; }

    /* Update old root's flag */
    ret = read_node(bt, bt->root_block, buf);
    if (ret == 0) {
        node_hdr(buf)->bn_flags &= ~OCSFS_BTREE_NODE_ROOT;
        node_hdr(buf)->bn_parent = new_root;
        node_update_checksum(buf, bt->block_size);
        write_node(bt, bt->root_block, buf);
    }

    /* Update new right node's parent */
    ret = read_node(bt, promote_child, buf);
    if (ret == 0) {
        node_hdr(buf)->bn_parent = new_root;
        node_update_checksum(buf, bt->block_size);
        write_node(bt, promote_child, buf);
    }

    bt->root_block = new_root;
    bt->height++;

    free(buf);
    return 0;
}

/* ─── Delete ────────────────────────────────────────────────── */

int ocsfs_btree_delete(struct ocsfs_btree *bt, uint64_t key)
{
    void *buf = calloc(1, bt->block_size);
    if (!buf) return -ENOMEM;

    int ret = find_leaf(bt, key, buf, NULL, NULL, 0);
    if (ret < 0) { free(buf); return ret; }

    struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);
    struct ocsfs_btree_entry *entries = leaf_entries(buf);

    /* Find the key */
    int lo = 0, hi = hdr->bn_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (entries[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (lo >= hdr->bn_count || entries[lo].key != key) {
        free(buf);
        return -ENOENT;
    }

    /* Remove entry by shifting */
    if (lo < hdr->bn_count - 1) {
        memmove(&entries[lo], &entries[lo + 1],
                (hdr->bn_count - lo - 1) * sizeof(struct ocsfs_btree_entry));
    }
    hdr->bn_count--;
    bt->entry_count--;

    /* If the leaf is empty and not the root, free it */
    if (hdr->bn_count == 0 && !(hdr->bn_flags & OCSFS_BTREE_NODE_ROOT)) {
        /* Update sibling links */
        if (hdr->bn_left_sibling) {
            void *left = calloc(1, bt->block_size);
            if (left && read_node(bt, hdr->bn_left_sibling, left) == 0) {
                node_hdr(left)->bn_right_sibling = hdr->bn_right_sibling;
                node_update_checksum(left, bt->block_size);
                write_node(bt, hdr->bn_left_sibling, left);
            }
            free(left);
        }
        if (hdr->bn_right_sibling) {
            void *right = calloc(1, bt->block_size);
            if (right && read_node(bt, hdr->bn_right_sibling, right) == 0) {
                node_hdr(right)->bn_left_sibling = hdr->bn_left_sibling;
                node_update_checksum(right, bt->block_size);
                write_node(bt, hdr->bn_right_sibling, right);
            }
            free(right);
        }

        /* Remove from parent — simplified: just mark the parent entry as removed.
         * A full implementation would handle recursive merges, but for the
         * prototype we keep it simple and let internal nodes become sparse. */
        if (hdr->bn_parent) {
            void *parent = calloc(1, bt->block_size);
            if (parent && read_node(bt, hdr->bn_parent, parent) == 0) {
                struct ocsfs_btree_node_hdr *phdr = node_hdr(parent);
                uint64_t *pfirst = internal_first_child(parent);
                struct ocsfs_btree_ptr *pptrs = internal_ptrs(parent);

                if (*pfirst == hdr->bn_block_num) {
                    /* Leftmost child removed */
                    if (phdr->bn_count > 0) {
                        *pfirst = pptrs[0].child;
                        memmove(&pptrs[0], &pptrs[1],
                                (phdr->bn_count - 1) * sizeof(struct ocsfs_btree_ptr));
                        phdr->bn_count--;
                    }
                } else {
                    for (int i = 0; i < phdr->bn_count; i++) {
                        if (pptrs[i].child == hdr->bn_block_num) {
                            memmove(&pptrs[i], &pptrs[i + 1],
                                    (phdr->bn_count - i - 1) * sizeof(struct ocsfs_btree_ptr));
                            phdr->bn_count--;
                            break;
                        }
                    }
                }
                node_update_checksum(parent, bt->block_size);
                write_node(bt, hdr->bn_parent, parent);
            }
            free(parent);
        }

        free_node(bt, hdr->bn_block_num);
        free(buf);
        return 0;
    }

    node_update_checksum(buf, bt->block_size);
    ret = write_node(bt, hdr->bn_block_num, buf);
    free(buf);
    return ret;
}

/* ─── Range Scan ────────────────────────────────────────────── */

int ocsfs_btree_range_scan(struct ocsfs_btree *bt,
                            uint64_t start_key, uint64_t end_key,
                            ocsfs_btree_scan_fn callback, void *ctx)
{
    void *buf = calloc(1, bt->block_size);
    if (!buf) return -ENOMEM;

    int ret = find_leaf(bt, start_key, buf, NULL, NULL, 0);
    if (ret < 0) { free(buf); return ret; }

    int scanned = 0;

    while (1) {
        struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);
        struct ocsfs_btree_entry *entries = leaf_entries(buf);

        for (int i = 0; i < hdr->bn_count; i++) {
            if (entries[i].key > end_key) {
                free(buf);
                return scanned;
            }
            if (entries[i].key >= start_key) {
                ret = callback(entries[i].key, entries[i].value, ctx);
                scanned++;
                if (ret != 0) {
                    free(buf);
                    return scanned;
                }
            }
        }

        /* Follow right sibling link */
        uint64_t right = hdr->bn_right_sibling;
        if (right == 0) break;

        ret = read_node(bt, right, buf);
        if (ret < 0) break;
    }

    free(buf);
    return scanned;
}
