/*
 * OCSFS — Extent Manager
 *
 * Manages file extents: allocation, lookup, insertion, and removal.
 * Each file stores extents inline in the inode (up to OCSFS_INLINE_EXTENTS).
 * When inline capacity is exceeded, extents overflow to an on-disk B+ tree.
 *
 * This module is the performance-critical path for VM disk I/O:
 * every read/write must resolve a logical block to a physical block
 * through extent lookup.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "ocsfs.h"

/* ─── In-memory extent representation ───────────────────────── */

struct ocsfs_extent_entry {
    uint64_t    logical_start;   /* file-relative block */
    uint64_t    physical_start;  /* volume-absolute block */
    uint32_t    length;          /* blocks */
    uint16_t    flags;           /* OCSFS_EXT_* */
};

/*
 * In-memory extent map for a file. Keeps extents sorted by
 * logical_start for binary search lookup.
 */
struct ocsfs_extent_map {
    struct ocsfs_extent_entry *entries;
    uint32_t    count;           /* current number of extents */
    uint32_t    capacity;        /* allocated capacity */
    uint64_t    inode_num;       /* owning inode */
    int         dirty;           /* needs writeback to disk */
};

/* ─── Extent Map Lifecycle ──────────────────────────────────── */

struct ocsfs_extent_map *ocsfs_extent_map_create(uint64_t ino)
{
    struct ocsfs_extent_map *map = calloc(1, sizeof(*map));
    if (!map) return NULL;

    map->capacity = OCSFS_INLINE_EXTENTS; /* start at inline size */
    map->entries = calloc(map->capacity, sizeof(struct ocsfs_extent_entry));
    if (!map->entries) {
        free(map);
        return NULL;
    }

    map->inode_num = ino;
    return map;
}

void ocsfs_extent_map_destroy(struct ocsfs_extent_map *map)
{
    if (map) {
        free(map->entries);
        free(map);
    }
}

/*
 * Load extents from on-disk inode (inline extents).
 */
int ocsfs_extent_map_load_inline(struct ocsfs_extent_map *map,
                                  const struct ocsfs_inode *inode)
{
    const struct ocsfs_extent *ext_array =
        (const struct ocsfs_extent *)inode->i_inline_extents;

    map->count = 0;
    for (uint16_t i = 0; i < inode->i_extent_count && i < OCSFS_INLINE_EXTENTS; i++) {
        const struct ocsfs_extent *e = &ext_array[i];
        if (e->e_length == 0)
            continue;

        if (map->count >= map->capacity) {
            uint32_t new_cap = map->capacity * 2;
            struct ocsfs_extent_entry *new_ents =
                realloc(map->entries, new_cap * sizeof(struct ocsfs_extent_entry));
            if (!new_ents) return -ENOMEM;
            map->entries = new_ents;
            map->capacity = new_cap;
        }

        map->entries[map->count].logical_start = e->e_logical_block;
        map->entries[map->count].physical_start = e->e_physical_block;
        map->entries[map->count].length = e->e_length;
        map->entries[map->count].flags = e->e_flags;
        map->count++;
    }

    map->dirty = 0;
    return 0;
}

/*
 * Write extents back to on-disk inode (inline extents).
 * Returns -ENOSPC if too many extents for inline storage.
 */
int ocsfs_extent_map_store_inline(const struct ocsfs_extent_map *map,
                                   struct ocsfs_inode *inode)
{
    if (map->count > OCSFS_INLINE_EXTENTS)
        return -ENOSPC; /* need B+ tree overflow */

    struct ocsfs_extent *ext_array =
        (struct ocsfs_extent *)inode->i_inline_extents;

    memset(ext_array, 0, OCSFS_INLINE_EXTENTS * sizeof(struct ocsfs_extent));

    for (uint32_t i = 0; i < map->count; i++) {
        ext_array[i].e_logical_block = map->entries[i].logical_start;
        ext_array[i].e_physical_block = map->entries[i].physical_start;
        ext_array[i].e_length = map->entries[i].length;
        ext_array[i].e_flags = map->entries[i].flags;
        /* CRC16 per extent */
        ext_array[i].e_checksum = (uint16_t)ocsfs_crc32c(0, &ext_array[i],
                                    sizeof(struct ocsfs_extent) - sizeof(uint16_t));
    }

    inode->i_extent_count = map->count;
    return 0;
}

/* ─── Extent Lookup (Binary Search) ─────────────────────────── */

/*
 * Find the extent containing logical block 'block'.
 * Returns pointer to the entry, or NULL if the block is in a hole.
 *
 * This is the hottest path in the filesystem: every data I/O
 * calls this to resolve logical -> physical mapping.
 */
const struct ocsfs_extent_entry *
ocsfs_extent_lookup(const struct ocsfs_extent_map *map, uint64_t block)
{
    if (map->count == 0)
        return NULL;

    /* Binary search: find the extent where logical_start <= block */
    uint32_t lo = 0, hi = map->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (map->entries[mid].logical_start <= block)
            lo = mid + 1;
        else
            hi = mid;
    }

    /* lo-1 is the last extent with logical_start <= block */
    if (lo == 0)
        return NULL;

    const struct ocsfs_extent_entry *e = &map->entries[lo - 1];
    if (block < e->logical_start + e->length)
        return e;

    return NULL; /* block is in a hole between extents */
}

/*
 * Translate a logical block to a physical block.
 * Returns the physical block number, or UINT64_MAX for holes.
 */
uint64_t ocsfs_extent_map_logical_to_physical(const struct ocsfs_extent_map *map,
                                               uint64_t logical_block)
{
    const struct ocsfs_extent_entry *e = ocsfs_extent_lookup(map, logical_block);
    if (!e)
        return UINT64_MAX; /* hole */

    if (e->flags & OCSFS_EXT_UNWRITTEN)
        return UINT64_MAX; /* unwritten extent = reads as zeros */

    return e->physical_start + (logical_block - e->logical_start);
}

/* ─── Extent Insertion ──────────────────────────────────────── */

/*
 * Insert a new extent into the map, maintaining sorted order.
 * Attempts to merge with adjacent extents.
 *
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
int ocsfs_extent_map_insert(struct ocsfs_extent_map *map,
                             uint64_t logical_start, uint64_t physical_start,
                             uint32_t length, uint16_t flags)
{
    /* Try to merge with the previous extent */
    if (map->count > 0) {
        struct ocsfs_extent_entry *prev = NULL;

        /* Find insertion point */
        uint32_t insert_pos = 0;
        for (insert_pos = 0; insert_pos < map->count; insert_pos++) {
            if (map->entries[insert_pos].logical_start >= logical_start)
                break;
        }

        /* Check merge with predecessor */
        if (insert_pos > 0) {
            prev = &map->entries[insert_pos - 1];
            if (prev->logical_start + prev->length == logical_start &&
                prev->physical_start + prev->length == physical_start &&
                prev->flags == flags) {
                /* Merge! */
                prev->length += length;
                map->dirty = 1;

                /* Also try merging with successor */
                if (insert_pos < map->count) {
                    struct ocsfs_extent_entry *next = &map->entries[insert_pos];
                    if (prev->logical_start + prev->length == next->logical_start &&
                        prev->physical_start + prev->length == next->physical_start &&
                        prev->flags == next->flags) {
                        prev->length += next->length;
                        /* Remove successor */
                        memmove(&map->entries[insert_pos],
                                &map->entries[insert_pos + 1],
                                (map->count - insert_pos - 1) * sizeof(*next));
                        map->count--;
                    }
                }
                return 0;
            }
        }

        /* Check merge with successor */
        if (insert_pos < map->count) {
            struct ocsfs_extent_entry *next = &map->entries[insert_pos];
            if (logical_start + length == next->logical_start &&
                physical_start + length == next->physical_start &&
                flags == next->flags) {
                next->logical_start = logical_start;
                next->physical_start = physical_start;
                next->length += length;
                map->dirty = 1;
                return 0;
            }
        }

        /* No merge possible — insert at position */
        if (map->count >= map->capacity) {
            uint32_t new_cap = map->capacity * 2;
            if (new_cap < 64) new_cap = 64;
            struct ocsfs_extent_entry *new_ents =
                realloc(map->entries, new_cap * sizeof(*new_ents));
            if (!new_ents) return -ENOMEM;
            map->entries = new_ents;
            map->capacity = new_cap;
        }

        /* Shift entries to make room */
        if (insert_pos < map->count) {
            memmove(&map->entries[insert_pos + 1],
                    &map->entries[insert_pos],
                    (map->count - insert_pos) * sizeof(struct ocsfs_extent_entry));
        }

        map->entries[insert_pos].logical_start = logical_start;
        map->entries[insert_pos].physical_start = physical_start;
        map->entries[insert_pos].length = length;
        map->entries[insert_pos].flags = flags;
        map->count++;
        map->dirty = 1;
        return 0;
    }

    /* Empty map — simple insert */
    if (map->count >= map->capacity) {
        uint32_t new_cap = map->capacity * 2;
        if (new_cap < 16) new_cap = 16;
        struct ocsfs_extent_entry *new_ents =
            realloc(map->entries, new_cap * sizeof(*new_ents));
        if (!new_ents) return -ENOMEM;
        map->entries = new_ents;
        map->capacity = new_cap;
    }

    map->entries[0].logical_start = logical_start;
    map->entries[0].physical_start = physical_start;
    map->entries[0].length = length;
    map->entries[0].flags = flags;
    map->count = 1;
    map->dirty = 1;
    return 0;
}

/* ─── Extent Removal (for truncate/punch_hole) ──────────────── */

/*
 * Remove all extents covering [logical_start, logical_start + length).
 * Handles partial overlap at both ends.
 *
 * Returns the number of blocks freed, or negative on error.
 * The caller is responsible for returning freed blocks to the AG bitmap.
 */
int64_t ocsfs_extent_map_remove_range(struct ocsfs_extent_map *map,
                                       uint64_t logical_start, uint64_t length)
{
    uint64_t end = logical_start + length;
    int64_t freed = 0;
    uint32_t i = 0;

    while (i < map->count) {
        struct ocsfs_extent_entry *e = &map->entries[i];
        uint64_t e_end = e->logical_start + e->length;

        /* No overlap */
        if (e_end <= logical_start || e->logical_start >= end) {
            i++;
            continue;
        }

        /* Fully contained — remove entire extent */
        if (e->logical_start >= logical_start && e_end <= end) {
            freed += e->length;
            memmove(&map->entries[i], &map->entries[i + 1],
                    (map->count - i - 1) * sizeof(struct ocsfs_extent_entry));
            map->count--;
            map->dirty = 1;
            continue; /* don't increment i */
        }

        /* Partial overlap at the start of extent */
        if (logical_start <= e->logical_start && end < e_end) {
            uint64_t trim = end - e->logical_start;
            freed += trim;
            e->physical_start += trim;
            e->logical_start += trim;
            e->length -= trim;
            map->dirty = 1;
            i++;
            continue;
        }

        /* Partial overlap at the end of extent */
        if (logical_start > e->logical_start && end >= e_end) {
            uint64_t new_len = logical_start - e->logical_start;
            freed += e->length - new_len;
            e->length = new_len;
            map->dirty = 1;
            i++;
            continue;
        }

        /* Hole punch in the middle — split extent */
        if (logical_start > e->logical_start && end < e_end) {
            uint64_t right_logical = end;
            uint64_t right_physical = e->physical_start + (end - e->logical_start);
            uint32_t right_len = e_end - end;
            uint16_t right_flags = e->flags;

            /* Trim left part */
            uint64_t orig_len = e->length;
            e->length = logical_start - e->logical_start;
            freed += orig_len - e->length - right_len;

            /* Insert right part */
            ocsfs_extent_map_insert(map, right_logical, right_physical,
                                     right_len, right_flags);
            map->dirty = 1;
            break; /* split can only happen once */
        }

        i++;
    }

    return freed;
}

/* ─── Statistics ────────────────────────────────────────────── */

/*
 * Calculate total allocated blocks across all extents.
 */
uint64_t ocsfs_extent_map_total_blocks(const struct ocsfs_extent_map *map)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < map->count; i++) {
        if (!(map->entries[i].flags & OCSFS_EXT_UNWRITTEN))
            total += map->entries[i].length;
    }
    return total;
}

/*
 * Calculate total blocks including unwritten (thin provisioned) extents.
 */
uint64_t ocsfs_extent_map_allocated_blocks(const struct ocsfs_extent_map *map)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < map->count; i++) {
        total += map->entries[i].length;
    }
    return total;
}

/*
 * Count number of extents (fragmentation indicator).
 */
uint32_t ocsfs_extent_map_count(const struct ocsfs_extent_map *map)
{
    return map->count;
}

/*
 * Check if extent map needs B+ tree overflow (too many for inline).
 */
int ocsfs_extent_map_needs_btree(const struct ocsfs_extent_map *map)
{
    return map->count > OCSFS_INLINE_EXTENTS;
}

/* ─── Debug ─────────────────────────────────────────────────── */

void ocsfs_extent_map_dump(const struct ocsfs_extent_map *map, FILE *out)
{
    fprintf(out, "Extent map for inode %lu (%u extents):\n",
            (unsigned long)map->inode_num, map->count);
    fprintf(out, "  %-16s  %-16s  %-10s  %s\n",
            "Logical", "Physical", "Length", "Flags");
    for (uint32_t i = 0; i < map->count; i++) {
        const struct ocsfs_extent_entry *e = &map->entries[i];
        fprintf(out, "  %-16lu  %-16lu  %-10u  %s%s%s\n",
                (unsigned long)e->logical_start,
                (unsigned long)e->physical_start,
                e->length,
                (e->flags & OCSFS_EXT_UNWRITTEN) ? "UNWRITTEN " : "",
                (e->flags & OCSFS_EXT_COMPRESSED) ? "COMPRESSED " : "",
                (e->flags & OCSFS_EXT_SHARED) ? "SHARED" : "");
    }
}
