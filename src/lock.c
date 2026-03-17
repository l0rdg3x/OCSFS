/*
 * OCSFS — On-Disk Distributed Lock Manager
 *
 * This is the heart of OCSFS and its key differentiator from GFS2/OCFS2.
 * All locking state is stored on-disk in the Lock Table region and
 * manipulated using SCSI Compare-And-Write (CAW) for atomicity.
 *
 * In the userspace prototype, we simulate CAW using pread/pwrite with
 * advisory file locks as a serialization fallback. The kernel module
 * will use the real SCSI_IOCTL_SEND_COMMAND / sg interface.
 *
 * Lock acquisition protocol:
 *   1. Hash resource to lock table slot
 *   2. Read lock entry
 *   3. Check compatibility
 *   4. CAS update (or wait + retry)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include "ocsfs.h"

/* ─── Lock compatibility matrix ─────────────────────────────── */

/*
 * Can a new lock of mode 'requested' be granted while 'held' is active?
 *
 *          NL   SH   EX   CW
 *    NL    Y    Y    Y    Y
 *    SH    Y    Y    N    N
 *    EX    Y    N    N    N
 *    CW    Y    N    N    Y
 */
static const int lock_compat[4][4] = {
    /*         NL  SH  EX  CW   <- held */
    /* NL */ { 1,  1,  1,  1 },
    /* SH */ { 1,  1,  0,  0 },
    /* EX */ { 1,  0,  0,  0 },
    /* CW */ { 1,  0,  0,  1 },
};

static inline int locks_compatible(uint16_t held, uint16_t requested)
{
    if (held > 3 || requested > 3)
        return 0;
    return lock_compat[requested][held];
}

/* ─── Lock Manager Context ──────────────────────────────────── */

struct ocsfs_lock_mgr {
    int         dev_fd;          /* file descriptor to the device */
    uint64_t    lock_table_off;  /* byte offset of lock table on disk */
    uint16_t    node_slot;       /* our node slot number */
    uint32_t    mount_gen;       /* our mount generation */

    /* Statistics */
    uint64_t    stat_acquires;
    uint64_t    stat_releases;
    uint64_t    stat_conflicts;
    uint64_t    stat_retries;
};

/*
 * Create lock manager.
 */
struct ocsfs_lock_mgr *ocsfs_lock_mgr_create(int dev_fd, uint64_t lock_table_off,
                                               uint16_t node_slot, uint32_t mount_gen)
{
    struct ocsfs_lock_mgr *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;

    mgr->dev_fd = dev_fd;
    mgr->lock_table_off = lock_table_off;
    mgr->node_slot = node_slot;
    mgr->mount_gen = mount_gen;

    return mgr;
}

void ocsfs_lock_mgr_destroy(struct ocsfs_lock_mgr *mgr)
{
    free(mgr);
}

/* ─── Disk I/O for Lock Entries ─────────────────────────────── */

static int read_lock_entry(struct ocsfs_lock_mgr *mgr, uint32_t slot,
                           struct ocsfs_lock_entry *le)
{
    uint64_t off = mgr->lock_table_off + (uint64_t)slot * OCSFS_LOCK_ENTRY_SIZE;
    if (pread(mgr->dev_fd, le, sizeof(*le), off) != sizeof(*le))
        return -EIO;
    return 0;
}

static int __attribute__((unused)) write_lock_entry(struct ocsfs_lock_mgr *mgr, uint32_t slot,
                            const struct ocsfs_lock_entry *le)
{
    uint64_t off = mgr->lock_table_off + (uint64_t)slot * OCSFS_LOCK_ENTRY_SIZE;
    if (pwrite(mgr->dev_fd, le, sizeof(*le), off) != sizeof(*le))
        return -EIO;
    return 0;
}

/*
 * Compare-and-swap a lock entry.
 *
 * In production kernel code, this would use SCSI COMPARE AND WRITE (opcode 0x89).
 * In the userspace prototype, we simulate it with:
 *   1. Acquire advisory lock on the lock table region
 *   2. Read current entry
 *   3. Compare with expected
 *   4. Write new entry if match
 *   5. Release advisory lock
 *
 * Returns 0 on success, -EAGAIN if entry changed (retry), -EIO on error.
 */
static int cas_lock_entry(struct ocsfs_lock_mgr *mgr, uint32_t slot,
                          const struct ocsfs_lock_entry *expected,
                          const struct ocsfs_lock_entry *new_entry)
{
    uint64_t off = mgr->lock_table_off + (uint64_t)slot * OCSFS_LOCK_ENTRY_SIZE;

    /* Simulate atomic CAS with advisory lock */
    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = off,
        .l_len = OCSFS_LOCK_ENTRY_SIZE,
    };

    if (fcntl(mgr->dev_fd, F_SETLKW, &fl) < 0)
        return -EIO;

    /* Read current */
    struct ocsfs_lock_entry current;
    if (pread(mgr->dev_fd, &current, sizeof(current), off) != sizeof(current)) {
        fl.l_type = F_UNLCK;
        fcntl(mgr->dev_fd, F_SETLK, &fl);
        return -EIO;
    }

    /* Compare version — this is what SCSI CAW compares */
    if (current.le_version != expected->le_version) {
        fl.l_type = F_UNLCK;
        fcntl(mgr->dev_fd, F_SETLK, &fl);
        return -EAGAIN;
    }

    /* Write new entry */
    if (pwrite(mgr->dev_fd, new_entry, sizeof(*new_entry), off) != sizeof(*new_entry)) {
        fl.l_type = F_UNLCK;
        fcntl(mgr->dev_fd, F_SETLK, &fl);
        return -EIO;
    }

    fl.l_type = F_UNLCK;
    fcntl(mgr->dev_fd, F_SETLK, &fl);
    return 0;
}

/* ─── Waiter Bitmask Operations ─────────────────────────────── */

static inline void set_waiter_bit(struct ocsfs_lock_entry *le, uint16_t slot)
{
    le->le_waiters[slot / 8] |= (1 << (slot % 8));
}

static inline void clear_waiter_bit(struct ocsfs_lock_entry *le, uint16_t slot)
{
    le->le_waiters[slot / 8] &= ~(1 << (slot % 8));
}

static inline int test_waiter_bit(const struct ocsfs_lock_entry *le, uint16_t slot)
{
    return (le->le_waiters[slot / 8] >> (slot % 8)) & 1;
}

static inline int has_waiters(const struct ocsfs_lock_entry *le)
{
    for (int i = 0; i < 32; i++) {
        if (le->le_waiters[i])
            return 1;
    }
    return 0;
}

/* Find first waiter slot */
static inline int find_first_waiter(const struct ocsfs_lock_entry *le)
{
    for (int i = 0; i < 256; i++) {
        if (test_waiter_bit(le, i))
            return i;
    }
    return -1;
}

/* ─── Timestamp ─────────────────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ─── Lock Acquisition ──────────────────────────────────────── */

/*
 * Acquire a lock on a resource.
 *
 * resource_id:   hash of the resource (from ocsfs_lock_hash_*)
 * resource_type: OCSFS_LOCKRES_*
 * mode:          OCSFS_LOCK_SH or OCSFS_LOCK_EX
 * timeout_ms:    max wait time (0 = non-blocking)
 *
 * Returns 0 on success, -EAGAIN if non-blocking and conflicting,
 * -ETIMEDOUT if timed out, -EIO on I/O error.
 */
int ocsfs_lock_acquire(struct ocsfs_lock_mgr *mgr,
                        uint64_t resource_id, uint32_t resource_type,
                        uint16_t mode, uint32_t timeout_ms)
{
    uint32_t slot = ocsfs_lock_slot(resource_id);
    uint64_t deadline = (timeout_ms > 0) ? now_ns() + (uint64_t)timeout_ms * 1000000 : 0;
    uint32_t backoff_us = 1000; /* start at 1ms */
    int ret;

retry:
    ;
    struct ocsfs_lock_entry le;
    ret = read_lock_entry(mgr, slot, &le);
    if (ret < 0) return ret;

    /* Empty slot or NL — claim it */
    if (le.le_mode == OCSFS_LOCK_NL || le.le_resource_id == 0) {
        struct ocsfs_lock_entry new_le = le;
        new_le.le_magic = OCSFS_LOCK_MAGIC;
        new_le.le_resource_id = resource_id;
        new_le.le_resource_type = resource_type;
        new_le.le_mode = mode;
        new_le.le_holder_slot = mgr->node_slot;
        new_le.le_holder_gen = mgr->mount_gen;
        new_le.le_grant_time = now_ns();
        new_le.le_version = le.le_version + 1;
        new_le.le_checksum = ocsfs_crc32c(0, &new_le,
                                            sizeof(new_le) - sizeof(uint32_t));

        ret = cas_lock_entry(mgr, slot, &le, &new_le);
        if (ret == -EAGAIN) {
            mgr->stat_retries++;
            goto retry;
        }
        if (ret == 0)
            mgr->stat_acquires++;
        return ret;
    }

    /* Slot has a different resource — linear probe */
    if (le.le_resource_id != resource_id) {
        /* TODO: implement linear probing chain.
         * For now, use a secondary hash slot. */
        slot = (slot + 1) % OCSFS_LOCK_ENTRY_COUNT;
        /* In production, we'd probe up to MAX_PROBE_DEPTH slots */
        goto retry;
    }

    /* Same resource — check compatibility */
    if (locks_compatible(le.le_mode, mode)) {
        /* Compatible — add ourselves as co-holder */
        struct ocsfs_lock_entry new_le = le;

        if (mode == OCSFS_LOCK_SH) {
            /* Add to SH holders bitmask */
            if (mgr->node_slot < 32) {
                new_le.le_sh_holders |= (1U << mgr->node_slot);
            } else {
                new_le.le_sh_holders_ext[(mgr->node_slot - 32) / 8] |=
                    (1 << ((mgr->node_slot - 32) % 8));
            }
        }

        new_le.le_version = le.le_version + 1;
        new_le.le_checksum = ocsfs_crc32c(0, &new_le,
                                            sizeof(new_le) - sizeof(uint32_t));

        ret = cas_lock_entry(mgr, slot, &le, &new_le);
        if (ret == -EAGAIN) {
            mgr->stat_retries++;
            goto retry;
        }
        if (ret == 0)
            mgr->stat_acquires++;
        return ret;
    }

    /* Conflict! */
    mgr->stat_conflicts++;

    /* Non-blocking? */
    if (timeout_ms == 0)
        return -EAGAIN;

    /* Set waiter bit */
    {
        struct ocsfs_lock_entry new_le = le;
        set_waiter_bit(&new_le, mgr->node_slot);
        new_le.le_version = le.le_version + 1;
        new_le.le_checksum = ocsfs_crc32c(0, &new_le,
                                            sizeof(new_le) - sizeof(uint32_t));
        cas_lock_entry(mgr, slot, &le, &new_le);
        /* Don't care if this CAS fails — we'll retry anyway */
    }

    /* Wait and retry with exponential backoff */
    if (deadline > 0 && now_ns() >= deadline)
        return -ETIMEDOUT;

    usleep(backoff_us);
    if (backoff_us < 100000) /* cap at 100ms */
        backoff_us *= 2;

    goto retry;
}

/* ─── Lock Release ──────────────────────────────────────────── */

/*
 * Release a lock held by this node.
 */
int ocsfs_lock_release(struct ocsfs_lock_mgr *mgr,
                        uint64_t resource_id, uint32_t resource_type __attribute__((unused)))
{
    uint32_t slot = ocsfs_lock_slot(resource_id);
    int ret;

retry:
    ;
    struct ocsfs_lock_entry le;
    ret = read_lock_entry(mgr, slot, &le);
    if (ret < 0) return ret;

    /* Find the correct slot (may need probing) */
    if (le.le_resource_id != resource_id) {
        slot = (slot + 1) % OCSFS_LOCK_ENTRY_COUNT;
        ret = read_lock_entry(mgr, slot, &le);
        if (ret < 0) return ret;
        if (le.le_resource_id != resource_id)
            return -ENOENT; /* lock not found */
    }

    struct ocsfs_lock_entry new_le = le;

    if (le.le_mode == OCSFS_LOCK_EX) {
        /* Exclusive holder releasing */
        if (le.le_holder_slot != mgr->node_slot)
            return -EPERM;

        if (has_waiters(&le)) {
            /* Promote first waiter */
            int waiter = find_first_waiter(&le);
            if (waiter >= 0) {
                new_le.le_holder_slot = waiter;
                /* We don't know waiter's mount_gen here.
                 * In the real implementation, waiter_modes would include gen. */
                clear_waiter_bit(&new_le, waiter);
                /* Keep mode as EX (waiter was waiting for EX) */
            }
        } else {
            /* No waiters — clear lock */
            new_le.le_mode = OCSFS_LOCK_NL;
            new_le.le_holder_slot = 0;
            new_le.le_holder_gen = 0;
            new_le.le_grant_time = 0;
        }
    } else if (le.le_mode == OCSFS_LOCK_SH) {
        /* Remove from SH holders */
        if (mgr->node_slot < 32) {
            new_le.le_sh_holders &= ~(1U << mgr->node_slot);
        } else {
            new_le.le_sh_holders_ext[(mgr->node_slot - 32) / 8] &=
                ~(1 << ((mgr->node_slot - 32) % 8));
        }

        /* Check if we were the last SH holder */
        int any_sh = new_le.le_sh_holders != 0;
        if (!any_sh) {
            for (int i = 0; i < 32; i++) {
                if (new_le.le_sh_holders_ext[i]) {
                    any_sh = 1;
                    break;
                }
            }
        }

        if (!any_sh) {
            if (has_waiters(&new_le)) {
                int waiter = find_first_waiter(&new_le);
                if (waiter >= 0) {
                    new_le.le_mode = OCSFS_LOCK_EX; /* promote waiting EX */
                    new_le.le_holder_slot = waiter;
                    clear_waiter_bit(&new_le, waiter);
                }
            } else {
                new_le.le_mode = OCSFS_LOCK_NL;
                new_le.le_holder_slot = 0;
            }
        }
    }

    new_le.le_version = le.le_version + 1;
    new_le.le_checksum = ocsfs_crc32c(0, &new_le,
                                        sizeof(new_le) - sizeof(uint32_t));

    ret = cas_lock_entry(mgr, slot, &le, &new_le);
    if (ret == -EAGAIN) {
        mgr->stat_retries++;
        goto retry;
    }
    if (ret == 0)
        mgr->stat_releases++;
    return ret;
}

/* ─── Lock Recovery (for failed nodes) ──────────────────────── */

/*
 * Scan lock table and release all locks held by a failed node.
 * Called by the recovery leader after fencing.
 *
 * Returns the number of locks recovered.
 */
int ocsfs_lock_recover_node(struct ocsfs_lock_mgr *mgr,
                             uint16_t failed_slot, uint32_t failed_gen)
{
    int recovered = 0;

    for (uint32_t i = 0; i < OCSFS_LOCK_ENTRY_COUNT; i++) {
        struct ocsfs_lock_entry le;
        if (read_lock_entry(mgr, i, &le) < 0)
            continue;

        if (le.le_mode == OCSFS_LOCK_NL || le.le_resource_id == 0)
            continue;

        int needs_recovery = 0;

        /* Check EX holder */
        if ((le.le_mode == OCSFS_LOCK_EX || le.le_mode == OCSFS_LOCK_CW) &&
            le.le_holder_slot == failed_slot &&
            le.le_holder_gen == failed_gen) {
            needs_recovery = 1;
        }

        /* Check SH holders */
        if (le.le_mode == OCSFS_LOCK_SH) {
            if (failed_slot < 32) {
                if (le.le_sh_holders & (1U << failed_slot))
                    needs_recovery = 1;
            } else {
                if (le.le_sh_holders_ext[(failed_slot - 32) / 8] &
                    (1 << ((failed_slot - 32) % 8)))
                    needs_recovery = 1;
            }
        }

        /* Check waiters (remove from waiting list) */
        if (test_waiter_bit(&le, failed_slot)) {
            struct ocsfs_lock_entry new_le = le;
            clear_waiter_bit(&new_le, failed_slot);
            new_le.le_version++;
            new_le.le_checksum = ocsfs_crc32c(0, &new_le,
                                                sizeof(new_le) - sizeof(uint32_t));
            cas_lock_entry(mgr, i, &le, &new_le);
        }

        if (needs_recovery) {
            /* Temporarily become the holder to release properly */
            uint16_t saved_slot = mgr->node_slot;
            uint32_t saved_gen = mgr->mount_gen;
            mgr->node_slot = failed_slot;
            mgr->mount_gen = failed_gen;

            ocsfs_lock_release(mgr, le.le_resource_id, le.le_resource_type);

            mgr->node_slot = saved_slot;
            mgr->mount_gen = saved_gen;
            recovered++;
        }
    }

    return recovered;
}

/* ─── Statistics ────────────────────────────────────────────── */

void ocsfs_lock_mgr_stats(const struct ocsfs_lock_mgr *mgr, FILE *out)
{
    fprintf(out, "Lock Manager Statistics (node %u, gen %u):\n",
            mgr->node_slot, mgr->mount_gen);
    fprintf(out, "  Acquires:   %lu\n", (unsigned long)mgr->stat_acquires);
    fprintf(out, "  Releases:   %lu\n", (unsigned long)mgr->stat_releases);
    fprintf(out, "  Conflicts:  %lu\n", (unsigned long)mgr->stat_conflicts);
    fprintf(out, "  CAS retries:%lu\n", (unsigned long)mgr->stat_retries);
}
