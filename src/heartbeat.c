/*
 * OCSFS — Heartbeat Subsystem
 *
 * Manages the on-disk heartbeat region for node liveness detection.
 * Each node periodically writes its heartbeat (timestamp + sequence)
 * to its dedicated sector on the shared device.
 *
 * Other nodes read heartbeats to detect failures. A node is considered
 * failed if its heartbeat hasn't been updated for heartbeat_timeout.
 *
 * This validates the actual I/O path to storage — fundamentally
 * more reliable than network-based heartbeats (like corosync).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include "ocsfs.h"

/* ─── Heartbeat Manager Context ─────────────────────────────── */

struct ocsfs_heartbeat_mgr {
    int         dev_fd;
    uint64_t    hb_region_off;      /* byte offset of heartbeat region */
    uint16_t    node_slot;
    uint32_t    mount_gen;
    uint16_t    max_nodes;
    uint32_t    interval_ms;        /* write interval */
    uint32_t    timeout_ms;         /* failure detection threshold */

    /* Writer thread */
    pthread_t   writer_thread;
    int         writer_running;
    uint64_t    sequence;           /* monotonic counter */

    /* Reader state */
    struct ocsfs_heartbeat *cached_beats;  /* cached heartbeat reads */
    uint64_t    *last_seen_seq;     /* last seen sequence per node */

    /* Failure callback */
    void (*on_node_failure)(uint16_t slot, void *ctx);
    void *cb_ctx;
};

/* ─── Time utilities ────────────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void sleep_ms(uint32_t ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

/* ─── Disk I/O ──────────────────────────────────────────────── */

static int write_heartbeat(struct ocsfs_heartbeat_mgr *mgr)
{
    struct ocsfs_heartbeat hb;
    memset(&hb, 0, sizeof(hb));

    hb.hb_magic = OCSFS_HEARTBEAT_MAGIC;
    hb.hb_node_slot = mgr->node_slot;
    hb.hb_state = OCSFS_NODE_ACTIVE;
    hb.hb_timestamp = now_ns();
    hb.hb_sequence = ++mgr->sequence;
    hb.hb_mount_gen = mgr->mount_gen;
    hb.hb_checksum = ocsfs_crc32c(0, &hb, sizeof(hb) - sizeof(uint32_t));

    uint64_t off = mgr->hb_region_off +
                   (uint64_t)mgr->node_slot * OCSFS_HEARTBEAT_ENTRY_SIZE;

    if (pwrite(mgr->dev_fd, &hb, sizeof(hb), off) != sizeof(hb)) {
        fprintf(stderr, "ocsfs-heartbeat: write failed for slot %u: %s\n",
                mgr->node_slot, strerror(errno));
        return -EIO;
    }

    /* Ensure it hits the device */
    fdatasync(mgr->dev_fd);
    return 0;
}

static int read_heartbeat(struct ocsfs_heartbeat_mgr *mgr, uint16_t slot,
                          struct ocsfs_heartbeat *hb)
{
    uint64_t off = mgr->hb_region_off +
                   (uint64_t)slot * OCSFS_HEARTBEAT_ENTRY_SIZE;

    if (pread(mgr->dev_fd, hb, sizeof(*hb), off) != sizeof(*hb))
        return -EIO;

    /* Verify checksum */
    uint32_t crc = ocsfs_crc32c(0, hb, sizeof(*hb) - sizeof(uint32_t));
    if (crc != hb->hb_checksum)
        return -EINVAL;

    return 0;
}

/* ─── Writer Thread ─────────────────────────────────────────── */

static void *heartbeat_writer_fn(void *arg)
{
    struct ocsfs_heartbeat_mgr *mgr = (struct ocsfs_heartbeat_mgr *)arg;

    while (mgr->writer_running) {
        if (write_heartbeat(mgr) < 0) {
            /* Critical: we can't write our heartbeat.
             * In production, this would trigger self-fencing. */
            fprintf(stderr, "ocsfs-heartbeat: CRITICAL: cannot write heartbeat! "
                    "Self-fencing recommended.\n");
        }
        sleep_ms(mgr->interval_ms);
    }

    return NULL;
}

/* ─── Failure Detection ─────────────────────────────────────── */

/*
 * Check all nodes' heartbeats and detect failures.
 * Called periodically by the monitor thread or from the main loop.
 *
 * Returns the number of newly detected failures.
 */
int ocsfs_heartbeat_check_all(struct ocsfs_heartbeat_mgr *mgr)
{
    uint64_t now = now_ns();
    uint64_t timeout_ns = (uint64_t)mgr->timeout_ms * 1000000ULL;
    int failures = 0;

    for (uint16_t i = 0; i < mgr->max_nodes; i++) {
        if (i == mgr->node_slot)
            continue; /* don't check ourselves */

        struct ocsfs_heartbeat hb;
        if (read_heartbeat(mgr, i, &hb) < 0)
            continue; /* can't read = possibly empty slot */

        if (hb.hb_magic != OCSFS_HEARTBEAT_MAGIC)
            continue; /* not initialized */

        if (hb.hb_state != OCSFS_NODE_ACTIVE)
            continue; /* not active */

        /* Check staleness */
        if (now - hb.hb_timestamp > timeout_ns) {
            /* Verify it's actually stale and not just a slow read */
            if (hb.hb_sequence == mgr->last_seen_seq[i]) {
                /* Sequence hasn't changed either — confirmed failure */
                fprintf(stderr, "ocsfs-heartbeat: Node %u FAILED "
                        "(heartbeat stale for %lu ms, seq=%lu)\n",
                        i, (unsigned long)((now - hb.hb_timestamp) / 1000000),
                        (unsigned long)hb.hb_sequence);

                if (mgr->on_node_failure)
                    mgr->on_node_failure(i, mgr->cb_ctx);

                failures++;
            }
        }

        mgr->last_seen_seq[i] = hb.hb_sequence;
    }

    return failures;
}

/* ─── Lifecycle ─────────────────────────────────────────────── */

struct ocsfs_heartbeat_mgr *
ocsfs_heartbeat_start(int dev_fd, uint64_t hb_region_off,
                      uint16_t node_slot, uint32_t mount_gen,
                      uint16_t max_nodes,
                      uint32_t interval_ms, uint32_t timeout_ms,
                      void (*on_failure)(uint16_t slot, void *ctx),
                      void *cb_ctx)
{
    struct ocsfs_heartbeat_mgr *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;

    mgr->dev_fd = dev_fd;
    mgr->hb_region_off = hb_region_off;
    mgr->node_slot = node_slot;
    mgr->mount_gen = mount_gen;
    mgr->max_nodes = max_nodes;
    mgr->interval_ms = interval_ms;
    mgr->timeout_ms = timeout_ms;
    mgr->on_node_failure = on_failure;
    mgr->cb_ctx = cb_ctx;
    mgr->sequence = 0;

    mgr->cached_beats = calloc(max_nodes, sizeof(struct ocsfs_heartbeat));
    mgr->last_seen_seq = calloc(max_nodes, sizeof(uint64_t));
    if (!mgr->cached_beats || !mgr->last_seen_seq) {
        free(mgr->cached_beats);
        free(mgr->last_seen_seq);
        free(mgr);
        return NULL;
    }

    /* Write initial heartbeat */
    write_heartbeat(mgr);

    /* Start writer thread */
    mgr->writer_running = 1;
    if (pthread_create(&mgr->writer_thread, NULL, heartbeat_writer_fn, mgr) != 0) {
        fprintf(stderr, "ocsfs-heartbeat: failed to start writer thread\n");
        free(mgr->cached_beats);
        free(mgr->last_seen_seq);
        free(mgr);
        return NULL;
    }

    printf("ocsfs-heartbeat: started (slot=%u, interval=%ums, timeout=%ums)\n",
           node_slot, interval_ms, timeout_ms);

    return mgr;
}

void ocsfs_heartbeat_stop(struct ocsfs_heartbeat_mgr *mgr)
{
    if (!mgr) return;

    mgr->writer_running = 0;
    pthread_join(mgr->writer_thread, NULL);

    /* Write a final "DEAD" heartbeat */
    struct ocsfs_heartbeat hb;
    memset(&hb, 0, sizeof(hb));
    hb.hb_magic = OCSFS_HEARTBEAT_MAGIC;
    hb.hb_node_slot = mgr->node_slot;
    hb.hb_state = OCSFS_NODE_DEAD;
    hb.hb_timestamp = now_ns();
    hb.hb_sequence = mgr->sequence + 1;
    hb.hb_mount_gen = mgr->mount_gen;
    hb.hb_checksum = ocsfs_crc32c(0, &hb, sizeof(hb) - sizeof(uint32_t));

    uint64_t off = mgr->hb_region_off +
                   (uint64_t)mgr->node_slot * OCSFS_HEARTBEAT_ENTRY_SIZE;
    ssize_t __attribute__((unused)) ret = pwrite(mgr->dev_fd, &hb, sizeof(hb), off);

    printf("ocsfs-heartbeat: stopped (slot=%u, final seq=%lu)\n",
           mgr->node_slot, (unsigned long)(mgr->sequence + 1));

    free(mgr->cached_beats);
    free(mgr->last_seen_seq);
    free(mgr);
}

/* ─── Status Reporting ──────────────────────────────────────── */

void ocsfs_heartbeat_dump_status(struct ocsfs_heartbeat_mgr *mgr, FILE *out)
{
    uint64_t now = now_ns();

    fprintf(out, "Heartbeat Status (node %u):\n", mgr->node_slot);
    fprintf(out, "  %-4s  %-8s  %-8s  %-12s  %s\n",
            "Slot", "State", "Gen", "Sequence", "Age (ms)");

    for (uint16_t i = 0; i < mgr->max_nodes; i++) {
        struct ocsfs_heartbeat hb;
        if (read_heartbeat(mgr, i, &hb) < 0)
            continue;
        if (hb.hb_magic != OCSFS_HEARTBEAT_MAGIC)
            continue;

        const char *state_str;
        switch (hb.hb_state) {
        case OCSFS_NODE_ACTIVE:   state_str = "ACTIVE"; break;
        case OCSFS_NODE_DEAD:     state_str = "DEAD"; break;
        case OCSFS_NODE_EVICTING: state_str = "EVICT"; break;
        default:                  state_str = "???"; break;
        }

        uint64_t age_ms = (now - hb.hb_timestamp) / 1000000;
        fprintf(out, "  %-4u  %-8s  %-8u  %-12lu  %lu%s\n",
                i, state_str, hb.hb_mount_gen,
                (unsigned long)hb.hb_sequence,
                (unsigned long)age_ms,
                (age_ms > mgr->timeout_ms) ? " STALE!" : "");
    }
}
