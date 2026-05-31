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
| 🏎️ **Near-raw VM-disk I/O** | Random 4K O_DIRECT read/write on a clustered LUN runs at ~device speed — the per-op clustering overhead is eliminated for a single active node |
| 📈 **Online grow** | Expand the filesystem into a grown LUN while it stays mounted — `ocsfs-grow /mnt/point` |
| 🧱 **Proxmox-native** | Storage plugin, mount helper, DKMS, Debian packaging |

---

## 🚦 Status & validation

> [!WARNING]
> **Alpha / Research.** The single-node data path, **2- and 3-node cross-node
> coherence**, and **real node-crash recovery with SCSI-PR fencing** are validated
> on real hardware. Still open: metadata throughput under heavy concurrent load
> (CAW-bound), xfstests, and long-haul soak. **Not production-ready — do not use
> with data that matters.**

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
- ✅ **near-raw random I/O** on a clustered volume: 4 KiB O_DIRECT random
  read/write to a pre-allocated VM-disk image reaches device speed (see
  [Performance](#-performance)) — the per-operation DLM/refresh overhead that
  previously capped it at the SAN's single-op rate is gone
- ✅ **online filesystem grow**: the volume is expanded into a grown LUN while it
  stays mounted, and peers pick up the new space on their next allocation

**Two and three nodes**, all mounting the same LUN read-write concurrently
(distinct slots and PR keys, hardware CAW), clean `fsck`, **zero kernel
warnings**:

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
- ❌ **Metadata-operation throughput under heavy concurrent load** — the on-disk
  DLM does a SCSI CAW per *cross-node* lock acquire/release; under sustained
  concurrent metadata churn from 3 nodes a *hot, contended* lock (e.g. a shared
  directory) can hit the 30 s acquire timeout as the target's CAW path saturates.
  Note this is about *metadata operations under cross-node contention* — the
  **data path** (random/sequential file I/O on a single active node) now runs at
  near-raw speed because an uncontended lock is held lazily and re-taken from
  cache with no CAW (see [Performance](#-performance)). The per-block CAW storm
  on delete/truncate is fixed; reducing the remaining per-op CAW on genuinely
  contended metadata locks is the open scaling item
- ❌ Long-haul soak

### Indicative scores

| Scenario | Score | Basis |
|---|--:|---|
| Single-node read/write (real SAN, full cluster) | ~94% | validated on hardware |
| Random VM-disk I/O on a clustered LUN (single active node) | ~95% | validated on hardware — near-raw (see [Performance](#-performance)) |
| Multi-node — coherence (create/delete/rename, 2–3 nodes) | ~92% | validated on hardware |
| Multi-node — crash recovery + PR fencing | ~88% | validated on hardware (one induced crash) |
| Multi-node — metadata-op throughput under cross-node contention | ~70% | CAW-bound on hot contended locks; data path is not affected |
| Multi-node — with encryption | ~85% † | not yet exercised under node failure |
| VMFS feature parity | ~72% | feature comparison |

† See [`docs/developer-guide.md`](docs/developer-guide.md) for the full changelog
and the per-sprint correctness history.

---

## ⚡ Performance

**Random 4 KiB I/O is what a running VM does to its disk image**, so that is what
we benchmark. Measured with `fio` (`--direct=1 --ioengine=libaio`) against a
**2 GiB pre-allocated file** on an **N=8 clustered** OCSFS volume, on a real
**TrueNAS SCALE iSCSI LUN** (1 GbE, SSD-backed zvol, SCSI PR + CAW). The baseline
is the **same workload run directly on the raw block device**.

| Workload (4 KiB O_DIRECT) | Raw device | OCSFS (clustered) | Efficiency |
|---|--:|--:|--:|
| Random write, QD1 | 3 889 IOPS | **3 878 IOPS** | 100% |
| Random write, QD32 | 26 600 IOPS | **26 600 IOPS** | 100% |
| Random read, QD1 | 3 889 IOPS | **4 109 IOPS** | ~100% |
| Random read, QD32 | 27 800 IOPS | **23 100 IOPS** | 83% |
| Random 50/50 r+w, QD32 | ~27 000 IOPS | **26 200 IOPS** (13.1k + 13.1k) | ~97% |

For comparison, **before** the per-operation overhead was removed the same
clustered volume did **247** random-write IOPS at QD32 (a 108× gap) and **692**
random-read IOPS — the throughput was capped at the SAN's single-op round-trip
rate because every 4 KiB I/O performed synchronous metadata work (a refcount DLM
round-trip per write, a forced inode re-read per read).

**Why it is now near-raw.** On a single active node the inode lock is **held
lazily and re-acquired from cache with no SCSI CAW**, the inode is re-read from
disk **only after a real cross-node hand-off** (not on every I/O), and the
copy-on-write check **short-circuits with no lock** when the file is not shared
(the common case for a VM image). A peer that needs the lock still gets it within
one lazy-revoke sweep, and **cross-node coherence is preserved** (validated by a
two-node buffered read/write ping-pong). Integrity is verified end-to-end with
`fio --verify=crc32c` and a clean `fsck`.

> The number that is *not* near-raw is **metadata-operation throughput when a
> lock is genuinely contended across nodes** (e.g. three nodes hammering one
> shared directory) — that path still pays a CAW per hand-off. The data path
> above is unaffected.

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
| `ocsfs-grow` | Grow the volume into an expanded LUN — online (mountpoint) or offline (device) (`tools/ocsfs_grow.c`) |
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

## 📋 Formatting, mounting & growing — full reference

This section documents **every** option of the on-disk tools so you can format,
mount, grow and check a volume deliberately rather than by copy-paste.

### 1. `mkfs.ocsfs` — format a volume

```
mkfs.ocsfs [options] <device>
```

`mkfs.ocsfs` **destroys all data** on `<device>`. Run it on **one node only**.
Geometry chosen here (block size, AG size, max nodes) is **fixed for the life of
the volume** — it cannot be changed later, so size it deliberately.

| Flag | Argument | Default | Meaning |
|---|---|---|---|
| `-L` | `<label>` | (none) | Volume label, ≤ 63 characters. Shown by `ocsfs-tool info` and usable with `mount LABEL=…`. |
| `-N` | `<max_nodes>` | `64` | **Maximum number of cluster nodes** that may ever mount this volume. Range 1–256. Sizes the node-slot table, the per-node journal array, the lock table and the heartbeat area. **`-N 1` makes a single-node (non-clustered) volume** — no DLM/CAW overhead, but it must never be mounted from two nodes. `-N ≥ 2` enables full cluster mode. Cannot be changed after format. |
| `-b` | `<bytes>` | `4096` | Block size. Accepts a plain number or a `K`/`M` suffix. 4096 matches the page size and the SAN logical block — leave it unless you have a specific reason. |
| `-E` | `<size>` | `1M` | Default extent (allocation unit) hint, e.g. `1M`, `4M`. Larger extents reduce metadata for big sequential files (VM images, ISOs); smaller suit many small files. |
| `-A` | `<size>` | `1G` | Allocation Group size. The volume is divided into AGs, each with its own bitmap, inode table and locks, so independent AGs are written without cross-node contention. |
| `-J` | `<size>` | `32M` | **Per-node** journal (write-ahead log) size, e.g. `16M`, `64M`. Total journal space = `J × N`. Bigger journals absorb larger metadata bursts; 16–32M is plenty for VM workloads. |
| `-K` | — | off | Enable **cluster authentication** (HMAC on journal/lock records + the encrypted key store). A `-K` volume **will not mount without** `-o cluster_secret=<64 hex>`. Required if you use fscrypt encryption. |
| `-T` | — | on | Enable thin provisioning (blocks allocated on first write). On by default; the flag is kept for explicitness. |
| `-f` | — | off | Force — skip the "this will erase the device" confirmation prompt. Required for scripted/unattended formatting. |
| `-v` | — | off | Verbose — print the computed geometry (AG count, journal offsets, inode-table layout). |
| `-h` | — | — | Show built-in help. |

**Recommended profiles**

| Cluster | `-N` | `-J` | `-E` | Example |
|---|---|---|---|---|
| Single node (lab/local) | `1` | `16M` | `1M` | `mkfs.ocsfs -L local -N 1 -J 16M -f /dev/sdb` |
| Small (2–4 nodes) | `8` | `16M` | `1M` | `mkfs.ocsfs -L vmstore -N 8 -J 16M -f /dev/sdb` |
| Medium (5–16 nodes) | `32` | `32M` | `4M` | `mkfs.ocsfs -L vmstore -N 32 -J 32M -E 4M -f /dev/sdb` |
| Large (17–64 nodes) | `64` | `64M` | `8M` | `mkfs.ocsfs -L vmstore -N 64 -J 64M -E 8M -f /dev/sdb` |
| Encrypted cluster | `8` | `16M` | `1M` | `mkfs.ocsfs -L secure -N 8 -J 16M -K -f /dev/sdb` |

> Tip: `mkfs.ocsfs -N <n>` over-allocates spare AG-descriptor slots so the volume
> can later be **grown online** (see §3) without relocating existing data.

### 2. Mounting

```
mount -t ocsfs [-o <options>] <device> <mountpoint>
```

| Mount option | When to use |
|---|---|
| *(none)* | **Full cluster mode.** The node claims a slot, registers an SCSI-3 PR key and uses hardware Compare-And-Write for locking. This is the correct mode for a shared LUN with PR + CAW. |
| `degraded` | **Single-node only.** Skips PR registration and the "no PR support" safety refusal. Use for loopback images or PR-less iSCSI. **Never mount the same device from two nodes with `degraded`** — there is no fencing and you *will* corrupt the volume. |
| `cluster_secret=<64 hex>` | **Required** for a volume formatted with `-K` (and for fscrypt). 64 hexadecimal characters = the 32-byte cluster secret. Generate once with `openssl rand -hex 32`; it must be identical on every node. |
| `heartbeat_timeout=<ms>` | Override the heartbeat death threshold (diagnostics / slow targets). |

```bash
# Full cluster mount (shared LUN, PR + CAW)
mount -t ocsfs /dev/sdb /mnt/ocsfs

# Single-node loopback (no PR)
mount -t ocsfs -o degraded /dev/loop0 /mnt/ocsfs

# Authenticated / encrypted volume
mount -t ocsfs -o cluster_secret=$(cat /etc/ocsfs.secret) /dev/sdb /mnt/ocsfs

# /etc/fstab (network-attached device → _netdev)
# /dev/sdb  /mnt/ocsfs  ocsfs  defaults,_netdev  0 0
```

### 3. `ocsfs-grow` — expand into a larger LUN

After you enlarge the backing LUN (e.g. grow the TrueNAS zvol and
`iscsiadm -m node -R` to rescan), give the new space to the filesystem. The same
binary does **online** (mounted) or **offline** (unmounted) grow depending on
whether you pass a **mountpoint** or a **block device**:

```bash
# ONLINE — volume stays mounted; pass the MOUNTPOINT.
#   Issues OCSFS_IOC_GROW; the node writes the new AG descriptors live and
#   bumps the AG count. Peers pick up the new space on their next allocation
#   (or immediately via statfs). Run on ONE node.
ocsfs-grow /mnt/ocsfs

# OFFLINE — volume unmounted on EVERY node; pass the BLOCK DEVICE.
ocsfs-grow /dev/sdb

# Dry run (offline only) — report what would be added, change nothing.
ocsfs-grow -n /dev/sdb
```

Existing AGs are never moved (each descriptor stores absolute geometry); new AGs
are described in an extension region carved from the added space
(`INCOMPAT_AG_GROW`). **Important:** rescan the LUN on **all** nodes
(`iscsiadm -m node -R`) before they try to use the new space, or a peer will
refuse it as "beyond device size".

### 4. `ocsfs-fsck` — offline check & repair

```bash
umount /mnt/ocsfs                       # must be unmounted on all nodes
python3 tools/ocsfs-fsck /dev/sdb       # read-only: report problems
python3 tools/ocsfs-fsck --repair /dev/sdb   # patch orphans, stale locks, slots
```

Checks the superblock + mirror, AG descriptors, journal headers, the free-block
cross-count, inode CRCs/orphans, extent consistency, the dedup index, the
heartbeat area, the lock table and the node-slot table. It understands the
AG-grow extension region and the per-AG refcount/dedup trees.

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
| **Metadata-op scaling under cross-node contention** | The on-disk DLM does a SCSI CAW per *cross-node* lock acquire/release. Under sustained concurrent metadata churn on a *hot, contended* lock (e.g. a shared directory) acquires can hit the 30 s timeout as the target's CAW path saturates (the op fails, the node stays up). **The data path is not affected** — an uncontended lock is held lazily and re-taken from cache with no CAW, so single-node random VM-disk I/O runs at near-raw speed (see [Performance](#-performance)). The per-block CAW storm on delete/truncate is fixed; reducing per-op CAW on genuinely contended metadata locks is the open scaling item |
| **Multi-node coherence** | ✅ Validated on real hardware (2- and 3-node): read/write/overwrite, concurrent same-directory create/delete/churn, and concurrent rename (within-dir, cross-dir, mixed) all converge correctly with a clean `fsck` |
| **Cluster recovery & fencing** | ✅ Real node-crash recovery validated: a survivor detects the death, **SCSI-PR preempt-and-abort** fences the dead node, replays its journal and recovers its locks; survivors stay online with data intact, and the crashed node reboots and rejoins. **Self-fencing** quiesces a node that can't write its heartbeat. *Open:* a falsely-recovered-then-rejoining node can be left with stale previous-generation locks under extreme load |
| **Fencing method** | SCSI-3 Persistent Reservations only; out-of-band STONITH (PDU / iDRAC / IPMI) is not wired |
| **Recovery concurrency** | One dead node recovered at a time; a second simultaneous failure is queued |
| **Slot-claim / refcount TOCTOU** | Node-table slot claim and cross-node refcount updates are CAW-serialised but retain a small TOCTOU window (architectural) |
| **Crash atomicity** | Extent-map and bitmap updates commit as two separate journal transactions (tiny crash window between them); `fsck` reconciles |
| **Filesystem grow** | ✅ Online (mounted) and offline grow implemented via `ocsfs-grow` (`INCOMPAT_AG_GROW`); validated 3-node. *Open:* a single grow per volume (re-growing an already-grown volume is rejected); rescan the LUN on every node before peers use the new space |
| **Integration tests** | No xfstests run yet; no long-haul soak |
| **Encryption — I/O** | No readahead, no O_DIRECT on encrypted files; reflink/snapshot/symlink inside encrypted dirs return `-EOPNOTSUPP` |
| **Encryption — keys** | fscrypt keys live in a shared encrypted key store on the LUN (ChaCha20-Poly1305 / cluster secret); enable with `mkfs.ocsfs -K`, requires `cluster_secret=` at mount; peers fetch via `ocsfs-tool keys restore` |
| **Compression** | Write path runs on fsync for buffered files only; O_DIRECT writes are never compressed; a file with >16 compressed extents is decompressed on B+tree migration |
| **Quota** | Block quota does not account for CoW/snapshot sharing or directory/metadata blocks |
| **Shared writable mmap** | `MAP_SHARED\|PROT_WRITE` returns `-EOPNOTSUPP` in *cluster* mode (works single-node); read-only and private-COW mappings always work |
| **Maturity** | Alpha / research — not production-ready |

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

1. ✅ ~~2- and 3-node cross-node coherence (read/write, directory, rename)~~ — validated on hardware
2. ✅ ~~Cluster recovery & SCSI-PR fencing under a real node crash~~ — validated on hardware
3. ✅ ~~Near-raw random VM-disk I/O on a clustered LUN~~ — validated on hardware (see [Performance](#-performance))
4. ✅ ~~Online filesystem grow when the backing LUN is expanded~~ — implemented (`ocsfs-grow`), validated 3-node
5. **Metadata-op throughput under cross-node contention** — reduce the per-op SCSI CAW on genuinely contended locks so a hot shared lock can't saturate the target's CAW path
6. **xfstests** `quick`+`auto` on a clustered testbed
7. Recovery hardening (parallel multi-node recovery, self-recovery of own stale locks) and out-of-band STONITH

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
