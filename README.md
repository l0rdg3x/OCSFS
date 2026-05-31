<div align="center">

# OCSFS — Open Cluster Shared FileSystem

**A clustered filesystem for Linux on shared block storage — a Proxmox-native alternative to VMware VMFS.**

[![License: GPL-2.0](https://img.shields.io/badge/License-GPL--2.0--only-blue.svg)](LICENSE)
[![Status: Alpha](https://img.shields.io/badge/status-alpha%20%2F%20research-orange.svg)]()
[![Platform: Linux](https://img.shields.io/badge/platform-Linux%20kernel%206.x%2F7.x-informational.svg)]()
[![On-disk rev](https://img.shields.io/badge/on--disk%20rev-2-lightgrey.svg)]()
[![AI-generated](https://img.shields.io/badge/code-100%25%20AI%20generated-ff69b4.svg)]()

</div>

> [!IMPORTANT]
> **This project is entirely AI-generated.** Every line of code, documentation,
> and tooling was written by [Claude](https://claude.ai) (Anthropic) through an
> iterative conversation with a human operator. No human has written or reviewed
> the source code directly. Treat it accordingly — **do not use it with data that
> matters.**

---

OCSFS lets **multiple Linux nodes mount the same SAN LUN at the same time**,
read-write, with cache coherence, crash recovery, and hardware fencing — the
same job VMware VMFS does for ESXi, but open-source and built for **Proxmox VE**.

It needs no lock-manager daemon and no cluster network: **all coordination lives
on the shared device itself**, using SCSI Compare-And-Write for distributed
locking and SCSI-3 Persistent Reservations for fencing.

## ✨ Highlights

| | |
|---|---|
| 🔗 **Shared-disk, multi-writer** | Many nodes, one LUN, concurrent read-write — coordinated entirely on-disk |
| 🧩 **No external dependencies** | No DLM daemon, no corosync, no cluster LAN — just the SAN |
| 🛡️ **Hardware fencing** | SCSI-3 Persistent Reservations evict a failed node at the fabric |
| 💓 **Storage-path heartbeat** | Liveness proven by writes to the LUN, immune to network partitions |
| 📓 **Crash-safe journaling** | WAL with redo; replay on mount reconstructs committed transactions |
| 🌳 **Modern data path** | iomap, extent B+ trees, O_DIRECT, sparse files, `FIEMAP`, `SEEK_HOLE/DATA` |
| 🪞 **Space efficiency** | Reflink (`FICLONE`), CoW snapshots, inline LZ4/ZSTD compression, **cross-file dedup** |
| 🔐 **Encryption** | fscrypt per-directory encryption with cluster-wide key distribution |
| ⚡ **VAAI offload** | `WRITE SAME`, `UNMAP`, `EXTENDED COPY` for array-accelerated VM ops |
| 🧱 **Proxmox-native** | Storage plugin, mount helper, DKMS, Debian packaging |

---

## 🚦 Status & validation

> [!WARNING]
> **Alpha / Research.** Both the single-node data path and **basic two-node
> cross-node coherence are now validated on real hardware.** Cluster recovery,
> fencing under real failures, and xfstests are still pending. Not
> production-ready.

### What's validated on real hardware

The kernel module is exercised on **Proxmox VE 9 nodes (kernel 7.0.x-pve)**
against a **real iSCSI LUN from TrueNAS SCALE** that supports **SCSI Persistent
Reservations + Compare-And-Write** — so OCSFS runs in **full cluster mode with
hardware CAS**, not a degraded single-node fallback.

**Single node**, full-cluster, end-to-end through the kernel module, with a
clean `fsck` and **zero kernel warnings**:

- ✅ 64 MiB buffered + **O_DIRECT** write/read with sha256 round-trip across `drop_caches`
- ✅ **reflink** (`FICLONE`) with copy-on-write isolation
- ✅ **CoW snapshots** with source/clone isolation
- ✅ **cross-file deduplication** (global DDT) with GC reclaim and delete-integrity
- ✅ extended attributes, POSIX ACLs, hard/symlinks, nested directories
- ✅ `fallocate` **punch-hole** / **zero-range**, sparse files, truncate grow/shrink
- ✅ **mmap** `MAP_SHARED` write + `msync` round-trip
- ✅ large directories (thousands of entries) via the directory B+ tree
- ✅ **crash recovery**: panic-injected power loss → reboot → remount → full data
  access → clean `fsck`, including crashes mid-reflink and with a wrapped journal
- ✅ **writer-priority fairness**: an exclusive writer is never starved by a
  continuous reader stream (worst lock-acquire ≤ 8 ms vs. a former 30 s timeout)

**Two nodes**, both mounting the same LUN read-write concurrently (slot 0 +
slot 1, distinct PR keys, hardware CAW), clean `fsck`, **zero warnings on
either node**:

- ✅ **cross-node write → read coherence** (both directions): a file written on
  one node reads back identically on the other
- ✅ **cross-node overwrite coherence** with **lazy-lock revocation hand-off** —
  a peer that needs a lazily-held lock gets it within the revoke-sweep interval
  and sees the new data (validates the VM-disk write optimizations cluster-wide)
- ✅ **directory coherence** both ways — files created on one node are listed on
  the other, with no inode-number collisions between nodes
- ✅ **3-node concurrent directory modification** — three nodes hammering the
  *same* directory at once (concurrent create, concurrent delete, and
  interleaved create+delete churn) all converge to the correct entry set with a
  consistent on-disk `i_size`, both below and above the directory-B+tree
  threshold; survives remount with a clean `fsck`
- ✅ 10-round **ping-pong overwrite** with a stable cross-node lock hand-off
- ✅ Concurrent cross-node **rename** — within-dir, cross-dir and mixed
  create+delete+rename churn from 3 nodes all converge correctly (clean `fsck`)
- ✅ **Real node-crash recovery** — a node hard-crashed mid-write (`sysrq-b`) is
  detected by a survivor, **SCSI-PR preempt-and-abort fenced**, its journal
  replayed and its locks recovered; survivors stay online with data intact and a
  clean `fsck`, and the crashed node reboots and rejoins reading correct data
- ✅ **Heartbeat self-fencing** — a node that can't write its heartbeat for the
  death threshold quiesces (refuses new exclusive locks) instead of being torn
  out by a peer mid-mutation

### What's *not* validated yet

- ❌ **xfstests** `quick`/`auto` on a clustered LUN
- ❌ **Metadata throughput under heavy concurrent load** — the on-disk DLM does a
  SCSI CAW per lock op; under sustained concurrent metadata churn from 3 nodes a
  hot lock (e.g. a shared directory) can hit the 30 s acquire timeout as the
  target's CAW path saturates. The per-block CAW storm on delete/truncate is
  fixed; reducing the general per-op CAW (lock leasing) is the open scaling item
- ❌ Long-haul soak / performance tuning

### Indicative scores

| Scenario | Score | Basis |
|---|--:|---|
| Single-node read/write (real SAN, full cluster) | ~94% | validated on hardware |
| Multi-node — coherence, stable workload | ~90% | basic 2-node validated on hardware |
| Multi-node — crash + recovery | ~85% † | structural estimate |
| Multi-node — with encryption | ~85% † | structural estimate |
| VMFS feature parity | ~72% | feature comparison |

† Not yet validated under real node failures. See
[`docs/developer-guide.md`](docs/developer-guide.md) for the full changelog and
the per-sprint correctness history.

---

## 🏗️ Architecture

```
        ┌──────────────────────────────────────────────────────────────┐
        │                    Shared block device (LUN)                  │
        │              iSCSI / Fibre Channel / loopback                 │
        │                                                               │
        │  SB │ AG descriptors │ Inode tables │ Bitmaps │ Lock table    │
        │     │ Journal (per node) │ Heartbeat │ CAS leases │ Key store │
        └───────┬───────────────────────────────────────────┬──────────┘
                │   SCSI-3 PR (fencing)                       │
                │   SCSI CAW / Compare-And-Write (locking)    │
       ┌────────┴────────┐   ┌─────────────────┐   ┌──────────┴──────┐
       │     node-1      │   │     node-2      │   │     node-N      │
       │    ocsfs.ko     │   │    ocsfs.ko     │   │    ocsfs.ko     │
       │  VFS · iomap ·  │   │  VFS · iomap ·  │   │  VFS · iomap ·  │
       │  DLM · journal  │   │  DLM · journal  │   │  DLM · journal  │
       └─────────────────┘   └─────────────────┘   └─────────────────┘
```

**Key design choices**

- **No lock-manager daemon.** All lock state lives on the shared LUN as
  versioned on-disk entries; acquire/release use **Compare-And-Write (CAS)** via
  SCSI — hardware-atomic where the array supports CAW, with a Persistent-
  Reservation lease fallback otherwise.
- **Storage-path heartbeat.** Node liveness is proven by writes to the shared
  device, *not* the management network, so a LAN partition cannot cause false
  positives.
- **SCSI-3 Persistent Reservations.** Hardware fencing: the SAN rejects I/O from
  a fenced node's HBA before recovery begins.
- **WAL journal with redo.** AFTER-images in the commit record; replay on mount
  reconstructs any committed-but-not-flushed transaction and stops cleanly at the
  torn tail a crash leaves behind (jbd2/xfs-style).
- **Reference-counted, writer-priority DLM.** Local lock holds are
  reference-counted so a lockless read can never release a writer's exclusive
  lock; pending writers take priority so readers can't starve them.

---

## 🧬 What's implemented

### Kernel module (`kmod/`)

| File | Responsibility |
|---|---|
| `super.c` | Mount/unmount, `fill_super`, statfs, `sync_fs`, module init/exit |
| `inode.c` | Inode read/write/alloc/free, setattr, CRC32c verification |
| `dir.c` · `dir_btree.c` · `dir_rename.c` | Directory ops, B+ tree index, rename / `RENAME_EXCHANGE` |
| `file.c` · `iomap.c` | read/write iter, fsync, `FIEMAP`, reflink, ioctls; O_DIRECT + buffered iomap path |
| `extent.c` · `extent_btree.c` | Inline extents (≤16) with B+ tree overflow; 4-bit flag encoding (v2) |
| `alloc.c` · `bitmap.c` | Goal-oriented allocator, prealloc, AG affinity; two-phase lockless bitmap |
| `thin.c` | `fallocate`, `PUNCH_HOLE`, `ZERO_RANGE`, `DISCARD` |
| `journal.c` · `journal_replay.c` | WAL with ordered checkpoint; CRC-gated COMMIT detection; redo on mount |
| `lock.c` · `lock_io.c` | On-disk DLM: EX/SH acquire/release, downgrade, epoch cache, writer priority |
| `scsi_pr.c` | SCSI-3 PR register / preempt-and-abort; CAW via BSG-direct |
| `heartbeat.c` · `node.c` | Storage-path heartbeat; node-slot table with HMAC-SHA256 auth |
| `recovery.c` | 5-phase recovery: leader election, PR fencing, journal replay, lock cleanup |
| `snapshot.c` · `refcount.c` | CoW snapshots; per-AG extent refcount B+ tree (CoW/dedup) |
| `compress.c` · `compress_file.c` | Inline LZ4/ZSTD compression (read path); compress-on-fsync |
| `dedup.c` · `dedup_index.c` | Content dedup ioctl + global cross-file index (DDT) with GC |
| `xattr.c` · `acl.c` | Extended attributes (DLM-protected) and POSIX ACLs |
| `crypto.c` | fscrypt: per-directory encryption, bounce-page I/O, cluster key safety |
| `btree.c` · `btree_mod.c` | Generic B+ tree (search/insert/delete/range) + structural ops |
| `flock.c` | Distributed POSIX locks: `fcntl(F_SETLK)` → DLM SH/EX |
| `vaai.c` | VAAI offload: `WRITE SAME` (0x93), `UNMAP` (0x42), `EXTENDED COPY` (0x83) |
| `debugfs.c` | `/sys/kernel/debug/ocsfs/<dev>/{lock_table,journal_stats}` |
| `test_lock.c` · `test_cas.c` | KUnit tests: B+ tree, directory threshold, CAS protocol |

### Userspace tools (`tools/`, `src/`)

| Tool | Description |
|---|---|
| `mkfs.ocsfs` | Volume formatter (`tools/mkfs_ocsfs.c`) |
| `ocsfs-tool` | Admin CLI: info, nodes, locks, df, check, key restore |
| `ocsfs_defrag` | Online defragmentation daemon (FIEMAP-based, bandwidth-limited) |
| `ocsfs-fsck` | Offline fsck (Python): structural + refcount + dedup-index checks, repair mode |
| `src/` | FUSE3 prototype — validates the on-disk format; **not for production** |

### Platform integration

| Component | Description |
|---|---|
| `proxmox/OCSFSPlugin.pm` | Proxmox VE storage plugin (all content types) |
| `proxmox/mount.ocsfs` | Mount helper invoked by `mount -t ocsfs` |
| `proxmox/install.sh` | One-step installer (DKMS + plugin + tools) |
| `conf/` | systemd template units + udev rules for SAN auto-detection |
| `debian/` | Packaging: `ocsfs-tools`, `ocsfs-dkms`, `ocsfs-proxmox` |
| `man/` | Man pages for all CLI tools |

---

## 🔨 Building

```bash
# Userspace tools + FUSE prototype
make all

# Kernel module (needs linux-headers for the running kernel)
cd kmod && make -j"$(nproc)"

# DKMS
sudo dkms add    kmod/
sudo dkms build  ocsfs/0.1.0
sudo dkms install ocsfs/0.1.0

# Debian packages
dpkg-buildpackage -us -uc -b
```

---

## 🚀 Quick start

### Single node (loopback)

```bash
truncate -s 2G /tmp/ocsfs.img
./mkfs.ocsfs -L my-datastore -f /tmp/ocsfs.img
./ocsfs-tool info /tmp/ocsfs.img

sudo modprobe lz4_compress
sudo insmod kmod/ocsfs.ko
sudo losetup /dev/loop0 /tmp/ocsfs.img
sudo mount -t ocsfs -o degraded /dev/loop0 /mnt/ocsfs   # 'degraded' = single-node, no PR

echo hello | sudo tee /mnt/ocsfs/test.txt
sudo umount /mnt/ocsfs && sudo losetup -d /dev/loop0 && sudo rmmod ocsfs
```

### Full cluster mode (real SAN LUN with SCSI-3 PR + CAW)

```bash
# Format for up to N nodes
sudo ./mkfs.ocsfs -L vmstore -N 2 -J 16M -f /dev/sdb

sudo insmod kmod/ocsfs.ko
# No 'degraded' → full cluster mode: claims a node slot, registers an SCSI PR key,
# and uses hardware Compare-And-Write for on-disk locking.
sudo mount -t ocsfs /dev/sdb /mnt/ocsfs
```

A volume that requires the cluster secret (encryption / journal HMAC) additionally
needs `-o cluster_secret=<64 hex chars>` at mount.

---

## 🧪 Multi-node testbed

The minimum viable testbed is two nodes sharing one SCSI-3 PR-capable LUN:

- 2–3 nodes (VMs or bare metal) attached to the same iSCSI LUN
- **TrueNAS SCALE** (LIO backend) or `targetcli-fb` as the iSCSI target —
  SCSI-3 PR and CAW work out of the box; a zvol-backed extent gives hardware CAS
- `open-iscsi` initiator on each node

See [`docs/developer-guide.md`](docs/developer-guide.md) for the full step-by-step
guide, and `tests/` for the kernel smoke test and cluster scenario scripts.

---

## 🖥️ Proxmox VE usage

```bash
# On each PVE node: install plugin + module
sudo proxmox/install.sh

# Register the shared LUN as storage
pvesm add ocsfs vmstore \
  --device /dev/disk/by-path/<your-lun> \
  --content images,iso,vztmpl,backup \
  --maxnodes 16 --shared 1

# Offline integrity check / repair
sudo ./tools/ocsfs-fsck /dev/sdb
sudo ./tools/ocsfs-fsck --repair /dev/sdb
```

---

## ⚠️ Known limitations

| Area | Status |
|---|---|
| **Multi-node coherence** | 2- and 3-node read/write/overwrite plus **concurrent same-directory create/delete/churn coherence validated on real hardware** (the directory lost-update class — stale `i_size`/extent rollback and stale dirent/B+tree reads — is fixed). Cross-node **rename** churn lacks a dedicated 3-node stress run |
| **Cluster recovery & fencing** | Implemented; not yet exercised by killing a live node in a 2-node cluster |
| **Integration tests** | No xfstests run yet |
| **Encryption — I/O** | No readahead, no O_DIRECT; reflink/snapshot/symlink inside encrypted dirs return `-EOPNOTSUPP` |
| **Encryption — keys** | fscrypt keys live in a shared encrypted key store on the LUN (ChaCha20-Poly1305 / cluster secret); peers fetch via `ocsfs-tool keys restore`. Enable with `mkfs.ocsfs -K`; requires `cluster_secret=` at mount |
| **Compression write path** | Applied on fsync for buffered files only; O_DIRECT writes are never compressed |
| **Shared writable mmap** | `MAP_SHARED\|PROT_WRITE` returns `-EOPNOTSUPP` in *cluster* mode (works single-node); read-only and private COW mappings always work |
| **STONITH** | Fencing uses SCSI PR; out-of-band STONITH (PDU/iDRAC) is not wired |
| **Recovery concurrency** | One dead node recovered at a time; a second simultaneous failure is queued |

---

## 📁 Project structure

```
ocsfs/
├── kmod/      Kernel module (C, GPL-2.0)
├── src/       FUSE3 prototype (format validation only)
├── tools/     mkfs, admin CLI, defrag daemon, fsck
├── proxmox/   Proxmox VE storage plugin (Perl)
├── tests/     KUnit + kernel smoke + cluster scenarios
├── conf/      systemd units + udev rules
├── man/       Man pages
├── docs/      Developer guide, admin guide
├── debian/    Debian packaging
├── include/   Shared on-disk-format headers
└── LICENSE    GPL-2.0-only
```

---

## 🗺️ Roadmap

1. ✅ ~~Real two-node cross-node coherence~~ — **done, validated on hardware**
2. **Cluster recovery & fencing under real node failure** — kill a live node, verify peer recovery + SCSI-PR fencing
3. Cross-node **block-bitmap coherence** (DLM-acquire-time invalidation) and inode-block write serialisation
4. **xfstests** `quick`+`auto` on a clustered testbed
5. Out-of-band STONITH integration; online `fsck` / scrub hardening

---

## 🤝 Contributing

The project is looking for:

- Kernel filesystem developers (VFS, block layer, crash safety)
- SCSI / FC storage engineers (PR, CAW, multipath)
- Proxmox / PVE integration developers
- QA engineers who can run xfstests on a real cluster

The single most impactful open task is **running xfstests on a 2-node LIO iSCSI
testbed**. SCSI CAW is implemented via BSG-direct (`ocsfs_bsg_execute_cdb()`) —
no kernel patch required.

---

## 📜 License

**GPL-2.0-only** — compatible with Linux kernel inclusion.
