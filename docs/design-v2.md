# OCSFS v2 — Rearchitecture Design Spec

**Date:** 2026-06-02
**Status:** Approved (design); pre-implementation
**Author:** Claude (Opus 4.8), tech lead per user delegation
**Supersedes:** the entire `kmod/` v1 implementation (kept as reference until v2 reaches parity)

---

## 1. Goal & non-goals

### Goal
A clustered shared-disk filesystem for **Proxmox VE** on a shared SAN LUN
(**TrueNAS iSCSI**), holding **VM/CT disk images**. Multiple nodes mount the same
LUN read-write concurrently. Functionally symmetric: any node can host any VM, no
metadata server, no special node.

**Priority order (non-negotiable): data integrity → reliability → performance.**

### Non-goals (v1)
- Being a fast general-purpose POSIX filesystem with high metadata concurrency.
  We optimize for the VM-disk workload: few large files, each written by one node.
- Cross-file dedup and compression (v2).
- Per-file encryption (do it at the LUN or guest layer), VAAI offload, quotas.

### Why v1 failed (root causes, not symptoms)
1. **Granularity ↔ medium mismatch.** Fine, frequent locks (per-inode/per-AG/
   per-refcount) on a slow medium (1 SCSI CAW per lock op). TrueNAS stalls CAW
   under concurrent load → D-state tasks → 30s lock-timeout cascade → hard hang.
2. **Cache coherence as a patch.** The buffered path (folio cache + iomap) fought
   cross-node coherence with epochs and "fresh-read" hacks → an endless tail of
   data-corruption bugs.
3. **Feature sprawl.** Dedup/compression/encryption/snapshots/VAAI/quota/grow on a
   base that wasn't reliable — each feature multiplied the coherence/recovery state
   space.

---

## 2. Central principle: single-writer ownership

> **Invariant SW1:** At any instant, every regular file is owned **read-write by at
> most one node**. All data-path I/O to a file happens **only on its owner**.
> Ownership is **coarse** (per-file) and **long-held** (VM lifetime), transferred
> only at open / close / live-migration. Read-only files (ISOs, templates) may have
> many **shared (SH)** readers; only **exclusive (EX)** write is single-node.

Consequences (these are *by construction*, not enforced by patches):
- **No cross-node data cache-coherence problem.** Only one node ever caches a file
  for write → the entire "stale folio / cross-node invalidation / fresh-read" bug
  class cannot occur.
- **Native data-path performance.** On the owner, reads/writes are local page cache
  + iomap to the block device: zero coordination, full device speed, no per-write
  CAW.
- **Coordination is rare.** Cross-node coordination happens only at ownership
  transfer and namespace ops (create/unlink/rename) — orders of magnitude rarer
  than data I/O. The slow on-disk medium is touched rarely → no CAW storm.

This is the real Proxmox workload and the reason VMFS works.

---

## 3. Layering

Each layer has one purpose, a defined interface, and is independently testable.

| Layer | Responsibility | Test surface |
|---|---|---|
| **L0 Transport** | device I/O, SCSI PR + CAW, crc32c | pure functions, KUnit |
| **L1 On-disk format** | superblock(+mirror), space map, inode store, extent map, journal, lease table, node table — all versioned & checksummed; authoritative `mkfs` + `fsck` | offline dump/verify |
| **L2 Local FS engine** | inodes, dirs, extents, allocation, **data-path (iomap)**, WAL/journal, reflink/snapshot/CoW. **Zero cluster code.** | **single-node / loopback** |
| **L3 Membership & fencing** | who is alive/dead, fence the dead, behind `struct ocsfs_cluster_ops` | mock + real 2–3 nodes |
| **L4 Ownership/lease manager** | acquire / release / revoke / steal per-file & namespace ownership — the only cross-node coordination on the normal path | real 2–3 nodes |
| **L5 Recovery** | on node death: fence → replay journal → reclaim leases | real node crash |

**Layering discipline:** L2 — where all the integrity bugs live — is validated
**completely on a single node** before any cluster code (L3–L5) is written. L2 must
not depend on L3/L4; in single-node mode the cluster layers are no-ops.

---

## 4. Coordination model

### v1 of the cluster layer: on-disk, coarse, zero external deps
- **Membership:** on-disk heartbeat (each node writes a liveness record to its slot).
- **Ownership:** on-disk **lease table** (see §6.5), mutated by SCSI CAW (with a
  PR-lease software fallback for non-CAW devices).
- **Fencing:** SCSI-3 Persistent Reservations (preempt-and-abort) — authoritative
  at the fabric, so even a slow-but-alive node cannot corrupt after it is fenced.

Because leases are **coarse and renewed by node liveness** (not per-lease, not
per-op — see §6.5), the on-disk CAW rate is ~`(opens + transfers + recoveries)`,
**not** ~`(writes)`. The CAW-storm that killed v1 does not arise.

### L3 is an interface, not a commitment
`struct ocsfs_cluster_ops` abstracts membership + failure detection:
```
struct ocsfs_cluster_ops {
    int  (*node_alive)(sb, slot, gen);     /* is slot@gen currently alive? */
    void (*on_node_dead)(sb, slot, gen);   /* callback → triggers recovery */
    int  (*self_liveness_ok)(sb);          /* are WE still considered alive? */
};
```
v1 provides an on-disk-heartbeat implementation. A **corosync-backed provider**
(faster, more reliable failure detection — the genuine weak point of on-disk
heartbeat) can be added later **without touching L2/L4/L5**. The corosync vs.
on-disk decision is deferred behind this interface; we do not block early data-path
testing on a cluster stack.

---

## 5. Data integrity mechanisms

- **Checksums everywhere (crc32c).** Every metadata block is self-describing:
  common header `{ h_magic, h_type, h_seq, h_crc }` covering the whole block.
  Data-block integrity is via extent checksums where enabled.
- **Per-node WAL (redo) journal.** A transaction journals *after-images* of metadata
  blocks; COMMIT is the durability point; replay re-applies committed txns.
- **Ordered durability.** Data blocks referenced by a new/changed extent are flushed
  to disk **before** the metadata that points at them is committed → after a crash,
  metadata never references uninitialized/garbage data.
- **CoW for metadata btrees.** B+tree node updates never overwrite a live block in
  place: write to a new block, then swap the parent pointer in the same txn →
  crash-atomic by pointer swap. Eliminates v1's fragile "journal before-image +
  in-place overwrite" pattern and the bug class around it.
- **Atomic single-block writes.** 4K metadata blocks map to device sectors written
  atomically; anything larger is journaled.
- **Single-writer (SW1).** Removes all cross-node data races; the journal handles
  intra-node crash atomicity.
- **fsck** is a first-class L1 tool: full structural + checksum + refcount
  verification, used as a gate after every test scenario.

---

## 6. On-disk format (v2)

Block size 4096 B; all fields little-endian. The layout keeps the XFS-like AG
structure of v1 (which is sound) and **replaces only the broken parts**: the
per-op lock table becomes a coarse lease table, and the metadata-update discipline
becomes CoW + ordered journaling.

### 6.1 Region map (byte offsets, fixed at mkfs)
```
[0]              Superblock primary (4K)
[4K]             Superblock mirror (4K)
[fixed region]   Cluster coordination:
                   - Node/membership table   (N slots)
                   - Heartbeat region        (N slots)
                   - Lease table             (hashed array + overflow)
                   - Recovery-leader block
[journal array]  Per-node WAL (one journal per node slot)
[AG array]       Allocation groups
[data]           Data blocks (within AGs)
```
`mkfs` records every region offset in the superblock; the kernel **rejects at
mount** any volume whose journal/data offsets overlap the coordination region
(the CRIT-O1 lesson from v1).

### 6.2 Superblock
Magic, version/revision, UUID, label, block/extent/AG geometry, feature bitmasks
(compat / incompat / ro_compat), region offsets, free-space counters, max_nodes.
Mirror written at 4K; mount validates primary, falls back to mirror on crc fail.

### 6.3 Allocation group (AG)
Each AG is self-contained: `{ ag_header, block bitmap, inode table, refcount btree
root }`. Per-AG free counters. Inode numbers map to AG by `ino / ag_size`.
- **Block bitmap:** 1 bit/block. Simple, robust.
- **Inode table:** fixed 512-byte on-disk inodes.
- **Refcount btree:** physical-block → refcount, for reflink/snapshot/CoW. Empty
  (root=0) until first shared block.

### 6.4 Inode (512 B)
`{ magic, ino, mode, nlink, uid, gid, size, blocks, times, flags, extent_count,
extent_tree_root, inline_extents[16], dir_btree_root, dirent_count, xattr_block,
crc }`. Small/contiguous files use inline extents; large/fragmented files spill to
the extent B+tree. Symlink targets ≤ inline area stored inline.

### 6.5 Lease table (the new coordination structure)
Hashed array of lease entries (open-addressing + overflow chain), indexed by
`resource_id` (inode number, or a namespace/dir resource id).
```
struct ocsfs_disk_lease {
    __le32  l_magic;
    __le64  l_resource_id;     /* inode # / dir resource */
    __le16  l_owner_slot;      /* node holding EX; 0xFFFF = none  */
    __le16  l_mode;            /* NL / SH / EX                    */
    __le32  l_owner_gen;       /* owner mount generation (anti-zombie) */
    __le32  l_sh_holders[8];   /* SH-holder bitmap, up to 256 nodes */
    __le16  l_want_slot;       /* a peer requesting handoff; 0xFFFF = none */
    __le32  l_seq;             /* CAS version                     */
    __le32  l_crc;
    ...                        /* padded to a fixed entry size    */
};
```
**Liveness-epoch validity (no per-lease renewal).** A lease is honored iff its
owner node is currently ALIVE with matching `l_owner_gen` (per L3 / heartbeat). The
deadline lives on the **node** (one heartbeat per node), not per lease — so a node
holding 100 leases still writes **one** heartbeat, never 100 lease renewals. A lease
entry is written **only** on acquire / transfer / release / revoke.

**Acquire EX(resource):**
1. CAW the lease entry from `{owner=none}` (or `{owner=self}`) to `{owner=self,
   gen=self, mode=EX}`.
2. If currently owned by a **dead** node (L3 says not alive, or gen mismatch):
   only the recovery leader may steal (after fencing) — see §8.
3. If owned by a **live** peer: set `l_want_slot=self` (CAW), wait for the peer's
   heartbeat thread to release (revocation). Rare path; in normal Proxmox flow the
   external migration coordinator ensures the source releases first.

**SH(resource):** set our bit in `l_sh_holders` via CAW. A node wanting EX must
wait for all SH holders to drop (signaled via `l_want_slot`).

**Release:** clear ownership / SH bit via CAW.

### 6.6 Journal
Per-node ring: `{ jh_header (head/tail/seq), txn records }`. Record types
BEGIN / METADATA(after-image block refs) / COMMIT / CHECKPOINT. Monotonic
sequence; replay scans forward, applies COMMITted txns, stops cleanly at a torn
record (jbd2-style). Wrap-safe (monotonic counters, not raw offsets).

### 6.7 Directory
Dirents packed in the directory inode's data blocks; a **hash B+tree** index
(`dir_btree_root`) is built once a dir exceeds a threshold, for O(log n) lookup.
Each dirent is crc-protected; any in-place field change recomputes the crc.

---

## 7. Module map (`kmod2/`)

```
kmod2/
  ocsfs.h            on-disk + in-memory types, constants
  transport/
    scsi_pr.c        SALVAGED — SCSI PR + CAW (block-layer pr_ops + BSG/scsi_execute_cmd)
    crc.h            crc32c helpers (trivial)
  format/
    super.c          mount/unmount, superblock, fill_super
    mkfs (userspace) authoritative formatter
    fsck  (userspace) authoritative checker
  engine/            L2 — no cluster code
    inode.c  dir.c  extent.c  extent_btree.c  alloc.c  bitmap.c
    iomap.c  file.c  journal.c  journal_replay.c
    reflink.c snapshot.c refcount.c
  cluster/           L3–L5
    cluster.c        struct ocsfs_cluster_ops + on-disk provider
    heartbeat.c  node.c  lease.c  fence.c  recovery.c
```
File size budget: keep each under ~500 lines (CLAUDE.md rule); split when a file
grows past its single purpose.

---

## 8. Recovery (L5)

On `on_node_dead(slot, gen)` from L3:
1. **Elect** a recovery leader (CAS on the recovery-leader block; epoch-guarded;
   crash-safe phase resume so a leader that dies mid-recovery is succeeded).
2. **Fence** the dead node: SCSI-PR preempt-and-abort its key → its in-flight I/O
   is rejected at the array. (Self-fence: a node whose heartbeat is starved past the
   death window self-quiesces — refuses new EX, goes read-only — *before* a peer can
   fence it, closing the alive-but-declared-dead window.)
3. **Replay** the dead node's journal (redo committed txns).
4. **Reclaim** the dead node's leases: every lease entry with `owner=slot, gen=gen`
   is reset to `owner=none`. Now those resources are acquirable by survivors.
5. Survivors continue; the dead node, on reboot, gets a fresh slot+gen and rejoins.

Recovery is **rare** (node death), so its on-disk coordination cost is irrelevant
to steady-state performance.

---

## 9. Scope

### v1 (must be bulletproof and tested before anything else)
- `mkfs` + `fsck` (authoritative).
- L2 single-node data path: files, dirs, extents, allocation, iomap (buffered +
  O_DIRECT), WAL + crash recovery (replay).
- Thin/sparse (unwritten extents), `fallocate` punch/zero, truncate.
- **reflink (FICLONE) + CoW snapshots** (needed for Proxmox clone/snapshot).
- POSIX essentials Proxmox needs: xattr, POSIX ACL, sym/hardlink, `fiemap`,
  `SEEK_HOLE/DATA`.
- L3–L5: ownership/lease, on-disk membership, SCSI-PR fencing, node-crash recovery
  for 2–3 nodes.

### v2 (after v1 is solid)
- Cross-file dedup (DDT), inline LZ4/ZSTD compression.

### Dropped/deferred
- Per-file encryption, VAAI offload, quotas.

---

## 10. Testing strategy (starts immediately, layer-by-layer)

**Two hard constraints from the user:**
- **No loopback devices** — they are slow and unrepresentative. All mount-based
  testing runs on **real iSCSI LUNs from TrueNAS** attached to Proxmox nodes.
- **The real validation target is multinode.** Single-node testing is only the
  first gate to lock down integrity; a feature is not "done" until it passes on the
  **2–3 node cluster**.

Phases (each gates the next, but the endpoint of every data-path feature is the
multinode run):

1. **L0/L1 offline:** KUnit on pure functions (crc, CAW CDB build, lease CAS state
   machine). `mkfs` writes a format; `fsck` reads it back clean. These run anywhere
   (no mount, no block device needed) and are the fastest feedback loop.
2. **L2 single-node, on a real iSCSI LUN:** a **dedicated TrueNAS LUN attached to
   one Proxmox node** (`/dev/sdX`). The proven integrity technique — `fsx`-style
   fuzzer (`tests/ocsfs_fsx.c`) + `repro`-style deterministic reproducers, with an
   in-memory mirror and **cross-checked against ext4 with identical seeds** (catches
   tester bugs / false positives). Cold-read verification across `drop_caches`.
   `fsck` clean after every run. This is the *integrity gate*, not the finish line.
3. **Crash recovery:** on a real node + real LUN — injected aborts / torn records /
   wrapped journal during replay, then full `sysrq-b` power-loss crash.
4. **Multinode (real HW) — the actual validation target:** Proxmox VE cluster
   (n1; n2=192.168.1.49; n3=192.168.1.45) on a real TrueNAS iSCSI LUN
   (TrueNAS=192.168.1.47) supporting SCSI PR + CAW. Every data-path feature is
   re-run here: cross-node ownership handoff, single-writer enforcement, SH-reader
   sharing, namespace coherence, fencing, and **real node-crash recovery**
   (`sysrq-b`) with survivors intact and `fsck` clean.
5. **Performance:** `fio` 4K O_DIRECT random read/write to a preallocated image vs.
   the raw LUN; target near-raw for the single-owner case, on the cluster.

**Standing constraints (from project memory):**
- The kernel module is **always built on a Proxmox node** (kernel match) — never on
  the workstation (`Invalid module format`).
- Use **moderate** concurrent load in cluster tests; v1's torture test wedged nodes.
- Never `fuser -km` a mountpoint (kills init). Validate every repro on ext4 first.

---

## 11. Repo layout & migration

- New code lands in `kmod2/`. The existing `kmod/` stays untouched as reference
  until v2 reaches functional parity, then `kmod/` is retired.
- Salvaged, audited-clean: `kmod/scsi_pr.c` → `kmod2/transport/scsi_pr.c`; crc32c
  helpers. Everything else is rewritten.
- `mkfs`/`fsck` rewritten against the v2 format.

### Documentation (final deliverable)
Once v2 is implemented and validated multinode, **`README.md` and the entire
`docs/` tree are rewritten from scratch** to describe the v2 architecture — the
single-writer ownership model, the layering, the coordination/lease design,
recovery, the on-disk format, and the (multinode, no-loopback) validation results.
The v1 documentation is discarded; nothing about the old per-op-DLM design carries
over.

---

## 12. Open risks & mitigations
- **On-disk membership is the weak link.** Mitigation: coarse leases make recovery
  rare; SCSI-PR fencing is authoritative; self-fence closes the zombie window; and
  L3 is pluggable for a corosync upgrade.
- **Namespace ops (shared dir create/unlink) are the one concurrent-write case.**
  Mitigation: brief per-dir EX lease + forced metadata refresh under the lease; low
  frequency vs. data I/O; optimizable later via directory delegation.
- **Effort.** Phased delivery: a single-node-correct L2 is the first shippable,
  testable milestone; cluster layers build on a proven base.
