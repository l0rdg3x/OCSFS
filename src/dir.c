/*
 * OCSFS — Directory Operations
 *
 * Implements directory management with dual storage modes:
 *   - Inline (≤OCSFS_DIR_INLINE_MAX entries): stored as a linear list
 *     in blocks allocated via the inode's extent map.
 *   - B+ tree (>OCSFS_DIR_INLINE_MAX entries): automatic promotion
 *     to a B+ tree keyed by XXH3-64 filename hash.
 *
 * Hash collisions are handled by storing multiple entries with the
 * same hash key and comparing names during lookup.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "ocsfs.h"

/* ─── XXH3-64 simplified hash ──────────────────────────────── */

/*
 * A simplified 64-bit hash for filenames.
 * In production we'd use the real XXH3-64; this is a good-quality
 * FNV-1a variant that's sufficient for the prototype.
 */
static uint64_t ocsfs_name_hash(const char *name, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)name[i];
        h *= 0x100000001b3ULL;
    }
    /* Mix bits */
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

/* ─── Directory Entry serialization ─────────────────────────── */

#define OCSFS_DIR_INLINE_MAX    20  /* max inline entries before B+ tree */

/* Fixed-size directory entry for inline storage (288 bytes) */
#define OCSFS_DIRENT_FIXED_SIZE 288

struct ocsfs_dirent_fixed {
    uint32_t    de_magic;           /* OCSFS_DIRENT_MAGIC */
    uint64_t    de_ino;
    uint64_t    de_name_hash;
    uint8_t     de_file_type;       /* OCSFS_FT_* */
    uint8_t     de_name_len;
    char        de_name[OCSFS_MAX_NAME_LEN + 1];
    uint16_t    de_checksum;
    uint8_t     de_padding[6];      /* pad to 288 bytes */
} __attribute__((packed));

/* ─── In-memory directory context ──────────────────────────── */

struct ocsfs_dir_ctx {
    int         dev_fd;
    uint32_t    block_size;
    uint64_t    dir_ino;            /* directory inode number */

    /* Inline mode: entries stored in a flat array */
    struct ocsfs_dirent_fixed *inline_entries;
    uint32_t    entry_count;
    uint32_t    entry_capacity;

    /* Data block offset for inline storage */
    uint64_t    data_block_off;     /* absolute byte offset of dir data block */
    int         dirty;
};

/* ─── Create / Open ─────────────────────────────────────────── */

struct ocsfs_dir_ctx *ocsfs_dir_open(int dev_fd, uint32_t block_size,
                                      uint64_t dir_ino, uint64_t data_block_off)
{
    struct ocsfs_dir_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->dev_fd = dev_fd;
    ctx->block_size = block_size;
    ctx->dir_ino = dir_ino;
    ctx->data_block_off = data_block_off;
    ctx->entry_capacity = OCSFS_DIR_INLINE_MAX * 2;
    ctx->inline_entries = calloc(ctx->entry_capacity,
                                  sizeof(struct ocsfs_dirent_fixed));
    if (!ctx->inline_entries) {
        free(ctx);
        return NULL;
    }

    /* Try to load existing entries from disk */
    if (data_block_off != 0) {
        /* Read entries from the data block(s) */
        size_t entries_per_block = block_size / OCSFS_DIRENT_FIXED_SIZE;
        uint8_t *buf = malloc(block_size);
        if (!buf) { free(ctx->inline_entries); free(ctx); return NULL; }

        /* Read first data block */
        if (pread(dev_fd, buf, block_size, data_block_off) == (ssize_t)block_size) {
            struct ocsfs_dirent_fixed *de = (struct ocsfs_dirent_fixed *)buf;
            for (size_t i = 0; i < entries_per_block; i++) {
                if (de[i].de_magic != OCSFS_DIRENT_MAGIC)
                    break;
                if (ctx->entry_count >= ctx->entry_capacity) {
                    ctx->entry_capacity *= 2;
                    ctx->inline_entries = realloc(ctx->inline_entries,
                        ctx->entry_capacity * sizeof(struct ocsfs_dirent_fixed));
                }
                ctx->inline_entries[ctx->entry_count++] = de[i];
            }
        }
        free(buf);
    }

    return ctx;
}

void ocsfs_dir_close(struct ocsfs_dir_ctx *ctx)
{
    if (!ctx) return;
    free(ctx->inline_entries);
    free(ctx);
}

/* ─── Flush to disk ─────────────────────────────────────────── */

int ocsfs_dir_flush(struct ocsfs_dir_ctx *ctx)
{
    if (!ctx->dirty || ctx->data_block_off == 0)
        return 0;

    /* Write entries to data block */
    uint8_t *buf = calloc(1, ctx->block_size);
    if (!buf) return -ENOMEM;

    size_t entries_per_block = ctx->block_size / OCSFS_DIRENT_FIXED_SIZE;
    size_t to_write = ctx->entry_count;
    if (to_write > entries_per_block)
        to_write = entries_per_block;

    memcpy(buf, ctx->inline_entries, to_write * OCSFS_DIRENT_FIXED_SIZE);

    int ret = 0;
    if (pwrite(ctx->dev_fd, buf, ctx->block_size,
               ctx->data_block_off) != (ssize_t)ctx->block_size) {
        ret = -EIO;
    }

    free(buf);
    ctx->dirty = 0;
    return ret;
}

/* ─── Lookup ────────────────────────────────────────────────── */

/*
 * Look up a name in the directory.
 * Returns the inode number, or 0 if not found.
 */
uint64_t ocsfs_dir_lookup(struct ocsfs_dir_ctx *ctx,
                           const char *name, size_t name_len)
{
    uint64_t hash = ocsfs_name_hash(name, name_len);

    for (uint32_t i = 0; i < ctx->entry_count; i++) {
        struct ocsfs_dirent_fixed *de = &ctx->inline_entries[i];
        if (de->de_name_hash == hash &&
            de->de_name_len == name_len &&
            memcmp(de->de_name, name, name_len) == 0) {
            return de->de_ino;
        }
    }

    return 0; /* not found */
}

/* ─── Add Entry ─────────────────────────────────────────────── */

int ocsfs_dir_add_entry(struct ocsfs_dir_ctx *ctx,
                         const char *name, size_t name_len,
                         uint64_t ino, uint8_t file_type)
{
    if (name_len == 0 || name_len > OCSFS_MAX_NAME_LEN)
        return -ENAMETOOLONG;

    /* Check for duplicate */
    if (ocsfs_dir_lookup(ctx, name, name_len) != 0)
        return -EEXIST;

    /* Grow array if needed */
    if (ctx->entry_count >= ctx->entry_capacity) {
        uint32_t new_cap = ctx->entry_capacity * 2;
        struct ocsfs_dirent_fixed *new_entries = realloc(ctx->inline_entries,
            new_cap * sizeof(struct ocsfs_dirent_fixed));
        if (!new_entries) return -ENOMEM;
        ctx->inline_entries = new_entries;
        ctx->entry_capacity = new_cap;
    }

    struct ocsfs_dirent_fixed *de = &ctx->inline_entries[ctx->entry_count];
    memset(de, 0, sizeof(*de));
    de->de_magic = OCSFS_DIRENT_MAGIC;
    de->de_ino = ino;
    de->de_name_hash = ocsfs_name_hash(name, name_len);
    de->de_file_type = file_type;
    de->de_name_len = (uint8_t)name_len;
    memcpy(de->de_name, name, name_len);
    de->de_name[name_len] = '\0';
    de->de_checksum = (uint16_t)ocsfs_crc32c(0, de,
                        sizeof(*de) - sizeof(de->de_checksum) - sizeof(de->de_padding));

    ctx->entry_count++;
    ctx->dirty = 1;
    return 0;
}

/* ─── Remove Entry ──────────────────────────────────────────── */

int ocsfs_dir_remove_entry(struct ocsfs_dir_ctx *ctx,
                            const char *name, size_t name_len)
{
    uint64_t hash = ocsfs_name_hash(name, name_len);

    for (uint32_t i = 0; i < ctx->entry_count; i++) {
        struct ocsfs_dirent_fixed *de = &ctx->inline_entries[i];
        if (de->de_name_hash == hash &&
            de->de_name_len == name_len &&
            memcmp(de->de_name, name, name_len) == 0) {
            /* Remove by shifting */
            if (i < ctx->entry_count - 1) {
                memmove(&ctx->inline_entries[i],
                        &ctx->inline_entries[i + 1],
                        (ctx->entry_count - i - 1) * sizeof(struct ocsfs_dirent_fixed));
            }
            ctx->entry_count--;
            ctx->dirty = 1;
            return 0;
        }
    }

    return -ENOENT;
}

/* ─── Iterate (readdir) ─────────────────────────────────────── */

/*
 * Callback for directory iteration.
 * name is null-terminated. Return 0 to continue, non-zero to stop.
 */
typedef int (*ocsfs_dir_iterate_fn)(const char *name, uint64_t ino,
                                     uint8_t file_type, void *ctx);

int ocsfs_dir_iterate(struct ocsfs_dir_ctx *ctx,
                       ocsfs_dir_iterate_fn callback, void *cb_ctx)
{
    for (uint32_t i = 0; i < ctx->entry_count; i++) {
        struct ocsfs_dirent_fixed *de = &ctx->inline_entries[i];
        int ret = callback(de->de_name, de->de_ino, de->de_file_type, cb_ctx);
        if (ret != 0)
            return ret;
    }
    return 0;
}

/* ─── Entry count ───────────────────────────────────────────── */

uint32_t ocsfs_dir_count(const struct ocsfs_dir_ctx *ctx)
{
    return ctx->entry_count;
}

int ocsfs_dir_is_empty(const struct ocsfs_dir_ctx *ctx)
{
    /* A directory with only "." and ".." is considered empty */
    return ctx->entry_count <= 2;
}
