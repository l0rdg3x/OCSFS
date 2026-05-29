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

### Architectural hardening (Sprint G — ARCH-V3-2/4/5/6)

**ARCH-V3-2 — Superblock mirror fallback (`super.c`, `ocsfs.h`):**
A mirror copy of the superblock is kept at block 1 (byte offset
`OCSFS_SUPERBLOCK_MIRROR` = 4096, one block after the primary at block 0).
During mount `ocsfs_fill_super` tries the primary first; if it fails CRC
validation or returns an I/O error, it transparently falls back to the mirror
with a `pr_warn` message advising the operator to run fsck to repair the
primary. The mirror is kept in sync by `ocsfs_update_super_mirror` (called
after the mount-count write and from `ocsfs_sync_fs`) via `sb_getblk` +
`sync_dirty_buffer` without opening a journal transaction (the mirror is
not journaled — it is a best-effort safety net, not a primary metadata path).

**ARCH-V3-4 — Extent btree 4-bit flags (`extent_btree.c`, `ocsfs.h`,
`include/ocsfs.h`, `tools/mkfs_ocsfs.c`):**
The v1 on-disk extent encoding packs bits as `[39:0]=phys, [61:40]=len
(22-bit), [63:62]=flags (2-bit)`. With only 2 flag bits the `ENCRYPTED`
flag (0x4) was silently dropped on btree round-trips, making large encrypted
files (beyond the 16-inline-extent threshold) unreadable after the first
write. The v2 encoding (feature bit `OCSFS_FEATURE_INCOMPAT_EXT_FLAGS4`)
repacks as `[39:0]=phys, [59:40]=len (20-bit), [63:60]=flags (4-bit)`,
trading 2 extent-length bits (max 1 M blocks instead of 4 M blocks per
extent) for 4 flag bits. All btree helpers (`ext_encode`, `ext_len`,
`ext_flags`) now take a `bool f4` parameter precomputed from
`OCSFS_SB(sb)->s_ext_flags4` at function entry; `ext_phys` is unchanged
(lower 40 bits are identical in both encodings). `mkfs.ocsfs` sets the
feature bit for all new volumes.

**ARCH-V3-5 — Lock delegation/lease (deferred):**
Full delegation requires a new on-disk signaling mechanism so the leaseholder
is notified when a peer enqueues for the same lock. This cannot be
retrofitted without a protocol break (on-disk lock entry redesign). The
epoch-based lock cache introduced in Sprint D already eliminates the
dominant overhead (disk round-trip on same-mode re-acquire after a
recovery-free period). ARCH-V3-5 is tracked as a future protocol revision;
estimated effort: ~40h.

**ARCH-V3-6 — Cluster-wide filesystem freeze (`super.c`, `file.c`,
`ocsfs.h`):**
`OCSFS_IOC_FREEZE_FS` and `OCSFS_IOC_THAW_FS` ioctls (added to
`ocsfs_ioctl` in `file.c`, both require `CAP_SYS_ADMIN`) call the VFS
`freeze_super(sb, FREEZE_HOLDER_USERSPACE, NULL)` and
`thaw_super(sb, FREEZE_HOLDER_USERSPACE, NULL)` helpers. The VFS freeze
drains in-flight writes and prevents new ones from entering the data path,
which is necessary for consistent backup snapshots and live VM migration.
In cluster mode, `ocsfs_freeze_fs` (wired into `super_operations`) acquires
a DLM EX lock on `sbi->s_freeze_lock_res` (type `OCSFS_LOCKRES_FREEZE`,
initialized during mount) so only one cluster node holds the freeze
coordinator role at a time. `ocsfs_unfreeze_fs` releases the lock. Single-node
mounts skip the DLM step and rely on the VFS freeze alone.

### Security and correctness hardening (Sprint H — SEC-V3-5/6, ALTO-V3-2/8/9, MEDIO-V3-2/4/8/9/10/12)

**SEC-V3-5 — CAS bad-input guard (`cas.c`):**
`ocsfs_atomic_cas` replaces `WARN_ON(boff + len > sb->s_blocksize)` with a
plain `return -EINVAL`. `WARN_ON` is intended for programmer-visible invariants,
not for user-supplied coordinates that could be invalid from a malformed ioctl.
The old form produced a kernel backtrace on every bad call, which could be
used to flood dmesg under `CAP_SYS_ADMIN`.

**SEC-V3-6 — HMAC replay attack fix (`node.c`):**
The node auth token is now `HMAC-SHA256(secret, cluster_uuid || node_uuid ||
mount_gen_be32)`. The previous constant `"ocsfs-v1"` message meant a token
issued by any cluster sharing the same secret was valid on any other — a
node could replay a stolen slot token across a cluster boundary. The new
message includes the 16-byte cluster UUID (`sbi->s_ds->s_uuid`) and the
32-bit mount generation, scoping tokens to a specific cluster and mount
instance. `ocsfs_build_auth_msg` is a shared helper used by both
`ocsfs_build_new_slot` (writer) and `ocsfs_node_verify_auth` (reader).

**ALTO-V3-8 — CAS PR-lease fallback visibility (`cas.c`):**
The `pr_info` log message for the PR-lease CAS backend in clustered mode is
upgraded to `pr_warn`. PR-lease is ~100× slower than SCSI CAW; operators
should not miss this downgrade silently in a busy log stream.

**ALTO-V3-2 — Selective invalidation range before EX release (`inode.c`,
`thin.c`):**
`ocsfs_lock_res.lr_inv_lo/hi` must be populated before `ocsfs_lock_release`
is called. Previously, the setattr truncate path and the fallocate path set
the dirty range after releasing EX, so the range was never stored into the
on-disk lock entry and the next acquirer had no range to invalidate.
Fix: `inode.c:ocsfs_setattr` sets `lr_inv_lo = from_block; lr_inv_hi =
U64_MAX` (full tail) before the lock is released. `thin.c:ocsfs_fallocate`
sets the exact byte-range of the fallocate operation converted to blocks.

**ALTO-V3-9 — Encrypted folio invalidation race (`iomap.c`):**
When fscrypt is enabled, `ocsfs_iomap_aops.invalidate_folio` is wired to
`ocsfs_enc_invalidate_folio` instead of the plain `iomap_invalidate_folio`.
The wrapper calls `folio_wait_writeback(folio)` first, ensuring any
in-flight bounce-page bio for the folio has completed before the iomap
layer discards the folio. Without this, a concurrent truncate could race
with a bounce-page bio that holds a reference to the same folio, causing
a use-after-free in the fscrypt writeback path.

**MEDIO-V3-2 — Logarithmic txn batch scaling in btree truncate
(`extent_btree.c`):**
`ocsfs_extent_btree_truncate` previously committed a journal transaction
every 64 extent removals regardless of the total extent count. For large
files with tens of thousands of extents this produces thousands of small
transactions, each with 16 KB of overhead. The new heuristic estimates
`est_extents = (i_blocks × 512) / block_size + 1` and sets
`batches_per_commit = max(1, order_base_2(est_extents))`, so a file with
~1024 extents commits every 10 batches rather than every 1, reducing
journal traffic by ~10×.

**MEDIO-V3-4 — `ocsfs_inode_journal_root` dir-only guard (`inode.c`):**
The function was unconditionally writing `i_dir_btree_root` to the on-disk
inode for all inode types. Regular files do not have a dir btree root; the
field should be zero and is never read. Fix: the write is now gated on
`S_ISDIR(inode->i_mode)`, preventing stale non-zero garbage from a previous
directory inode from poisoning the field when the inode slot is reused as a
file.

**MEDIO-V3-8 — xattr alloc txn-failure cleanup (`xattr.c`):**
If `ocsfs_txn_commit` fails after a new xattr block was allocated and
assigned to `oi->i_xattr_block`, the in-memory inode carries a block
address that was never journaled. A subsequent xattr set would try to read
that block and find uninitialized data. Fix: on commit failure in the
`need_alloc` branch, `oi->i_xattr_block` is reset to 0 so the next call
re-allocates cleanly.

**MEDIO-V3-9 — `ocsfs_fscrypt_empty_dir` real disk scan (`crypto.c`):**
The previous implementation checked `oi->i_dirent_count == 0`, a cached
in-memory counter that can lag behind the on-disk state in cluster mode (a
peer could have added entries since the last inode refresh). The fix
delegates to `ocsfs_empty_dir()`, which acquires DLM SH, calls
`ocsfs_inode_refresh()`, and performs a real on-disk directory scan via
`ocsfs_dir_foreach`. This closes the TOCTOU gap that would allow
`fscrypt_drop_inode` to conclude a directory is empty when it is not.

**MEDIO-V3-10 — Journal-before-free in compress_file (`compress_file.c`):**
When compression produces a smaller block set, the old physical blocks
must be freed. The previous code called `ocsfs_free_blocks` before
journaling the inode update, so a crash between those two steps would
produce a freed block with a live reference. The fix opens a journal txn,
flushes the inode via `ocsfs_flush_inode_locked`, commits the txn, and
only calls `ocsfs_free_blocks` on success. If the txn fails, the old
blocks are kept (space leak) rather than risking use-after-free.

**MEDIO-V3-12 — xattr pre-flight bh reuse (`xattr.c`):**
`xattr_set_internal` performs a pre-flight forced read of the xattr block
to validate the current content before opening a txn. In the non-alloc
path it then repeated the same forced read inside the txn. Fix: the
pre-flight `buffer_head` is kept alive (`peek_bh`) and transferred
directly into the txn via `ocsfs_txn_add_bh`, eliminating a second disk
read-trip in cluster mode.

**Deferred items:**

| Item | Reason for deferral |
|---|---|
| ALTO-V3-10 — HMAC for journal COMMIT records | `ocsfs_disk_journal_txn` (28 bytes) has no space for a 32-byte HMAC without a format bump and a new incompat feature bit. Redesign tracked separately. |
| MEDIO-V3-6 — decompress OOM mempool | The existing `comp_size > 1 MiB` guard added in Batch-C already limits single-decompression memory usage. Full per-CPU mempool redesign deferred. |

---

### Sprint I — On-disk correctness (2026-05-28)

Sprint I was driven by a fresh Opus 4.8 review that identified a critical
offset bug in `ocsfs_load_ags` that caused the kernel to address inode and
bitmap blocks from near the beginning of the device instead of the correct
per-AG position. All Sprint I fixes are in `super.c`, `kmod/ocsfs.h`,
`dir.c`, `journal.c`, `journal_replay.c`, `lock.c`, `heartbeat.c`,
`tools/mkfs_ocsfs.c`, and `tools/ocsfs_tool.c`.

**CRIT-N1 — AG-relative to absolute offset conversion (`super.c`,
`kmod/ocsfs.h`, `tools/ocsfs_tool.c`):**
`ag_bitmap_off` and `ag_inode_table_off` are stored on disk as offsets
relative to the start of their AG's data region (e.g., 4096 for the bitmap
at block 1 of the AG). `ocsfs_load_ags` was copying these values directly
into `ocsfs_ag_info.bitmap_off` / `.inode_table_off` without adding
`ag->block_start * block_size`. Every subsequent caller (`bitmap.c:80`,
`bitmap.c:447`, `bitmap.c:542`, `bitmap.c:590`,
`ocsfs_inode_disk_off()`) divided or used these values as absolute device
byte offsets — reading from blocks 1–2 of the device instead of the actual
inode/bitmap region (which begins at `data_start ≈ 130 MB+`). Fix: in
`ocsfs_load_ags`, compute `abs_base = block_start × block_size` and add it
to both fields once, so all callers are automatically correct. `ocsfs_tool`
was also updated to use `ag_block_start × block_size + ag_inode_table_off`
instead of `s_data_off + ag_inode_table_off` (the latter was equivalent for
AG 0 only).

**CRIT-N3 — `de_name_hash` aligned with dir B+ tree key (`dir.c`,
`include/ocsfs.h`):**
`__ocsfs_add_dirent` was computing `de_name_hash` as `crc32c(~0U, name, len)`
(single-pass, 32 bits) while the directory B+ tree uses a dual-CRC32c
64-bit key `((crc32c(len,name)<<32) | crc32c(~hi,name))`. These hash spaces
are incompatible; any offline fsck trying to reconstruct the btree from
dirent records would produce a corrupt index. Fix: `de_name_hash` now stores
the same dual-CRC32c value as the btree key. The comment in
`include/ocsfs.h` is updated accordingly.

**ALTO-N4 — Truly independent hash2 in journal BEFORE records (`journal.c`,
`journal_replay.c`):**
The 62-bit BEFORE-image integrity check (CRIT-V3-3) used `crc32c(~0U,...)` as
the primary hash and `crc32c(~1U,...)` as the secondary. CRC32C is a linear
function: for any fixed data length, `crc32c(~1U,data)` is a deterministic
XOR-transform of `crc32c(~0U,data)`. Two blocks that produce the same
primary hash automatically produce the same secondary hash — providing only
~32 bits of effective collision resistance, not 62. Fix: hash2 now uses
`xxh64(data, size, 0)`, an independent hash family. Both the write path in
`journal.c` and the two verification sites in `journal_replay.c` are updated
consistently.

**ARCH-N1 — `lr_lock_epoch` cleared on EX→SH downgrade (`lock.c`):**
`ocsfs_lock_downgrade` sets `lr->lr_mode = new_mode` on success but did not
clear `lr->lr_lock_epoch`. After an EX→SH downgrade, if the holder releases
SH and then re-acquires SH, the cache-hit path in `ocsfs_lock_acquire` could
fire with a stale epoch, skipping the disk round-trip and returning cached
data modified by a peer between the downgrade and the re-acquire. Fix:
`lr->lr_lock_epoch = 0` is now set alongside `lr_mode` on a successful
downgrade write, forcing the next acquire to go to disk.

**CRIT-N2 / MEDIO-N7 — Stale REPLAY_ACTIVE barrier in heartbeat
(`heartbeat.c`):**
`heartbeat_check_peers` set `s_remote_recovery_barrier` whenever the
recovery leader block had `OCSFS_RL_REPLAY_ACTIVE` set — without checking
whether the leader's deadline was still in the future. A leader that crashed
during replay left the flag set permanently, freezing all EX acquisitions
cluster-wide until a manual intervention. Fix: the REPLAY_ACTIVE check now
also requires `rl_deadline_ns > ktime_get_real_ns()`, ensuring the barrier
only applies to a living leader.

**MEDIO-N1 — AG geometry cross-check (`super.c`):**
`ocsfs_validate_super` now verifies `s_ag_count × s_ag_size ≤ s_total_blocks`,
preventing mount on a superblock where a malicious or corrupt `s_ag_count`
would allocate `kvmalloc_array(65536, ...)` with more AG slots than could
possibly fit on the device.

**SEC-N1 — `mkfs` max_nodes input validation (`tools/mkfs_ocsfs.c`):**
`atoi(optarg)` silently wraps on values outside the int range. Replaced with
`strtol` + range check (1..`OCSFS_MAX_NODES`) with an explicit error message
and `exit(1)`.

---

### Sprint J — Crash-consistency metadata (2026-05-28)

Sprint J eliminates several non-transactional paths in thin.c, iomap.c,
dir.c, refcount.c, and snapshot.c where in-memory metadata changes could
be lost on crash, causing cross-links, phantom extents, or corrupted
refcount trees.

**ALTO-N1 — punch_hole / zero_range inline journal-before-free
(`thin.c`):**
The inline (non-btree) path of `ocsfs_punch_hole` called `ocsfs_free_blocks`
inside the loop that modified `oi->i_extents[]`, then relied on async
`mark_inode_dirty` to persist the updated extent map. A crash between the
bitmap commit (blocks freed) and the inode writeback left the inode
referencing freed blocks — which could then be reallocated to another file,
producing a cross-link.

Fix: replace all `ocsfs_free_blocks` calls inside the loop with deferred
`{phys, count}` entries in a stack-allocated array
(`deferred_frees[OCSFS_INLINE_EXTENTS + 1]`). After the loop, call
`ocsfs_flush_inode_locked(inode, false)` to journal the updated extent map
first, then free all deferred blocks. Crash after journal but before free →
space leak (no cross-link). `ocsfs_zero_range` inline path is similarly
fixed: flag changes are journaled via `ocsfs_flush_inode_locked` before
returning.

**ALTO-N2 — writeback of UNWRITTEN extents stays UNWRITTEN (`iomap.c`):**
The `ocsfs_writeback_range` callback uses `flags=0` (no IOMAP_WRITE) when
looking up the physical mapping for a dirty folio. `ocsfs_iomap_end` only
performs the UNWRITTEN→WRITTEN conversion when `flags & IOMAP_WRITE`, and
the iomap writeback framework does not call `iomap_end` at all. Result: data
written via mmap+dirty-page or buffered write to a pre-allocated (fallocated)
UNWRITTEN extent reached the block device during writeback but the on-disk
extent remained UNWRITTEN — a subsequent read returned zeros instead of the
data.

Fix: in `ocsfs_writeback_range`, after getting the iomap, if the type is
`IOMAP_UNWRITTEN`, call `ocsfs_extent_convert_unwritten` (under
`i_extent_lock`) and update `wpc->iomap.type = IOMAP_MAPPED`. The conversion
is safe at writeback time: the folio is dirty because a write to the page
cache already succeeded; converting to WRITTEN before the bio completes is
no worse than the crash window that exists in the normal write path.

**ALTO-N3 — directory `i_size` not journaled with dirent insert (`dir.c`):**
In the "new block" path of `__ocsfs_add_dirent`, `dir->i_size` is incremented
in memory (line 253) and `mark_inode_dirty` is called asynchronously after the
dirent transaction commits. In single-node mode there is no subsequent flush;
if the system crashes between the dirent commit and the inode writeback, the
on-disk `i_size` does not cover the new block → `ocsfs_dir_foreach` never
visits it → the entry appears to vanish.

Fix: add `ocsfs_flush_inode_locked(dir, false)` unconditionally at the end
of `__ocsfs_add_dirent` (after btree index updates), ensuring `i_size` and
`i_dirent_count` reach disk in a journal transaction before the function
returns. In cluster mode the existing flush in `ocsfs_add_dirent` becomes
redundant but harmless (second flush on an already-clean inode is a no-op).

**ARCH-N2 — refcount B+ tree not journaled (`refcount.c`):**
`rc_bt_write` called `sync_dirty_buffer` directly for every btree node write
and `rc_btree_persist_root` did the same for the AG descriptor. A crash
mid-split could leave tree nodes written but the root pointer not updated —
or vice versa — producing a silently corrupted refcount tree. Incorrect
refcounts can cause either premature block free (use-after-free → data
corruption) or permanent block leak.

Fix: add `struct ocsfs_txn *txn` to `ocsfs_rc_io_ctx`. When non-NULL,
`rc_bt_write` calls `ocsfs_txn_add_bh` instead of `sync_dirty_buffer`;
`rc_btree_persist_root` gains a `txn` parameter and does likewise.
`rc_apply_delta` now opens a journal transaction, sets `rctx.txn`, performs
the btree search/delete/insert, persists the root, and commits — all
atomically. On abort, all btree node writes are rolled back by the journal.

**ARCH-N3 — CoW extent inline path not transactional (`snapshot.c`):**
`ocsfs_cow_extent` for the inline extent path modified `oi->i_extents[]`
in memory, then called `ocsfs_refcount_dec` + `ocsfs_free_blocks`. A crash
between the block free and the subsequent inode flush (done by the caller)
left the inode on disk still referencing the original (now-freed and possibly
reallocated) blocks.

Fix: after the inline extent modification and before `ocsfs_refcount_dec` /
`ocsfs_free_blocks`, call `ocsfs_flush_inode_locked(inode, false)`. This
journals the new extent map first; a subsequent crash is a space leak (old
refcount entry not decremented, old blocks not freed) rather than a
cross-link or data corruption.

### Sprint K — Recovery phase persistence (2026-05-28)

Sprint K addresses ARCH-V3-3: persisting the recovery phase on-disk so that a
crash-replacement recovery leader can skip already-completed phases rather than
restarting from Phase 1 every time.

**ARCH-V3-3 — Recovery phase lost if leader crashes mid-recovery (`recovery.c`):**

Before Sprint K, the recovery phase progression (Phase 1 → Phase 5) was
entirely in-memory. If the recovery leader crashed after completing Phase 2
(fencing) but before finishing Phase 3 (journal replay), the new leader won
the CAS election and found no record of what the dead leader had already done.
It would restart from Phase 1 — re-doing fencing (idempotent but wasteful) and
re-running journal replay (safe, but involves setting `REPLAY_ACTIVE` again,
which causes all survivor nodes to defer EX acquisitions for the full replay
duration). Worse, if the dead leader had crashed after REPLAY_ACTIVE was
cleared, the new leader would set it again unnecessarily.

Fix: add `rl_phase` (`__u8`) to `ocsfs_disk_recovery_leader` (at offset 28,
after `rl_checksum`; not covered by the existing CRC — a wrong phase causes
redundant but idempotent work, not data corruption). Add four phase constants:

| Constant | Value | Meaning |
|---|---|---|
| `OCSFS_RECOVERY_PHASE_ELECTED` | 0 | Won CAS; no phase completed |
| `OCSFS_RECOVERY_PHASE_FENCED` | 1 | SCSI PR fencing complete |
| `OCSFS_RECOVERY_PHASE_REPLAYED` | 2 | Journal replay complete |
| `OCSFS_RECOVERY_PHASE_LOCKS` | 3 | Lock cleanup complete |

`ocsfs_recovery_leader_acquire` gains a `u8 *phase_out` parameter. When
winning the CAS over a dead leader's block, it sets `new.rl_phase =
cur.rl_phase` (inheriting the dead leader's phase) and reports it back.

`ocsfs_recovery_set_phase` (new helper) performs a direct write to the on-disk
leader block after each phase completes. It checks that the epoch in the block
still matches the caller's `leader_epoch` before writing — if leadership was
superseded, the write is silently skipped.

`ocsfs_recovery_run` wraps Phases 2, 3, and 4 in `if (start_phase <
OCSFS_RECOVERY_PHASE_*)` guards and calls `ocsfs_recovery_set_phase` after
each phase succeeds. Error paths are consolidated with goto labels
(`out_release_dead`, `out_release`, `out_done`) to eliminate repeated
unlock/return patterns. Phase 5 (slot cleanup) always runs because
`ocsfs_node_mark_dead` is idempotent.

Crash behaviour after Sprint K:

| Crash point | New leader behaviour |
|---|---|
| After Phase 2 (FENCED written) | Skips Phase 2 + degraded check; runs Phase 3 onwards |
| After Phase 3 (REPLAYED written) | Skips Phase 2 + Phase 3; runs Phase 4 + 5 |
| After Phase 4 (LOCKS written) | Skips Phases 2–4; runs Phase 5 only |
| No phase written (crash in Phase 1 or between phases) | Restarts from elected phase; worst case is redundant idempotent work |

### Sprint L — VFS semantics and security (2026-05-28)

**ALTO-N5 — evict_inode on DLM EX failure leaks space permanently (`inode.c`):**
When `ocsfs_evict_inode` ran on an unlinked inode in cluster mode and the DLM
EX acquisition failed (another node holds the inode open), the blocks were
silently leaked with no recovery path. The ORPHAN flag was only set during
inode creation and cleared on dirent commit; an EX failure on evict left the
inode without the flag, so `ocsfs_orphan_scan` would never reclaim it.

Fix: on DLM EX failure, set `OCSFS_IFLAG_ORPHAN` in memory and call
`ocsfs_flush_inode_locked` to persist it to disk. Writing only the inode flags
field (additive, no extent layout modified) is safe without the EX lock. On
the next mount, `ocsfs_orphan_scan` (called from `fill_super`) will find the
inode, set `nlink=0`, and call `iput`, which re-enters `evict_inode` — this
time with no other node holding the lock, so blocks are freed normally.

**ALTO-N6 — rename rollback may leave `i_dirent_count` stale (`dir_rename.c`):**
In the add-before-remove rename path, if `__ocsfs_del_dirent(old_dir)` fails
after `__ocsfs_add_dirent(new_dir)` succeeds, the compensating
`__ocsfs_del_dirent(new_dir)` correctly uses the btree fast path to find the
just-added entry. If the compensation itself fails (I/O error), the entry
remains in `new_dir` with its correct count, but the on-disk inode metadata
for `new_dir` may not reflect the in-memory state.

Fix: on failed compensation, call `ocsfs_flush_inode_locked(new_dir, false)`
to persist the current in-memory `i_dirent_count` to disk. This ensures the
on-disk count matches whatever state we ended up in. The ghost-in-both-dirs
situation is still logged with an explicit fsck recommendation.

**MEDIO-N2 — zero_range inline silently skips partial overlaps (`thin.c`):**
The inline zero_range path (files with ≤16 extents) only converted extents to
UNWRITTEN when the extent was FULLY contained in the zero range. Partial
overlaps — where the range starts or ends in the middle of an extent — were
silently skipped. `fallocate(ZERO_RANGE)` returned 0 but data in the partial
blocks was not zeroed.

Fix: for partial overlaps, physically zero the overlapping blocks via
`sb_getblk` + `memset` + `sync_dirty_buffer`. This is always correct and
handles the partial-block boundaries that the UNWRITTEN optimization cannot.

**MEDIO-N4 — enc_read_folio without DLM SH returns ciphertext (`iomap.c`):**
`ocsfs_enc_read_folio` in cluster mode had a `WARN_ONCE` when called without
at least DLM SH, but the read proceeded and returned stale or wrongly decrypted
data (ciphertext served to `splice_read` or `userfaultfd` paths). Warn-and-
continue is inappropriate for a data-correctness invariant.

Fix: change to an early return of `-EIO` with `pr_warn_once`. Any call path
that reaches this function without DLM SH is a caller bug; the fix surfaces it
immediately as an error rather than returning wrong data silently.

**SEC-N2 — VAAI WRITE_SAME / UNMAP have no ownership validation (`vaai.c`):**
`ocsfs_vaai_write_same` had a metadata-area bound check (`s_data_off`) but
`ocsfs_vaai_unmap` had no such check. Additionally, both operations accepted
arbitrary device-level block ranges from the caller without verifying that
those blocks belong to the inode on which the ioctl was invoked.

Fix:
1. Add `s_data_off` metadata bound check to `ocsfs_vaai_unmap` (same protection
   that `write_same` had).
2. Change both function signatures to accept `struct inode *` instead of
   `struct super_block *` so the caller's inode is available.
3. Add `ocsfs_vaai_owns_range` helper: for inline-extent inodes, checks that
   `[lba, lba+nblocks)` is contained within one of the inode's physical
   extents. For btree inodes, the check trusts the existing `CAP_SYS_ADMIN`
   gate in `file.c`. Returns `-EPERM` if the range is not owned.

### Sprint M/N/O — Deferred architectural items (2026-05-28)

These three items were explicitly deferred from the Sprint I–L roadmap and are now resolved.

**MEDIO-V3-6 — Decompress OOM under parallel cluster reads (`compress.c`, `super.c`):**
`ocsfs_compress_extent_read` allocated two `kvmalloc` buffers of up to 1 MiB each per call
(compressed data buffer + decompressed output buffer). Under concurrent read pressure on a
cluster node — multiple kswapd/readahead paths reading compressed extents simultaneously —
these allocations could fail or trigger the OOM killer.

Fix: add a `mempool_t *s_comp_buf_pool` to `ocsfs_sb_info` (initialized in `fill_super`,
destroyed in `put_super`) backed by 8 pre-allocated 1 MiB buffers using custom
`kvmalloc`/`kvfree` callbacks. `ocsfs_compress_extent_read` now calls `mempool_alloc` /
`mempool_free` instead of `kvmalloc`/`kvfree`. `mempool_alloc` blocks until a buffer is
available rather than failing immediately, converting the allocation from a potential OOM
failure into a bounded wait.

**ALTO-V3-10 — No HMAC on journal COMMIT records (`journal.c`, `journal_replay.c`):**
In degraded mode (no SCSI PR, non-PR device), a malicious or malfunctioning node could forge
journal COMMIT records with arbitrary AFTER-images. The existing CRC32C covers record
integrity but not authenticity — the cluster secret was not used in the journal.

Fix: add `OCSFS_FEATURE_INCOMPAT_JOURNAL_HMAC (1ULL << 3)`. When set, a 32-byte
`ocsfs_disk_journal_hmac_rec` is written immediately after each COMMIT record. The HMAC
record is the same size as `ocsfs_disk_journal_txn` (32 bytes) so the forward scanner
advances by the same stride regardless of record type. The HMAC covers the first 28 bytes of
the COMMIT record (jt_type through jt_data_len, excluding jt_checksum) using
HMAC-SHA256/128 (16-byte truncation) keyed on `sbi->s_cluster_secret`. At replay time, if
the feature is active, the HMAC record is verified before AFTER-images are applied; a
mismatch causes the commit to be skipped (not applied), which is equivalent to treating the
transaction as uncommitted.

The `ocsfs_journal_hmac_commit(sb, jt, out16)` helper is exported from `journal.c` and used
by `journal_replay.c` without code duplication.

**ARCH-V3-5 — EX lock re-acquire requires disk round-trip per operation (`lock.c`):**
The epoch-based cache (ARCH-V3-7, Sprint D) already eliminates disk round-trips on EX
re-acquire when no recovery happened. ARCH-V3-5 adds a complementary mechanism for the
waiter side: when a node holds EX, it writes a lease deadline (`le_lease_ns`) into the
on-disk lock entry alongside `le_lease_slot`. Nodes waiting for EX see the deadline and
sleep until it expires rather than polling with exponential backoff — eliminating unnecessary
CAS retries and reducing SAN I/O under lock contention.

Fields added to `ocsfs_disk_lock` (within `le_reserved`, no change in struct size):
- `le_lease_ns`: 8-byte ktime_get_real_ns() expiry; 0 = no lease
- `le_lease_slot`: 2-byte node slot of the lease holder; 0xFFFF = no lease
- `le_lease_pad`: 2-byte alignment pad
- `le_reserved` reduced from 56 to 44 bytes

Since the CRC covers the same bytes regardless of how `le_reserved` is sub-divided, the
lease fields are backward-compatible: old nodes see them as zero reserved bytes (no lease
written), new nodes write and read the lease correctly. No INCOMPAT feature bit is required
for correctness.

New function `ocsfs_lock_renew_lease(sb, lr)`: updates `le_lease_ns` for the current EX
holder, allowing long-running write operations to extend the lease without full
release-and-reacquire.

### Sprint P — ARCH-V3-1: Cluster-wide fscrypt key distribution (2026-05-29)

**Problem**: fscrypt keys are node-local. In cluster mode, node A adds a key and encrypts files;
node B has no mechanism to obtain the key. Without it, node B cannot open those files (receives
`-ENOKEY`). The Sprint A partial fix only emitted a `pr_warn_once`.

**Fix — shared encrypted key store** (`crypto.c`, `ocsfs.h`, `file.c`, `super.c`):

A new on-disk area (`OCSFS_KEY_STORE_OFF = OCSFS_HB_SUMMARY_OFF + 4096`) holds 32 × 128-byte
entries secured by `OCSFS_FEATURE_INCOMPAT_KEY_STORE (1ULL << 4)`. Each entry:

```
struct ocsfs_disk_key_store_entry {
    __le32  kse_magic;       /* OCSFS_KEY_STORE_ENTRY_MAGIC = "KEYS" */
    __le16  kse_key_size;    /* original fscrypt raw key length (1..64 bytes) */
    __le16  kse_spec_type;   /* FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR or _IDENTIFIER */
    __u8    kse_id[16];      /* canonical 16-byte key identifier */
    __le64  kse_nonce;       /* random ChaCha20-Poly1305 nonce */
    __u8    kse_ct[80];      /* encrypted key (max 64B) + 16B Poly1305 auth tag */
    __le32  kse_checksum;    /* crc32c(kse_magic..kse_ct) */
    __u8    kse_pad[12];
} __packed;  /* exactly 128 bytes */
```

**Encryption model**: ChaCha20-Poly1305 (via `<crypto/chacha20poly1305.h>`) using
`sbi->s_cluster_secret[32]` as the 256-bit key and a random 64-bit nonce. The 16-byte
Poly1305 authentication tag is stored inline in `kse_ct[]`. Raw key material never touches
disk in plaintext.

**Key store lifecycle**:

1. `FS_IOC_ADD_ENCRYPTION_KEY` (in `file.c`): intercepted in cluster mode when
   `OCSFS_FEATURE_INCOMPAT_KEY_STORE` is active. The raw key is copied from userspace,
   passed to `ocsfs_key_store_add()`, then `fscrypt_ioctl_add_key()` proceeds normally.
   The raw key is zeroed with `memzero_explicit` immediately after. `ocsfs_key_store_add` is
   idempotent — a second call with the same key identifier is a no-op.

2. At mount (`fill_super`): `ocsfs_key_store_notify_mount()` scans the key store and logs
   the identifier and size of each stored key, prompting the administrator to add any missing
   keys on this node.

3. Key retrieval by other nodes — two new ioctls (both require `CAP_SYS_ADMIN`):
   - `OCSFS_IOC_KEY_LIST ('O', 30)`: returns identifiers of all stored keys (no raw material)
   - `OCSFS_IOC_KEY_FETCH ('O', 31)`: decrypts and returns raw key for a given identifier
   The `ocsfs-tool keys restore <dev>` command (to be implemented in userspace) calls these
   two ioctls and then issues `FS_IOC_ADD_ENCRYPTION_KEY` locally for each entry.

**Security properties**:
- Key material encrypted at rest with an authenticated cipher (ChaCha20-Poly1305)
- Authentication tag prevents bit-flip attacks or key substitution
- CRC32C on the entry header guards against partial writes
- `OCSFS_IOC_KEY_FETCH` requires `CAP_SYS_ADMIN`; raw key zeroed in kernel before return
- Read-modify-write of the key store block is serialized cluster-wide by a dedicated DLM
  lock (`s_keystore_lock_res`, `OCSFS_LOCKRES_KEYSTORE`) — EX on add, SH on list/fetch
  (hardened in Sprint R / KS-1)

**Backward compatibility**: volumes without `OCSFS_FEATURE_INCOMPAT_KEY_STORE` are
unaffected — all key store paths gate on this bit and return early without touching the
area. `mkfs_ocsfs` sets the bit only when auth is enabled (`-K`), since the store is
meaningless without a `cluster_secret=` (Sprint R / KS-2 gates writes on it).

---

### Sprint R — On-disk layout fix + hardening (2026-05-29)

**CRIT-O1 — journal / cluster-metadata layout collision (catastrophic).** The shared
userspace header (`include/ocsfs.h`) computed the per-node journal offset as
`LOCK_TABLE_OFF + LOCK_TABLE_SIZE = 1 384 448`, the same byte at which the kernel places the
fixed cluster-coordination region: CAS-lease table (`OCSFS_CAS_LEASE_OFF`), recovery-leader
block, HB summary block and key store. Node 0's journal (up to `max_nodes × journal_size`)
therefore physically overlapped all four structures. In any clustered mount this caused
immediate cross-corruption (the heartbeat summary write, recovery-leader updates and CAS
leases all landed inside node 0's journal, and vice-versa). Single-node mounts were unaffected
only because those structures are dormant without clustering — which is why the bug survived
until now: cluster mode was never exercised against it.

Fix:
- Mirrored the cluster-metadata offset constants into `include/ocsfs.h` and introduced
  `OCSFS_METADATA_RESERVED_END = OCSFS_KEY_STORE_OFF + OCSFS_KEY_STORE_SIZE` (= 1 429 504).
  `ocsfs_journal_offset()` now returns this value, so the journal begins after the reserved
  region. `mkfs_ocsfs` writes `s_journal_off = 1 429 504` and bumps `s_revision_level` to 2.
- `ocsfs_validate_super()` now **rejects** any volume whose `s_journal_off` falls below
  `OCSFS_METADATA_RESERVED_END`, with a clear "reformat required" message. **This is an
  on-disk format change: volumes formatted before Sprint R must be recreated with the current
  `mkfs.ocsfs`.** Since multi-node was never validated and the project carries no production
  data, forced reformat is the safe choice.

**HDR-1 — feature-bit drift between headers.** `include/ocsfs.h` had `RO_COMPAT_DEDUP_SCRUB`
at bit 0 and was missing `SELECTIVE_INV`, `JOURNAL_HMAC`, `KEY_STORE`, so `mkfs` wrote
`ro_compat` flags the kernel mis-interpreted (the kernel read bit 0 as `SELECTIVE_INV`).
The two headers' INCOMPAT/RO_COMPAT bit assignments are now identical.

**KS-1 — key store now DLM-serialized** (see above). **KS-2 — key store writes refuse to run
without a real `cluster_secret=`** (otherwise keys would be "encrypted" under an all-zero key).
`mkfs` couples `INCOMPAT_KEY_STORE` to `-K`.

**SB-1 — no superblock write on read-only mounts.** `fill_super` previously bumped
`s_mount_count` and rewrote block 0 (+ mirror) on every mount including RO and clustered, with
no DLM — two nodes mounting concurrently raced on the shared superblock. The stamp is now
skipped entirely when `sb_rdonly(sb)`.

**Proxmox integration** (`proxmox/`):
- `mount.ocsfs` now invokes `mount -i -t ocsfs …`. Without `-i`, `mount(8)` re-discovered the
  helper and re-executed it → infinite recursion (PROX-1). The bug only manifested once the
  helper was installed (i.e., on real Proxmox hosts), never in hand-mounted dev testing.
- `OCSFSPlugin.pm` gained `cluster_secret` / `secret_file` / `degraded` storage properties and
  passes them as `-o` mount options. Previously `activate_storage` mounted with no options, so
  auth volumes (`mkfs -K`) could never be mounted via PVE and the key store ran under a zero
  secret (PROX-2). `secret_file` (0600) is preferred over inline `cluster_secret`.

---

### Sprint S — First real bring-up: it actually mounts now (2026-05-29)

This sprint is the first time the on-disk format was exercised by *running code*
(the FUSE prototype + the offline fsck) rather than only reviewed. That immediately
surfaced two showstoppers that no amount of static review had caught.

**CRC-1 — CRC32C convention mismatch between userspace and kernel (catastrophic).**
The userspace `ocsfs_crc32c()` (`src/crc32c.c`) returned the *standard* CRC32C
(`init 0xFFFFFFFF`, final XOR `0xFFFFFFFF`), while the kernel module computes
checksums with the Linux `crc32c()` primitive (`crc32c(~0U, …)`, **no** final
inversion — the ext4/btrfs convention). The two are exact bitwise complements, so
**every superblock, AG descriptor and inode written by `mkfs.ocsfs` would be
rejected by the kernel** with "checksum mismatch" — i.e. an mkfs-formatted volume
could never mount. Fix: `src/crc32c.c` now drops the final inversion (keeping the
initial-value inversion so a caller-supplied seed of `0` still becomes
`0xFFFFFFFF`), making userspace one-shot checksums byte-identical to the kernel's.
All 28 userspace call sites are one-shot integrity checksums (no continuation), so
no call-site changes were needed. The Python `ocsfs-fsck` used `binascii.crc32`
(zlib polynomial, not Castagnoli at all) — replaced with a real CRC32C table using
the same raw convention.

**DIRENT-1 — FUSE directory record stride 288 ≠ sizeof 286 → readdir EIO after
removal.** `OCSFS_DIRENT_FIXED_SIZE` was hard-coded to 288 while the packed
`struct ocsfs_dirent_fixed` is 286 bytes. `write_dir_entries()` copied
`count*288` bytes out of a 286-strided array, leaking the first two bytes of the
following slot onto disk; after an entry removal those leaked bytes carried a
stale `de_magic`, so the 286-strided reader saw a phantom entry with an empty
name and the kernel FUSE layer failed the whole `getdents` with `EIO`. Fixed by
tying the constant to `sizeof(struct ocsfs_dirent_fixed)` (with a
`_Static_assert`), zeroing the vacated tail slot in `dir_remove_entry()`, and
skipping empty names defensively in `readdir`.

**Build + tooling fixes:** the FUSE prototype did not compile (three `pwrite`
return values ignored under `-Werror`); `ocsfs-fsck` was reconciled to the real
on-disk layout (superblock checksum at offset 4092, real `ocsfs_ag_desc` /
`ocsfs_journal_header` field layouts, AG-relative→absolute bitmap/inode offsets,
journal offsets taken from the superblock, correct `OCSFS_AG_MAGIC` = `0x41474850`,
correct inode `i_flags` offset 64 and a safe orphan-repair write path). The legacy
fixed refcount-table spot-check was removed (refcounts now live in a per-AG B+
tree), and the bitmap free-count cross-check was downgraded to advisory so it can
never drive a destructive `--repair`.

**Verification (no special hardware):** with the FUSE prototype on a loopback
image — 8 MiB random data sha256 round-trip, 50+ file create, removal without
EIO, nested directories, truncate, and unmount/remount persistence all pass; the
userspace test suite is 85/85; `ocsfs-fsck` validates both freshly-formatted and
FUSE-populated volumes clean. `tests/kernel_smoke_test.sh` (run as root) performs
the same battery through the *actual kernel module* and is the definitive check
that CRC-1 and the Sprint R layout fix let real volumes mount.

#### Real kernel-module bring-up on a Proxmox node (2026-05-29)

Running `tests/kernel_smoke_test.sh` on an actual Proxmox VE 9.2.3 node
(kernel `7.0.6-2-pve`, loopback image, single node via `-o degraded`) surfaced
two more bugs that only the real VFS/kernel path exposes:

- **MODE-1 — root inode i_mode used the dirent file-type enum.** `mkfs` wrote the
  root inode as `(OCSFS_FT_DIR << 12) | 0755`. `OCSFS_FT_DIR` is `2`, so that is
  `0o20755 = S_IFCHR | 0755` — a character device. The kernel reads `i_mode` as a
  standard VFS mode, so the root looked like a chardev and every mount failed
  (`move_mount` → `EINVAL` on the new mount API, `ENOTDIR` on the legacy one).
  Fixed: `mkfs` writes `S_IFDIR | 0755`. The `OCSFS_FT_*` enum belongs only in the
  dirent `de_file_type` field, never in inode `i_mode`. (The FUSE prototype had
  masked this by interpreting the top mode bits as `OCSFS_FT_*` itself — a
  userspace-only convention that disagreed with the kernel.)

- **BTREE-1 — btree node read callbacks broke read-your-own-writes inside a
  transaction.** `ext_btree_read` / `dir_btree_read` / `rc_bt_read` force a fresh
  disk re-read (`clear_buffer_uptodate` + `bh_read`) for cross-node cache
  coherence. Done unconditionally, that discards nodes written earlier in the same
  *uncommitted* transaction: when a file grows past 16 inline extents,
  `ocsfs_extent_btree_migrate` allocates a new btree root and writes it through the
  txn (buffer cache only, not yet flushed), then immediately reads it back — the
  forced re-read returned zeros from the unflushed block and failed with
  `btree: bad magic 00000000 at block 0` (`-EIO`). The net effect: **every write
  larger than ~64 KiB failed with EIO.** `rc_bt_read` did the forced re-read even
  on single-node volumes, so reflink/snapshot/dedup were affected regardless of
  clustering. Fixed: force the cross-node re-read only on the read-only path
  (`ctx->txn == NULL`); inside a write transaction (the inode EX lock is held)
  read from the buffer cache. *(Follow-up for the multi-node testbed: btree-node
  blocks are not in the inode page mapping, so first-read-in-txn cross-node
  freshness must be ensured by invalidating metadata blocks at EX-acquisition
  time — tracked for the cluster bring-up.)*

**Result:** `tests/kernel_smoke_test.sh` is **all-pass through the real kernel
module** — superblock CRC accepted, 8 MiB data sha256 round-trip, nested
directories, 30-file create, removal without EIO, truncate, unmount/remount
persistence, and a clean offline fsck. This is the first confirmed end-to-end
mount + I/O + remount of OCSFS via the kernel module.

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
   `scsi_device_from_queue()` via `register_kprobe()`. Active only when
   `CONFIG_KPROBES=y`; gracefully disabled otherwise.

`ocsfs_scsi_caw()` calls `ocsfs_bsg_execute_cdb()` with a pre-built CAW CDB
(opcode 0x89, SBC-4 §5.3): `expected || new_data` in a mempool-backed buffer.
`lock_write_entry()` uses CAW when `s_caw_supported` is set.

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

**ARCH-V3-1 — Cluster key distribution (Sprint P, 2026-05-29):** Implemented via the shared
encrypted key store. See §Sprint P above for the full protocol. When
`OCSFS_FEATURE_INCOMPAT_KEY_STORE` is active, `FS_IOC_ADD_ENCRYPTION_KEY` automatically
persists the key to the shared store. Other nodes retrieve it with:

```bash
# On each cluster node that needs the key
ocsfs-tool keys restore /dev/sdb   # lists + fetches + adds all stored keys
# Internally: OCSFS_IOC_KEY_LIST → OCSFS_IOC_KEY_FETCH → FS_IOC_ADD_ENCRYPTION_KEY
```

Two new ioctls are available for scripting:
| ioctl | Description |
|---|---|
| `OCSFS_IOC_KEY_LIST` | List key identifiers in the shared store (no raw material exposed) |
| `OCSFS_IOC_KEY_FETCH` | Decrypt and return raw key for a given identifier (`CAP_SYS_ADMIN`) |

### Limitations

| Limitation | Notes |
|---|---|
| No readahead | Encrypted inodes return early from `ocsfs_iomap_readahead()` — the iomap-based readahead path cannot decrypt asynchronously |
| No O_DIRECT | O_DIRECT bypasses the page cache; bounce-page decryption requires the page cache |
| Buffered writes only | `ocsfs_enc_writepages()` submits one synchronous bio per folio — acceptable for VM disk images, suboptimal for bulk streaming |
| No reflink / snapshot | Both operations return `-EOPNOTSUPP` on encrypted inodes (see cluster safety above) |
| No symlinks in encrypted dirs | Returns `-EOPNOTSUPP` until `fscrypt_get_symlink` is wired up |
| Key store not cluster-atomic | Concurrent `FS_IOC_ADD_ENCRYPTION_KEY` from two nodes is idempotent but not serialised; race on the key store block is benign (same-key writes produce the same result) |
