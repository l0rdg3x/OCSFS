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
> **OCSFS v2 is a from-scratch rearchitecture** (kernel module `ocsfs2`, sources in
> `kmod2/`, tools in `tools2/`). It replaces the v1 design (preserved on the
> `v1-legacy` branch) whose per-operation distributed lock manager could not scale
> metadata under cross-node contention and fought a never-ending stream of
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
| 📓 **Crash-safe journaling** | Per-node WAL with redo; log-scan replay on mount + replay of a *dead peer's* journal during recovery |
| 🌳 **Modern data path** | iomap, inline extents + per-inode extent B+tree, O_DIRECT, **large folios**, sparse, `FIEMAP`, `SEEK_HOLE/DATA` |
| 🪞 **Space efficiency** | Reflink (`FICLONE`), CoW snapshots, **cross-file dedup** (`FIDEDUPERANGE`), thin/sparse, **discard/TRIM** |
| 📈 **Autonomous online autogrow** | Grow the LUN any number of times — each node detects it and grows into the new space hot, **repeatedly** |
| 🩺 **Online metadata + data scrub** | Verify every on-disk checksum on a live volume (`OCSFS_IOC_SCRUB`) to catch silent bitrot |
| 🔐 **End-to-end data checksums** | Opt-in `mkfs -C`: per-physical-block CRC32c, **verified inline on every read** (buffered + O_DIRECT, cross-node) → `-EIO` not corruption, on **any** SAN; **deferred + batched so it is near-free even for cluster random writes**, CoW/reflink-safe |
| 🏎️ **Near-raw VM-disk I/O** | O_DIRECT (`cache=none`); **zero per-I/O clustering tax** (single-writer); the FS software ceiling is ~250k IOPS/node (RAM-measured) — a node is bound by the LUN/fabric, not by OCSFS |
| 🧱 **Proxmox-native** | `mount -t ocsfs2 -o cluster`; storage plugin with reflink clones; clone/snapshot/backup/restore/resize **and online live migration** validated on a 3-node cluster |

---

## 🚦 Status & validation

> [!WARNING]
> **Alpha / Research.** The single-node data path is `fsx`-validated; the cluster
> (membership, single-writer ownership, directory coherence, crash recovery, the
> Proxmox storage battery) is validated on a **real 3-node iSCSI cluster**.
> **Not production-ready — do not use with data that matters.**

All testing runs on **Proxmox VE 9 nodes (kernel 7.0.x-pve)** against **real iSCSI
LUNs from TrueNAS SCALE** with **SCSI Persistent Reservations + Compare-And-Write**
— full cluster mode with hardware CAS, never a degraded fallback. **No loopback
devices.** The module is always built on the Proxmox nodes (matching kernel) and
installed via **DKMS** (auto-rebuilds on kernel upgrade).

### Proxmox VE storage battery (3-node, validated)

The OCSFS storage plugin drives the real PVE CLI (`qm` / `pvesm` / `vzdump` /
`qmrestore`) against a shared LUN, end-to-end:

- ✅ **Linked/template clone** via reflink — `qm clone` of a Debian 13 cloud-init
  template = **~1.3 s** (copy-on-write, instant), the cloned VM boots a real kernel
- ✅ **Snapshot** create / rollback / delete (`qm snapshot` / `rollback` / `delsnapshot`)
- ✅ **Backup → restore** (`vzdump` → `qmrestore`) to a new VMID, the restored VM boots
- ✅ **Disk resize** (`qm resize`, grow), guest sees the new size, `fsck` clean
- ✅ **Crash recovery under PVE** — a node power-loss (`sysrq-b`) is detected by a
  survivor, which replays the dead node's journal; data + checksums intact

- ✅ **Online live migration** (`qm migrate --online`) — validated **bidirectionally
  across all 3 nodes** (n1↔n2, n1↔n3), a running VM, ~6–7 s, ~55 ms downtime, VM
  stays up, `fsck` clean. The destination opens the disk during `-incoming` via a
  **deferred write lease** (the EX lease is taken at the first write, not at open),
  so it never collides with the source — the lease hand-off *is* the migration,
  with no data copy.

### Single node — the integrity gate (`fsck` clean, zero kernel warnings)

- ✅ buffered + **O_DIRECT** write/read with sha256 round-trip across `drop_caches`
- ✅ **inline extents → per-inode extent B+tree** spill, validated with `fsx`
  (30 000+ ops) against an in-memory mirror, cross-checked vs XFS with identical seeds
- ✅ **reflink** (`FICLONE`) + **CoW snapshots** with copy-on-write isolation
- ✅ **cross-file dedup** (`FIDEDUPERANGE`), xattr, POSIX ACLs, sym/hardlinks, `mknod`
- ✅ `fallocate` preallocate / **punch-hole** / **zero-range**, sparse, truncate
- ✅ **discard/TRIM** (`fstrim`) reclaims free space on the thin LUN (SCSI UNMAP)
- ✅ **autonomous online autogrow**, including **repeated** grows in one mount
- ✅ **online scrub** verifies every metadata checksum **and every data block** (`-C`)
- ✅ **rename is crash- and cluster-safe** — a regression where a coherent metadata
  re-read could clobber an in-transaction directory block (silently destroying a
  renamed file, e.g. every `vzdump` archive) was found and fixed; validated 3-node

### Two / three nodes — the real validation target (`fsck` clean)

- ✅ **L3 membership + fencing** — storage heartbeat; a paused node is DECLARED
  DEAD past the window and SCSI-PR fenced
- ✅ **L4 single-writer ownership** — an EX owner blocks a peer's open-for-write;
  after release the peer takes it and reads coherent data
- ✅ **L4b directory coherence** — concurrent create/rename in the **same**
  directory across nodes converge with no lost entries; **90-way concurrent create
  (3×30) = 90/90, 0 errors**, `fsck` clean
- ✅ **L5 recovery** — a dead node's leases are reclaimed and its per-node journal
  is **log-scan replayed** by a survivor (off the heartbeat path), content intact
  — validated by real power-loss with files held open (uncheckpointed) at crash
- ✅ **data checksums cross-node** — a raw `dd` corruption of a block on the LUN is
  caught on a *peer's* O_DIRECT and buffered read (`-EIO`), deferred csums durable
  across a crash (flushed at `fsync` / lease release)

---

## ⚡ Performance

Measured on the real testbed: **Proxmox VE 9 (kernel 7.0.x-pve)**, a **TrueNAS
SCALE iSCSI LUN over 1 GbE**, single SSD, SCSI PR + CAW; fio O_DIRECT
(`--direct=1 --ioengine=libaio`, the Proxmox `cache=none` path). The 1 GbE link
and the single shared SSD are the ceilings — treat absolutes as *this rig* and
read the **ratios**.

### The headline: checksums are no longer the cluster random-write cost

The hard problem on a clustered FS is small random writes: each one used to pay a
synchronous **SCSI Compare-And-Write** to store its data checksum — a *fabric
round-trip* whose latency does **not** scale with bandwidth, so it stays the
ceiling even on faster fabrics. Three changes removed it:

| Cluster `-C` random-4 KiB write (O_DIRECT) | IOPS | |
|---|--:|---|
| synchronous per-block checksum CAW (before) | ~2 050 | baseline |
| **deferred + batched checksums (now)** | **~15 000–20 000** | **~8–10×** |

The checksum now accumulates in memory and flushes in **batched** CAWs (one per
checksum block) at coherence points (`fsync`, lease hand-off, `sync`), so the hot
write path no longer round-trips the fabric. `-C` cluster random write now sits at
the **no-checksum** rate — the integrity feature is effectively free — while
corruption is still caught on read and the only trade is a narrow post-crash
window where a not-yet-flushed checksum yields a benign read false-positive that a
rewrite or the scrub clears.

### Backup / restore / clone (real Proxmox ops, 3-node)

| Operation | Time | Note |
|---|--:|---|
| `qm clone` (template → VM, reflink) | **~1.3 s** | copy-on-write, instant |
| `vzdump` backup (3 GB VM) | **~17 s** | |
| `qmrestore` (5 GB image) | **~25 s** | was ~280 s before the allocation reservation |

The restore (a scattered small-write import) went from **~280 s to ~25–40 s** via a
per-inode **allocation reservation**: a node claims a contiguous block run in one
CAW and carves writes out of it, instead of one CAW per 64 KiB qcow2 cluster.

### Why the FS isn't the bottleneck — RAM-measured ceiling

To separate the filesystem software from the fabric, OCSFS single-node was
measured on a RAM block device (no SAN, no CAW):

| random-4 KiB write | IOPS |
|---|--:|
| raw RAM device (1 thread QD32) | 280 000 |
| **OCSFS single-node (1 thread QD32)** | **252 000** (90 % of raw) |
| OCSFS, 4 threads on the **same** file | **495 000** (scales — no lock serialisation) |

The FS software adds ~10 % over raw and **scales with concurrent writers on one
inode** — there is no per-inode lock serialising the I/O path. So the entire
single-node→cluster gap is the SAN/CAW round-trip, not OCSFS: **the lever for
faster fabrics (FC 32 G, iSCSI 40 G) is fewer CAW round-trips**, which is exactly
what the deferred checksums, the allocation reservation, and the **node-owned-AG
allocation affinity** (nodes allocate from disjoint AGs → no cross-node CAW
contention) target.

### Sequential & read

Sequential write/read run at the 1 GbE line rate (~50–90 MB/s on this rig);
**buffered** sequential write went from ~7 MB/s to ~50–70 MB/s once the page cache
was given **large folios** (writeback maps + checksums a multi-block folio at once
instead of one 4 KiB block). Random-4 KiB **read** on `-C` pays the inline checksum
verify (one stored-CRC fetch per block) — that is the one remaining `-C` read cost
and is most visible on uncached random reads.

### The biggest lever is on the SAN — match `volblocksize`

A 4 KiB random write to a **16 KiB-`volblocksize`** zvol forces a 16 KiB ZFS
read-modify-write (4× amplification). **Match the zvol `volblocksize` to the
guest** (4 KiB for random-heavy disks) for ~4× random-write, entirely SAN-side.
**Scale aggregate I/O by spreading VMs across multiple LUNs**, not by adding nodes
to one LUN — a single shared target is the ceiling.

> **Inline compression is out of scope** (not a TODO): it breaks the iomap 1:1
> logical↔physical mapping and O_DIRECT (`cache=none`, the Proxmox default). Space
> savings come from dedup + thin + discard instead.

---

## 🏗️ Architecture

```
        ┌──────────────────────────────────────────────────────────────┐
        │                    Shared block device (LUN)                  │
        │                 iSCSI / Fibre Channel SAN                     │
        │                                                               │
        │  SB │ node table │ heartbeat │ lease table │ recovery         │
        │     │ journal[per node] │ AG[0..N): bitmap·inodes·refcount·csum·data│
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
| **L2** local engine | iomap data path, extents (inline + B+tree), allocation, namespace, journal, reflink/CoW/dedup — **validated single-node before any cluster code** | `iomap.c` `inode.c` `dir.c` `bitmap.c` `journal.c` `refcount.c` `reflink.c` `extent_btree.c` `file.c` `xattr.c` `csum.c` |
| **L3** membership | On-disk heartbeat (liveness-epoch / observer clock) + SCSI-PR fencing, behind `ocsfs2_cluster_ops` (pluggable for corosync) | `cluster.c` |
| **L4** ownership | Per-file lease table, **true optimistic CAS over CAW** (read → check → CAW); L4b coarse metadata lease for namespace/allocation | `lease.c` |
| **L5** recovery | Leader election → log-scan replay of dead peer's journal → reclaim its leases, off the heartbeat path | `lease.c` + `journal.c` |

### Key design choices

- **Single-writer ownership** (above) is the load-bearing decision.
- **Coordination on the LUN.** No DLM daemon, no cluster LAN: lease/membership
  state are versioned on-disk records mutated by **Compare-And-Write**.
- **Coherence by construction + CAW.** File data is single-writer; the small
  shared metadata (bitmap, inode table, checksums) is mutated by **CAW** and read
  fresh via direct bios, so a node never acts on a stale peer view. An
  in-transaction block is never re-read from disk (it would clobber the
  uncommitted change — the rename fix).
- **Deferred journal + checkpoint-on-lease-release.** The per-node WAL batches
  commits (jbd2-lite); a node flushes its deferred metadata to home blocks when it
  releases an EX lease, so the next owner reads current state — coherence at lease
  boundaries, crash recovery by log-scan replay.
- **Liveness-epoch leases.** A lease is honoured only while its owner is ALIVE at a
  matching generation — no per-lease renewal, just one heartbeat per node.

---

## 🧬 What's implemented (`kmod2/`)

| File | Responsibility |
|---|---|
| `super.c` | Mount/unmount, `-o cluster` / `-o csum_async` options, AG headers (pre-sized for autogrow), statfs, sync |
| `inode.c` | Inode read/write/alloc/free (CAW in cluster), extent map (inline ↔ B+tree), node-owned-AG inode affinity, coherent re-read |
| `dir.c` · `rename.c` | Namespace ops under the metadata lease; dir blocks read fresh; in-txn block protected from re-read clobber |
| `iomap.c` | Buffered + O_DIRECT data path; large folios; block-granular CoW; per-inode **allocation reservation**; async inline data-checksum read verify |
| `csum.c` | Per-data-block CRC32c (`mkfs -C`): **deferred + per-checksum-block batched** store in cluster, batched coherent read, inline verify, clear-on-free |
| `file.c` | `fallocate` (preallocate/punch/zero), `fiemap`, `SEEK_HOLE/DATA`; lease on open/release |
| `extent_btree.c` | Per-inode extent B+tree (spill past 16 inline extents) |
| `bitmap.c` | Per-AG allocation; **node-owned-AG affinity**; cluster alloc/free + **FITRIM** via CAW; drops freed blocks' checksums |
| `journal.c` | Per-node WAL: deferred/batched redo log, log-scan replay on mount, **log-scan replay of a dead peer's slot** (L5) |
| `refcount.c` | Per-AG refcount B+tree for reflink/snapshot/dedup sharing |
| `reflink.c` | `FICLONE`/`copy_file_range`, `FIDEDUPERANGE` dedup, snapshot ioctl, `FITRIM`/`GROWFS`/`SCRUB`/`DEFRAG` dispatch |
| `defrag.c` | Online extent compaction (`OCSFS_IOC_DEFRAG`) |
| `xattr.c` | xattr (user/trusted/security) + POSIX ACL in one block |
| `cluster.c` | L3: node-slot claim, heartbeat kthread, coherent bio + CAW helpers (incl. multi-slot CAW), fencing |
| `lease.c` | L4 ownership leases + L4b metadata lease + L5 recovery (leader, reclaim); checkpoint + csum flush on EX release |
| `grow.c` | L2: autonomous online autogrow (watcher + `OCSFS_IOC_GROWFS`); checksums new AGs |
| `scrub.c` | Online scrub: every metadata checksum **+ every data block** (`OCSFS_IOC_SCRUB`) |
| `transport/scsi_pr.c` | SCSI-3 PR + Compare-And-Write (the only file carried over from v1) |

### Userspace tools (`tools2/`)

| Tool | Description |
|---|---|
| `mkfs.ocsfs2` | Volume formatter; uniform autogrow-ready AGs; `-N` max nodes (default 32), `-C` data checksums, `-s` format-a-prefix (testing) |
| `fsck.ocsfs2` | Offline structural + checksum + refcount-tree check |
| `ocsfs2-scrub` · `ocsfs2-defrag` · `ocsfs2-tool` | Online scrub / defrag / growfs+snapshot CLIs (also driven by the systemd timers) |

---

## 🔨 Building

The one-step installer (below) is the supported path — it installs the module via
**DKMS**, so it is **rebuilt automatically on every kernel upgrade**. To build by
hand (on the node whose kernel will run it):

```bash
make -C /lib/modules/$(uname -r)/build M=$PWD/kmod2 modules    # kernel module
cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2                 # tools
cc -O2 -std=gnu11 tools2/fsck.c -o fsck.ocsfs2
```

## 🚀 Quick start

```bash
# Format ONCE from a single node (destroys data).
#   -N = max cluster nodes baked into the layout (default 32; 1 = single-node).
#        Format headroom, not a runtime cap — raising it later needs a reformat.
#   -C = enable per-data-block checksums (recommended on any SAN without its own
#        end-to-end integrity; near-free now, detects silent corruption on read).
sudo ./mkfs.ocsfs2 -L vmstore -N 32 -C -f /dev/sdb

sudo modprobe ocsfs2                  # on every node (DKMS-installed; see below)

# Single node (no clustering overhead): plain mount
sudo mount -t ocsfs2 /dev/sdb /mnt/ocsfs

# Cluster: mount -o cluster on EACH node sharing the LUN
sudo mount -t ocsfs2 -o cluster /dev/sdb /mnt/ocsfs
```

`-o cluster` opts into multinode mode (claims a node slot, registers an SCSI PR
key, starts the heartbeat). Without it the volume is single-node even if formatted
with `-N > 1`. **All nodes sharing a LUN must mount with `-o cluster`** (mounting
the same LUN on two nodes *without* it is two independent single-node mounts =
corruption).

---

## 🧰 Command-line reference (all flags)

Only the positional argument (`<device>` / `<path>`) is ever mandatory — **every
flag below has a default**. Sizes for `-j`/`-s` are **bytes** (accept `0x…` hex).
The administrator guide (`docs/admin-guide.md` §9) carries the same reference with
worked examples.

### `mkfs.ocsfs2` — format a volume *(run once, destroys data)*

```
mkfs.ocsfs2 [-L <label>] [-N <max-nodes>] [-j <bytes>] [-s <bytes>] [-C] [-f] <device>
```

| Flag | Arg | Default | Meaning |
|---|---|---|---|
| `-N` | max nodes | **32** | Cluster nodes baked into the layout (lease/heartbeat slots), range `1..256`. `1` = single-node. **Format headroom, not a runtime cap** — each node reserves a ~16 MiB journal + slot + heartbeat (32 ≈ 512 MiB); raising it later needs a reformat, so pick the cluster's eventual maximum now. |
| `-C` | — | off | Enable **per-physical-block data checksums** (CRC32c), verified inline on every read → `-EIO` not silent corruption. Recommended on any SAN without its own end-to-end integrity. Near-free now (deferred + batched). |
| `-L` | label | none | Volume label stored in the superblock (shows up in `blkid`). |
| `-j` | bytes | 16 MiB | Per-node journal (WAL) size, rounded up to a 4 KiB block; total journal area = `<bytes> × -N`. Rarely changed. |
| `-s` | bytes | whole device | Format only the first *N* bytes and let **autogrow** extend into the rest later (thin initial layout / grow testing). |
| `-f` | — | off | Force: overwrite an existing OCSFS/foreign signature instead of refusing. |

### `mount` — attach the volume

```
mount -t ocsfs2 [-o cluster[,csum_async]] <device> <mountpoint>
```

| `-o` option | Default | Effect |
|---|---|---|
| `cluster` | off | Opt into **multinode** mode: claim a node slot, register the SCSI-PR key, start the heartbeat / liveness epoch, enable ownership + metadata leases and crash recovery. **Required on *every* node sharing a LUN.** |
| `csum_async` | off | *(`-C` volumes, **single-node only**)* defer the per-write checksum `sync` to writeback — faster sustained 4 KiB random write, wider post-crash false-positive window (a rewrite/scrub clears it). **Ignored in cluster mode** (there the checksum *is* the coherence CAW). |

### `ocsfs2-tool` — snapshots & online grow

```
ocsfs2-tool snapshot <src-file> <snap-name>   # CoW snapshot, created next to <src-file>
ocsfs2-tool growfs   <path-on-fs>             # force an autogrow check now
```

`<snap-name>` must be a **bare name** (no `/`) — the snapshot lands in `<src-file>`'s
directory and shares all its blocks until the next write. `growfs` makes the FS
pick up a LUN that grew underneath it (safe to repeat any number of times).

### `ocsfs2-scrub` — online checksum verify

```
ocsfs2-scrub [-q] <mountpoint>
```

| Flag | Meaning |
|---|---|
| `-q` | Print only the one-line summary. |

Verifies every metadata checksum **and every data block** (on `-C` volumes) on the
live filesystem. Exit `0` = clean, `1` = checksum findings, `2` = usage/error.

### `ocsfs2-defrag` — online extent compaction

```
ocsfs2-defrag [-r] [-n] [-t <min-extents>] <path>
```

| Flag | Arg | Default | Meaning |
|---|---|---|---|
| `-r` | — | off | Recurse into a directory and defrag every regular file under it (stays within one mount). |
| `-n` | — | off | Dry-run: only report each file's current extent count / fragmentation. |
| `-t` | min extents | **8** | Only defrag files with **more than** *n* extents. |

Relocates only a file's **private** extents — shared (reflink/snapshot/dedup)
blocks are skipped, so defrag never breaks sharing or inflates space.

### `fsck.ocsfs2` — check (online *or* offline)

```
fsck.ocsfs2 [-r] <device>        # OFFLINE: full structural + checksum pass (unmounted)
fsck.ocsfs2 <mountpoint>         # ONLINE: live check via the scrub ioctl (no downtime)
```

| Flag | Meaning |
|---|---|
| `-r` | **Accepted but not yet implemented** — cross-referential *repair* is a roadmap item; today it prints a notice and runs the read-only check. Restore from backup if a check reports errors. |

Read-only verifier. Pass a **mountpoint** to check a *running* filesystem (same
engine as `ocsfs2-scrub`), or a **device** for the full off-disk pass unmounted.

### `fstrim` — thin reclaim *(standard util-linux)*

```
fstrim <mountpoint>
```

OCSFS turns it into SCSI **UNMAP** over the volume's free blocks so the SAN can
thin-reclaim them. The weekly discard is left to your own policy / `fstrim.timer`.

---

## 🖥️ Proxmox VE

A one-step installer wires up everything on a node — prerequisites, the kernel
module **via DKMS**, on-disk/online tools, mount helper, the PVE storage plugin,
and the weekly scrub/defrag timers:

```bash
sudo ./proxmox2/install.sh        # run ONCE on EACH node — no re-run after a kernel upgrade
```

Then format the shared LUN once and declare the storage:

```bash
mkfs.ocsfs2 -L vmstore -N 32 -C -f /dev/disk/by-id/<your-lun>
```
```ini
# /etc/pve/storage.cfg  (or via the GUI once the plugin loads)
ocsfs2: vmstore
    path /mnt/pve/vmstore
    device /dev/disk/by-id/<your-lun>
    content images,iso,vztmpl,backup,rootdir,snippets
    cluster 1
    shared 1
```

The plugin owns the clustered mount/unmount, prefers **reflink** for fast VM
clones, and supports every PVE content type. Clone, snapshot, backup/restore and
disk resize are validated end-to-end on a 3-node cluster (see Status). **Online
live migration works** with no data copy — it *is* the OCSFS write-ownership lease
handing off from source to destination (deferred write lease; the destination
opens the disk while the source still runs, and claims the lease at the
switchover's first write).

---

## ⚠️ Known limitations

| Area | Status |
|---|---|
| **Maturity** | Alpha / research — not production-ready, AI-generated, unreviewed |
| **Inline compression** | **Out of scope — not planned.** Breaks the iomap 1:1 mapping and O_DIRECT (`cache=none`). Space savings come from dedup + thin + discard. |
| **`-C` random read** | Each read fetches its stored checksum (one coherent read per checksum block) — near-free for sequential/cached, but uncached random-read pays it. The write side is now near-free (deferred + batched). |
| **Cluster size (max nodes)** | Fixed at format time by `mkfs -N` (default **32**), like OCFS2 node slots — each node reserves a private journal (~16 MiB) + slot + heartbeat. Format headroom, not a runtime cap; raising it needs a reformat. |
| **Concurrent evict-time free under cluster** | Concurrent delete+alloc across nodes is not yet fully coordinated at evict time (known follow-up). |
| **`df` free-count in cluster mode** | The on-disk **block bitmap is authoritative and always correct** (`fsck` recomputes it; allocation never returns a false ENOSPC), but the superblock's *cached* free-block counter drifts in cluster mode until a remount. Cosmetic — not a data/integrity issue. |
| **Extreme concurrent metadata churn** | Hundreds of simultaneous cross-node `create()`s can saturate the SAN's CAW path and fail some ops with `-EIO` (cleanly — the create rolls back, `fsck` clean); use moderate metadata concurrency. The **data path is unaffected**. |
| **Encryption** | Out of scope by design — encrypt at the SAN/LUN (LUKS on the zvol) or in the guest. |
| **Fencing** | SCSI-3 Persistent Reservations only; out-of-band STONITH not wired. |

---

## 📁 Project structure

```
ocsfs/
├── kmod2/      OCSFS v2 kernel module (C, GPL-2.0) — single-writer ownership
│   ├── transport/scsi_pr.c   SCSI-3 PR + Compare-And-Write
│   └── dkms.conf             DKMS packaging (auto-rebuild on kernel upgrade)
├── tools2/     mkfs.ocsfs2, fsck.ocsfs2, ocsfs2-{scrub,defrag,tool}
├── proxmox2/   PVE storage plugin + mount helper + one-step install.sh
├── tests/v2/   single-node + real-cluster validation scripts
├── docs/       design-v2.md (spec), admin/developer guides, TODO, plans/
└── LICENSE     GPL-2.0-only
```

> The v1 codebase (FUSE prototype + first kernel module) lives on the **`v1-legacy`**
> branch; `main` carries only v2.

---

## 🗺️ Roadmap

1. ✅ ~~L2 single-node data path, fsx-validated~~
2. ✅ ~~reflink + CoW snapshots; cross-file dedup~~
3. ✅ ~~L3 membership + L4 single-writer ownership + L4b directory coherence~~
4. ✅ ~~L5 recovery (peer journal log-scan replay + eager lease reclaim)~~
5. ✅ ~~discard/TRIM, autonomous repeatable autogrow, online metadata + data scrub~~
6. ✅ ~~per-data-block checksums (`mkfs -C`) with inline read verification + DKMS~~
7. ✅ ~~Proxmox storage battery (clone/snapshot/backup/restore/resize), 3-node~~
8. ✅ ~~cluster perf: deferred+batched checksums, allocation reservation, large folios, node-owned-AG affinity (random-write checksum cost removed)~~
9. ✅ ~~online live migration (deferred write lease + lazy EX on first write), validated bidirectional 3-node~~
10. Corosync membership provider (the L3 interface is already pluggable); parallel multi-node recovery; out-of-band STONITH

---

## 📜 License

**GPL-2.0-only** — compatible with Linux kernel inclusion.
