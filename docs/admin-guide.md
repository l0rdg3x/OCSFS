# OCSFS — Administrator Guide

**Version:** 0.1 — May 2026
**Platform:** Proxmox VE 8.x, Linux kernel 6.6+ LTS
**Status:** Alpha — do not use with production data

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
> Run this only on a dedicated LUN.

```bash
# Identify the multipath device
multipath -ll

# Format — run on ONE node only
mkfs.ocsfs \
  -L my-datastore \   # volume label (max 64 chars)
  -N 16 \             # max concurrent nodes (default 64, max 256)
  -E 4M \             # extent size (default 1M, range 64K–64M)
  -f \                # force (overwrite existing data)
  -v \                # verbose output
  /dev/mapper/mpath0

# Verify
ocsfs-tool info /dev/mapper/mpath0
```

### Recommended mkfs parameters for Proxmox clusters

| Cluster size | Max nodes (`-N`) | Extent size (`-E`) |
|---|---|---|
| Small (2–4 nodes) | 8 | 1M |
| Medium (5–16 nodes) | 32 | 4M |
| Large (17–64 nodes) | 64 | 8M |

> `max_nodes` is fixed at format time and cannot be changed afterwards.
> Oversize slightly to allow for future growth.

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
| Node slot TOCTOU (mitigated) | Hardware atomicity requires SCSI CAW — now implemented via BSG-direct path | CAW is active; kprobe shim kept as fallback for patched kernels |
| No xfstests coverage yet | Unknown edge cases in VFS layer | Requires a 2-node testbed; KVM + LIO is sufficient |
| No encryption | Data at rest is unencrypted | fscrypt integration not implemented |
| Quota (partial) | inode and block quotas are enforced via VFS dquot | CoW, snapshot creation, and directory/metadata blocks are not charged against block quota |
| Snapshot for large files | Supported on V2 volumes (requires `INCOMPAT_RC_BTREE_PER_AG`; format with `mkfs.ocsfs` or upgrade with `ocsfs-tool tune --upgrade`) | Returns `-EOPNOTSUPP` on V1 volumes only |
| Shared mmap unsupported in cluster mode | `MAP_SHARED|PROT_WRITE` returns EOPNOTSUPP | Private and read-only mappings work |
| Single recovery at a time | If two nodes die simultaneously, the second is not recovered | `s_recovery_target` is a single u16; bitmask queue is the fix |
| No out-of-band STONITH | SCSI PR fencing works; hardware PDU/iDRAC not wired | Proxmox API can serve as soft STONITH in lab environments |

> **Alpha status:** OCSFS is under active development. Do not deploy with
> critical data without a tested backup plan and thorough evaluation in your
> own environment. The cluster protocol has not yet been validated against a
> real multi-node testbed.
