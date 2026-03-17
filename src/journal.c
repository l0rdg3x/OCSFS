/*
 * OCSFS — Write-Ahead Journal (WAL)
 *
 * Per-node circular journal for crash recovery. Each metadata operation
 * is wrapped in a transaction that logs before/after images of modified
 * blocks. On crash, uncommitted transactions are discarded and committed
 * but not checkpointed transactions are replayed.
 *
 * Journal layout (per node):
 *   [journal_header (4KB)] [transaction records...] (circular)
 *
 * Transaction record format:
 *   TX_BEGIN record
 *   For each modified block:
 *     block_ref (block_num, flags, checksum)
 *     after_image (block_size bytes)
 *   TX_COMMIT record (with CRC32C of entire transaction)
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

/* ─── Journal Context ──────────────────────────────────────── */

struct ocsfs_journal_ctx {
    int         dev_fd;             /* device file descriptor */
    uint64_t    journal_off;        /* absolute byte offset of journal region */
    uint64_t    journal_size;       /* total journal size in bytes */
    uint16_t    node_slot;
    uint32_t    block_size;

    /* In-memory journal header (mirrors on-disk) */
    struct ocsfs_journal_header hdr;

    /* Current transaction state */
    int         tx_active;
    uint64_t    tx_id;
    uint32_t    tx_block_count;

    /* Transaction buffer: accumulates block references + data */
    uint8_t    *tx_buf;
    size_t      tx_buf_used;
    size_t      tx_buf_capacity;
};

/* ─── Time helper ──────────────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ─── Journal I/O helpers ──────────────────────────────────── */

/*
 * Write data to the journal at the given offset within the journal region.
 * Handles circular wrap-around. The first block (header) is skipped in
 * the data area; usable space starts at offset sizeof(journal_header).
 */
static int journal_write_at(struct ocsfs_journal_ctx *ctx,
                             uint64_t journal_offset, const void *data, size_t len)
{
    uint64_t abs_off = ctx->journal_off + journal_offset;
    if (pwrite(ctx->dev_fd, data, len, abs_off) != (ssize_t)len)
        return -EIO;
    return 0;
}

static int journal_read_at(struct ocsfs_journal_ctx *ctx,
                            uint64_t journal_offset, void *data, size_t len)
{
    uint64_t abs_off = ctx->journal_off + journal_offset;
    if (pread(ctx->dev_fd, data, len, abs_off) != (ssize_t)len)
        return -EIO;
    return 0;
}

/*
 * Write data to the circular journal area (after the header).
 * Wraps around if necessary.
 */
static int journal_write_circular(struct ocsfs_journal_ctx *ctx,
                                   uint64_t *head, const void *data, size_t len)
{
    uint64_t data_start = sizeof(struct ocsfs_journal_header);
    uint64_t data_end = ctx->journal_size;
    uint64_t pos = *head;
    const uint8_t *src = (const uint8_t *)data;
    size_t remaining = len;

    while (remaining > 0) {
        uint64_t space = data_end - pos;
        size_t chunk = (remaining < space) ? remaining : (size_t)space;

        int ret = journal_write_at(ctx, pos, src, chunk);
        if (ret < 0) return ret;

        src += chunk;
        remaining -= chunk;
        pos += chunk;

        if (pos >= data_end)
            pos = data_start; /* wrap around */
    }

    *head = pos;
    return 0;
}

/*
 * Read data from the circular journal area.
 */
static int journal_read_circular(struct ocsfs_journal_ctx *ctx,
                                  uint64_t *pos, void *data, size_t len)
{
    uint64_t data_start = sizeof(struct ocsfs_journal_header);
    uint64_t data_end = ctx->journal_size;
    uint8_t *dst = (uint8_t *)data;
    size_t remaining = len;

    while (remaining > 0) {
        uint64_t space = data_end - *pos;
        size_t chunk = (remaining < space) ? remaining : (size_t)space;

        int ret = journal_read_at(ctx, *pos, dst, chunk);
        if (ret < 0) return ret;

        dst += chunk;
        remaining -= chunk;
        *pos += chunk;

        if (*pos >= data_end)
            *pos = data_start;
    }

    return 0;
}

/* ─── Flush journal header to disk ─────────────────────────── */

static int flush_header(struct ocsfs_journal_ctx *ctx)
{
    ctx->hdr.jh_checksum = ocsfs_crc32c(0, &ctx->hdr,
                                          sizeof(ctx->hdr) - sizeof(uint32_t));
    return journal_write_at(ctx, 0, &ctx->hdr, sizeof(ctx->hdr));
}

/* ─── Lifecycle ─────────────────────────────────────────────── */

struct ocsfs_journal_ctx *
ocsfs_journal_open(int dev_fd, uint64_t journal_off,
                    uint64_t journal_size, uint16_t node_slot,
                    uint32_t block_size)
{
    struct ocsfs_journal_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->dev_fd = dev_fd;
    ctx->journal_off = journal_off;
    ctx->journal_size = journal_size;
    ctx->node_slot = node_slot;
    ctx->block_size = block_size;

    /* Read journal header */
    if (pread(dev_fd, &ctx->hdr, sizeof(ctx->hdr), journal_off) != sizeof(ctx->hdr)) {
        free(ctx);
        return NULL;
    }

    if (ctx->hdr.jh_magic != OCSFS_JOURNAL_MAGIC) {
        fprintf(stderr, "ocsfs-journal: bad magic for node %u\n", node_slot);
        free(ctx);
        return NULL;
    }

    /* Allocate transaction buffer (max 1 MB) */
    ctx->tx_buf_capacity = 1 << 20;
    ctx->tx_buf = malloc(ctx->tx_buf_capacity);
    if (!ctx->tx_buf) {
        free(ctx);
        return NULL;
    }

    return ctx;
}

void ocsfs_journal_close(struct ocsfs_journal_ctx *ctx)
{
    if (!ctx) return;

    /* Flush header */
    flush_header(ctx);
    fdatasync(ctx->dev_fd);

    free(ctx->tx_buf);
    free(ctx);
}

/* ─── Transaction API ──────────────────────────────────────── */

int ocsfs_journal_begin(struct ocsfs_journal_ctx *ctx)
{
    if (ctx->tx_active)
        return -EINVAL; /* nested transactions not supported */

    ctx->tx_active = 1;
    ctx->tx_id = ctx->hdr.jh_sequence++;
    ctx->tx_block_count = 0;
    ctx->tx_buf_used = 0;

    /* Write TX_BEGIN record to tx buffer */
    struct ocsfs_journal_txn txn;
    memset(&txn, 0, sizeof(txn));
    txn.jt_type = OCSFS_JTYPE_BEGIN;
    txn.jt_id = ctx->tx_id;
    txn.jt_timestamp = now_ns();
    txn.jt_node_slot = ctx->node_slot;
    txn.jt_block_count = 0;
    txn.jt_data_len = 0;
    txn.jt_checksum = ocsfs_crc32c(0, &txn, sizeof(txn) - sizeof(uint32_t));

    /* Add to tx buffer */
    if (ctx->tx_buf_used + sizeof(txn) > ctx->tx_buf_capacity) {
        size_t new_cap = ctx->tx_buf_capacity * 2;
        uint8_t *new_buf = realloc(ctx->tx_buf, new_cap);
        if (!new_buf) return -ENOMEM;
        ctx->tx_buf = new_buf;
        ctx->tx_buf_capacity = new_cap;
    }
    memcpy(ctx->tx_buf + ctx->tx_buf_used, &txn, sizeof(txn));
    ctx->tx_buf_used += sizeof(txn);

    return 0;
}

/*
 * Log a metadata block modification.
 * after_image: the new content of the block (block_size bytes).
 * block_addr:  the volume-absolute block number.
 */
int ocsfs_journal_log_block(struct ocsfs_journal_ctx *ctx,
                             uint64_t block_addr,
                             const void *after_image)
{
    if (!ctx->tx_active)
        return -EINVAL;

    /* Block reference */
    struct ocsfs_journal_block_ref ref;
    ref.jbr_block_num = block_addr;
    ref.jbr_flags = OCSFS_JBR_AFTER;
    ref.jbr_checksum = ocsfs_crc32c(0, after_image, ctx->block_size);

    /* Grow tx buffer if needed */
    size_t needed = sizeof(ref) + ctx->block_size;
    while (ctx->tx_buf_used + needed > ctx->tx_buf_capacity) {
        size_t new_cap = ctx->tx_buf_capacity * 2;
        uint8_t *new_buf = realloc(ctx->tx_buf, new_cap);
        if (!new_buf) return -ENOMEM;
        ctx->tx_buf = new_buf;
        ctx->tx_buf_capacity = new_cap;
    }

    memcpy(ctx->tx_buf + ctx->tx_buf_used, &ref, sizeof(ref));
    ctx->tx_buf_used += sizeof(ref);
    memcpy(ctx->tx_buf + ctx->tx_buf_used, after_image, ctx->block_size);
    ctx->tx_buf_used += ctx->block_size;

    ctx->tx_block_count++;
    return 0;
}

/*
 * Commit the current transaction.
 * Writes the full transaction (BEGIN + blocks + COMMIT) to the journal.
 */
int ocsfs_journal_commit(struct ocsfs_journal_ctx *ctx)
{
    if (!ctx->tx_active)
        return -EINVAL;

    /* Update the BEGIN record with final block count */
    struct ocsfs_journal_txn *begin = (struct ocsfs_journal_txn *)ctx->tx_buf;
    begin->jt_block_count = ctx->tx_block_count;
    begin->jt_data_len = ctx->tx_buf_used - sizeof(struct ocsfs_journal_txn);
    begin->jt_checksum = ocsfs_crc32c(0, begin, sizeof(*begin) - sizeof(uint32_t));

    /* Append COMMIT record */
    struct ocsfs_journal_txn commit_rec;
    memset(&commit_rec, 0, sizeof(commit_rec));
    commit_rec.jt_type = OCSFS_JTYPE_COMMIT;
    commit_rec.jt_id = ctx->tx_id;
    commit_rec.jt_timestamp = now_ns();
    commit_rec.jt_node_slot = ctx->node_slot;
    /* Checksum covers the entire transaction buffer */
    commit_rec.jt_checksum = ocsfs_crc32c(0, ctx->tx_buf, ctx->tx_buf_used);

    size_t needed = sizeof(commit_rec);
    while (ctx->tx_buf_used + needed > ctx->tx_buf_capacity) {
        size_t new_cap = ctx->tx_buf_capacity * 2;
        uint8_t *new_buf = realloc(ctx->tx_buf, new_cap);
        if (!new_buf) return -ENOMEM;
        ctx->tx_buf = new_buf;
        ctx->tx_buf_capacity = new_cap;
    }
    memcpy(ctx->tx_buf + ctx->tx_buf_used, &commit_rec, sizeof(commit_rec));
    ctx->tx_buf_used += sizeof(commit_rec);

    /* Write entire transaction to journal at head position */
    uint64_t head = ctx->hdr.jh_head;
    int ret = journal_write_circular(ctx, &head, ctx->tx_buf, ctx->tx_buf_used);
    if (ret < 0) {
        ctx->tx_active = 0;
        return ret;
    }

    /* Ensure data hits disk */
    fdatasync(ctx->dev_fd);

    /* Update head */
    ctx->hdr.jh_head = head;
    flush_header(ctx);
    fdatasync(ctx->dev_fd);

    ctx->tx_active = 0;
    return 0;
}

/*
 * Checkpoint: write all journaled blocks to their final on-disk locations.
 * Then advance the tail to reclaim journal space.
 */
int ocsfs_journal_checkpoint(struct ocsfs_journal_ctx *ctx)
{
    uint64_t pos = ctx->hdr.jh_tail;
    uint64_t head = ctx->hdr.jh_head;

    if (pos == head)
        return 0; /* nothing to checkpoint */

    while (pos != head) {
        /* Read transaction header */
        struct ocsfs_journal_txn txn;
        int ret = journal_read_circular(ctx, &pos, &txn, sizeof(txn));
        if (ret < 0) return ret;

        if (txn.jt_type == OCSFS_JTYPE_BEGIN) {
            /* Read and replay each block */
            for (uint16_t b = 0; b < txn.jt_block_count; b++) {
                struct ocsfs_journal_block_ref ref;
                ret = journal_read_circular(ctx, &pos, &ref, sizeof(ref));
                if (ret < 0) return ret;

                /* Read the after-image */
                void *block_data = malloc(ctx->block_size);
                if (!block_data) return -ENOMEM;

                ret = journal_read_circular(ctx, &pos, block_data, ctx->block_size);
                if (ret < 0) { free(block_data); return ret; }

                /* Verify checksum */
                uint32_t crc = ocsfs_crc32c(0, block_data, ctx->block_size);
                if (crc == ref.jbr_checksum) {
                    /* Write to final location */
                    uint64_t disk_off = ref.jbr_block_num * ctx->block_size;
                    if (pwrite(ctx->dev_fd, block_data, ctx->block_size,
                               disk_off) != (ssize_t)ctx->block_size) {
                        free(block_data);
                        return -EIO;
                    }
                }
                free(block_data);
            }
        } else if (txn.jt_type == OCSFS_JTYPE_COMMIT) {
            /* Transaction fully checkpointed */
            continue;
        } else {
            /* Skip unknown record types */
            break;
        }
    }

    fdatasync(ctx->dev_fd);

    /* Advance tail */
    ctx->hdr.jh_tail = head;
    flush_header(ctx);
    fdatasync(ctx->dev_fd);

    return 0;
}

/*
 * Replay committed but not checkpointed transactions.
 * Called during mount after an unclean shutdown.
 */
int ocsfs_journal_replay(struct ocsfs_journal_ctx *ctx)
{
    if (ctx->hdr.jh_tail == ctx->hdr.jh_head) {
        return 0; /* clean — nothing to replay */
    }

    printf("ocsfs-journal: replaying journal for node %u...\n", ctx->node_slot);

    int replayed = 0;
    int ret = ocsfs_journal_checkpoint(ctx);
    if (ret < 0) {
        fprintf(stderr, "ocsfs-journal: replay failed: %d\n", ret);
        return ret;
    }

    printf("ocsfs-journal: replay complete (%d transactions)\n", replayed);
    return replayed;
}

/* ─── Statistics ────────────────────────────────────────────── */

uint64_t ocsfs_journal_used_bytes(const struct ocsfs_journal_ctx *ctx)
{
    if (ctx->hdr.jh_head >= ctx->hdr.jh_tail)
        return ctx->hdr.jh_head - ctx->hdr.jh_tail;
    return (ctx->journal_size - ctx->hdr.jh_tail) +
           (ctx->hdr.jh_head - sizeof(struct ocsfs_journal_header));
}

uint64_t ocsfs_journal_free_bytes(const struct ocsfs_journal_ctx *ctx)
{
    uint64_t data_area = ctx->journal_size - sizeof(struct ocsfs_journal_header);
    return data_area - ocsfs_journal_used_bytes(ctx);
}
