# OCSFS v2 — Developer Guide

How OCSFS v2 is built, how it is laid out on disk, how the code is organised, and
how to build, test and debug it. The design rationale lives in
[`design-v2.md`](design-v2.md); this guide is the practical companion.

---

## 1. Design in one paragraph

OCSFS v2 is a **shared-disk** filesystem: many nodes open the *same* block LUN.
The defining choice is **single-writer ownership** — every file (and metadata
object) is owned for writing by **one node at a time** via a coarse,
long-held *lease*, instead of a per-operation distributed lock. The owner does
all I/O locally at near-raw speed; ownership is handed over on close or
live-migration. This removes, by construction, the per-I/O cache-coherence
traffic and the compare-and-write storm that a fine-grained DLM would impose,
which is exactly right for the "one VM writes its disk from one host" workload.

The stack is layered L0–L5 so each layer is testable in isolation and the whole
single-node data path is validated **before** any cluster code runs.

| Layer | Responsibility | Files |
|---|---|---|
| **L0** transport | SCSI PR (fencing) + Compare-and-Write (atomic on-disk CAS) | `transport/scsi_pr.c` |
| **L1** on-disk format | superblock(+mirror), AG layout, inode store, journal, lease/membership tables — versioned + CRC32c | `mkfs`, `fsck`, `super.c` |
| **L2** local engine | inodes, dirs, extents, allocation, **iomap data path**, journal/WAL, reflink/CoW/dedup | `inode.c dir.c rename.c iomap.c bitmap.c journal.c refcount.c reflink.c file.c xattr.c extent_btree.c` |
| **L3** membership | heartbeat slots, liveness epochs, fencing; pluggable (corosync later) | `cluster.c` |
| **L4** leases | per-file ownership lease (L4) + metadata lease (L4b) | `lease.c` |
| **L5** recovery | dead-node detection → journal replay + lease reclaim | `lease.c` (`recover_*`) |
| growth / scrub / defrag | autonomous online grow, metadata scrub (= online fsck), defragmentation | `grow.c scrub.c defrag.c` |

---

## 2. Source layout

```
kmod2/            the v2 kernel module (the only kernel code)
  ocsfs.h         on-disk structs, in-core structs, all prototypes, feature bits
  super.c         mount/unmount, sb_info, statfs, sync_fs, inode cache
  inode.c         inode read/write/alloc/free, the extent map (inline ↔ B+tree)
  extent_btree.c  per-inode extent B+tree (spill past 16 inline extents)
  iomap.c         iomap_begin/end, the data path, CoW (ocsfs2_cow_range)
  bitmap.c        per-AG block allocator + free, FITRIM/discard
  dir.c rename.c  directory entries + hash B+tree index, rename
  xattr.c         extended attributes
  journal.c       per-node WAL: txn begin/commit/abort, replay, txn_forget
  refcount.c      per-AG refcount B+tree (reflink/snapshot/dedup/CoW)
  reflink.c       FICLONE/copy_file_range sharing + snapshot ioctl
  file.c          file_operations, fallocate (punch/zero), ioctl dispatch
  cluster.c       L3 membership, heartbeat, SCSI-PR fencing, workqueues
  lease.c         L4 ownership + L4b metadata leases, L5 recovery
  grow.c          autonomous, repeatable online autogrow
  scrub.c         online metadata-checksum + structural scrub (online fsck engine)
  csum.c          per-data-block CRC32c (mkfs -C): store (batched), inline read verify
  defrag.c        online extent compaction (OCSFS_IOC_DEFRAG)
  transport/scsi_pr.c   SCSI PR + CAW via the block layer
tools2/           userspace: mkfs.ocsfs2, fsck.ocsfs2, scrub/defrag/snap tools
proxmox2/         PVE storage plugin, mount helper, installer, systemd units
tests/v2/         test scripts (see §8) incl. the differential fsx harness
docs/             this guide, design-v2.md, admin-guide.md, testbed-setup.md
```

> v1 lives under `kmod/`, `tools/`, `src/` and on the `v1-legacy` branch. v2 is
> a clean rewrite; do not mix the two.

---

## 3. On-disk format (L1)

All metadata blocks carry a magic + CRC32c and are versioned by feature bitmasks
(`compat` / `ro_compat` / `incompat`). Block size = device logical block (4096).

```
[ superblock ][ mirror ] ... [ AG 0 ][ AG 1 ] ... [ per-node journals ]
                              [ lease table ][ membership/heartbeat table ]
```

- **Superblock**: magic, version/revision, UUID, label, geometry (block/AG
  counts), feature bitmasks, journal location, lease/membership offsets. Mirrored.
- **Allocation group (AG)** — self-contained:
  `{ ag_header, block bitmap, inode table, refcount B+tree root }`. Uniform size
  (the `AUTOGROW` compat layout) so grow can append more AGs.
- **Inode** (512 B): `{ magic, ino, mode, nlink, uid/gid, size, blocks, times,
  flags, extent_count, extent_tree_root, inline_extents[16], dir_btree_root,
  dirent_count, xattr_block, crc }`. Small/contiguous files use the 16 inline
  extents; large/fragmented files spill the whole map into the extent B+tree.
- **Refcount B+tree** (per AG): `{ phys, len, refcount }` records, `refcount ≥ 2`
  for shared ranges; a block with no record has an implicit refcount of 1.
- **Lease table**: one record per file under shared ownership:
  `{ ino, owner_slot, mode (NL/SH/EX), epoch, want_slot }`.
- **Journal**: one ring per node slot (WAL, redo).

`mkfs.ocsfs2` is authoritative for the layout; `fsck.ocsfs2` re-derives and
verifies it (offline against the device, or online against a mountpoint — §8).

---

## 4. The data path (L2, iomap)

OCSFS uses the kernel **iomap** framework for both buffered and O_DIRECT I/O, so
there is one mapping path and no `buffer_head`-based aliasing.

`ocsfs2_iomap_begin()` (iomap.c) maps a logical range to physical blocks:

1. `ocsfs2_extent_find()` looks up the covering extent (inline array or B+tree).
2. **Write/zero on a SHARED extent → copy-on-write.** It checks
   `ocsfs2_needs_cow()` (`refcount > 1`) for **every block the write touches**
   (an extent flagged SHARED can hold a mix of still-shared and now-private
   blocks). If any is shared, `ocsfs2_cow_range()` allocates fresh private
   blocks, copies the touched sub-range, repoints the extent and refcount-decs
   the old range — all in one journal txn. Block-granular, so a small write to a
   big reflinked extent copies little.
3. **Write to a hole** allocates (bounded by `OCSFS2_ALLOC_CAP_BLOCKS` and the
   next extent) and inserts a new extent.
4. **Read of a hole** is clamped to the next allocated extent so a readahead
   never swallows a following mapped extent (a classic stale-data trap).

> There is **no `.mmap`**: the Proxmox workload never mmaps an OCSFS file (QEMU
> uses pread/pwrite/aio; LXC images go through a loop device). Omitting it also
> removes the mmap+CoW stale-read hazard.

### Extent map: inline ↔ B+tree

≤ 16 extents live inline in the inode. On overflow the **whole** map spills to a
per-inode B+tree (`extent_btree.c`, proactive top-down split). **All** mutating
paths spill — `ocsfs2_extent_insert` (writes) and, via
`ocsfs2_extent_spill_only()`, the punch (`ocsfs2_extent_punch_range`) and CoW
remap (`ocsfs2_extent_remap_range`) splits. So a file can hold an effectively
unbounded number of extents; none of these paths returns a spurious `-ENOSPC` on
a fragmented-but-non-full volume.

---

## 5. Journaling (L2, Plan 3)

A per-node redo WAL. Metadata mutations run inside a transaction:

```c
struct ocsfs2_txn *txn = ocsfs2_txn_begin(sb);
ocsfs2_jbuf(bh);            /* enrol a buffer's after-image */
... mutate bh ...
ocsfs2_txn_commit(txn);     /* or ocsfs2_txn_abort(txn) + reload in-core state */
```

- Transactions **nest by participation**: if one is already current, inner
  `begin/commit` are no-ops and everything joins the outer txn (`TREE_TXN_*` in
  `extent_btree.c` uses this).
- `ocsfs2_txn_forget(sb, start, count)` removes just-freed blocks from the live
  txn and `clean_bdev_aliases()` so a freed metadata block cannot be checkpointed
  over a block that has been reallocated as data.
- Replay on mount is idempotent and stops cleanly at a torn record (jbd2-style).
- L5 replays a **dead peer's** journal (`ocsfs2_journal_replay_slot`) using
  coherent reads, off the heartbeat path.

---

## 6. reflink / snapshot / dedup / CoW (L2, Plan 4)

- **Share** (`reflink.c`): walk the source by logical, `ocsfs2_refcount_inc()`
  the shared physical sub-range, insert a `SHARED` extent into the destination,
  flag the source extent `SHARED`. FICLONE, `copy_file_range`, the snapshot ioctl
  and FIDEDUPERANGE all funnel through this (dedup = "share if the VFS compare
  says the ranges are identical").
- **CoW** (`iomap.c`): the first write to a shared block copies it out (see §4).
- **Refcount store** (`refcount.c`): per-AG B+tree; `refcount_get`, `refcount_inc`
  and the refcount-aware `free_blocks_rc` (decrement; free to the bitmap only at
  0). Range arithmetic (`rc_apply`) handles partial overlaps.

> **Invariant that bit us before:** a buffered write must CoW *before* the dirty
> folio is written back, and the SHARED flag must never be cleared on a block
> that still has `refcount ≥ 2`. Decide CoW per-block over the written range.

---

## 7. Cluster: membership, leases, recovery (L3–L5)

- **L0/L3**: each node owns a heartbeat slot and a liveness epoch; SCSI-PR keys
  fence a dead node. The L3 interface is pluggable (corosync is a future
  provider). On-disk CAW (`scsi_pr.c`) is the atomic primitive for lease/HB CAS.
- **L4 ownership lease**: a file is `EX`-owned by the writing node (coarse,
  long-held), `SH` for readers. Conflicts return `-EBUSY` until release/handoff;
  open/close drives acquisition so live migration just moves the lease.
- **L4b metadata lease**: serialises namespace ops on a shared directory.
- **L5 recovery**: a survivor wins an `EX` lease on the dead slot's recovery
  resource, replays its journal, reclaims its leases — then peers proceed.

All of L2 is validated single-node first; cluster correctness is tested on 2–3
real nodes (see `testbed-setup.md`).

---

## 8. Build, test, debug

### Build the module (on a node with the target kernel)

```bash
make -C /lib/modules/$(uname -r)/build M=$PWD/kmod2 modules
insmod kmod2/ocsfs2.ko          # or: modprobe ocsfs2 (after install.sh + depmod)
```

> The module is out-of-tree: **build it on a node whose `uname -r` matches**, not
> on a workstation (mismatched kernel → "Invalid module format").

For deployment, `proxmox2/install.sh` builds it through **DKMS** (`kmod2/dkms.conf`,
source staged to `/usr/src/ocsfs2-2.0`), which rebuilds the module automatically on
every kernel upgrade. Iterate with the plain `make` above; use DKMS for the
install/upgrade path (`dkms status`, `dkms build/install -m ocsfs2 -v 2.0`).

### Build the tools

```bash
cc -O2 -Wall -o mkfs.ocsfs2 tools2/mkfs.c
cc -O2 -Wall -o fsck.ocsfs2 tools2/fsck.c
# scrub/defrag/snap CLIs are small ioctl wrappers (tools2/*.c)
```

### Online vs offline fsck

`fsck.ocsfs2` runs in two modes:

- **offline** — given a `/dev/...` **device** (must be unmounted): full
  read-only structural + checksum verification straight off the disk.
- **online** — given a **mountpoint** (mounted, in-use): it calls the
  `OCSFS_IOC_SCRUB` ioctl, which is the same engine `scrub.c` uses — it verifies
  every metadata checksum (super, AG headers, inode table, extent + refcount
  B+trees, xattrs) and per-AG structural consistency while the FS keeps running,
  taking the normal leases. Cross-referential **repair** stays offline.

```bash
fsck.ocsfs2 /dev/disk/by-id/scsi-XXXX     # offline (unmounted)
fsck.ocsfs2 /mnt/vmstore                   # online (mounted) — via the scrub ioctl
ocsfs2-scrub /mnt/vmstore                  # same online check, scrub-flavoured output
```

### The reliable test method — **differential vs XFS**

Raw `xfstests ./check` is unreliable for a custom FS: its golden output flags
benign stdout as failure, and `fsx` emits ops that even XFS rejects (e.g.
unaligned O_DIRECT writes). So we run the **same** `fsx` seed+params on OCSFS and
on XFS (`reflink=1`) and treat a result as a real bug **only when OCSFS diverges
from XFS**:

```bash
tests/v2/fsx_diff.sh <ocsfs_dev> <xfs_dev> [N] ["seeds"]
# e.g. tests/v2/fsx_diff.sh /dev/sdc /dev/sdd 20000 "1 2 3 4 5 6 7 8"
```

The matrix is buffered, O_DIRECT and clone-isolated, all with aligned params
(`-r/-t/-w = 4096`) and mmap disabled (`-R -W`, since OCSFS has no mmap). A clean
run prints `FSX_DIFF_CLEAN`.

### Pinpointing a corruption

1. **Replay-bisection**: `fsx --record-ops=f.ops` the failing seed, then
   binary-search the smallest `head -K f.ops` whose replay makes OCSFS and XFS
   files differ (`cmp`). Line *K* is the corrupting op.
2. **FIEMAP** (`filefrag -v`) before/after that op to see the extent mapping.
3. **bpftrace** for the kernel ground truth (no BTF needed for scalar args):
   ```bash
   bpftrace -e 'kprobe:ocsfs2_free_blocks_rc /arg1<=BLK && BLK<arg1+arg2/
                { printf("dec %llu+%u %s\n", arg1, arg2, kstack(3)); }'
   ```
   and `tracepoint:block:block_bio_queue` (filter the victim sector + `kstack`)
   to find *who* wrote a block.

Always reproduce a suspected bug on **XFS/ext4 with the same seed first** — if it
fails there too, it is a tester artifact, not an OCSFS bug.

### Other test scripts (`tests/v2/`)

`test_datapath.sh`, `test_extent.sh`, `test_reflink.sh`, `test_fallocate.sh`,
`test_xattr.sh`, `test_posix.sh` (single-node features); `test_cluster*.sh`,
`test_cluster_recovery.sh`, `test_cluster_perf.sh` (2–3 nodes);
`test_grow*.sh`, `test_dedup.sh`, `test_discard.sh`, `test_scrub.sh`,
`test_crash.sh`.

---

## 9. Adding a feature — checklist

1. New on-disk state? add a feature bit (`compat`/`ro_compat`/`incompat` in
   `ocsfs.h`), write it in `mkfs`, verify it in `fsck`, gate the code on it.
2. Mutate metadata only inside a journal txn; enrol every changed buffer with
   `ocsfs2_jbuf()`; free blocks with `ocsfs2_free_blocks_rc()` if they can be
   shared.
3. Keep buffered and O_DIRECT coherent (go through iomap; never read metadata via
   a path that bypasses the txn/cache-coherence helpers).
4. Add a differential `fsx` case if it touches the data path; add a `test_*.sh`
   if it is a feature; validate single-node, then 2–3 nodes; finish with
   `fsck.ocsfs2` clean (online and offline).
5. Keep files < 500 lines; match the surrounding style.
