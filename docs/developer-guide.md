# OCSFS — Developer Guide

**Version:** 0.1 — May 2026
**Status:** Alpha — research / development

---

## Table of Contents

1. [Architectural Overview](#1-architectural-overview)
2. [On-Disk Layout](#2-on-disk-layout)
3. [Key Data Structures](#3-key-data-structures)
4. [Kernel Module Subsystems](#4-kernel-module-subsystems)
5. [Distributed Locking Protocol](#5-distributed-locking-protocol)
6. [Heartbeat and Failure Detection](#6-heartbeat-and-failure-detection)
7. [5-Phase Recovery](#7-5-phase-recovery)
8. [Journal and Crash Recovery](#8-journal-and-crash-recovery)
9. [I/O Path](#9-io-path)
10. [Extent Management](#10-extent-management)
11. [Testbed Setup](#11-testbed-setup)
12. [Build and Development](#12-build-and-development)
13. [Debugfs Instrumentation](#13-debugfs-instrumentation)
14. [VAAI Storage Offload](#14-vaai-storage-offload)
15. [Open Issues and Limitations](#15-open-issues-and-limitations)
16. [Encryption (fscrypt)](#16-encryption-fscrypt)

---

## 1. Architectural Overview

OCSFS is a cluster-aware filesystem for Linux targeting shared block storage
(FC SAN / iSCSI) in Proxmox VE environments. The fundamental design
difference from GFS2 and OCFS2 is that **all lock state lives on disk** using
versioned CAS entries, with no external DLM daemon and no dependency on the
management network.

```
┌────────────────────────────────────────────────────────────┐
│  Proxmox VE  (PVE::Storage::OCSFSPlugin)                   │
├────────────────────────────────────────────────────────────┤
│  Linux VFS  (inode_operations, file_operations, aops)      │
├───────────┬───────────┬────────────────┬───────────────────┤
│ super.c   │ inode.c   │ dir.c          │ file.c + iomap.c  │
│ (mount)   │ (VFS ops) │ (dir ops)      │ (I/O path)        │
├───────────┴───────────┴────────────────┴───────────────────┤
│ extent.c  extent_btree.c  bitmap.c  alloc.c  thin.c        │
│ journal.c  journal_replay.c  snapshot.c  refcount.c        │
├────────────────────────────────────────────────────────────┤
│ lock.c  lock_io.c  heartbeat.c  node.c  recovery.c         │
│ scsi_pr.c  dedup.c  compress.c  xattr.c  acl.c             │
├────────────────────────────────────────────────────────────┤
│  Linux block layer — FC LUN / iSCSI / loopback             │
└────────────────────────────────────────────────────────────┘
```

### Source file responsibilities

| File | Responsibility |
|---|---|
| `super.c` | Module init/exit, mount, fill_super, statfs, sync_fs |
| `inode.c` | VFS inode ops: iget, write_inode, evict, new_inode, setattr |
| `dir.c` | Directory ops: lookup, create, mkdir, rmdir, readdir, link, mknod |
| `dir_btree.c` | B+ tree-backed directory index: O(log N) lookup and delete |
| `dir_rename.c` | rename, RENAME_NOREPLACE, RENAME_EXCHANGE (atomic single-txn) |
| `file.c` | File ops: read/write iter, fsync, FIEMAP, reflink (FICLONE), ioctl |
| `iomap.c` | iomap I/O: O_DIRECT, buffered, readahead, UNWRITTEN conversion |
| `extent.c` | Inline extent manager: lookup, insert, truncate, merge |
| `extent_btree.c` | B+ tree overflow for files with more than 16 extents |
| `alloc.c` | Smart allocator: prealloc, goal-oriented, AG affinity |
| `bitmap.c` | Per-AG block and inode bitmap; two-phase lockless allocation |
| `thin.c` | Thin provisioning: fallocate, PUNCH_HOLE, ZERO_RANGE, DISCARD |
| `journal.c` | WAL: txn begin/commit/abort, ordered checkpoint, before-image rollback |
| `journal_replay.c` | Forward-scan replay: CRC-gated COMMIT detection, redo on mount |
| `lock.c` | On-disk DLM: EX/SH acquire, release, downgrade, epoch invalidation |
| `lock_io.c` | Forced disk read/write for lock table (bypasses page cache) |
| `scsi_pr.c` | SCSI-3 PR: register, preempt-and-abort, SHA-256 key; `ocsfs_bsg_execute_cdb()` BSG-direct CAW |
| `heartbeat.c` | Storage-path heartbeat kthread; CRC-validated entries |
| `node.c` | Node slot table: claim, release, stable UUID via SHA-256(hostname) |
| `recovery.c` | 5-phase recovery orchestration |
| `snapshot.c` | CoW file snapshots: create, delete, CoW-on-write trigger |
| `refcount.c` | Per-AG extent reference counting for CoW and dedup |
| `compress.c` | Inline LZ4/ZSTD compression (read path + fsync write path) |
| `compress_file.c` | Compress-on-fsync for buffered files |
| `dedup.c` | Content-based deduplication via OCSFS_IOC_DEDUP ioctl |
| `xattr.c` | Extended attributes with DLM SH protection in cluster mode |
| `acl.c` | POSIX ACL (getfacl/setfacl) |
| `btree.c` | Generic B+ tree: search, insert, delete, range scan |
| `btree_mod.c` | B+ tree structural modifications: split, merge, rebalance |
| `test_lock.c` | KUnit tests: B+ tree search, dir btree threshold |
| `test_cas.c` | KUnit tests: CAS lock protocol |

---

## 2. On-Disk Layout

The volume occupies an entire block device. Regions are sequential from offset 0.

```
Offset 0        Superblock              (4 KB)
Offset 4 KB     Superblock mirror       (4 KB)
Offset 8 KB     Node Slot Table         (64 KB — 256 slots × 256 bytes)
Offset 72 KB    Heartbeat Region        (256 KB — 256 entries × 1 KB)
Offset 328 KB   Lock Table              (1 MB — 4096 entries × 256 bytes)
Offset ~1.3 MB  Journal Region          (N × 32 MB, N = max_nodes)
After journals  AG Descriptors          (ag_count × 4 KB)
After descs     Data Region             (Allocation Groups)
```

All multi-byte fields are **little-endian** on disk.

### Superblock (offsets 0 and 4 KB)

```c
struct ocsfs_disk_super {
    __le32 s_magic;           /* 0x4F435346 'OCSF' */
    __le16 s_version_major;   /* 0 */
    __le16 s_version_minor;   /* 1 */
    __u8   s_uuid[16];
    __u8   s_label[64];
    __le32 s_block_size;      /* 4096 (only supported value) */
    __le32 s_extent_size;     /* default 1 MB */
    __le64 s_total_blocks;
    __le64 s_free_blocks;     /* approximate; per-AG is authoritative */
    __le32 s_ag_count;
    __le64 s_ag_size;         /* blocks per AG */
    __le16 s_max_nodes;       /* default 64, max 256 */
    __le64 s_feature_flags;   /* OCSFS_FEAT_* */
    __le32 s_journal_size;    /* bytes per per-node journal */
    /* ... offsets, timestamps ... */
    __le32 s_checksum;        /* CRC32C of bytes 0..4091 */
};
```

CRC32C covers the entire superblock except the last 4 bytes (the checksum
field itself). The mirror superblock at 4 KB is written identically.

### Inode (512 bytes)

Each inode holds up to **16 inline extents** (16 × 24 = 384 bytes).
When more extents are needed, `i_extent_tree_root` holds the physical block
number of the B+ tree root, managed by `extent_btree.c`.

### Lock Table Entry (256 bytes)

```c
struct ocsfs_disk_lock {
    __le32 le_magic;           /* OCSFS_LOCK_MAGIC */
    __le64 le_resource_id;     /* resource hash */
    __le32 le_resource_type;   /* INODE, AG, JOURNAL, ... */
    __le16 le_mode;            /* NL / SH / EX / CW */
    __le16 le_holder_slot;     /* slot of the EX holder */
    __le32 le_holder_gen;      /* mount generation of the holder */
    __le64 le_grant_time;
    __le32 le_sh_holders;      /* bitmask: nodes 0-31 holding SH */
    __u8   le_sh_holders_ext[32]; /* nodes 32-255 */
    __u8   le_waiters[32];     /* bitmask: waiting nodes */
    __u8   le_waiter_modes[64];
    __le32 le_version;         /* CAS version counter */
    /* ... reserved + CRC32c checksum ... */
};
```

---

## 3. Key Data Structures

### `ocsfs_sb_info` — in-memory superblock (`sb->s_fs_info`)

| Field | Type | Description |
|---|---|---|
| `s_ags` | `ocsfs_ag_info[]` | AG array (kvmalloc at mount) |
| `s_journal` | `ocsfs_journal` | Current node's journal |
| `s_node_slot` | `u16` | Slot claimed at mount |
| `s_mount_gen` | `u32` | Current mount generation |
| `s_clustered` | `bool` | True when multi-node mode is active |
| `s_nodes[]` | `ocsfs_node_info[256]` | In-memory state of all peers |
| `s_hb` | `ocsfs_heartbeat_info` | Heartbeat kthread and state |
| `s_pr` | `ocsfs_pr_info` | Registered SCSI PR key |
| `s_lock_epoch` | `atomic_t` | Incremented on each recovery; invalidates SH cache |
| `s_recovery_work` | `work_struct` | Async recovery work item |

### `ocsfs_inode_info` — per-inode (`container_of(inode, ...)`)

```c
struct ocsfs_inode_info {
    u64                  i_disk_ino;
    u32                  i_ag;
    u32                  i_flags;            /* OCSFS_IFLAG_* */
    u16                  i_extent_count;
    struct ocsfs_extent  i_extents[16];      /* inline extents */
    u64                  i_extent_tree_root; /* B+ tree root block, or 0 */
    struct mutex         i_extent_lock;
    struct ocsfs_lock_res i_lock_res;        /* cross-node DLM resource */
    struct inode         vfs_inode;          /* MUST be last */
};
```

Access pattern: `OCSFS_I(inode)` → `container_of(inode, struct ocsfs_inode_info, vfs_inode)`

### `ocsfs_extent`

```c
struct ocsfs_extent {
    __le64 logical_block;
    __le64 physical_block;
    __le32 length;       /* logical block count */
    __le16 phys_length;  /* physical block count (compressed extents only; 0 = same as length) */
    __le16 flags;        /* OCSFS_EXT_WRITTEN / UNWRITTEN / COMPRESSED */
};
```

`ocsfs_ext_phys_blocks(e)` is the inline helper that returns `phys_length` if
non-zero (compressed), otherwise `length`. Always use this helper when
computing physical block counts — never use `e->length` directly for
compressed extents.

### `ocsfs_lock_res` — lock resource

```c
struct ocsfs_lock_res {
    u64          lr_resource_id;
    u32          lr_resource_type;
    u16          lr_mode;         /* currently held mode */
    u16          lr_slot;         /* slot in the lock table */
    bool         lr_cached;       /* SH cache active */
    u64          lr_cache_expires;
    u64          lr_cache_epoch;  /* epoch when cache was populated */
    struct mutex lr_mutex;        /* local serialization */
    struct list_head lr_list;
};
```

---

## 4. Kernel Module Subsystems

### Mount sequence (`super.c`)

```
ocsfs_fill_super()
  ├── forced read of block 0 → validate superblock (magic, version, CRC32c)
  ├── ocsfs_load_ags()         → reads all AG descriptors
  ├── ocsfs_cluster_init()     → node.c: claim slot + scsi_pr: register
  │     ├── ocsfs_node_claim_slot()   → writes ACTIVE entry, derives UUID
  │     ├── ocsfs_pr_register()       → SCSI-3 PR REGISTER
  │     └── ocsfs_heartbeat_start()   → spawns heartbeat kthread
  ├── ocsfs_journal_init()     → locates this node's journal region
  ├── ocsfs_journal_replay()   → crash recovery for this node
  └── ocsfs_iget(OCSFS_ROOT_INO) → mounts root inode
```

### Unmount sequence (`super.c`)

```
ocsfs_put_super()
  ├── ocsfs_journal_exit()     → drains in-flight checkpoints, writes header
  └── ocsfs_cluster_exit()
        ├── ocsfs_heartbeat_stop()     → wakes kthread, waits for exit
        ├── ocsfs_node_release_slot()  → marks slot INACTIVE on disk
        └── ocsfs_pr_unregister()      → SCSI-3 PR RELEASE
```

### Inode refresh (`inode.c:ocsfs_inode_refresh`)

Called by read/write/stat paths when the caller holds DLM SH. Re-reads the
inode from disk and populates all VFS fields. **Sprint D (ALTO-V3-3):** now
refreshes `i_mode`, `i_nlink`, `i_uid`, `i_gid`, and `i_atime` in addition
to the previously-refreshed size/timestamps/extents, so a remote `chmod` or
`chown` is visible without remounting.

---

## 5. Distributed Locking Protocol

### Lock compatibility matrix

| Held \ Requested | NL | SH | EX | CW |
|---|---|---|---|---|
| NL | ✅ | ✅ | ✅ | ✅ |
| SH | ✅ | ✅ | ❌ | ❌ |
| EX | ✅ | ❌ | ❌ | ❌ |
| CW | ✅ | ❌ | ❌ | ✅ |

### Lock acquire (`lock.c:ocsfs_lock_acquire`)

```
1. Epoch cache check: if lr_mode >= requested AND lr_lock_epoch == s_lock_epoch → return 0 (no disk)
2. ocsfs_lock_probe_slot()  → find physical slot (linear probing, max 16)
3. lock_read_entry()        → forced disk read (bypasses page cache)
4. Compatible?              → update entry via lock_write_entry() with version check; record lr_lock_epoch
5. Conflict?                → set_waiter_bit() + exponential backoff (1ms → 100ms, wall-clock deadline 30s)
```

**Atomicity:** Software versioning (read-version → check → write) as the CAS
layer; hardware atomicity via SCSI CAW (opcode 0x89) is used when a SCSI
device is detected. `ocsfs_bsg_execute_cdb()` is the entry point — see §13.

**DLM EX invariant:** All B+ tree write functions (`extent_btree_insert`,
`extent_btree_truncate`, `extent_btree_replace`, `dir_btree_insert`,
`dir_btree_delete`) assert `OCSFS_WARN_NO_EX(inode)` at entry. In cluster
mode this fires `WARN_ON` if EX is not held, catching callers that bypass
the protocol.

### Resource hashing

```c
/* Inode: FNV-1a mixing on inode number */
ocsfs_lock_hash_inode(ino)

/* AG: mixing with 0xA6... prefix */
ocsfs_lock_hash_ag(ag_num)

/* Slot in lock table (4096 entries) */
slot = resource_id % OCSFS_LOCK_ENTRY_COUNT
```

Collisions are handled by linear probing (max `OCSFS_LOCK_PROBE_MAX = 16` slots).

### Epoch-based lock cache (Sprint D — MEDIO-V3-1, ARCH-V3-7)

Each `ocsfs_lock_res` carries `lr_lock_epoch` (recorded from `sbi->s_lock_epoch`
after each disk acquisition). On the next `ocsfs_lock_acquire`, if
`lr_mode >= requested_mode` and `lr_lock_epoch == s_lock_epoch`, the disk
round-trip is skipped entirely — no probe, no read, no CAS. This eliminates
the per-inode disk access on repeated `stat`/`open`/`readdir` calls in
cluster mode, making `find`/`ls -lR` workloads practical.

Invalidation: `ocsfs_lock_recover_node()` atomically increments `sbi->s_lock_epoch`
after any node crash. All cached entries whose `lr_lock_epoch` is stale then
miss on the next acquire, forcing a fresh disk read. On lock release,
`lr_lock_epoch` is cleared to 0 so the next acquire always goes to disk.

### Selective page cache invalidation (ARCH-7)

When a node acquires a lock in either SH or EX mode, `lock_acquire` snapshots
the previous EX holder's dirty byte range (`dl.le_inv_lo / dl.le_inv_hi`) into
`lr->lr_inv_lo / lr->lr_inv_hi`.

- **Read path (`ocsfs_file_read_iter`):** If the previous writer was a
  different node and a valid range is recorded, `invalidate_mapping_pages()`
  flushes only those pages.  Pages outside the dirty range are untouched.
  Falls back to `invalidate_inode_pages2()` only when `lo == hi == 0` (no range
  recorded) or when the writer was this node (cache is coherent).

- **Write path (`ocsfs_file_write_iter`):** Same selective logic before the
  write starts.  After invalidation, `lr_inv_lo/hi` are zeroed so `iomap_end`
  records only this session's write range.

- **Release path (`ocsfs_lock_release`):** Stores `lr_inv_lo/hi` into
  `dl.le_inv_lo/hi` on disk and bumps `dl.le_inv_epoch` for future acquirers.

### Single-node mode

When `sbi->s_clustered == false` (device does not support PR), all lock
functions return immediately without disk I/O. The filesystem behaves as a
standard single-node filesystem.

---

## 6. Heartbeat and Failure Detection

The `ocsfs-hb/<slot>` kthread runs two operations on a timer:

| Operation | Interval | Action |
|---|---|---|
| Write heartbeat | `HB_INTERVAL_MS` = 5 s | Writes timestamp and monotone sequence to own sector |
| Check peers | `HB_CHECK_MS` = 2 s | Reads heartbeat of all ACTIVE nodes (batched by block) |

Heartbeat reads are batched: if multiple node slots share the same disk block,
a single forced read covers all of them, reducing I/O to up to 4× less than
one read per node.

**HB summary block (NUOV-ARCH-3, `OCSFS_FEATURE_RO_COMPAT_HB_SUMMARY`):** When
the feature bit is set (default on V2 filesystems), `ocsfs_heartbeat_write()` also
updates a 4 KiB summary block at `OCSFS_HB_SUMMARY_OFF` (one 16-byte entry per slot).
`ocsfs_heartbeat_check_peers()` reads this single block for O(1) peer checks, then
falls back to the slow batched path if the read fails.

**Sprint B (CRIT-V3-2) — atomic summary entry update:** The former implementation
used a plain full-block RMW, allowing two nodes to clobber each other's entries.
`ocsfs_hb_summary_update()` now uses SCSI CAW at sector granularity (512 B, the
same pattern as `lock_io.c`): it reads the 512-byte sector containing its 16-byte
entry, builds expected/new buffers, and retries up to 8× on `EAGAIN`. At most 32
nodes share a sector, so conflicts are rare and resolve quickly. Falls back to the
plain RMW when CAW is not available.

### Two-phase failure detection (`heartbeat.c`)

```
Heartbeat stale (> 15 s)   → ni_state = SUSPECTED, record ni_suspect_time
Still stale after 10 s     → ocsfs_recovery_trigger(sb, slot)
Heartbeat refreshed        → back to ACTIVE (transient slowdown)
```

The double window reduces false positives from transient storage path
congestion. CRC32c is validated on every heartbeat entry before accepting the
timestamp.

`kthread_stop()` triggers immediate wakeup via `wake_up(&hb_waitq)` rather
than waiting for the next timer expiry.

---

## 7. 5-Phase Recovery

Orchestrated by `recovery.c`. Only the surviving node with the lowest slot
number becomes the recovery leader.

| Phase | Action | Location |
|---|---|---|
| 1 — Leader election | Lowest-slot surviving node wins | `recovery.c:ocsfs_is_recovery_leader()` |
| 2 — SCSI PR fencing | `PREEMPT_AND_ABORT` using the dead node's PR key | `scsi_pr.c:ocsfs_pr_preempt_abort()` |
| 3 — Journal replay | Replay the dead node's journal | `journal_replay.c:ocsfs_journal_replay_node()` |
| 4 — Lock recovery | Scan entire lock table; release locks owned by dead node | `lock.c:ocsfs_lock_recover_node()` |
| 5 — Slot cleanup | Mark slot as DEAD on disk | `node.c:ocsfs_node_mark_dead()` |

Recovery is asynchronous (`schedule_work`) and serialized by `s_recovery_lock`.

If fencing fails and the device is PR-capable, `recovery.c` forces `SB_RDONLY`
and aborts rather than proceeding with potentially split-brain state. On
non-PR devices (degraded mode), a warning is logged and recovery continues.

**Current limitation:** Only one recovery target at a time (`s_recovery_target`
is a single `u16`). If two nodes fail simultaneously, the second is not
recovered until the first recovery completes.

**Sprint C (2026-05-28) — recovery robustness:**

| Issue | Fix | Detail |
|---|---|---|
| ALTO-V3-4: busy-wait livelock on -EAGAIN | Exponential backoff 50 ms → 5 s | `ocsfs_recovery_work_fn`: `eagain_ms` doubles each contended round, caps at `OCSFS_RECOVERY_EAGAIN_MAX_MS` (5000 ms); resets on success or next slot |
| ALTO-V3-5: umount drops in-flight recovery | `flush_work` before `cancel_work_sync` | `ocsfs_recovery_exit`: drains current execution first; warns if pending bits remain after drain |
| MEDIO-V3-3: lock recovery misses overflow chain | Traverse ARCH-2 overflow chain per primary slot | `ocsfs_lock_recover_node`: after processing each primary entry follows `le_overflow_block` chain via `lock_read_entry_at_addr` / `lock_write_entry_at_addr`; helper `ocsfs_lock_recover_entry` avoids code duplication |

---

## 8. Journal and Crash Recovery

Per-node circular journal, 32 MB by default. Structure:

```
[Journal Header 4 KB][TXN Begin][Block Ref][Block Data]...[TXN Commit][...]
```

### Transaction lifecycle

```c
txn = ocsfs_txn_begin(sb);
ocsfs_txn_add_bh(txn, bh);    /* snapshots before-image into txn->bufs[i].before_buf */
/* ... modify bh ... */
ocsfs_txn_commit(txn);         /* writes after-image + COMMIT record; waits for flush */
```

`ocsfs_txn_abort()` restores all modified buffers from their `before_buf`
snapshots and clears `buffer_dirty`, ensuring that partial in-memory changes
do not survive an abort.

### Replay (`journal_replay.c`)

- Forward scan from journal tail to head.
- A `TXN_BEGIN` record without a matching `TXN_COMMIT` (CRC-validated) is
  discarded — uncommitted transactions are not replayed.
- AFTER-images are applied only when the on-disk block content matches the
  stored BEFORE-image hash, preventing replay from overwriting concurrent writes
  from surviving nodes.

**Sprint E (CRIT-V3-3) — 62-bit BEFORE-image hash:**
The previous 32-bit CRC32C (`jbr_checksum`) could produce false-positive
matches on filesystems with billions of blocks (~1/4B probability per block).
A false match causes a stale AFTER-image to be applied to a block written by
a live peer — silent data corruption.

Fix: a secondary CRC32C (seed `~1U`) is packed into `jbr_flags[31:2]`
(`OCSFS_JBR_HASH2_MASK`), giving 30 additional bits. Combined 62-bit hash
reduces false-positive probability to ~1/4.6×10¹⁸. Both writer (`journal.c`)
and replay reader (`journal_replay.c`) updated. The structure `ocsfs_disk_journal_bref`
is unchanged — bits 0-1 of `jbr_flags` were already used for BEFORE/AFTER; bits
2-31 were previously zero.

### Dedup safety (Sprint E — CRIT-V3-4, MEDIO-V3-11)

**CRIT-V3-4:** `dedup_apply_pair` called `ocsfs_extent_lookup` and
`ocsfs_extent_btree_replace` without `i_extent_lock`, creating a data race
with concurrent truncate that could silently corrupt the extent map. Both
calls are now wrapped under `mutex_lock(&oi->i_extent_lock)`.

**MEDIO-V3-11:** `dedup_blocks_equal` used `sb_bread` (page-cache) for
comparison. In cluster mode, a peer may have modified one of the blocks
since it was cached, producing a false content match and causing dedup to
collapse two logically distinct files onto the same physical blocks. Now uses
`sb_getblk` + `bh_read` (forced disk read) in cluster mode.

### Ordered checkpoint

`journal.c` uses a ticket-based checkpoint system (`j_ckpt_ticket`,
`j_ckpt_now`, `j_ckpt_waitq`). The journal lock is released after the
`COMMIT` record is durable; checkpoint I/O runs outside the lock. FIFO
ordering via `wait_event(j_ckpt_waitq, j_ckpt_now == my_ticket)` ensures the
journal tail advances monotonically.

### Security hardening (Sprint F — SEC-V3-1/4/7/8)

**SEC-V3-1 — Node slot post-CAS auth re-verify (`node.c`):**
After a successful CAS write in `ocsfs_node_claim_slot`, the slot is
immediately re-read from disk and `ocsfs_node_verify_auth` is called on the
read-back data. A mismatch (wrong cluster secret, CAS implementation bug in
storage array) causes the slot to be released via `ocsfs_node_release_slot`
and `-EACCES` to be returned, preventing a node with the wrong secret from
joining the cluster. Active only when `sbi->s_auth_required` is set.

**SEC-V3-4 — VAAI WRITE SAME metadata guard (`vaai.c`):**
`ocsfs_vaai_write_same` now rejects requests where `arg.offset <
sbi->s_data_off`. Without this check a `CAP_SYS_ADMIN` caller could silently
overwrite the superblock, journal, lock table, or node table via the SCSI
WRITE SAME(16) offload path. Returns `-EPERM` for out-of-bounds targets.

**SEC-V3-7 — Snapshot create EROFS check (`file.c`):**
`OCSFS_IOC_SNAP_CREATE` now returns `-EROFS` immediately if
`sb->s_flags & SB_RDONLY`. Previously the call propagated into
`ocsfs_snapshot_create` and failed silently or with a confusing error deep in
the journal path.

**SEC-V3-8 — Dedup ioctl rate-limit (`file.c`, `ocsfs.h`):**
`OCSFS_IOC_DEDUP` enforces a per-inode minimum interval of 60 seconds via
`i_dedup_last_jiffies` (new field in `ocsfs_inode_info`). Returns `-EBUSY`
if called again within the window. Prevents a file owner from continuously
triggering expensive full-file dedup scans as a local DoS against the work
queues and I/O scheduler.

---

## 9. I/O Path

### Data (regular files) — iomap path

```
write_iter → ocsfs_file_write_iter()
  ├── Cluster mode: acquire DLM EX (before inode_lock to avoid ABBA)
  ├── O_DIRECT:  iomap_dio_rw() → ocsfs_iomap_ops → extent_lookup / alloc
  └── Buffered:  iomap_file_buffered_write() → page cache → writepages
                 iomap_end() converts UNWRITTEN → WRITTEN after bytes are written
```

### Metadata (directory, inode) — buffer_head path

```
dir lookup → ocsfs_dir_bread() → ocsfs_extent_lookup() → sb_getblk() + bh_read()
```

In cluster mode, directory block reads use forced I/O (bypasses page cache)
to avoid stale data from peer writes.

### Preallocation and UNWRITTEN extents

`alloc.c` uses goal-oriented block allocation with AG affinity. New blocks
are initially marked `OCSFS_EXT_UNWRITTEN`; reads from unwritten ranges
return zeroes without I/O. `iomap_end()` converts the range to `WRITTEN`
after the write completes successfully.

### Two-phase block allocation (`bitmap.c`)

```
Phase 1 (lockless):
  READ_ONCE(ag.free_blocks) for each AG — no txn, no lock
  Select the first AG with enough free blocks

Phase 2 (locked):
  ocsfs_txn_begin() + DLM EX only for the chosen AG
  Actual bitmap scan and allocation
  Fallback: if chosen AG was stale, try remaining AGs
```

This avoids holding the journal lock (`j_lock`) during a full multi-AG scan.

---

## 10. Extent Management

### Inline extents (`extent.c`)

Up to 16 extents are stored directly in the inode, sorted by `logical_block`.
Adjacent extents with compatible flags are merged on insert. The
`try_merge_next` label handles the three-way merge case (extend-at-end →
absorb-next).

### B+ tree overflow (`extent_btree.c`)

When the 16-slot inline array is full, `ocsfs_extent_btree_migrate()` is
called before the 17th insert. It:
1. Opens a txn
2. Decompresses any compressed extents first (btree encoding has no
   `phys_length` field)
3. Inserts all inline extents into the B+ tree
4. Sets `i_extent_tree_root` and clears `i_extent_count`

All B+ tree write operations assert `OCSFS_WARN_NO_EX(inode)` in cluster
mode (see §5).

### Compressed extents

`e->phys_length` stores the actual number of physical blocks consumed by a
compressed extent. `e->length` always stores the logical block count.
Always use `ocsfs_ext_phys_blocks(e)` for any calculation that involves
freeing or allocating physical blocks.

When truncating or punching a hole into a compressed extent, the extent is
decompressed in-place first so that the tail is addressable as plain blocks.

---

## 11. Testbed Setup

The minimum viable cluster testbed for testing OCSFS:

### KVM + LIO iSCSI (zero additional hardware)

```
Host (any Linux machine with 16+ GB RAM and KVM)
├── LIO iSCSI target  ─────────────────────────┐
│   1 × 50 GB zvol or file                      │ SCSI-3 PR
├── VM ocsfs-node1 (4 GB RAM, 4 vCPU)  ─────────┤
├── VM ocsfs-node2 (4 GB RAM, 4 vCPU)  ─────────┤
└── VM ocsfs-node3 (4 GB RAM, 4 vCPU)  ─────────┘
    (all VMs connect to the LUN directly via open-iscsi)
```

Install targetcli on the host:

```bash
pip install targetcli-fb
modprobe target_core_mod configfs
mount -t configfs configfs /sys/kernel/config 2>/dev/null || true
```

Configure the target:

```bash
targetcli

/backstores/fileio create name=ocsfs-lun file_or_dev=/srv/ocsfs-shared.img size=50G
/backstores/fileio/ocsfs-lun set attribute emulate_pr=1
/iscsi create iqn.2026-05.example:ocsfs-storage
/iscsi/iqn.2026-05.example:ocsfs-storage/tpg1/luns create /backstores/fileio/ocsfs-lun
/iscsi/iqn.2026-05.example:ocsfs-storage/tpg1/acls create iqn.2026-05.example:node1
/iscsi/iqn.2026-05.example:ocsfs-storage/tpg1/acls create iqn.2026-05.example:node2
/iscsi/iqn.2026-05.example:ocsfs-storage/tpg1/acls create iqn.2026-05.example:node3
/iscsi/iqn.2026-05.example:ocsfs-storage/tpg1 set attribute authentication=0
saveconfig
exit
```

Inside each VM:

```bash
echo "InitiatorName=iqn.2026-05.example:node1" > /etc/iscsi/initiatorname.iscsi
systemctl restart iscsid
iscsiadm -m discovery -t sendtargets -p <host-ip>:3260
iscsiadm -m node --targetname iqn.2026-05.example:ocsfs-storage --login
# shared disk now appears as /dev/sdb (or similar)
```

### TrueNAS SCALE (external NAS)

TrueNAS SCALE uses LIO as its iSCSI backend and supports SCSI-3 PR natively.
Configure the iSCSI target and extent in the TrueNAS UI, then connect from
each VM using `open-iscsi` as above.

### Deploy the module to all nodes

From the development machine:

```bash
cd /path/to/OCSFS/kmod && make -j$(nproc)

for IP in 10.0.0.11 10.0.0.12 10.0.0.13; do
  rsync -az kmod/ user@$IP:/opt/ocsfs/kmod/
  ssh user@$IP "cd /opt/ocsfs/kmod && make -j\$(nproc) \
    && sudo rmmod ocsfs 2>/dev/null; sudo insmod ocsfs.ko"
done
```

---

## 12. Build and Development

### Dependencies

```bash
# Debian / Ubuntu / Proxmox VE
apt install build-essential uuid-dev linux-headers-$(uname -r)

# For the FUSE prototype
apt install libfuse3-dev
```

### Build targets

```bash
make all          # userspace tools + FUSE prototype
make test         # run userspace test suite (36 tests)
make kmod         # kernel module (alias for: cd kmod && make)
make demo         # format a 1 GiB loopback image and inspect it

sudo dkms add kmod/ && sudo dkms build ocsfs/0.1.0 && sudo dkms install ocsfs/0.1.0
dpkg-buildpackage -us -uc -b   # build Debian packages
```

### Recommended kernel config for development VMs

```
CONFIG_KASAN=y             # AddressSanitizer — catches use-after-free
CONFIG_LOCKDEP=y           # Lock ordering validator
CONFIG_LOCK_STAT=y         # Lock contention statistics
CONFIG_DEBUG_PAGEALLOC=y   # Page use-after-free detection
CONFIG_FAULT_INJECTION=y   # Simulate allocation failures
CONFIG_FAILSLAB=y          # Simulate slab allocation failures
CONFIG_KUNIT=y             # KUnit test harness
CONFIG_CRASH_DUMP=y        # kdump support
CONFIG_KEXEC=y             # Required for kdump
```

### Running KUnit tests

```bash
# Load the module with KUnit enabled (built into the module)
sudo insmod kmod/ocsfs.ko

# Results appear in dmesg
dmesg | grep -E "KTAP|PASS|FAIL|ocsfs"
```

### Quick single-node test

```bash
dd if=/dev/zero of=/tmp/test.img bs=1M count=2048
./mkfs.ocsfs -L test -N 4 -f /tmp/test.img

sudo losetup /dev/loop0 /tmp/test.img
sudo insmod kmod/ocsfs.ko
sudo mount -t ocsfs /dev/loop0 /mnt/ocsfs

ls /mnt/ocsfs && df /mnt/ocsfs

sudo umount /mnt/ocsfs
sudo rmmod ocsfs
sudo losetup -d /dev/loop0
```

---

## 13. Debugfs Instrumentation

OCSFS exposes runtime state via the kernel debugfs at
`/sys/kernel/debug/ocsfs/<devname>/` (created by `debugfs.c`).

| File | Content |
|---|---|
| `lock_table` | One line per active in-memory `ocsfs_lock_res`: resource_id, mode, type, slot, overflow address |
| `journal_stats` | Journal head, tail, size, used bytes, sequence counter, checkpoint ticket/now counters |

**Usage examples:**

```bash
# Show all cluster locks currently held or contested
cat /sys/kernel/debug/ocsfs/sdb/lock_table

# Monitor journal fill ratio
watch -n1 cat /sys/kernel/debug/ocsfs/sdb/journal_stats
```

The directory is created in `fill_super` and removed in `put_super`.
The root directory `/sys/kernel/debug/ocsfs/` is created at module load
and removed at module unload.

---

## 14. VAAI Storage Offload

`vaai.c` implements three SCSI offload operations via `ocsfs_bsg_execute_cdb()`
(the same BSG-direct path used by CAW and PR). All are best-effort: a device
CHECK CONDITION causes the ioctl to return an error and the caller falls back
to normal host-side I/O (same behaviour as VMFS VAAI).

| ioctl | SCSI opcode | Description |
|---|---|---|
| `OCSFS_IOC_WRITE_SAME` | 0x93 — WRITE SAME(16), NDOB bit | Zero-fill a block range without data transfer |
| `OCSFS_IOC_UNMAP` | 0x42 — UNMAP(10) | TRIM/discard a block range (VM delete, thin reclaim) |
| `OCSFS_IOC_XCOPY` | 0x83 — EXTENDED COPY | Server-side block copy within the same LUN (VM clone offload) |

All three require `CAP_SYS_ADMIN`. Arguments are byte-aligned
(`ocsfs_vaai_arg` / `ocsfs_vaai_xcopy_arg`); block-alignment is validated
inside the kernel.

**Usage (from userspace):**

```c
struct ocsfs_vaai_arg arg = { .offset = 0, .length = 512 * 1024 };
ioctl(fd, OCSFS_IOC_WRITE_SAME, &arg);   /* zero first 512 KiB */
ioctl(fd, OCSFS_IOC_UNMAP, &arg);        /* discard first 512 KiB */

struct ocsfs_vaai_xcopy_arg xarg = {
    .src_offset = 0,
    .dst_offset = 512 * 1024,
    .length     = 512 * 1024,
};
ioctl(fd, OCSFS_IOC_XCOPY, &xarg);       /* server-side copy */
```

---

## 15. Open Issues and Limitations

### Blocking for multi-node production use

**SCSI CAW — implemented via BSG-direct**

`ocsfs_bsg_execute_cdb()` in `scsi_pr.c` is the unified entry point for
sending raw CDBs. It uses a two-path design:

1. **BSG-direct (primary):** reads `scsi_device *` from `q->queuedata` — the
   pointer placed there by `scsi_mq_setup_tags()`. Works on hardened kernels
   where `CONFIG_KPROBES=n`. Validated with `sdev->host != NULL`.
2. **kprobe shim (fallback):** resolves the unexported
   `scsi_device_from_queue()` via `register_kprobe()`. Kept for compatibility
   with unpatched kernels that cannot use the BSG-direct path.

`ocsfs_scsi_caw()` calls `ocsfs_bsg_execute_cdb()` with a pre-built CAW CDB
(opcode 0x89, SBC-4 §5.3): `expected || new_data` in a mempool-backed buffer.
`lock_write_entry()` uses CAW when `s_caw_supported` is set.

The kernel patch in `docs/kernel-patches/` exports `scsi_device_from_queue()`
unconditionally — that is the upstream path if the BSG-direct workaround is
ever rejected.

**No integration test suite**

No xfstests run has been performed. The minimum testbed (2 nodes + LIO
iSCSI) is sufficient to run `xfstests quick` and `xfstests auto`. This
is now the top priority.

### Architectural limitations

| Gap | Impact | Path to fix |
|---|---|---|
| Single recovery target | Second node death during recovery is not handled | Change `s_recovery_target` to a bitmask and process the queue serially |
| Snapshot / refcount table fill-up | Resolved on V2 volumes via per-AG refcount B+ tree (ARCH-5, `INCOMPAT_RC_BTREE_PER_AG`). Tree grows by allocating blocks from the AG itself — no fixed-size limit. Returns `-EOPNOTSUPP` on V1 only. | Upgrade V1 volumes with `ocsfs-tool tune --upgrade` |
| Compression write path (O_DIRECT) | O_DIRECT writes are never compressed | Architectural: O_DIRECT bypasses the page cache where compression hooks live |
| Shared mmap in cluster mode | `MAP_SHARED|PROT_WRITE` returns `-EOPNOTSUPP` | Would require distributed cache coherence — out of scope for v0.1 |
| POSIX distributed file locking | Implemented at inode granularity (`flock.c`): `F_RDLCK`→DLM SH, `F_WRLCK`→DLM EX. Same-node byte-range semantics via `posix_lock_file`; cross-node coherence via on-disk DLM. Multiple SH holders on the same inode will serialize if any node holds EX (DLM release/re-acquire path). | — implemented |
| Encryption | Implemented via fscrypt (`crypto.c`). Per-directory policy; bounce-page I/O. Cluster-safe write path (Sprint A). Reflink/snapshot/symlinks in encrypted dirs return `-EOPNOTSUPP`. Node-local keys (ARCH-V3-1 open). | See §16 |
| Quota | Implemented: `i_dquot[MAXQUOTAS]` in `ocsfs_inode_info`; inode quota via `dquot_alloc/free_inode`; block quota via `dquot_alloc/free_space_nodirty` in `ocsfs_iomap_begin` and `ocsfs_alloc_extent`. Block quota is not charged for CoW, snapshot creation, or directory/metadata blocks. | Commits `8bc4c38` (inode) + `58933a7` (block) |
| No out-of-band STONITH | Fencing relies solely on SCSI PR | Wire Proxmox API or iDRAC as a fallback fencing agent |
| Node table TOCTOU | Mitigated by SCSI CAW (BSG-direct, now implemented) | Full fix requires per-device PR-scoped reservation on slot claim |

---

## 16. Encryption (fscrypt)

OCSFS supports optional per-directory encryption via the Linux fscrypt
framework (`CONFIG_FS_ENCRYPTION=y`). Encryption is entirely opt-in: an
unencrypted filesystem behaves identically to previous versions.

### Design

| Component | Implementation |
|---|---|
| Context storage | xattr `security.c` on the directory inode (`ocsfs_fscrypt_ops.get/set_context`) |
| Key management | Standard fscrypt key ring: `FS_IOC_ADD_ENCRYPTION_KEY` / `FS_IOC_REMOVE_ENCRYPTION_KEY` |
| Policy | Per-directory: `FS_IOC_SET_ENCRYPTION_POLICY` on an empty directory |
| Data path | Bounce pages (`needs_bounce_pages=1`): encrypted read/write handled in `ocsfs_enc_read_folio()` / `ocsfs_enc_writepages()` |
| inode_info_offs | `ptrdiff_t` offset of `i_crypt_info` relative to `vfs_inode` inside `ocsfs_inode_info` — required by the fscrypt ABI |

### Enabling encryption on a directory

```bash
# Add a key to the filesystem keyring
fscryptctl add_key /mnt/ocsfs

# Set an encryption policy on an empty directory
fscryptctl set_policy <key_identifier> /mnt/ocsfs/private

# All files created inside will be encrypted automatically
echo "hello" > /mnt/ocsfs/private/secret.txt
```

Alternatively, use `ioctl` directly:

```c
struct fscrypt_policy_v2 policy = {
    .version            = FSCRYPT_POLICY_V2,
    .contents_encryption_mode  = FSCRYPT_MODE_AES_256_XTS,
    .filenames_encryption_mode = FSCRYPT_MODE_AES_256_CTS,
    .flags              = 0,
};
memcpy(policy.master_key_identifier, key_id, FSCRYPT_KEY_IDENTIFIER_SIZE);
ioctl(dirfd, FS_IOC_SET_ENCRYPTION_POLICY, &policy);
```

### Supported ioctls

| ioctl | Description |
|---|---|
| `FS_IOC_SET_ENCRYPTION_POLICY` | Set per-directory encryption policy |
| `FS_IOC_GET_ENCRYPTION_POLICY_EX` | Read current policy |
| `FS_IOC_ADD_ENCRYPTION_KEY` | Add a master key to the filesystem |
| `FS_IOC_REMOVE_ENCRYPTION_KEY` | Remove a master key (current user) |
| `FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS` | Remove a master key (all users, requires `CAP_SYS_ADMIN`) |
| `FS_IOC_GET_ENCRYPTION_KEY_STATUS` | Query key presence |
| `FS_IOC_GET_ENCRYPTION_NONCE` | Retrieve per-inode nonce |

### Data path

**Read:** `ocsfs_enc_read_folio()` reads each filesystem block synchronously
via `sb_bread()`, copies the data into the folio, zero-fills holes, then
calls `fscrypt_decrypt_pagecache_blocks()` to decrypt the folio in-place
before marking it uptodate.

**Write:** `ocsfs_enc_writepages()` (called from `ocsfs_writepages()`)
iterates dirty folios with `writeback_iter()`. For each folio it calls
`fscrypt_encrypt_pagecache_blocks()` to obtain an encrypted bounce `struct page *`,
then submits a synchronous bio via `bio_add_page()` + `submit_bio_wait()`.
The bounce page is freed with `fscrypt_free_bounce_page()` after I/O.

### Cluster safety (Sprint A — 2026-05-28)

The following correctness issues identified in the Opus v3 review have been fixed:

| Issue | Fix |
|---|---|
| Async writeback without DLM EX (CRIT-V3-1) | `ocsfs_enc_writepages()` skips early if `lr_mode != OCSFS_LOCK_EX`. Background writeback (kswapd, bdi_writeback) is a no-op; `ocsfs_file_write_iter()` flushes dirty pages under DLM EX via `filemap_write_and_wait()` before releasing. |
| Reflink of encrypted file (CRIT-V3-5) | `ocsfs_remap_file_range()` returns `-EOPNOTSUPP` if either source or destination is encrypted. Sharing physical blocks with different fscrypt IVs produces unreadable ciphertext. |
| Snapshot of encrypted file (CRIT-V3-6) | `ocsfs_snapshot_create()` returns `-EOPNOTSUPP` for encrypted inodes. The snapshot inode has a different `i_ino`, so fscrypt derives a different IV and reads produce garbage. |
| Symlinks in encrypted directories (CRIT-V3-7) | `ocsfs_symlink()` returns `-EOPNOTSUPP` if the parent directory is encrypted (`fscrypt_get_symlink` plumbing not yet implemented). `ocsfs_iget()` no longer loads inline ciphertext as a plaintext symlink target for encrypted inodes. |
| `fscrypt_set_context` after `add_dirent` (ALTO-V3-7) | In `ocsfs_create()` and `ocsfs_mkdir()`, the encryption context is now persisted and flushed to disk **before** the directory entry is added to the parent. This closes the window where a peer node could see the new inode without a crypto context and write plaintext. |
| `i_crypt_info` not reset on slab reuse (ALTO-V3-1) | `ocsfs_alloc_inode()` now initialises `oi->i_crypt_info = NULL`. |
| `enc_read_folio` without DLM SH (ALTO-V3-6) | Added `WARN_ONCE` in `ocsfs_enc_read_folio()` to surface call sites (splice_read, userfaultfd) that arrive without DLM SH in cluster mode. |

**Architectural gap (ARCH-V3-1, not yet fixed):** fscrypt keys are node-local. A node that has not added the master key will write plaintext to files that other nodes encrypted. `FS_IOC_ADD_ENCRYPTION_KEY` emits a `pr_warn_once` reminding operators to add the key on all cluster nodes. A cluster-wide key propagation protocol requires dedicated engineering effort.

### Limitations

| Limitation | Notes |
|---|---|
| No readahead | Encrypted inodes return early from `ocsfs_iomap_readahead()` — the iomap-based readahead path cannot decrypt asynchronously |
| No O_DIRECT | O_DIRECT bypasses the page cache; bounce-page decryption requires the page cache |
| Buffered writes only | `ocsfs_enc_writepages()` submits one synchronous bio per folio — acceptable for VM disk images, suboptimal for bulk streaming |
| No reflink / snapshot | Both operations return `-EOPNOTSUPP` on encrypted inodes (see cluster safety above) |
| No symlinks in encrypted dirs | Returns `-EOPNOTSUPP` until `fscrypt_get_symlink` is wired up |
| Node-local keys | Key must be added independently on each cluster node (`FS_IOC_ADD_ENCRYPTION_KEY`); warns at runtime if in cluster mode |
