# OCSFS v2 — Administrator Guide

**Filesystem:** OCSFS v2 (`ocsfs2`) — a shared-disk clustered filesystem with
**single-writer ownership**, built for Proxmox VE VM/CT image storage on a SAN
LUN (iSCSI/FC) that supports SCSI Persistent Reservations + Compare-and-Write.

> Status: alpha / research. Validated on real iSCSI LUNs, single-node and 2–3
> nodes. Not production-hardened. Always keep backups.

---

## 1. What it is (and is not)

OCSFS v2 lets several Proxmox nodes mount the **same block device** and use it as
one POSIX filesystem. Unlike a per-I/O distributed-lock filesystem, OCSFS gives
each open file to **one node at a time** (a coarse, long-held *ownership lease*).
That node does all I/O to the file at near-raw speed with no per-operation
network/SCSI round-trip; on close (or live-migration handoff) ownership passes to
another node. This matches the VM-disk workload: one running VM writes its disk
from one host.

| In scope | Out of scope |
|---|---|
| VM/CT disk images (raw, qcow2) | General multi-writer POSIX on one file |
| reflink clones, CoW snapshots, cross-file dedup | Inline compression (breaks O_DIRECT) |
| thin provisioning + discard/TRIM | Per-file encryption (use LUKS on the LUN/guest) |
| online metadata scrub, defrag | mmap (`-ENODEV`; unused by the workload) |
| autonomous, repeatable online grow | Quotas (hooks only, not enforceable) |

> **Designed for high availability — never stop the filesystem.** Every
> maintenance task is **online**: check (`fsck`/scrub), defragment, grow and
> discard all run on a mounted, in-use volume with no downtime. A node crash is
> survived: the remaining nodes keep serving their own files, fence the dead node
> and recover its files automatically (journal replay + lease reclaim) within
> seconds, with no data loss. There is no offline-maintenance window in normal
> operation — unmounting is only needed for the optional full off-disk `fsck`
> repair pass.

---

## 2. Requirements

- A **shared block LUN** reachable as the same device from every node
  (`/dev/disk/by-id/...`). For clustered mode the target must support **SCSI-3
  Persistent Reservations** and **Compare-and-Write (CAW / opcode 0x89)**
  (TrueNAS SCST, most enterprise arrays). Single-node mode needs neither.
- **Identical kernel across all nodes** — the module is out-of-tree and must
  match `uname -r` on each node.

### Prerequisites (install on every node)

The module is built per node against its running kernel via **DKMS**, so each
node needs `dkms`, the build toolchain and matching kernel headers:

```bash
apt-get install -y dkms build-essential proxmox-headers-$(uname -r) proxmox-default-headers
# (on stock Debian: dkms build-essential linux-headers-$(uname -r) linux-headers-amd64)
```

That is the entire end-user prerequisite set — no debug or test tooling is needed
in production. `proxmox2/install.sh` **installs all of these automatically** if
missing, so on a standard Debian/PVE node you can run the installer directly.
Because the module is managed by DKMS, it is **rebuilt automatically on every
kernel upgrade** — you do *not* re-run the installer after an upgrade (the
`proxmox-default-headers` meta-package keeps headers available for new kernels).

---

## 3. Install

The one-step installer builds and installs the module, the user tools and (on
Proxmox) the storage plugin:

```bash
cd OCSFS/proxmox2
./install.sh
```

It performs:

1. Installs prerequisites (`dkms`, `build-essential`, kernel headers).
2. Registers + builds `ocsfs2` via **DKMS** (`/usr/src/ocsfs2-2.0`) → installs to
   `/lib/modules/$(uname -r)/updates/dkms/` and runs `depmod`. DKMS rebuilds it
   for every future kernel automatically.
3. Builds `mkfs.ocsfs2`, `fsck.ocsfs2`, `ocsfs2-{scrub,defrag,tool}` → `/usr/sbin`.
4. Installs the PVE storage plugin `OCSFS2Plugin.pm` and `mount.ocsfs2`.
5. Installs and enables the periodic `ocsfs2-scrub.timer` and `ocsfs2-defrag.timer`.

Run it **once on every** node. `dkms status` shows the module state; after a
kernel upgrade it is rebuilt automatically (no re-run needed).

---

## 4. Create a filesystem

```bash
# single node (no cluster services), whole device:
mkfs.ocsfs2 -f -N 1 /dev/disk/by-id/scsi-XXXX

# clustered with data checksums (recommended), headroom for up to 32 nodes:
mkfs.ocsfs2 -f -N 32 -C /dev/disk/by-id/scsi-XXXX
```

| Flag | Meaning |
|---|---|
| `-N <n>` | maximum cluster nodes baked into the layout (slots). `1` = single-node. **Default 32.** Format headroom, not a runtime cap — each node reserves a ~16 MiB journal + slot + heartbeat, so 32 ≈ 512 MiB; raising it later needs a reformat, so pick the cluster's eventual max now. |
| `-C` | enable **per-data-block checksums** (CRC32c) — silent-corruption detection on any SAN; verified inline on every read. Recommended. Near-free except sustained 4 KiB-random-write (see §10). |
| `-f` | force (overwrite an existing filesystem) |
| `-s <MiB>` | format only the first *N* MiB and let autogrow extend later |

`mkfs` lays out uniform allocation groups (AG), each self-contained
(header + block bitmap + inode table + per-AG refcount B+tree), a per-node
journal area, the lease/membership tables and a superblock + mirror. The
`AUTOGROW` compat feature is set so the volume can grow online later.

---

## 5. Mount

```bash
# single node:
mount -t ocsfs2 /dev/disk/by-id/scsi-XXXX /mnt/vmstore

# clustered (every node, same LUN):
mount -t ocsfs2 -o cluster /dev/disk/by-id/scsi-XXXX /mnt/vmstore
```

`-o cluster` enables membership (heartbeat slot), SCSI-PR fencing, the ownership
/ metadata leases and crash recovery. Without it the volume is single-node only.

On mount each node claims a heartbeat slot and starts its liveness epoch; a node
that stops heartbeating is fenced (its PR key is preempted) and its files are
recovered by a survivor (journal replay + lease reclaim).

---

## 6. Proxmox integration

Add a storage of type `ocsfs2` in `/etc/pve/storage.cfg`:

```
ocsfs2: vmstore
    path /mnt/pve/vmstore
    device /dev/disk/by-id/scsi-3600...   # the shared LUN (stable path)
    content images,iso,vztmpl,backup,rootdir,snippets
    cluster 1
    shared 1
```

The plugin owns mount/unmount of the LUN and presents it like a directory
datastore, so VM and CT image management, ISOs and backups all work. It prefers
**reflink** for clones (`cp --reflink=always`), so a linked clone or a template
deploy is near-instant and space-efficient. Live migration works because the
file's write-ownership lease is handed from source to destination on open/close —
no data copy.

- **VM disks**: store as `raw` (reflink/snapshot/discard all work) or `qcow2`.
- **CT (LXC)**: stored as `raw` images on a loop device (`subvol=0`), so the
  container's own filesystem lives inside the image — OCSFS sees only
  pread/pwrite, never mmap.

### Proxmox disk cache modes

All QEMU cache modes work, because OCSFS keeps the buffered and O_DIRECT paths
coherent and CoW-correct:

| `cache=` | Guest I/O to OCSFS | Notes |
|---|---|---|
| `none` (default) / `directsync` | O_DIRECT | hot path; validated clean vs XFS |
| `writeback` / `writethrough` / `unsafe` | buffered | validated clean vs XFS |

---

## 7. Day-2 operations

### Snapshots & clones

```bash
cp --reflink=always disk.raw disk-clone.raw     # FICLONE
ocsfs2-tool snapshot disk.raw disk.snap         # OCSFS_IOC_SNAP_CREATE
```

A snapshot/clone shares blocks; the first write to either side copies just the
touched blocks (block-granular CoW), so sharers stay isolated.

### Online grow (repeatable)

Extend the LUN on the SAN, rescan it on each node, then either let the autogrow
watcher pick it up (it polls every ~30 s) or force it:

```bash
ocsfs2-tool growfs /mnt/vmstore        # OCSFS_IOC_GROWFS — force a grow check now
```

Grow appends new AGs onto the freshly-available space and publishes the new size
to peers. It can be run **any number of times** (unlike v1's one-shot grow).

### Discard / thin reclaim

```bash
fstrim /mnt/vmstore                    # FITRIM → SCSI UNMAP on free blocks
```

### Scrub (periodic + on-demand)

Verifies every metadata checksum (superblock, AG headers, inode table, extent
and refcount B+trees, xattrs) online while mounted:

```bash
ocsfs2-scrub /mnt/vmstore              # OCSFS_IOC_SCRUB; prints a summary
```

The installer enables `ocsfs2-scrub.timer` (weekly) to scrub every mounted
`ocsfs2` volume and log findings to the journal. See §8.

### Data integrity (data checksums)

Format with **`-C`** to enable per-data-block CRC32c checksums — silent-data-
corruption detection that works on **any** SAN (not only one with its own
integrity like ZFS). A CRC is stored on every write (buffered and O_DIRECT, kept
coherent across nodes via CAW) and **verified inline on every read** on both
paths: a mismatch returns **`-EIO`** to the reader (the app/VM sees a read error
instead of corrupt data) and logs `DATA checksum mismatch on read at block N` —
restore the affected file from backup. The online scrub does the same check in
bulk (`ocsfs2-scrub` / the weekly timer). Checksums follow the physical block, so
reflink/snapshot/CoW stay correct, and a freed block's CRC is dropped so reuse
never false-positives. Opt-in; existing volumes are unaffected. The cost is
deliberately small — see §10. Caveats: a crash mid-writeback can leave a block
whose stored CRC doesn't match the not-yet-written data (a benign false-positive
that a rewrite/scrub clears); AGs added by online autogrow are not yet checksummed.

### Defragment (periodic + on-demand)

Coalesces a file's extent map by relocating its **private** (non-shared) data
into contiguous runs, reducing fragmentation that random VM-disk writes,
snapshots and discard accumulate over time:

```bash
ocsfs2-defrag /mnt/vmstore/vm-100-disk-0.raw   # one file
ocsfs2-defrag -r /mnt/vmstore                  # walk a tree, defrag fragmented files
ocsfs2-defrag -n /mnt/vmstore/disk.raw         # dry-run: report fragmentation only
```

Shared (reflinked/snapshotted/deduped) extents are left intact so defrag never
breaks sharing or inflates space. The installer enables `ocsfs2-defrag.timer`
(weekly, after scrub). See §8.

### Check (online **or** offline)

```bash
fsck.ocsfs2 /mnt/vmstore                         # ONLINE: mounted, in-use — via scrub ioctl
# or, with the volume unmounted, a full off-disk pass:
umount /mnt/vmstore && fsck.ocsfs2 /dev/disk/by-id/scsi-XXXX
```

Give `fsck.ocsfs2` a **mountpoint** to check a *running* filesystem (no downtime —
it verifies every metadata checksum and per-AG structure via `OCSFS_IOC_SCRUB`),
or a **device** for the full off-disk structural pass. Cross-referential *repair*
still requires unmounting.

---

## 8. The periodic services

Two systemd timers are installed and enabled by `install.sh`:

| Unit | Default schedule | Action |
|---|---|---|
| `ocsfs2-scrub.timer` | weekly (Sun 03:00) | scrub every mounted `ocsfs2` mount |
| `ocsfs2-defrag.timer` | weekly (Sun 04:00) | defrag fragmented files under every mount |

They both invoke `/usr/sbin/ocsfs2-maint <scrub|defrag>`, which discovers the
mounted `ocsfs2` filesystems (`findmnt -t ocsfs2`) and runs the corresponding
tool on each, logging to the systemd journal. Tune them with a drop-in:

```bash
systemctl edit ocsfs2-scrub.timer      # change OnCalendar=
journalctl -u ocsfs2-scrub.service     # read findings
systemctl disable --now ocsfs2-defrag.timer   # opt out
```

Both are **online** and safe to run on a mounted, in-use volume.

**On a cluster the timer is enabled on every node, but only ONE node runs each
job per cycle.** `ocsfs2-maint` elects a single node with an atomic lock file on
the shared filesystem (`.ocsfs2-maint.<mode>.lock` at the mount root); the others
log "skipped … running on another node". A lock left by a crashed node is stolen
after `OCSFS2_MAINT_STALE` seconds (default 24 h). Keeping the timer on all nodes
means maintenance still happens if the usual node is down — no single point of
failure. Defrag additionally skips any file actively owned by another node
(it returns `-EBUSY`), so a running VM's disk is never relocated from elsewhere.

---

## 9. Command reference

Every command below is shown as **synopsis → what it does → options → example**.
All tools accept `-h`/`--help`.

### `mkfs.ocsfs2` — create a filesystem

```
mkfs.ocsfs2 [-f] -N <max-nodes> [-s <MiB>] [-C] <device>
```
Writes a fresh OCSFS v2 filesystem onto `<device>` (a whole disk/LUN or a
partition). **Destroys existing data.**

| Option | Meaning |
|---|---|
| `-N <n>` | maximum cluster nodes (lease/HB slots). `1` = single-node, no cluster services. **Required.** |
| `-f` | force: overwrite an existing filesystem/signature |
| `-s <MiB>` | format only the first *MiB* and rely on autogrow to extend later (thin initial layout) |
| `-C` | **data checksums**: reserve a per-AG CRC32c region so silent data corruption is detectable on *any* SAN (see §7 *Data integrity*). Opt-in (small space + write overhead). |

```bash
# 3-node clustered FS on the whole LUN:
mkfs.ocsfs2 -f -N 3 /dev/disk/by-id/scsi-3600abcd
```

### `mount` — attach the filesystem

```
mount -t ocsfs2 [-o cluster] <device> <mountpoint>
```
Mounts the volume. Add `-o cluster` on a LUN shared by more than one node to
turn on membership, fencing, leases and recovery; omit it for single-host use.

```bash
mount -t ocsfs2 -o cluster /dev/disk/by-id/scsi-3600abcd /mnt/vmstore
```

### `ocsfs2-tool` — snapshots, grow and tuning

```
ocsfs2-tool snapshot <src-file> <snap-name>       # point-in-time reflink copy (snap-name is a bare name, created next to <src-file>)
ocsfs2-tool growfs   <mountpoint>                 # force an autogrow check now
```
`snapshot` creates a new file that shares all of `<src-file>`'s blocks and
diverges on the next write to either side. `growfs` makes the FS notice that the
underlying LUN got bigger and append new allocation groups (safe to repeat).

```bash
ocsfs2-tool snapshot /mnt/vmstore/vm-100-disk-0.raw vm-100-disk-0.snap
ocsfs2-tool growfs /mnt/vmstore
```

### `ocsfs2-scrub` — verify metadata checksums (online)

```
ocsfs2-scrub [-q] <mountpoint>
```
Walks all metadata and verifies every CRC32c while the volume stays mounted and
in use. Prints how many objects were checked and how many errors were found
(exit `0` = clean, non-zero = findings). `-q` prints only the one-line summary.

```bash
ocsfs2-scrub /mnt/vmstore
# ocsfs2-scrub: /mnt/vmstore — checked 29 AGs / 1284 inodes, 0 errors → CLEAN
```

### `ocsfs2-defrag` — reduce file fragmentation (online)

```
ocsfs2-defrag [-r] [-n] [-t <min-extents>] <path>
```
Relocates a file's **private** data into contiguous runs to shrink its extent
count. Shared (reflink/snapshot/dedup) blocks are skipped, so it never breaks
sharing.

| Option | Meaning |
|---|---|
| `-r` | recurse into a directory and defrag every regular file under it |
| `-n` | dry-run: only report each file's current extent count / fragmentation |
| `-t <n>` | only defrag files with more than *n* extents (default 8) |

```bash
ocsfs2-defrag -n /mnt/vmstore/vm-100-disk-0.raw     # report only
ocsfs2-defrag /mnt/vmstore/vm-100-disk-0.raw        # defrag one file
ocsfs2-defrag -r -t 16 /mnt/vmstore                 # defrag the whole store
```

### `fstrim` — reclaim free space on the SAN (thin)

```
fstrim <mountpoint>
```
Standard util-linux command; OCSFS turns it into SCSI UNMAP on the volume's free
blocks so the SAN can thin-reclaim them.

### `fsck.ocsfs2` — check, online or offline

```
fsck.ocsfs2 <device>        # OFFLINE: full structural + checksum pass (unmounted)
fsck.ocsfs2 <mountpoint>    # ONLINE: live check via OCSFS_IOC_SCRUB (no downtime)
```
Read-only verifier. Pass a **mountpoint** to check a *running* filesystem without
stopping it (it runs the same engine as `ocsfs2-scrub`), or a **device** for the
full off-disk pass with the volume unmounted. Repair is offline-only.

```bash
fsck.ocsfs2 /mnt/vmstore                                   # online, no downtime
umount /mnt/vmstore && fsck.ocsfs2 /dev/disk/by-id/scsi-3600abcd   # offline
```

---

## 10. Performance tuning

All guidance below is for the Proxmox `cache=none` (O_DIRECT) VM-disk workload,
measured on a 1 GbE iSCSI LUN backed by an SSD zvol.

**1. Match the SAN `volblocksize` to the guest — the single biggest lever.**
A 4 KiB random write to a **16 KiB-`volblocksize`** zvol forces a 16 KiB ZFS
read-modify-write (4× amplification). Creating the zvol with `volblocksize=4K`
gave **3.4× the random-write IOPS** (≈5.4k → ≈18k IOPS, 4 KiB QD32) — far more
than any FS-level effect. Use 4 KiB for random-heavy VM disks; 16 KiB is fine for
large-sequential / backup volumes. This is a TrueNAS/ZFS setting, set at zvol
creation (it cannot be changed later).

**2. Keep data checksums (`-C`) on — they are nearly free where it matters.**
Inline read verification and large/sequential writes cost ≈0–5 %; the *only*
measurable cost is **sustained pure 4 KiB-random-write**, capped at ~3.5k IOPS by
the crash-safe per-write checksum `sync` (independent of LUN speed). For typical
VM workloads (mixed, buffered, larger I/O) this is invisible; for a benchmark that
does nothing but 4 KiB O_DIRECT random writes, expect that ceiling.

| Workload (O_DIRECT, single node) | `-C` cost |
|---|---|
| reads (seq + random, any size) | ≈0 % |
| sequential / large-aligned writes | ≈2–5 % |
| 16 KiB random write | ≈10 % |
| pure 4 KiB random write | capped ~3.5k IOPS (integrity price) |

**3. SAN durability vs speed.** `sync=disabled` on the zvol is fastest but loses
recent writes on a SAN power failure; use `sync=standard` (optionally with a SLOG)
when the SAN lacks battery/UPS-backed cache. (Orthogonal to `-C`, which detects
*corruption*, not *lost* writes.)

**4. Cluster scaling needs multiple LUNs.** Three nodes on one LUN share that one
device's ceiling (there is no per-I/O clustering tax — single-writer ownership
does zero on-disk locking in steady state, so the limit is the disk/network, not
OCSFS). Spread VMs across several LUNs / a faster pool to scale aggregate I/O.

---

## 11. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `mount: unknown filesystem type 'ocsfs2'` | `modprobe ocsfs2`; check the module matches `uname -r` (rebuild on the right kernel) |
| `mount ... -EBUSY` on a file open | another live node owns the write lease; it releases on close/migration |
| node fenced / files briefly unavailable | a peer stopped heartbeating and was fenced; its files are recovered (journal replay) within a few seconds |
| `fstrim` returns 0 bytes | nothing to reclaim, or the LUN does not advertise UNMAP |
| `mmap(): No such device (ENODEV)` | by design — OCSFS has no mmap; the workload never needs it |
| scrub reports a checksum error | a metadata block is damaged; unmount and run `fsck.ocsfs2`, restore from backup if needed |

**Never** `fuser -km` a mountpoint on a Proxmox node (it can kill `init`). Use
`umount` (non-lazy) and retry.

---

## 12. Limits & caveats

- Encryption is **out of scope** — encrypt the LUN (LUKS on the zvol) or the
  guest. `-K` provides cluster-auth HMAC only.
- Quotas are stubbed (hooks present, not enforceable).
- Compression is **out of scope** (incompatible with O_DIRECT and the iomap 1:1
  mapping); space efficiency comes from dedup + thin + discard.
- Multi-node recovery is sequential (one dead node at a time; none are dropped).
- Out-of-band STONITH (PDU/iDRAC) is not wired; SCSI-PR fencing is.
