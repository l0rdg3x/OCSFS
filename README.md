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

## Current Status: Phase 2 — Multi-Node Clustering

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

### Phase 2 — Multi-Node Clustering (in progress)

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
│   ├── file.c               # VFS file + address_space operations
│   ├── extent.c             # Inline extent manager
│   ├── journal.c            # WAL journaling + crash recovery
│   ├── bitmap.c             # Block/inode bitmap allocator
│   ├── scsi_pr.c            # SCSI-3 Persistent Reservations
│   ├── lock.c               # On-disk distributed lock manager
│   ├── heartbeat.c          # Storage-path heartbeat kthread
│   ├── node.c               # Node slot table management
│   ├── recovery.c           # 5-phase crash recovery protocol
│   ├── Kbuild               # In-tree build rules
│   ├── Makefile             # Out-of-tree build
│   └── dkms.conf            # DKMS configuration
├── tools/
│   ├── mkfs_ocsfs.c         # mkfs.ocsfs formatter
│   └── ocsfs_tool.c         # ocsfs-tool admin CLI
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
| Phase 2 | Multi-node — distributed locking, heartbeat, recovery | **In progress** |
| Phase 3 | Performance — direct I/O, iomap, readahead, prealloc | Planned |
| Phase 4 | Proxmox VE integration — storage plugin | Planned |
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
