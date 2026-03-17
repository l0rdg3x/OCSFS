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

## Current Status: Phase 0 — Prototype

This is the initial codebase containing:

| Component | Status | Description |
|-----------|--------|-------------|
| `include/ocsfs.h` | ✅ Complete | On-disk format specification (all structures) |
| `src/crc32c.c` | ✅ Complete | CRC32C checksum (Castagnoli polynomial) |
| `src/bitmap.c` | ✅ Complete | Block bitmap allocator with extent search |
| `src/extent.c` | ✅ Complete | Extent map (insert, lookup, merge, punch hole) |
| `src/lock.c` | ✅ Complete | On-disk lock manager (CAS protocol) |
| `src/heartbeat.c` | ✅ Complete | Heartbeat writer/reader with failure detection |
| `tools/mkfs_ocsfs.c` | ✅ Complete | Volume formatter |
| `tools/ocsfs_tool.c` | ✅ Complete | Admin CLI (info, nodes, locks, df, check) |
| `tests/test_ocsfs.c` | ✅ 24/24 pass | Comprehensive test suite |

## Building

```bash
# Dependencies (Debian/Ubuntu/Proxmox)
apt install build-essential uuid-dev

# Build everything
make all

# Run tests
make test

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

## Architecture

See `OCSFS_Technical_Architecture_v0.1.docx` for the full 14-chapter
technical specification covering:

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
│   └── ocsfs.h          # On-disk format (shared kernel/userspace header)
├── src/
│   ├── crc32c.c          # CRC32C implementation
│   ├── bitmap.c          # Block bitmap allocator
│   ├── extent.c          # Extent manager (lookup, insert, merge, remove)
│   ├── lock.c            # On-disk distributed lock manager
│   └── heartbeat.c       # Heartbeat subsystem
├── tools/
│   ├── mkfs_ocsfs.c      # mkfs.ocsfs formatter
│   └── ocsfs_tool.c      # ocsfs-tool admin CLI
├── tests/
│   └── test_ocsfs.c      # Test suite
├── Makefile
└── README.md
```

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
