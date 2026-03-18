# OCSFS — Open Cluster Shared FileSystem

**A VMFS-class open-source cluster filesystem for Linux**

OCSFS is a purpose-built, cluster-aware filesystem designed for shared block
storage (Fibre Channel SAN) in virtualized environments. It targets Proxmox VE
as its primary integration platform, providing a true open-source alternative
to VMware's VMFS.

## Key Differentiators

- **On-disk distributed locking** — No DLM daemon, no network dependency.
  All lock state lives on the shared LUN using SCSI Compare-And-Write.
- **Storage-path heartbeat** — Node liveness is validated via I/O to the
  shared device, not through the management network.
- **SCSI-3 Persistent Reservations** — Hardware-level fencing prevents
  split-brain without external stonith devices.
- **Extent-based allocation** — Optimized for large VM disk images with
  minimal metadata contention via per-AG allocation.
- **Thin provisioning** — Lazy allocation with UNMAP/DISCARD support.
- **Proxmox VE integration** — First-class storage plugin supporting all
  content types (images, ISO, templates, backups).

## Current Status: Phase 4 — Advanced Features + Proxmox VE (complete)

### Phase 0 — Prototype (complete)

| Component | Status | Description |
|-----------|--------|-------------|
| `include/ocsfs.h` | Complete | On-disk format specification (all structures) |
| `include/ocsfs_btree.h` | Complete | B+ tree header |
| `src/crc32c.c` | Complete | CRC32C checksum (Castagnoli polynomial) |
| `src/bitmap.c` | Complete | Block bitmap allocator with extent search |
| `src/extent.c` | Complete | Extent map (insert, lookup, merge, punch hole) |
| `src/lock.c` | Complete | On-disk lock manager (CAS protocol) |
| `src/heartbeat.c` | Complete | Heartbeat writer/reader with failure detection |
| `src/btree.c` | Complete | B+ tree (insert, search, delete, range scan) |
| `src/inode.c` | Complete | Inode allocator (userspace prototype) |
| `src/journal.c` | Complete | Journal / WAL (userspace prototype) |
| `src/dir.c` | Complete | Directory operations (userspace prototype) |
| `src/fuse_main.c` | Complete | FUSE3 filesystem (requires libfuse3-dev) |
| `tools/mkfs_ocsfs.c` | Complete | Volume formatter |
| `tools/ocsfs_tool.c` | Complete | Admin CLI (info, nodes, locks, df, check) |
| `tests/test_ocsfs.c` | 36/36 pass | Comprehensive test suite (1770 assertions) |

### Phase 1 — Kernel Module (complete)

| Component | Status | Description |
|-----------|--------|-------------|
| `kmod/ocsfs.h` | Complete | Internal kernel header (on-disk + in-memory structs) |
| `kmod/super.c` | Complete | Module init/exit, mount/unmount, fill_super, statfs |
| `kmod/inode.c` | Complete | Inode read/write, alloc/free, setattr/getattr |
| `kmod/dir.c` | Complete | Directory ops (lookup, create, mkdir, rmdir, rename, readdir) |
| `kmod/file.c` | Complete | File ops + address_space ops (buffer_head I/O) |
| `kmod/extent.c` | Complete | Inline extent manager (lookup, insert, truncate, merge) |
| `kmod/journal.c` | Complete | WAL journal with crash recovery (replay on mount) |
| `kmod/bitmap.c` | Complete | Block bitmap + inode number allocator per-AG |

**Milestone:** Single-node read/write with crash recovery.

### Phase 2 — Multi-Node Clustering (complete)

| Component | Status | Description |
|-----------|--------|-------------|
| `kmod/scsi_pr.c` | Complete | SCSI-3 Persistent Reservations (register, preempt, fencing) |
| `kmod/lock.c` | Complete | On-disk distributed lock manager (CAS-based acquire/release) |
| `kmod/heartbeat.c` | Complete | Storage-path heartbeat writer + failure detection kthread |
| `kmod/node.c` | Complete | Node slot table management (claim, release, state machine) |
| `kmod/recovery.c` | Complete | 5-phase crash recovery (elect, fence, replay, locks, cleanup) |

**Milestone:** 2-node concurrent mount and I/O.

#### Clustering Architecture

- **On-disk locking:** All lock state in the 1 MB Lock Table on the shared LUN.
  No external lock manager, no network dependency. Uses CAS-style versioned entries.
- **Heartbeat:** Background kthread writes timestamp to shared LUN every 5s.
  Detects failures after 15s (3 missed intervals). Tests actual I/O path, not network.
- **SCSI-3 PR fencing:** Hardware-level fencing via PREEMPT AND ABORT.
  SAN fabric rejects I/O from fenced node's HBA — eliminates zombie nodes.
- **Recovery protocol:** Leader election (lowest slot), PR fencing, journal replay,
  lock table scan, slot cleanup. Fully automated, no manual intervention.
- **Graceful fallback:** Non-SCSI devices (loopback, files) skip PR commands
  and operate in single-node mode.

### Phase 3 — Performance Optimization (complete)

| Component | Status | Description |
|-----------|--------|-------------|
| `kmod/iomap.c` | Complete | iomap-based I/O: direct I/O (O_DIRECT), buffered I/O, readahead |
| `kmod/alloc.c` | Complete | Smart allocator: goal-oriented, preallocation, AG affinity |
| `kmod/thin.c` | Complete | Thin provisioning: fallocate, punch hole, zero range, DISCARD |
| `kmod/file.c` | Updated | iomap read/write iter, fallocate integration, O_DIRECT support |
| `kmod/extent.c` | Updated | UNWRITTEN extent conversion (split on partial write) |
| `kmod/ocsfs.h` | Updated | Phase 3 function declarations (iomap, alloc, thin) |

**Milestone:** Direct I/O, thin provisioning, and preallocation for VM workloads.

#### Performance Architecture

- **iomap I/O path:** Modern Linux I/O framework (used by XFS, ext4, btrfs).
  Maps file logical offsets to physical device offsets; VFS handles I/O submission.
  Replaces buffer_head-based I/O for data files.
- **Direct I/O (O_DIRECT):** Zero-copy between userspace and block device via
  `iomap_dio_rw()`. Critical for VM disk image I/O (QEMU raw format).
- **Extent preallocation:** Speculative multi-block allocation reduces fragmentation.
  8-256 blocks per allocation, scaled by file size. Goal-oriented placement
  keeps files physically contiguous.
- **Thin provisioning:** UNWRITTEN extents for `fallocate()` preallocation.
  `FALLOC_FL_PUNCH_HOLE` returns blocks to pool. `DISCARD` passthrough to
  SAN/SSD for physical space reclaim.
- **UNWRITTEN conversion:** 4-way split on partial write (head/middle/tail/full).
  Reads from UNWRITTEN extents return zeroes without I/O.

### Phase 4 — Advanced Features + Proxmox VE Integration (complete)

| Component | Status | Description |
|-----------|--------|-------------|
| `proxmox/OCSFSPlugin.pm` | Complete | Proxmox VE storage plugin (all content types) |
| `proxmox/mount.ocsfs` | Complete | Mount helper for Proxmox (modprobe + validation) |
| `proxmox/install.sh` | Complete | One-step PVE plugin installer (DKMS + Perl + tools) |
| `kmod/snapshot.c` | Complete | CoW file-level snapshots (create, delete, CoW on write) |
| `kmod/refcount.c` | Complete | Extent reference counting (per-AG hash table) |
| `kmod/compress.c` | Complete | Inline compression (LZ4 fast, ZSTD high-ratio) |
| `tools/ocsfs_defrag.c` | Complete | Online defragmentation daemon (FIEMAP, bandwidth-limited) |
| `kmod/ocsfs.h` | Updated | Phase 4 declarations (snapshot, refcount, compression) |

**Milestone:** VMFS-6 feature parity + Proxmox VE integration.

#### Proxmox VE Integration

- **Storage plugin:** Full PVE::Storage::Plugin implementation. Supports all
  content types: images, iso, vztmpl, backup, rootdir, snippets.
- **Configuration:** `storage.cfg` format with thin provisioning, compression,
  and extent size options. Shared storage for multi-node PVE clusters.
- **Thin provisioning:** New VM disks are thin-provisioned by default. Guest
  TRIM/DISCARD reclaims space via `fallocate(PUNCH_HOLE)` → DISCARD passthrough.
- **Mount helper:** `mount.ocsfs` validates superblock, loads kernel module,
  and mounts. Called automatically by `mount -t ocsfs`.

#### CoW Snapshots

- **File-level snapshots:** Superior to VMFS's VMDK-only snapshots.
  O(n) creation (n = number of extents, typically <16 for inline).
- **Copy-on-Write:** Shared extents (refcount > 1) trigger CoW on write.
  New blocks allocated, old data copied, extent map updated atomically.
- **Refcount table:** Per-AG hash table (16 blocks). Blocks with refcount=1
  have no entry (space-efficient). Supports 100+ snapshot layers.

#### Inline Compression

- **LZ4:** Default algorithm, optimized for speed. Uses kernel LZ4 library.
- **ZSTD:** Higher compression ratio for archival content (ISOs, backups).
- **Per-file control:** Compression enabled/disabled via inode flags.
- **O_DIRECT bypass:** VM data path (O_DIRECT) completely bypasses compression.
  Only buffered I/O is compressed — zero overhead for VM workloads.

#### Online Defragmentation

- **FIEMAP-based analysis:** Detects fragmented files via FIEMAP ioctl.
- **Non-disruptive:** Bandwidth-limited background operation (configurable MB/s).
- **Single-instance:** Lock file coordination — one defrag daemon per mount.
- **Pause/resume:** SIGUSR1/SIGUSR2 signals for live control.

## Building

```bash
# Dependencies (Debian/Ubuntu/Proxmox)
apt install build-essential uuid-dev

# Build userspace tools + tests
make all

# Run tests
make test

# Build FUSE prototype (requires libfuse3-dev)
make fuse

# Build kernel module (requires linux-headers-$(uname -r))
make kmod
# or directly:
cd kmod && make

# DKMS install
sudo dkms add kmod/
sudo dkms build ocsfs/0.1.0
sudo dkms install ocsfs/0.1.0

# Demo: create a 1 GiB test image and inspect it
make demo

# Install Proxmox VE storage plugin (on PVE host)
sudo proxmox/install.sh
```

## Quick Start (on a test image)

```bash
# Create a test image
dd if=/dev/zero of=/tmp/test.img bs=1M count=2048

# Format
./mkfs.ocsfs -L my-datastore -N 16 -f -v /tmp/test.img

# Inspect
./ocsfs-tool info /tmp/test.img
./ocsfs-tool df /tmp/test.img
./ocsfs-tool check /tmp/test.img
./ocsfs-tool nodes /tmp/test.img
./ocsfs-tool locks /tmp/test.img
```

### Kernel Module Usage (Phase 1)

```bash
# Load module
sudo insmod kmod/ocsfs.ko

# Mount (requires a formatted block device or loopback)
sudo losetup /dev/loop0 /tmp/test.img
sudo mount -t ocsfs /dev/loop0 /mnt/ocsfs

# Use as a normal filesystem
ls /mnt/ocsfs
echo "hello" > /mnt/ocsfs/test.txt

# Unmount
sudo umount /mnt/ocsfs
sudo losetup -d /dev/loop0
sudo rmmod ocsfs
```

### Proxmox VE Usage (Phase 4)

```bash
# Add OCSFS storage via CLI
pvesm add ocsfs fc-shared \
  --path /mnt/pve/fc-shared \
  --device /dev/mapper/mpath-3600508b... \
  --content images,iso,vztmpl,backup,rootdir,snippets \
  --maxnodes 16 --thin 1 --shared 1

# Or edit /etc/pve/storage.cfg directly:
# ocsfs: fc-shared
#   path /mnt/pve/fc-shared
#   device /dev/mapper/mpath-3600508b...
#   content images,iso,vztmpl,backup,rootdir,snippets
#   maxnodes 16
#   thin 1
#   shared 1

# Online defragmentation
./ocsfs-defrag /mnt/pve/fc-shared -v -b 50 -t 4
./ocsfs-defrag /mnt/pve/fc-shared -n  # dry run (report only)
```

## Architecture

See `OCSFS_Technical_Architecture_v0.1.md` for the full technical
specification covering:

- VMFS deep analysis and feature mapping
- On-disk layout (superblock, AGs, inodes, extents)
- Distributed locking protocol (SCSI CAW)
- Journaling and crash recovery
- I/O path optimization for VM workloads
- Proxmox VE integration design
- Development roadmap (36-month plan)

## Project Structure

```
ocsfs/
├── include/
│   ├── ocsfs.h              # On-disk format (shared kernel/userspace)
│   └── ocsfs_btree.h        # B+ tree interface
├── src/
│   ├── crc32c.c             # CRC32C implementation
│   ├── bitmap.c             # Block bitmap allocator (userspace)
│   ├── extent.c             # Extent manager (userspace)
│   ├── lock.c               # On-disk distributed lock manager
│   ├── heartbeat.c          # Heartbeat subsystem
│   ├── btree.c              # B+ tree implementation
│   ├── inode.c              # Inode allocator (userspace)
│   ├── journal.c            # Journal / WAL (userspace)
│   ├── dir.c                # Directory operations (userspace)
│   └── fuse_main.c          # FUSE3 filesystem prototype
├── kmod/
│   ├── ocsfs.h              # Internal kernel header
│   ├── super.c              # Superblock, mount, module init, cluster init
│   ├── inode.c              # VFS inode operations
│   ├── dir.c                # VFS directory operations
│   ├── file.c               # VFS file + address_space operations (iomap)
│   ├── extent.c             # Inline extent manager + UNWRITTEN conversion
│   ├── journal.c            # WAL journaling + crash recovery
│   ├── bitmap.c             # Block/inode bitmap allocator
│   ├── iomap.c              # iomap-based I/O (direct I/O, buffered, readahead)
│   ├── alloc.c              # Smart allocator (prealloc, goal-oriented, AG affinity)
│   ├── thin.c               # Thin provisioning (fallocate, punch hole, DISCARD)
│   ├── scsi_pr.c            # SCSI-3 Persistent Reservations
│   ├── lock.c               # On-disk distributed lock manager
│   ├── heartbeat.c          # Storage-path heartbeat kthread
│   ├── node.c               # Node slot table management
│   ├── recovery.c           # 5-phase crash recovery protocol
│   ├── snapshot.c           # CoW file-level snapshots
│   ├── refcount.c           # Extent reference counting for CoW
│   ├── compress.c           # Inline compression (LZ4/ZSTD)
│   ├── Kbuild               # In-tree build rules
│   ├── Makefile             # Out-of-tree build
│   └── dkms.conf            # DKMS configuration
├── proxmox/
│   ├── OCSFSPlugin.pm       # Proxmox VE storage plugin (Perl)
│   ├── mount.ocsfs          # Mount helper
│   └── install.sh           # PVE plugin installer
├── tools/
│   ├── mkfs_ocsfs.c         # mkfs.ocsfs formatter
│   ├── ocsfs_tool.c         # ocsfs-tool admin CLI
│   └── ocsfs_defrag.c       # Online defragmentation daemon
├── tests/
│   └── test_ocsfs.c         # Test suite (36 tests, 1770 assertions)
├── Makefile
├── .gitignore
└── README.md
```

## Development Roadmap

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 0 | FUSE prototype — validate on-disk format | Complete |
| Phase 1 | Kernel module — single-node read/write + crash recovery | Complete |
| Phase 2 | Multi-node — distributed locking, heartbeat, recovery | Complete |
| Phase 3 | Performance — direct I/O, iomap, readahead, prealloc | Complete |
| Phase 4 | Advanced features + Proxmox VE integration | Complete |
| Phase 5 | Mainline kernel submission | Planned |

## License

GPL-2.0-only (compatible with Linux kernel inclusion)

## Contributing

This project needs:
- Kernel filesystem developers (C, VFS, block layer)
- SCSI/FC storage experts (PR, CAW, multipath)
- Proxmox integration developers (Perl, PVE API)
- QA engineers (fio, fault injection, multi-node testing)
- Technical writers

If you're interested in contributing, especially if you're affected by the
VMware/Broadcom license changes, please reach out.
