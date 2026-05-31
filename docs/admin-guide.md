# OCSFS — Administrator Guide

**Version:** 0.1 — May 2026
**Platform:** Proxmox VE 8.x / 9.x, Linux kernel 6.6+ LTS (validated on 7.0.x-pve)
**Status:** Alpha — do not use with production data

> **Performance note.** Random 4 KiB O_DIRECT I/O to a VM-disk image on a
> clustered LUN runs at **near-raw device speed** for a single active node (the
> per-operation DLM/CAW overhead is held off the hot path). The bound that
> remains is *metadata-operation* throughput when a lock is genuinely contended
> across nodes. See the **Performance** section of the project `README.md`.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Installation](#2-installation)
3. [Formatting the LUN](#3-formatting-the-lun)
4. [Proxmox VE Configuration](#4-proxmox-ve-configuration)
5. [Mount and Basic Operations](#5-mount-and-basic-operations)
6. [Monitoring](#6-monitoring)
7. [Maintenance Operations](#7-maintenance-operations)
8. [Troubleshooting](#8-troubleshooting)
9. [Known Limitations](#9-known-limitations)

---

## 1. Prerequisites

### Storage Infrastructure

- **Shared block device:** FC SAN LUN or iSCSI target with SCSI-3 Persistent
  Reservations support, accessible from all cluster nodes simultaneously.
- **Multipath (recommended):** `multipathd` configured so that
  `/dev/mapper/mpath*` devices are visible on all nodes. Not strictly required
  for single-node or lab setups.
- **SCSI-3 PR:** The storage target must support SCSI-3 Persistent
  Reservations. All enterprise FC arrays do. For iSCSI, LIO (Linux) and
  TrueNAS SCALE support PR natively; check your target's documentation.

### Kernel and OS

- Linux kernel 6.6 LTS or newer
- Proxmox VE 8.0 or newer (for PVE integration)

### Verify SCSI PR support

```bash
# Run on the node that will connect to the shared device
sg_persist -i -k /dev/mapper/mpathX
# Expected: "PR generation=0x0" (empty is fine; any response means PR works)

# If sg_persist is not installed:
apt install sg3-utils
```

### Required packages on each node

```bash
apt install build-essential dkms linux-headers-$(uname -r) \
            multipath-tools sg3-utils uuid-runtime open-iscsi
```

---

## 2. Installation

### Option A — Debian packages (recommended)

```bash
git clone https://github.com/l0rdg3x/OCSFS /opt/ocsfs
cd /opt/ocsfs

# Build packages
dpkg-buildpackage -us -uc -b

# Install on every Proxmox node
dpkg -i ../ocsfs-tools_0.1.0-1_amd64.deb \
        ../ocsfs-dkms_0.1.0-1_all.deb \
        ../ocsfs-proxmox_0.1.0-1_all.deb
```

### Option B — Manual installation

```bash
cd /opt/ocsfs

# Userspace tools
make all
sudo make install        # installs to /usr/local/bin

# Kernel module via DKMS
sudo dkms add kmod/
sudo dkms build ocsfs/0.1.0
sudo dkms install ocsfs/0.1.0

# Proxmox VE plugin
sudo proxmox/install.sh
```

### Verify installation

```bash
sudo modprobe ocsfs
dmesg | grep ocsfs
# Expected: "ocsfs: Open Cluster Shared FileSystem v0.1 loaded"

mkfs.ocsfs --version
ocsfs-tool --help
```

---

## 3. Formatting the LUN

> **Warning:** `mkfs.ocsfs` destroys all existing data on the device.
> Run this only on a dedicated LUN, on **one node**.

```bash
# Identify the multipath device
multipath -ll

# Format — run on ONE node only
mkfs.ocsfs -L my-datastore -N 16 -J 32M -E 4M -f -v /dev/mapper/mpath0

# Verify
ocsfs-tool info /dev/mapper/mpath0
```

### Full option reference

Syntax: `mkfs.ocsfs [options] <device>`. **Geometry chosen here (block size, AG
size, max nodes) is fixed for the life of the volume** and cannot be changed
later — size it deliberately.

| Flag | Argument | Default | Meaning |
|---|---|---|---|
| `-L` | `<label>` | (none) | Volume label, ≤ 63 chars. Shown by `ocsfs-tool info`; usable as `mount LABEL=…`. |
| `-N` | `<max_nodes>` | `64` | Maximum cluster nodes that may ever mount the volume (1–256). Sizes the node-slot table, per-node journals, lock table and heartbeat area. **`-N 1` = single-node volume** (no DLM/CAW); `-N ≥ 2` = full cluster mode. Fixed at format. |
| `-b` | `<bytes>` | `4096` | Block size (plain number or `K`/`M` suffix). Leave at 4096 to match page size and SAN logical block. |
| `-E` | `<size>` | `1M` | Default extent (allocation-unit) hint, e.g. `1M`, `4M`. Larger = less metadata for big sequential files (VM images); smaller = better for many small files. |
| `-A` | `<size>` | `1G` | Allocation Group size. Each AG has its own bitmap/inode-table/locks, so independent AGs avoid cross-node contention. |
| `-J` | `<size>` | `32M` | **Per-node** journal size, e.g. `16M`, `64M`. Total journal = `J × N`. 16–32M suits VM workloads. |
| `-K` | — | off | Enable cluster authentication (HMAC + encrypted key store). A `-K` volume will **not** mount without `-o cluster_secret=`; required for fscrypt encryption. |
| `-T` | — | on | Enable thin provisioning (default on; flag kept for explicitness). |
| `-f` | — | off | Force — skip the erase-confirmation prompt (needed for scripted runs). |
| `-v` | — | off | Verbose — print computed geometry (AG count, journal offsets, inode-table layout). |
| `-h` | — | — | Show help. |

### Recommended profiles for Proxmox clusters

| Cluster size | `-N` | `-J` | `-E` | Example |
|---|---|---|---|---|
| Single node (lab) | `1` | `16M` | `1M` | `mkfs.ocsfs -L local -N 1 -J 16M -f /dev/sdb` |
| Small (2–4 nodes) | `8` | `16M` | `1M` | `mkfs.ocsfs -L vmstore -N 8 -J 16M -f /dev/mapper/mpath0` |
| Medium (5–16 nodes) | `32` | `32M` | `4M` | `mkfs.ocsfs -L vmstore -N 32 -J 32M -E 4M -f /dev/mapper/mpath0` |
| Large (17–64 nodes) | `64` | `64M` | `8M` | `mkfs.ocsfs -L vmstore -N 64 -J 64M -E 8M -f /dev/mapper/mpath0` |
| Encrypted cluster | `8` | `16M` | `1M` | `mkfs.ocsfs -L secure -N 8 -J 16M -K -f /dev/mapper/mpath0` |

> `max_nodes` is fixed at format time. Oversize slightly for future growth — and
> note `mkfs.ocsfs` over-allocates spare AG-descriptor slots so the volume can
> later be **grown online** (see §7) without relocating data.

---

## 4. Proxmox VE Configuration

### Add storage via CLI (runs on one node, replicates automatically)

```bash
pvesm add ocsfs fc-shared \
  --path /mnt/pve/fc-shared \
  --device /dev/mapper/mpath0 \
  --content images,iso,vztmpl,backup,rootdir,snippets \
  --maxnodes 16 \
  --thin 1 \
  --shared 1
```

### Manual configuration in `/etc/pve/storage.cfg`

```
ocsfs: fc-shared
    path /mnt/pve/fc-shared
    device /dev/mapper/mpath0
    content images,iso,vztmpl,backup,rootdir,snippets
    maxnodes 16
    thin 1
    shared 1
```

### Storage options reference

| Option | Description | Default |
|---|---|---|
| `path` | Local mount point | (required) |
| `device` | Block device (use multipath path) | (required) |
| `content` | Supported content types | images |
| `maxnodes` | Max concurrent nodes | 64 |
| `thin` | Enable thin provisioning for VM disks | 0 |
| `shared` | Mark storage as shared between nodes | 0 |
| `cluster_secret` | 64 hex-char (32-byte) cluster secret. Required for volumes formatted with `mkfs.ocsfs -K` and for the encrypted key store. Stored in `storage.cfg` — prefer `secret_file`. | (none) |
| `secret_file` | Path to a `0600` file whose first line is the cluster secret. Takes precedence over `cluster_secret`. | (none) |
| `degraded` | Allow clustered mount without SCSI-3 PR fencing (zombie-node risk; lab only). | 0 |

> **Auth/encrypted clusters:** a volume created with `mkfs.ocsfs -K` will *not* mount
> without a cluster secret. Set `secret_file` on every node (same secret), e.g.:
>
> ```
> ocsfs: fc-shared
>     path /mnt/pve/fc-shared
>     device /dev/mapper/mpath0
>     content images
>     maxnodes 16
>     thin 1
>     shared 1
>     secret_file /etc/pve/priv/ocsfs-fc-shared.secret
> ```
>
> Create the secret once with `openssl rand -hex 32 > /etc/pve/priv/ocsfs-fc-shared.secret`
> (it lands under `/etc/pve/priv`, replicated and root-only across the PVE cluster).

### Enable on all nodes

The Proxmox plugin mounts the storage automatically via `ocsfs-mount@.service`:

```bash
# Verify the service is active on each node
systemctl status ocsfs-mount@fc-shared.service

# Manual mount if needed
mount -t ocsfs /dev/mapper/mpath0 /mnt/pve/fc-shared
```

---

## 5. Mount and Basic Operations

### Manual mount

```bash
sudo modprobe ocsfs

# Standard mount
sudo mount -t ocsfs /dev/mapper/mpath0 /mnt/ocsfs

# With cluster authentication (if the volume has FEAT_AUTH)
sudo mount -t ocsfs -o cluster_secret=<64-hex-chars> \
  /dev/mapper/mpath0 /mnt/ocsfs
```

### Unmount

```bash
# Check that no process is using the filesystem
lsof /mnt/ocsfs

sudo umount /mnt/ocsfs
```

### Persistent mount via `/etc/fstab`

```
/dev/mapper/mpath0  /mnt/ocsfs  ocsfs  defaults,_netdev  0 0
```

---

## 6. Monitoring

### Cluster status

```bash
ocsfs-tool status /mnt/ocsfs

# Example output:
# Volume: shared-vm-store  UUID: a1b2c3...
# Block size: 4096  Total: 4.8 TiB  Free: 2.7 TiB
#
# Node 0 [pve1]  ACTIVE  HB: 2s ago  Locks: 0
# Node 1 [pve2]  ACTIVE  HB: 3s ago  Locks: 5
# Node 2 [pve3]  ACTIVE  HB: 1s ago  Locks: 3
```

### Active nodes

```bash
ocsfs-tool nodes /mnt/ocsfs
```

### Lock table

```bash
# Show all active locks (useful for diagnosing stalls or orphan locks)
ocsfs-tool locks /mnt/ocsfs
```

### Space usage

```bash
ocsfs-tool df /mnt/ocsfs
# Reports: total/used/free, thin-allocated vs. written, inodes per AG
```

### Kernel log

```bash
# Live OCSFS messages
dmesg -w | grep ocsfs

# Heartbeat and recovery events
journalctl -k | grep ocsfs
```

---

## 7. Maintenance Operations

### Online defragmentation

```bash
# Start defrag in background, limited to 50 MB/s
ocsfs-defrag /mnt/ocsfs -b 50

# Dry run — report only, no changes
ocsfs-defrag /mnt/ocsfs -n

# Verbose with custom fragmentation threshold
ocsfs-defrag /mnt/ocsfs -v -t 4 -b 100

# Pause / Resume via signals
kill -USR1 <defrag-pid>   # pause
kill -USR2 <defrag-pid>   # resume
```

The defrag daemon uses the lock table to ensure only one instance runs
cluster-wide at a time.

### Growing the filesystem onto a larger LUN

When you enlarge the backing LUN, hand the new space to OCSFS with `ocsfs-grow`.
The same binary picks **online** vs **offline** from its argument: a **mountpoint**
grows the live filesystem via `OCSFS_IOC_GROW`; a **block device** grows it offline.

```bash
# 1. Enlarge the LUN on the storage side (e.g. grow the TrueNAS zvol).

# 2. Rescan the new size on EVERY node that has the LUN attached:
iscsiadm -m node -R                 # iSCSI
# (FC: echo 1 > /sys/class/scsi_device/<h:c:t:l>/device/rescan)

# 3a. ONLINE grow — volume stays mounted. Run on ONE node, pass the MOUNTPOINT:
ocsfs-grow /mnt/pve/fc-shared
#    Peers see the extra space on their next allocation (or immediately via statfs/df).

# 3b. OFFLINE grow — unmount on ALL nodes first, pass the DEVICE:
ocsfs-grow /dev/mapper/mpath0

# Dry run (offline only): report what would be added, change nothing.
ocsfs-grow -n /dev/mapper/mpath0
```

Existing AGs are never moved (descriptors store absolute geometry); new AGs are
described in an extension region in the added space (`INCOMPAT_AG_GROW`).

> **Rescan first, on every node.** If a peer has not rescanned the larger LUN it
> will reject the new AGs as "beyond device size" until it does. One grow per
> volume is supported (re-growing an already-grown volume is rejected).

### Offline filesystem check

```bash
# The device MUST NOT be mounted
umount /mnt/ocsfs

# Run all 9 checks (read-only)
python3 /opt/ocsfs/tools/ocsfs-fsck /dev/mapper/mpath0

# Run with repair mode (patches orphans, stale locks, node slots)
python3 /opt/ocsfs/tools/ocsfs-fsck --repair /dev/mapper/mpath0
```

Checks performed:

| # | Check |
|---|---|
| 1 | Superblock magic and CRC32c |
| 2 | AG descriptor consistency |
| 3 | Journal header validity |
| 3b | Free block cross-check: bitmap popcount vs. AG descriptors vs. superblock |
| 4 | Inode table: orphan detection and repair |
| 5 | Extent consistency within each inode |
| 6 | Heartbeat region: stale entries |
| 7 | Lock table: stale or orphaned locks |
| 8 | Refcount table: unreferenced entries |
| 9 | Node slot table: stale ACTIVE entries |

### Thin provisioning — reclaiming space

Space is reclaimed automatically when guest OSes issue TRIM/DISCARD. To force it:

```bash
# From inside the guest Linux VM
fstrim -v /

# Verify reclaimed space on the host
ocsfs-tool df /mnt/ocsfs
```

### Manual recovery of a dead node

Recovery is automatic when heartbeat timeout expires. If it does not start:

```bash
# Force recovery of a specific slot
ocsfs-tool recover /mnt/ocsfs --node <slot-number>

# Emergency fencing
ocsfs-tool fence /mnt/ocsfs --node <slot-number>
```

---

## 8. Troubleshooting

### Module fails to load

```bash
dmesg | grep ocsfs
modinfo ocsfs        # verify module is compiled for the running kernel
dkms status          # verify DKMS state
```

**Common cause:** kernel was updated without rebuilding the module.

```bash
dkms autoinstall
```

### Mount fails with "bad magic"

```bash
ocsfs-tool info /dev/mapper/mpath0
# Error → device is not formatted as OCSFS
# Verify the correct multipath device: multipath -ll
```

### Mount fails with "superblock checksum mismatch"

The primary superblock is corrupted. OCSFS automatically tries the mirror
superblock at offset 4 KB. If both fail, run offline fsck:

```bash
python3 /opt/ocsfs/tools/ocsfs-fsck --repair /dev/mapper/mpath0
```

### Node fails to join the cluster (heartbeat timeout)

```bash
# Verify the multipath device is accessible for read/write
dd if=/dev/mapper/mpath0 of=/dev/null bs=4096 count=100

# Check if the storage path is saturated
iostat -x 1 5 dm-3

# Temporarily increase the timeout (requires remount)
mount -t ocsfs -o heartbeat_timeout=30000 /dev/mapper/mpath0 /mnt/ocsfs
```

### Lock timeout on an operation

```bash
# Identify who holds the lock
ocsfs-tool locks /mnt/ocsfs

# If the holder node is DEAD but not recovered:
ocsfs-tool recover /mnt/ocsfs --node <slot>
```

### SCSI PR not supported (loopback, basic iSCSI)

OCSFS detects automatically when the device does not support PR and operates
in **degraded single-node mode**. The kernel log shows:

```
ocsfs: device does not support SCSI PR; cluster safety depends on exclusive SAN zoning
```

In this mode, mounting on multiple nodes simultaneously is **unsafe**.
Use only FC SAN or LIO/TrueNAS SCALE iSCSI for multi-node deployments.

---

## 9. Known Limitations

| Limitation | Impact | Notes |
|---|---|---|
| Metadata-op throughput under cross-node contention | A *hot, contended* lock (e.g. one shared directory hammered by 3 nodes) does a SCSI CAW per hand-off and can hit the 30 s acquire timeout | **Data path is unaffected** — random VM-disk I/O on a single active node runs at near-raw speed (lock held lazily, no per-op CAW). Reducing per-op CAW on contended metadata locks is the open scaling item |
| No xfstests coverage yet | Unknown edge cases in the VFS layer | Requires a 2-node testbed; KVM + LIO is sufficient |
| Encryption I/O restrictions | fscrypt per-directory encryption **is** implemented (format with `-K`, mount with `cluster_secret=`) | No readahead and no O_DIRECT on encrypted files; reflink/snapshot/symlink **inside** encrypted dirs return `-EOPNOTSUPP` |
| Quota (stub) | dquot *hooks* are wired (data-path and reflink block charges, `dq_op`/`get_dquots`) but quota *enforcement* is not enableable yet | No on-disk quota inodes and no `quotaon` path, so limits cannot be set; metadata blocks (dir/extent-btree/xattr) are also not charged. CoW correctly does not double-charge (it swaps physical blocks, the logical count is unchanged) |
| Snapshot for large files | Supported on V2 volumes (requires `INCOMPAT_RC_BTREE_PER_AG`; format with `mkfs.ocsfs` or upgrade with `ocsfs-tool tune --upgrade`) | Returns `-EOPNOTSUPP` on V1 volumes only |
| Shared mmap unsupported in cluster mode | `MAP_SHARED\|PROT_WRITE` returns `-EOPNOTSUPP` in cluster mode | Private and read-only mappings work; single-node works |
| Single grow per volume | `ocsfs-grow` adds space once (online or offline) | Re-growing an already-grown volume is rejected; rescan the LUN on every node first |
| Sequential multi-node recovery | Multiple dead nodes are recovered one at a time | Pending failures are tracked in a bitmask (`s_recovery_pending`) and drained in sequence — none are dropped; concurrent recovery is just not parallelised |
| No out-of-band STONITH | SCSI PR fencing works; hardware PDU/iDRAC not wired | Proxmox API can serve as soft STONITH in lab environments |

> **Zombie self-fence (gen-change self-recovery).** If a node's heartbeat is
> merely *slow* and a peer recovers it while it is still alive, the node detects
> this on its next heartbeat check (its own slot reads DEAD or its mount
> generation changed), invalidates its cached locks and forces itself
> **read-only** — writes then fail with `EROFS`. Recover by unmounting and
> remounting that node (it rejoins with a fresh generation). Watch for
> `ocsfs: ═══ ZOMBIE FENCE ═══` in `dmesg`.

> **Alpha status:** OCSFS is under active development. Do not deploy with
> critical data without a tested backup plan and thorough evaluation in your
> own environment. Single-node I/O, **2- and 3-node cross-node coherence**, and
> **real node-crash recovery with SCSI-PR fencing** are validated on a real
> iSCSI testbed (TrueNAS SCALE); xfstests and long-haul soak are still open.
