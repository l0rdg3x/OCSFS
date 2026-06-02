<div align="center">

# OCSFS v2 — Open Cluster Shared FileSystem

**A clustered filesystem for Linux on shared block storage — a Proxmox-native alternative to VMware VMFS.**

[![License: GPL-2.0](https://img.shields.io/badge/License-GPL--2.0--only-blue.svg)](LICENSE)
[![Status: Alpha](https://img.shields.io/badge/status-alpha%20%2F%20research-orange.svg)]()
[![Platform: Linux](https://img.shields.io/badge/platform-Linux%20kernel%206.x%2F7.x-informational.svg)]()
[![Architecture: single-writer ownership](https://img.shields.io/badge/arch-single--writer%20ownership-blueviolet.svg)]()
[![AI-generated](https://img.shields.io/badge/code-100%25%20AI%20generated-ff69b4.svg)]()

</div>

> [!IMPORTANT]
> **This project is entirely AI-generated.** Every line of code, documentation,
> and tooling was written by [Claude](https://claude.ai) (Anthropic) through an
> iterative conversation with a human operator. No human has written or reviewed
> the source code directly. Treat it accordingly — **do not use it with data that
> matters.**

> [!NOTE]
> **This is OCSFS v2 — a from-scratch rearchitecture** (kernel module `ocsfs2`,
> sources in `kmod2/`, tools in `tools2/`). It replaces the v1 design (still on
> `main` under `kmod/`) whose per-operation distributed lock manager could not
> scale metadata under cross-node contention and fought a never-ending stream of
> cross-node buffered-cache coherence bugs. v2's organizing principle —
> **single-writer ownership** — removes *both* problem classes by construction.

---

OCSFS lets **multiple Linux nodes mount the same SAN LUN at the same time**,
read-write, with crash recovery and hardware fencing — the same job VMware VMFS
does for ESXi, but open-source and built for **Proxmox VE**.

It needs no lock-manager daemon and no cluster network: **all coordination lives
on the shared device itself**, using SCSI Compare-And-Write for atomic on-disk
state changes and SCSI-3 Persistent Reservations for fencing.

## 🧭 The core idea — single-writer ownership

A running VM writes only to *its own* disk image. So instead of a fine-grained,
per-operation distributed lock manager (v1), v2 gives each regular file a single
**owner node** that holds it read-write, acquired at open-for-write and held for
the life of the VM; read-only files (ISOs, templates) have many shared readers.

This one decision pays off twice:

- **No cross-node cache-coherence bug class.** Only one node ever writes a file's
  data, so there is nothing to keep coherent across nodes for that data — the
  defect family that dominated v1 simply cannot occur.
- **No per-operation CAW storm.** Ownership is coarse and long-held, so steady-
  state file I/O does **zero** on-disk locking. The SAN's Compare-And-Write path
  is touched only at ownership hand-off (open/close/migration) and for the small
  amount of genuinely shared metadata (namespace, allocation), never per I/O.

It is also exactly the Proxmox workload, which is why VMFS works the same way.

## ✨ Highlights

| | |
|---|---|
| 🧭 **Single-writer ownership** | Each file owned RW by ≤1 node (coarse, long-held lease); RO files = many SH readers |
| 🧩 **No external dependencies** | No DLM daemon, no corosync, no cluster LAN — coordination is on the LUN |
| 🛡️ **Hardware fencing** | SCSI-3 Persistent Reservations evict a failed node at the fabric |
| 💓 **Storage-path heartbeat** | Liveness proven by writes to the LUN (liveness-epoch), immune to LAN partitions |
| 📓 **Crash-safe journaling** | Per-node WAL with redo; replay on mount + replay of a *dead peer's* journal during recovery |
| 🌳 **Modern data path** | iomap, inline extents + per-inode extent B+tree, O_DIRECT, sparse, `FIEMAP`, `SEEK_HOLE/DATA` |
| 🪞 **Space efficiency** | Reflink (`FICLONE`), CoW snapshots, **cross-file dedup** (`FIDEDUPERANGE`), thin/sparse, **discard/TRIM** |
| 📈 **Autonomous online autogrow** | Grow the LUN any number of times — each node detects it and grows into the new space hot, **repeatedly** |
| 🩺 **Online metadata scrub** | Verify every on-disk checksum on a live volume (`OCSFS_IOC_SCRUB`) to catch silent bitrot |
| 🏎️ **Near-raw VM-disk I/O** | O_DIRECT (`cache=none`) seq write **scales with node count**; random is bound by the shared LUN, with no clustering tax |
| 🧱 **Proxmox-native** | `mount -t ocsfs2 -o cluster`; live migration *is* the lease hand-off |

---

## 🚦 Status & validation

> [!WARNING]
> **Alpha / Research.** The single-node data path is `fsx`-validated; the cluster
> (membership, single-writer ownership, directory coherence, crash recovery) is
> validated on a **real 2–3 node iSCSI cluster**. **Not production-ready — do not
> use with data that matters.**

All testing runs on **Proxmox VE 9 nodes (kernel 7.0.x-pve)** against **real
iSCSI LUNs from TrueNAS SCALE** with **SCSI Persistent Reservations +
Compare-And-Write** — full cluster mode with hardware CAS, never a degraded
fallback. **No loopback devices.**

### Single node — the integrity gate (`fsck` clean, zero kernel warnings)

- ✅ buffered + **O_DIRECT** write/read with sha256 round-trip across `drop_caches`
- ✅ **inline extents → per-inode extent B+tree** spill, validated with `fsx`
  (buffered 3×30 000 ops + O_DIRECT) against an in-memory mirror, cross-checked
  vs XFS with identical seeds
- ✅ **reflink** (`FICLONE`) + **CoW snapshots** with copy-on-write isolation
- ✅ **cross-file dedup** (`FIDEDUPERANGE`): identical files share storage, a
  write diverges via CoW, differing files dedupe nothing
- ✅ xattr, POSIX ACLs, sym/hardlinks, `mknod`, nested directories
- ✅ `fallocate` preallocate / **punch-hole** / **zero-range**, sparse, truncate
- ✅ **discard/TRIM** (`fstrim`) reclaims free space on the thin LUN (SCSI UNMAP)
- ✅ **autonomous online autogrow**, including **repeated** grows in one mount
  (2→6→14→28 GiB via dm-linear over the real LUN; data intact each step)
- ✅ **online metadata scrub** verifies all checksums; detects an injected
  inode-checksum corruption
- ✅ **crash recovery**: journal replay on mount reconstructs a committed-but-
  uncheckpointed transaction (validated by `sysrq` power-loss)

### Two / three nodes — the real validation target (`fsck` clean)

- ✅ **L3 membership + fencing** — nodes see each other alive via the storage
  heartbeat; a paused node is DECLARED DEAD past the window and SCSI-PR fenced
- ✅ **L4 single-writer ownership** — an EX owner blocks a peer's open-for-write;
  after release the peer takes it and reads coherent data; concurrent writes to
  *different* files both land
- ✅ **L4b directory coherence** — two nodes creating files/dirs in the **same**
  directory both converge with no lost entries and a consistent on-disk `i_size`
- ✅ **L5 recovery** — a dead node's owner-leases are eagerly reclaimed and its
  per-node journal is replayed by the survivor (off the heartbeat path), so its
  files become usable again with content intact
- ✅ **multinode performance** (see below) — sequential O_DIRECT write scales
  with node count; no metadata collapse under concurrent load
- ✅ **xfstests** curated `generic` integrity set (`fsx`, `fsstress`, posix) —
  see `tests/v2/`

### What's deferred (see [Known limitations](#️-known-limitations) → *Fase F*)

- ⏳ inline LZ4/ZSTD **compression** — architecturally at odds with the iomap
  data path and O_DIRECT; deliberately not bolted on
- ⏳ per-**data-block** checksums — needs a format-v3 region + write hooks on
  both the buffered and O_DIRECT paths; metadata is already fully checksummed and
  the scrub verifies it

---

## ⚡ Performance

Random 4 KiB O_DIRECT (`--direct=1 --ioengine=libaio`) is what a running VM does
to its disk, measured against a **2 GiB file** on a clustered OCSFS v2 volume,
real **TrueNAS SCALE iSCSI LUN** (1 GbE, SSD-backed zvol, SCSI PR + CAW), each
node writing **its own** file (single-writer ownership).

| Workload (O_DIRECT) | 1 node | 2 nodes concurrent (aggregate) | Note |
|---|--:|--:|---|
| Sequential write, 1 MiB | 80.7 MB/s | **160 MB/s** (91 + 68) | **scales ~2×** |
| Random write, 4 KiB QD32 | 4 084 IOPS | 3 833 IOPS (1 906 + 1 927) | LUN-bound, no collapse |
| Random read, 4 KiB QD32 | 26 900 IOPS | ~28 000 IOPS | LUN / 1 GbE-bound |

The point is the **shape**: because ownership is coarse and long-held, the second
node adds throughput on independent files (sequential scales) and the random
ceiling is the **shared LUN/network**, not OCSFS — there is no per-operation
clustering tax. (v1, by contrast, collapsed to a few hundred IOPS under cluster
load because every 4 KiB I/O did synchronous on-disk lock work.)

---

## 🏗️ Architecture

```
        ┌──────────────────────────────────────────────────────────────┐
        │                    Shared block device (LUN)                  │
        │                 iSCSI / Fibre Channel SAN                     │
        │                                                               │
        │  SB │ node table │ heartbeat │ lease table │ recovery         │
        │     │ journal[per node] │ AG[0..N): bitmap·inodes·refcount·data│
        └───────┬───────────────────────────────────────────┬──────────┘
                │   SCSI-3 PR (fencing)                       │
                │   SCSI CAW (atomic on-disk state)           │
       ┌────────┴────────┐   ┌─────────────────┐   ┌──────────┴──────┐
       │     node-1      │   │     node-2      │   │     node-N      │
       │    ocsfs2.ko    │   │    ocsfs2.ko    │   │    ocsfs2.ko    │
       │ VFS·iomap·lease │   │ VFS·iomap·lease │   │ VFS·iomap·lease │
       └─────────────────┘   └─────────────────┘   └─────────────────┘
```

### Layering (L0–L5)

| Layer | Responsibility | Source |
|---|---|---|
| **L0** transport | SCSI-3 PR (register/preempt-abort) + Compare-And-Write via BSG-direct | `transport/scsi_pr.c` |
| **L1** on-disk format | XFS-like AGs, lease table, per-node journals; CRC32c on every metadata block | `ocsfs.h`, `tools2/` |
| **L2** local engine | iomap data path, extents (inline + B+tree), allocation, namespace, journal, reflink/CoW/dedup — **validated single-node before any cluster code** | `iomap.c` `inode.c` `dir.c` `bitmap.c` `journal.c` `refcount.c` `reflink.c` `extent_btree.c` `file.c` `xattr.c` |
| **L3** membership | On-disk heartbeat (liveness-epoch / observer clock) + SCSI-PR fencing, behind `ocsfs2_cluster_ops` (pluggable for corosync) | `cluster.c` |
| **L4** ownership | Per-file lease table, **true optimistic CAS over CAW** (read → check → CAW); L4b coarse metadata lease for namespace/allocation | `lease.c` |
| **L5** recovery | Leader election → replay dead peer's journal → reclaim its leases, off the heartbeat path | `lease.c` + `journal.c` |

### Key design choices

- **Single-writer ownership** (above) is the load-bearing decision.
- **Coordination on the LUN.** No DLM daemon, no cluster LAN: lease/membership
  state are versioned on-disk records mutated by **Compare-And-Write**.
- **Coherence by construction + CAW.** File data is single-writer; the small
  shared metadata (bitmap, inode table) is mutated by **per-block CAW** and read
  fresh via direct bios (the per-node buffer cache is bypassed for those), so a
  node never acts on a stale peer view.
- **Liveness-epoch leases.** A lease is honoured only while its owner is ALIVE at
  a matching generation — so there is **no per-lease renewal**, just one heartbeat
  per node.
- **Per-node WAL.** Each node journals to its own region; recovery replays a dead
  peer's journal.

---

## 🧬 What's implemented (`kmod2/`)

| File | Responsibility |
|---|---|
| `super.c` | Mount/unmount, `-o cluster` option, AG headers (pre-sized for autogrow), statfs, sync |
| `inode.c` | Inode read/write/alloc/free (CAW in cluster), extent map (inline ↔ B+tree), coherent re-read |
| `dir.c` · `rename.c` | Namespace ops under the metadata lease; directory blocks read fresh via bio |
| `iomap.c` | Buffered + O_DIRECT data path; block-granular CoW via bios |
| `file.c` | `fallocate` (preallocate/punch/zero), `fiemap`, `SEEK_HOLE/DATA`; lease on open/release |
| `extent_btree.c` | Per-inode extent B+tree (spill past 16 inline extents) |
| `bitmap.c` | Per-AG allocation; cluster alloc/free + **FITRIM** via CAW |
| `journal.c` | Per-node WAL: redo log, replay on mount, **replay a dead peer's slot** (L5) |
| `refcount.c` | Per-AG refcount B+tree for reflink/snapshot/dedup sharing |
| `reflink.c` | `FICLONE`/`copy_file_range`, `FIDEDUPERANGE` dedup, snapshot ioctl, `FITRIM`/`GROWFS`/`SCRUB` dispatch |
| `xattr.c` | xattr (user/trusted/security) + POSIX ACL in one block |
| `cluster.c` | L3: node-slot claim, heartbeat kthread, coherent bio + CAW helpers, fencing |
| `lease.c` | L4 ownership leases + L4b metadata lease + L5 recovery (leader, reclaim) |
| `grow.c` | L2: autonomous online autogrow (watcher + `OCSFS_IOC_GROWFS`) |
| `scrub.c` | Online metadata scrub (`OCSFS_IOC_SCRUB`) |
| `transport/scsi_pr.c` | SCSI-3 PR + Compare-And-Write (the only file carried over from v1) |

### Userspace tools (`tools2/`)

| Tool | Description |
|---|---|
| `mkfs.ocsfs2` | Volume formatter; uniform autogrow-ready AGs; `-N` max nodes, `-s` format-a-prefix (testing) |
| `fsck.ocsfs2` | Offline structural + checksum + refcount-tree check |

---

## 🔨 Building

```bash
# Kernel module (build on the node whose kernel will run it)
make -C /lib/modules/$(uname -r)/build M=$PWD/kmod2 modules

# Tools
cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -O2 -std=gnu11 tools2/fsck.c -o fsck.ocsfs2
```

## 🚀 Quick start

```bash
# Format for up to N cluster nodes (one node only — destroys data)
sudo ./mkfs.ocsfs2 -L vmstore -N 3 -f /dev/sdb

sudo insmod kmod2/ocsfs2.ko          # on every node

# Single node (no clustering overhead): plain mount
sudo mount -t ocsfs2 /dev/sdb /mnt/ocsfs

# Cluster: mount -o cluster on EACH node sharing the LUN
sudo mount -t ocsfs2 -o cluster /dev/sdb /mnt/ocsfs
```

`-o cluster` opts into multinode mode (claims a node slot, registers an SCSI PR
key, starts the heartbeat). Without it the volume is single-node even if formatted
with `-N > 1` — so there is zero clustering overhead for a lone host.

### Growing, trimming, scrubbing

```bash
# Autogrow: enlarge the LUN on the SAN; each node grows into it within ~30 s.
# Force it now from any node:  ioctl OCSFS_IOC_GROWFS on the mountpoint.
fstrim -v /mnt/ocsfs                 # reclaim free space on the thin LUN (UNMAP)
# Scrub:  ioctl OCSFS_IOC_SCRUB — verify every metadata checksum online
```

See [`tests/v2/`](tests/v2/) for `growfs_tool.c`, `scrub_tool.c`, `dedup_tool.c`
and the full validation scripts.

---

## ⚠️ Known limitations

| Area | Status |
|---|---|
| **Maturity** | Alpha / research — not production-ready, AI-generated, unreviewed |
| **Inline compression (Fase F)** | Not implemented. Transparent compression breaks the iomap 1:1 logical↔physical mapping and O_DIRECT (`cache=none`, the Proxmox default), and would require a parallel non-iomap data path — explicitly avoided. Space savings come from dedup + thin + discard instead. |
| **Per-data-block checksums (Fase F)** | Not implemented. All **metadata** is CRC32c-checksummed and the online scrub verifies it; per-data-block checksums need a format-v3 per-AG region plus write hooks on both the buffered and O_DIRECT paths (a half-correct version would false-positive on every VM write). |
| **Metadata-op throughput under cross-node contention** | Namespace ops on a *shared* directory serialise on one metadata lease; fine for the VM-disk workload (rare namespace churn), not tuned for many nodes hammering one directory. The **data path is unaffected** (single-writer, no per-I/O CAW). |
| **Concurrent evict-time free under cluster** | Concurrent delete+alloc across nodes is not yet fully coordinated at evict time (known follow-up). |
| **Encryption** | Out of scope by design — encrypt at the SAN/LUN (LUKS on the zvol) or in the guest (qcow2/LUKS); per-file fscrypt would disable O_DIRECT and block reflink/snapshot. |
| **Fencing** | SCSI-3 Persistent Reservations only; out-of-band STONITH not wired. |

---

## 📁 Project structure

```
ocsfs/
├── kmod2/      OCSFS v2 kernel module (C, GPL-2.0) — single-writer ownership
│   └── transport/scsi_pr.c   SCSI-3 PR + Compare-And-Write
├── tools2/     mkfs.ocsfs2, fsck.ocsfs2 (authoritative on-disk format)
├── tests/v2/   single-node + real-cluster validation scripts
├── docs/       design-v2.md (spec), admin/developer guides, plans/
├── kmod/ tools/ ...   v1 (superseded; kept for reference on `main`)
└── LICENSE     GPL-2.0-only
```

---

## 🗺️ Roadmap

1. ✅ ~~L2 single-node data path, fsx-validated~~
2. ✅ ~~reflink + CoW snapshots; cross-file dedup~~
3. ✅ ~~L3 membership + L4 single-writer ownership + L4b directory coherence~~
4. ✅ ~~L5 recovery (peer journal replay + eager lease reclaim)~~
5. ✅ ~~discard/TRIM, autonomous repeatable autogrow, online metadata scrub~~
6. ✅ ~~multinode performance + curated xfstests~~
7. **Fase F** — inline compression and per-data-block checksums (format-v3), if/when the trade-offs are worth it
8. Corosync membership provider (the L3 interface is already pluggable); parallel multi-node recovery; out-of-band STONITH

---

## 📜 License

**GPL-2.0-only** — compatible with Linux kernel inclusion.
