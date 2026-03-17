/*
 * OCSFS — Inode Allocator
 *
 * Manages inode allocation/deallocation within Allocation Groups.
 * Each AG has an inode table (fixed-size array of 512-byte inodes)
 * and a bitmap tracking which inodes are allocated.
 *
 * The inode bitmap is stored in the first bytes of the inode table
 * region, followed by the actual inode slots.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "ocsfs.h"

/* ─── Inode bitmap helpers ──────────────────────────────────── */

/* External bitmap functions from bitmap.c */
extern void ocsfs_bitmap_set_range(uint8_t *bitmap, uint64_t start, uint64_t count);
extern void ocsfs_bitmap_clear_range(uint8_t *bitmap, uint64_t start, uint64_t count);
extern uint64_t ocsfs_bitmap_count_free(const uint8_t *bitmap, uint64_t total_bits);

static inline int inode_bitmap_test(const uint8_t *bitmap, uint64_t bit)
{
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static inline void inode_bitmap_set(uint8_t *bitmap, uint64_t bit)
{
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void inode_bitmap_clear(uint8_t *bitmap, uint64_t bit)
{
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

/* ─── Inode Allocator Context ──────────────────────────────── */

struct ocsfs_inode_alloc_ctx {
    int         dev_fd;             /* device file descriptor */
    uint32_t    ag_number;          /* AG index */
    uint64_t    ag_data_start;      /* absolute byte offset of AG data region */
    uint64_t    inode_table_off;    /* relative offset within AG to inode table */
    uint64_t    inode_count;        /* total inode slots in this AG */
    uint64_t    free_inodes;        /* free inode count */
    uint8_t    *inode_bitmap;       /* in-memory inode allocation bitmap */
    size_t      bitmap_size;        /* bitmap size in bytes */
    uint32_t    block_size;         /* filesystem block size */
};

/* ─── Timestamp helper ──────────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ─── Lifecycle ─────────────────────────────────────────────── */

/*
 * Initialize inode allocator for an AG.
 *
 * dev_fd:          file descriptor to device/image
 * ag_number:       AG index
 * ag_data_start:   absolute byte offset where AG data begins
 * inode_table_off: relative byte offset within AG to inode table
 * inode_count:     total inode slots
 * block_size:      filesystem block size
 */
struct ocsfs_inode_alloc_ctx *
ocsfs_inode_alloc_init(int dev_fd, uint32_t ag_number,
                        uint64_t ag_data_start, uint64_t inode_table_off,
                        uint64_t inode_count, uint32_t block_size)
{
    struct ocsfs_inode_alloc_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->dev_fd = dev_fd;
    ctx->ag_number = ag_number;
    ctx->ag_data_start = ag_data_start;
    ctx->inode_table_off = inode_table_off;
    ctx->inode_count = inode_count;
    ctx->block_size = block_size;

    /* Bitmap: 1 bit per inode */
    ctx->bitmap_size = (inode_count + 7) / 8;
    ctx->inode_bitmap = calloc(1, ctx->bitmap_size);
    if (!ctx->inode_bitmap) {
        free(ctx);
        return NULL;
    }

    /* Scan inode table to build bitmap.
     * An inode is "allocated" if its i_magic == OCSFS_INODE_MAGIC.
     */
    uint64_t table_abs = ag_data_start + inode_table_off;
    struct ocsfs_inode ino;

    for (uint64_t i = 0; i < inode_count; i++) {
        uint64_t off = table_abs + i * OCSFS_INODE_SIZE;
        if (pread(dev_fd, &ino, sizeof(ino), off) == sizeof(ino)) {
            if (ino.i_magic == OCSFS_INODE_MAGIC && ino.i_nlink > 0) {
                inode_bitmap_set(ctx->inode_bitmap, i);
            }
        }
    }

    /* Also mark reserved inodes (0..OCSFS_FIRST_USER_INO-1) as allocated
     * if this is AG 0 */
    if (ag_number == 0) {
        for (uint64_t i = 0; i < OCSFS_FIRST_USER_INO && i < inode_count; i++) {
            inode_bitmap_set(ctx->inode_bitmap, i);
        }
    }

    ctx->free_inodes = ocsfs_bitmap_count_free(ctx->inode_bitmap, inode_count);

    return ctx;
}

void ocsfs_inode_alloc_destroy(struct ocsfs_inode_alloc_ctx *ctx)
{
    if (ctx) {
        free(ctx->inode_bitmap);
        free(ctx);
    }
}

/* ─── Inode Number ↔ AG mapping ─────────────────────────────── */

/*
 * Compute global inode number from AG-local index.
 * Global ino = ag_number * inodes_per_ag + local_index
 */
static inline uint64_t local_to_global_ino(struct ocsfs_inode_alloc_ctx *ctx,
                                            uint64_t local_idx)
{
    return (uint64_t)ctx->ag_number * ctx->inode_count + local_idx;
}

/*
 * Compute AG-local index from global inode number.
 */
static inline uint64_t global_to_local_ino(struct ocsfs_inode_alloc_ctx *ctx,
                                            uint64_t ino)
{
    return ino % ctx->inode_count;
}

/* ─── Disk offset for an inode ──────────────────────────────── */

static uint64_t inode_disk_offset(struct ocsfs_inode_alloc_ctx *ctx, uint64_t local_idx)
{
    return ctx->ag_data_start + ctx->inode_table_off + local_idx * OCSFS_INODE_SIZE;
}

/* ─── Allocate Inode ────────────────────────────────────────── */

/*
 * Allocate a new inode in the AG.
 *
 * mode: file type + permissions (e.g., (OCSFS_FT_REG_FILE << 12) | 0644)
 * uid, gid: owner
 *
 * Returns the global inode number, or 0 on failure.
 */
uint64_t ocsfs_inode_alloc(struct ocsfs_inode_alloc_ctx *ctx,
                            uint16_t mode, uint32_t uid, uint32_t gid)
{
    if (ctx->free_inodes == 0)
        return 0;

    /* Find first free inode slot */
    uint64_t start = (ctx->ag_number == 0) ? OCSFS_FIRST_USER_INO : 0;

    for (uint64_t i = start; i < ctx->inode_count; i++) {
        if (!inode_bitmap_test(ctx->inode_bitmap, i)) {
            /* Found free slot — initialize inode */
            struct ocsfs_inode ino;
            memset(&ino, 0, sizeof(ino));

            uint64_t global_ino = local_to_global_ino(ctx, i);

            ino.i_magic = OCSFS_INODE_MAGIC;
            ino.i_ino = global_ino;
            ino.i_mode = mode;
            ino.i_nlink = 1;
            ino.i_uid = uid;
            ino.i_gid = gid;
            ino.i_size = 0;
            ino.i_blocks = 0;
            ino.i_atime = now_ns();
            ino.i_mtime = now_ns();
            ino.i_ctime = now_ns();
            ino.i_extent_count = 0;
            ino.i_extent_max = OCSFS_INLINE_EXTENTS;
            ino.i_extent_tree_root = 0;
            ino.i_ag = ctx->ag_number;
            ino.i_checksum = ocsfs_crc32c(0, &ino,
                                           sizeof(ino) - sizeof(uint32_t));

            /* Write to disk */
            uint64_t off = inode_disk_offset(ctx, i);
            if (pwrite(ctx->dev_fd, &ino, sizeof(ino), off) != sizeof(ino))
                return 0;

            inode_bitmap_set(ctx->inode_bitmap, i);
            ctx->free_inodes--;

            return global_ino;
        }
    }

    return 0; /* no free inodes */
}

/* ─── Free Inode ────────────────────────────────────────────── */

/*
 * Free an inode. Clears magic and marks slot as available.
 * Returns 0 on success.
 */
int ocsfs_inode_free(struct ocsfs_inode_alloc_ctx *ctx, uint64_t ino)
{
    uint64_t local = global_to_local_ino(ctx, ino);
    if (local >= ctx->inode_count)
        return -EINVAL;

    if (!inode_bitmap_test(ctx->inode_bitmap, local))
        return -ENOENT; /* already free */

    /* Zero out the inode on disk */
    struct ocsfs_inode zero_ino;
    memset(&zero_ino, 0, sizeof(zero_ino));

    uint64_t off = inode_disk_offset(ctx, local);
    if (pwrite(ctx->dev_fd, &zero_ino, sizeof(zero_ino), off) != sizeof(zero_ino))
        return -EIO;

    inode_bitmap_clear(ctx->inode_bitmap, local);
    ctx->free_inodes++;

    return 0;
}

/* ─── Read / Write Inode ────────────────────────────────────── */

/*
 * Read an inode from disk.
 * Returns 0 on success, validates magic and checksum.
 */
int ocsfs_inode_read(int dev_fd, uint64_t ag_data_start,
                      uint64_t inode_table_off, uint64_t ino_local,
                      struct ocsfs_inode *out)
{
    uint64_t off = ag_data_start + inode_table_off + ino_local * OCSFS_INODE_SIZE;

    if (pread(dev_fd, out, sizeof(*out), off) != sizeof(*out))
        return -EIO;

    if (out->i_magic != OCSFS_INODE_MAGIC)
        return -EINVAL;

    /* Validate checksum */
    uint32_t expected = out->i_checksum;
    uint32_t computed = ocsfs_crc32c(0, out, sizeof(*out) - sizeof(uint32_t));
    if (computed != expected)
        return -EILSEQ;

    return 0;
}

/*
 * Write an inode to disk, updating the checksum.
 */
int ocsfs_inode_write(int dev_fd, uint64_t ag_data_start,
                       uint64_t inode_table_off, uint64_t ino_local,
                       struct ocsfs_inode *inode)
{
    /* Update checksum */
    inode->i_checksum = ocsfs_crc32c(0, inode,
                                      sizeof(*inode) - sizeof(uint32_t));

    uint64_t off = ag_data_start + inode_table_off + ino_local * OCSFS_INODE_SIZE;
    if (pwrite(dev_fd, inode, sizeof(*inode), off) != sizeof(*inode))
        return -EIO;

    return 0;
}

/* ─── Helper: Read inode by global number ───────────────────── */

/*
 * Read an inode by its global inode number, given the superblock
 * and AG descriptor information.
 *
 * This is a convenience function for the FUSE layer.
 */
int ocsfs_inode_read_by_ino(int dev_fd, const struct ocsfs_superblock *sb,
                             uint64_t ino, struct ocsfs_inode *out)
{
    /* Determine which AG this inode belongs to */
    /* Read AG 0 descriptor to get inodes_per_ag */
    struct ocsfs_ag_desc ag0;
    if (pread(dev_fd, &ag0, sizeof(ag0), sb->s_ag_desc_off) != sizeof(ag0))
        return -EIO;

    uint64_t inodes_per_ag = ag0.ag_inode_count;
    uint32_t ag_num = (uint32_t)(ino / inodes_per_ag);
    uint64_t local_ino = ino % inodes_per_ag;

    if (ag_num >= sb->s_ag_count)
        return -EINVAL;

    /* Read AG descriptor */
    struct ocsfs_ag_desc agd;
    uint64_t agd_off = sb->s_ag_desc_off + (uint64_t)ag_num * sizeof(agd);
    if (pread(dev_fd, &agd, sizeof(agd), agd_off) != sizeof(agd))
        return -EIO;

    uint64_t ag_data_start = (uint64_t)agd.ag_block_start * sb->s_block_size;

    return ocsfs_inode_read(dev_fd, ag_data_start, agd.ag_inode_table_off,
                             local_ino, out);
}

/*
 * Write an inode by its global inode number.
 */
int ocsfs_inode_write_by_ino(int dev_fd, const struct ocsfs_superblock *sb,
                              uint64_t ino, struct ocsfs_inode *inode)
{
    struct ocsfs_ag_desc ag0;
    if (pread(dev_fd, &ag0, sizeof(ag0), sb->s_ag_desc_off) != sizeof(ag0))
        return -EIO;

    uint64_t inodes_per_ag = ag0.ag_inode_count;
    uint32_t ag_num = (uint32_t)(ino / inodes_per_ag);
    uint64_t local_ino = ino % inodes_per_ag;

    if (ag_num >= sb->s_ag_count)
        return -EINVAL;

    struct ocsfs_ag_desc agd;
    uint64_t agd_off = sb->s_ag_desc_off + (uint64_t)ag_num * sizeof(agd);
    if (pread(dev_fd, &agd, sizeof(agd), agd_off) != sizeof(agd))
        return -EIO;

    uint64_t ag_data_start = (uint64_t)agd.ag_block_start * sb->s_block_size;

    return ocsfs_inode_write(dev_fd, ag_data_start, agd.ag_inode_table_off,
                              local_ino, inode);
}

/* ─── Statistics ────────────────────────────────────────────── */

uint64_t ocsfs_inode_alloc_free_count(const struct ocsfs_inode_alloc_ctx *ctx)
{
    return ctx->free_inodes;
}

uint64_t ocsfs_inode_alloc_total_count(const struct ocsfs_inode_alloc_ctx *ctx)
{
    return ctx->inode_count;
}
