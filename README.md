# OCSFS — Open Cluster Shared FileSystem

> **This project is entirely AI-generated.**
> Every line of code, documentation, and tooling was written by
> [Claude](https://claude.ai) (Anthropic) through an iterative conversation
> with a human operator. No human has written or reviewed the source code
> directly. Treat it accordingly.

An open-source cluster filesystem for Linux, designed for shared block storage
(iSCSI / Fibre Channel SAN) in virtualized environments. Primary target:
Proxmox VE as an open alternative to VMware VMFS.

> **Status: Alpha / Research.**
> The kernel module builds and runs. Single-node I/O is stable.
> Multi-node clustering has been extensively hardened but has not yet been
> validated against a real testbed. Do not use with data that matters.

---

## Production Readiness (as of 2026-05-27)

| Scenario | Score |
|---|---|
| Single-node read/write | ~88% |
| Multi-node — no crashes | ~85% |
| Multi-node — crash + recovery | ~70% |
| VMFS feature parity | ~58% |

The main gap keeping the cluster score below 90% is the absence of
real-hardware integration testing (xfstests on a multi-node testbed).
SCSI CAW is now implemented via BSG-direct path — no kprobe required.

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                 Shared block device                  │
│         (iSCSI LUN / FC LUN / loopback)              │
│                                                      │
│  SB │ AGs │ Inode table │ Lock table │ Journal │ HB  │
└─────┬───────────────────────────────────────────────┘
      │  SCSI-3 PR (fencing)  │  CAS (on-disk locking)
      │
┌─────▼──────┐  ┌────────────┐  ┌────────────┐
│  node-1    │  │  node-2    │  │  node-3    │
│  ocsfs.ko  │  │  ocsfs.ko  │  │  ocsfs.ko  │
└────────────┘  └────────────┘  └────────────┘
```

**Key design choices:**

- **No DLM daemon.** All lock state lives on the shared LUN as versioned
  on-disk entries; acquire/release use Compare-And-Write (CAS) via SCSI.
- **Storage-path heartbeat.** Node liveness is proven by writes to the shared
  device — not through the management network — so a network partition does
  not cause false positives.
- **SCSI-3 Persistent Reservations.** Hardware fencing: the SAN fabric rejects
  I/O from a fenced node's HBA before recovery begins.
- **WAL journal with redo.** AFTER-images in the commit record; replay on
  mount reconstructs any committed-but-not-flushed transaction.

---

## What is implemented

### Kernel module (`kmod/`)

| File | Description |
|---|---|
| `super.c` | Mount/unmount, fill_super, statfs, module init/exit |
| `inode.c` | Inode read/write/alloc/free, setattr, CRC32c verification |
| `dir.c` | lookup, create, mkdir, rmdir, readdir, hard link, mknod |
| `dir_btree.c` | B+ tree-backed directory index (O(log N) lookup/delete) |
| `dir_rename.c` | rename, RENAME_NOREPLACE, RENAME_EXCHANGE |
| `file.c` | read/write iter, fsync, FIEMAP, reflink (FICLONE), ioctl |
| `iomap.c` | iomap I/O path: O_DIRECT, buffered, readahead, UNWRITTEN conversion |
| `extent.c` | Inline extent manager (up to 16 extents): lookup, insert, truncate, merge |
| `extent_btree.c` | B+ tree overflow for files with >16 extents |
| `alloc.c` | Block allocator: goal-oriented, prealloc, AG affinity |
| `bitmap.c` | Per-AG block/inode bitmap; two-phase lockless allocation |
| `thin.c` | fallocate, PUNCH_HOLE, ZERO_RANGE, DISCARD |
| `journal.c` | WAL with ordered checkpoint; BEFORE/AFTER images; txn abort rollback |
| `journal_replay.c` | Forward-scan replay; CRC-gated COMMIT detection; redo on mount |
| `lock.c` | On-disk DLM: EX/SH acquire/release, downgrade, epoch invalidation |
| `lock_io.c` | Forced disk read/write for lock table (bypasses page cache) |
| `scsi_pr.c` | SCSI-3 PR: register, preempt-and-abort, SHA-256 key; CAW via BSG-direct + kprobe fallback |
| `heartbeat.c` | Storage-path heartbeat kthread, CRC-validated entries, wakeup on stop |
| `node.c` | Node slot table: claim, release, stable UUID via SHA-256(hostname) |
| `recovery.c` | 5-phase recovery: elect leader, fence via PR, replay journal, cleanup |
| `snapshot.c` | CoW file snapshots: create, delete, CoW-on-write trigger |
| `refcount.c` | Per-AG extent reference counting for CoW/dedup |
| `compress.c` | Inline LZ4/ZSTD compression (read path); decompression on demand |
| `compress_file.c` | Compress-on-fsync for buffered files |
| `dedup.c` | Content-based deduplication via `OCSFS_IOC_DEDUP` ioctl (btree-backed inodes) |
| `xattr.c` | Extended attributes with DLM SH protection in cluster mode |
| `acl.c` | POSIX ACL (getfacl/setfacl) |
| `btree.c` | Generic B+ tree (search, insert, delete, range scan) |
| `btree_mod.c` | B+ tree structural modifications (split, merge, rebalance) |
| `test_lock.c` | KUnit tests: B+ tree search, dir btree threshold |
| `test_cas.c` | KUnit tests: CAS lock protocol |

### Userspace tools (`tools/`, `src/`)

| Tool | Description |
|---|---|
| `tools/mkfs_ocsfs.c` | Volume formatter |
| `tools/ocsfs_tool.c` | Admin CLI: info, nodes, locks, df, check |
| `tools/ocsfs_defrag.c` | Online defragmentation daemon (FIEMAP-based, bandwidth-limited) |
| `tools/ocsfs-fsck` | Offline fsck (Python): 9 checks + repair mode |
| `src/` | FUSE3 prototype — validates on-disk format; not for production use |

### Platform integration

| Component | Description |
|---|---|
| `proxmox/OCSFSPlugin.pm` | Proxmox VE storage plugin (all content types) |
| `proxmox/mount.ocsfs` | Mount helper called by `mount -t ocsfs` |
| `proxmox/install.sh` | One-step installer (DKMS + plugin + tools) |
| `conf/` | systemd template units + udev rules for SAN auto-detection |
| `debian/` | Debian packaging: ocsfs-tools, ocsfs-dkms, ocsfs-proxmox |
| `man/` | Man pages for all CLI tools |
| `docs/kernel-patches/` | Patch to export `scsi_device_from_queue` (upstream path for CAW) |

---

## What is missing / known limitations

### Blocking for cluster production use

| Gap | Notes |
|---|---|
| **Integration test suite** | No xfstests run yet. Needs a 2-node testbed (KVM + LIO iSCSI is sufficient). |
| **Node table TOCTOU** | Two nodes can claim the same slot without hardware atomicity. Mitigated by SCSI CAW (now implemented via BSG-direct). |

### Architectural limitations

| Gap | Notes |
|---|---|
| **Encryption** | Zero code. Would require fscrypt integration. |
| **Quota** | Implemented: VFS `dquot` inode quota (commit `8bc4c38`) and block quota (commit `58933a7`). CoW, snapshot, and directory/metadata blocks are not charged. |
| **Snapshot for large files** | Resolved for V2 filesystems: `snapshot_copy_btree_extents` + per-AG refcount B+ tree (ARCH-5, commit `eb88eeb`). Returns `-EOPNOTSUPP` only on V1 volumes without `INCOMPAT_RC_BTREE_PER_AG`. |
| **Compression write path** | Compression is applied on fsync for buffered files only. No inline compression during O_DIRECT writes. |
| **Shared mmap** | `MAP_SHARED|PROT_WRITE` returns `-EOPNOTSUPP` in cluster mode. Read-only and private (COW) mappings work. |
| **POSIX distributed file locking** | `fcntl` locks are local only. |
| **VAAI XCOPY/WRITE_SAME** | Not implemented. |
| **STONITH** | Fencing via SCSI PR works; out-of-band STONITH (PDU, iDRAC) not wired. For Proxmox labs, the Proxmox API can serve as soft STONITH. |

---

## Building

```bash
# Userspace tools and FUSE prototype
make all

# Tests (userspace only)
make test

# Kernel module (requires linux-headers for the running kernel)
cd kmod && make -j$(nproc)

# DKMS
sudo dkms add kmod/
sudo dkms build ocsfs/0.1.0
sudo dkms install ocsfs/0.1.0

# Debian packages
dpkg-buildpackage -us -uc -b
```

---

## Quick start (single node, loopback)

```bash
# Format a test image
dd if=/dev/zero of=/tmp/test.img bs=1M count=2048
./mkfs.ocsfs -L my-datastore -v /tmp/test.img

# Inspect
./ocsfs-tool info /tmp/test.img

# Mount
sudo modprobe loop
sudo losetup /dev/loop0 /tmp/test.img
sudo insmod kmod/ocsfs.ko
sudo mount -t ocsfs /dev/loop0 /mnt/ocsfs

# Use
echo "hello" | sudo tee /mnt/ocsfs/test.txt
sudo umount /mnt/ocsfs
sudo losetup -d /dev/loop0
sudo rmmod ocsfs
```

---

## Testbed setup (multi-node, KVM + LIO iSCSI)

See [`docs/developer-guide.md`](docs/developer-guide.md) for the full
step-by-step guide. The minimum viable testbed:

- 1 host with KVM + 3 VMs (4 GB RAM each is enough)
- `targetcli-fb` on the host, exporting one LIO iSCSI LUN (~50 GB)
- `open-iscsi` inside each VM; each VM connects directly to the LUN
- SCSI-3 PR is supported natively by LIO (`emulate_pr=1`)

TrueNAS SCALE (LIO backend) works as an external iSCSI target and is the
recommended option when a NAS is available.

---

## Proxmox VE usage

```bash
# Install plugin and module on each PVE node
sudo proxmox/install.sh

# Add storage
pvesm add ocsfs fc-shared \
  --device /dev/disk/by-path/<your-lun> \
  --content images,iso,vztmpl,backup \
  --maxnodes 16 --shared 1

# Run fsck offline (unmounted)
sudo python3 tools/ocsfs-fsck /dev/sdb

# Run fsck with repair
sudo python3 tools/ocsfs-fsck --repair /dev/sdb
```

---

## Project structure

```
ocsfs/
├── kmod/           Kernel module (C, GPL-2.0)
├── src/            FUSE3 prototype (format validation only)
├── tools/          mkfs, admin CLI, defrag daemon, fsck
├── proxmox/        Proxmox VE storage plugin (Perl)
├── tests/          KUnit tests + xfstests config
├── conf/           systemd units + udev rules
├── man/            Man pages
├── docs/           Admin guide, developer guide, kernel patch
├── debian/         Debian packaging
├── include/        Shared headers (on-disk format)
└── LICENSE         GPL-2.0-only
```

---

## License

GPL-2.0-only — compatible with Linux kernel inclusion.

## Contributing

The project is looking for:

- Kernel filesystem developers (VFS, block layer, crash safety)
- SCSI/FC storage engineers (PR, CAW, multipath)
- Proxmox / PVE integration developers
- QA engineers who can run xfstests on a real cluster

The most impactful open task right now is running xfstests quick+auto on a
2-node KVM testbed with LIO iSCSI. SCSI CAW is implemented (`ocsfs_bsg_execute_cdb()`
uses the BSG-direct path; no kprobe required).
