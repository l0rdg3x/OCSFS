/*
 * OCSFS — Block Bitmap Allocator
 *
 * Manages per-AG block allocation bitmaps. Each bit represents
 * one filesystem block: 0 = free, 1 = allocated.
 *
 * The allocator is designed for extent-based allocation: it tries
 * to find contiguous free regions of the requested size, falling
 * back to smaller extents if necessary.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ocsfs.h"

/* ─── Bitmap Operations ─────────────────────────────────────── */

/*
 * Test if bit 'bit' is set in bitmap.
 */
static inline int bitmap_test(const uint8_t *bitmap, uint64_t bit)
{
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

/*
 * Set bit 'bit' in bitmap.
 */
static inline void bitmap_set(uint8_t *bitmap, uint64_t bit)
{
    bitmap[bit / 8] |= (1 << (bit % 8));
}

/*
 * Clear bit 'bit' in bitmap.
 */
static inline void bitmap_clear(uint8_t *bitmap, uint64_t bit)
{
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

/*
 * Set a range of bits [start, start+count).
 */
void ocsfs_bitmap_set_range(uint8_t *bitmap, uint64_t start, uint64_t count)
{
    for (uint64_t i = 0; i < count; i++) {
        bitmap_set(bitmap, start + i);
    }
}

/*
 * Clear a range of bits [start, start+count).
 */
void ocsfs_bitmap_clear_range(uint8_t *bitmap, uint64_t start, uint64_t count)
{
    for (uint64_t i = 0; i < count; i++) {
        bitmap_clear(bitmap, start + i);
    }
}

/*
 * Count free (zero) bits in bitmap of total_bits size.
 */
uint64_t ocsfs_bitmap_count_free(const uint8_t *bitmap, uint64_t total_bits)
{
    uint64_t free_count = 0;

    /* Fast path: count by bytes */
    uint64_t full_bytes = total_bits / 8;
    for (uint64_t i = 0; i < full_bytes; i++) {
        /* __builtin_popcount counts set bits; we want free bits */
        free_count += 8 - __builtin_popcount(bitmap[i]);
    }

    /* Remaining bits */
    for (uint64_t bit = full_bytes * 8; bit < total_bits; bit++) {
        if (!bitmap_test(bitmap, bit))
            free_count++;
    }

    return free_count;
}

/* ─── Extent Allocator ──────────────────────────────────────── */

/*
 * Find a contiguous free region of at least 'min_blocks' blocks,
 * preferably 'goal_blocks' blocks, starting the search at 'hint'.
 *
 * Returns the starting bit position, or UINT64_MAX if no space.
 * *out_length is set to the actual length found.
 *
 * Strategy:
 *   1. Search forward from hint for goal_blocks contiguous free bits.
 *   2. If not found, accept anything >= min_blocks.
 *   3. Wrap around if needed.
 *
 * This is the core hot path for VM disk allocation.
 */
uint64_t ocsfs_bitmap_find_free_extent(const uint8_t *bitmap, uint64_t total_bits,
                                        uint64_t hint, uint64_t goal_blocks,
                                        uint64_t min_blocks, uint64_t *out_length)
{
    if (hint >= total_bits)
        hint = 0;

    uint64_t best_start = UINT64_MAX;
    uint64_t best_len = 0;

    /* Search starting from hint, then wrap around */
    uint64_t search_start = hint;
    int wrapped = 0;

    while (1) {
        uint64_t pos = search_start;

        /* Skip allocated bits */
        while (pos < total_bits && bitmap_test(bitmap, pos))
            pos++;

        if (pos >= total_bits) {
            if (wrapped)
                break;
            wrapped = 1;
            search_start = 0;
            continue;
        }

        /* Count contiguous free bits */
        uint64_t run_start = pos;
        uint64_t run_len = 0;
        while (pos < total_bits && !bitmap_test(bitmap, pos) &&
               run_len < goal_blocks) {
            pos++;
            run_len++;
        }

        /* Found exact goal? Return immediately */
        if (run_len >= goal_blocks) {
            *out_length = goal_blocks;
            return run_start;
        }

        /* Track best candidate */
        if (run_len > best_len && run_len >= min_blocks) {
            best_start = run_start;
            best_len = run_len;
        }

        search_start = pos;
        if (wrapped && search_start >= hint)
            break;
    }

    if (best_start != UINT64_MAX) {
        *out_length = best_len;
    }
    return best_start;
}

/*
 * Allocate 'count' blocks from the bitmap.
 * Sets the allocated bits and returns the starting position.
 * Returns UINT64_MAX on failure.
 */
uint64_t ocsfs_bitmap_alloc(uint8_t *bitmap, uint64_t total_bits,
                             uint64_t hint, uint64_t count,
                             uint64_t *out_length)
{
    uint64_t actual_len = 0;
    uint64_t start = ocsfs_bitmap_find_free_extent(bitmap, total_bits,
                                                    hint, count, 1, &actual_len);
    if (start == UINT64_MAX)
        return UINT64_MAX;

    /* Allocate only what we found (may be less than requested) */
    ocsfs_bitmap_set_range(bitmap, start, actual_len);
    if (out_length)
        *out_length = actual_len;
    return start;
}

/*
 * Free 'count' blocks starting at 'start'.
 */
void ocsfs_bitmap_free(uint8_t *bitmap, uint64_t start, uint64_t count)
{
    ocsfs_bitmap_clear_range(bitmap, start, count);
}

/* ─── AG Allocator Context ──────────────────────────────────── */

/*
 * In-memory AG allocation state. One per mounted AG.
 * In the kernel module, this will be protected by the AG lock.
 */
struct ocsfs_ag_alloc_ctx {
    uint32_t    ag_number;
    uint64_t    total_blocks;
    uint64_t    free_blocks;
    uint8_t    *bitmap;         /* in-memory bitmap */
    size_t      bitmap_size;    /* bitmap size in bytes */
    uint64_t    last_alloc_hint; /* last allocation position for sequential affinity */
};

/*
 * Initialize AG allocator from on-disk bitmap.
 */
struct ocsfs_ag_alloc_ctx *ocsfs_ag_alloc_init(uint32_t ag_num,
                                                uint64_t total_blocks,
                                                const uint8_t *disk_bitmap,
                                                size_t bitmap_bytes)
{
    struct ocsfs_ag_alloc_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->ag_number = ag_num;
    ctx->total_blocks = total_blocks;
    ctx->bitmap_size = bitmap_bytes;
    ctx->bitmap = malloc(bitmap_bytes);
    if (!ctx->bitmap) {
        free(ctx);
        return NULL;
    }

    memcpy(ctx->bitmap, disk_bitmap, bitmap_bytes);
    ctx->free_blocks = ocsfs_bitmap_count_free(ctx->bitmap, total_blocks);
    ctx->last_alloc_hint = 0;

    return ctx;
}

/*
 * Allocate extent from AG.
 *
 * goal_blocks:  preferred extent size
 * min_blocks:   minimum acceptable extent size
 * out_start:    returned start block (AG-relative)
 * out_length:   returned extent length
 *
 * Returns 0 on success, -1 on no space.
 */
int ocsfs_ag_alloc_extent(struct ocsfs_ag_alloc_ctx *ctx,
                           uint64_t goal_blocks, uint64_t min_blocks,
                           uint64_t *out_start, uint64_t *out_length)
{
    if (ctx->free_blocks < min_blocks)
        return -1;

    uint64_t actual_len = 0;
    uint64_t start = ocsfs_bitmap_find_free_extent(ctx->bitmap, ctx->total_blocks,
                                                    ctx->last_alloc_hint,
                                                    goal_blocks, min_blocks,
                                                    &actual_len);
    if (start == UINT64_MAX)
        return -1;

    ocsfs_bitmap_set_range(ctx->bitmap, start, actual_len);
    ctx->free_blocks -= actual_len;
    ctx->last_alloc_hint = start + actual_len;

    *out_start = start;
    *out_length = actual_len;
    return 0;
}

/*
 * Free extent in AG.
 */
void ocsfs_ag_free_extent(struct ocsfs_ag_alloc_ctx *ctx,
                           uint64_t start, uint64_t length)
{
    ocsfs_bitmap_clear_range(ctx->bitmap, start, length);
    ctx->free_blocks += length;
}

/*
 * Cleanup.
 */
void ocsfs_ag_alloc_destroy(struct ocsfs_ag_alloc_ctx *ctx)
{
    if (ctx) {
        free(ctx->bitmap);
        free(ctx);
    }
}
