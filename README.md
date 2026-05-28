# OCSFS — Open Cluster Shared FileSystem

> **This project is entirely AI-generated.**
> Every line of code, documentation, and tooling was written by
> [Claude](https://claude.ai) (Anthropic) through an iterative conversation
> with a human operator. No human has written or reviewed the source code
> directly. Treat it accordingly.

An open-source cluster filesystem for Linux targeting shared block storage
(iSCSI / Fibre Channel SAN) in virtualized environments. Primary goal:
a Proxmox VE-native alternative to VMware VMFS.

> **Status: Alpha / Research.**
> The kernel module builds and runs. Single-node I/O is stable.
> Multi-node clustering has been extensively hardened but has not yet been
> validated against a real testbed. Do not use with data that matters.

---

## Production Readiness

Eight rounds of correctness, security, and architectural fixes from a
structured Opus v3 review have been applied (Sprints A–H). See
[`docs/developer-guide.md`](docs/developer-guide.md) for the full changelog.

| Scenario | Estimated score |
|---|---|
| Single-node read/write | ~92% |
| Multi-node — stable workload | ~93% |
| Multi-node — crash + recovery | ~85% |
| Multi-node — with encryption | ~85% |
| VMFS feature parity | ~72% |

The remaining gap is real-hardware integration testing (xfstests on a
multi-node testbed with SCSI-3 PR).

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
| `extent_btree.c` | B+ tree overflow for files with >16 extents; 4-bit flags encoding (v2) |
| `alloc.c` | Block allocator: goal-oriented, prealloc, AG affinity |
| `bitmap.c` | Per-AG block/inode bitmap; two-phase lockless allocation |
| `thin.c` | fallocate, PUNCH_HOLE, ZERO_RANGE, DISCARD |
| `journal.c` | WAL with ordered checkpoint; BEFORE/AFTER images; txn abort rollback |
| `journal_replay.c` | Forward-scan replay; 62-bit CRC-gated COMMIT detection; redo on mount |
| `lock.c` | On-disk DLM: EX/SH acquire/release, downgrade, epoch-based cache |
| `lock_io.c` | Forced disk read/write for lock table (bypasses page cache) |
| `scsi_pr.c` | SCSI-3 PR: register, preempt-and-abort; CAW via BSG-direct + kprobe fallback |
| `heartbeat.c` | Storage-path heartbeat kthread; CRC-validated entries; O(1) summary block |
| `node.c` | Node slot table: claim, release, HMAC-SHA256 auth, replay-attack protection |
| `recovery.c` | 5-phase recovery: leader election, PR fencing, journal replay, lock cleanup |
| `snapshot.c` | CoW file snapshots: create, delete, CoW-on-write trigger |
| `refcount.c` | Per-AG extent reference counting for CoW/dedup (B+ tree, V2 volumes) |
| `compress.c` | Inline LZ4/ZSTD compression (read path); decompression on demand |
| `compress_file.c` | Compress-on-fsync for buffered files; journal-before-free |
| `dedup.c` | Content-based deduplication via `OCSFS_IOC_DEDUP` ioctl |
| `xattr.c` | Extended attributes with DLM SH protection in cluster mode |
| `acl.c` | POSIX ACL (getfacl/setfacl) |
| `btree.c` | Generic B+ tree (search, insert, delete, range scan) |
| `btree_mod.c` | B+ tree structural modifications (split, merge, rebalance) |
| `debugfs.c` | Debugfs: `/sys/kernel/debug/ocsfs/<dev>/lock_table`, `journal_stats` |
| `vaai.c` | VAAI offload: WRITE SAME (0x93), UNMAP (0x42), EXTENDED COPY (0x83) via BSG |
| `flock.c` | Distributed POSIX file locking: fcntl(F_SETLK) → DLM SH/EX |
| `crypto.c` | fscrypt integration: per-directory encryption, bounce-page I/O, cluster safety |
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

---

## Known limitations

| Area | Status |
|---|---|
| **Integration test suite** | No xfstests run yet — top priority. Needs a 2-node testbed with a SCSI-3 PR-capable LUN (KVM + LIO iSCSI is sufficient). |
| **Encryption — node-local keys** | fscrypt keys must be added independently on every cluster node (`FS_IOC_ADD_ENCRYPTION_KEY`). A node that hasn't added the key writes plaintext. No cluster-wide propagation protocol yet. |
| **Encryption — I/O restrictions** | No readahead, no O_DIRECT. Reflink, snapshot, and symlinks inside encrypted directories return `-EOPNOTSUPP`. |
| **Compression write path** | Compression is applied on fsync for buffered files only. O_DIRECT writes are never compressed. |
| **Shared mmap** | `MAP_SHARED|PROT_WRITE` returns `-EOPNOTSUPP` in cluster mode. Read-only and private (COW) mappings work. |
| **STONITH** | Fencing uses SCSI PR. Out-of-band STONITH (PDU, iDRAC) is not wired — the Proxmox API can serve as soft STONITH in lab setups. |
| **Journal COMMIT HMAC** | HMAC authentication covers node slot tokens (implemented) but not journal COMMIT records — requires an on-disk format change and a new incompat feature bit. |
| **Single recovery target** | Only one dead node is recovered at a time. If two nodes fail simultaneously, the second is queued until the first recovery completes. |

---

## Building

```bash
# Userspace tools and FUSE prototype
make all

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
dd if=/dev/zero of=/tmp/test.img bs=1M count=2048
./mkfs.ocsfs -L my-datastore -v /tmp/test.img
./ocsfs-tool info /tmp/test.img

sudo losetup /dev/loop0 /tmp/test.img
sudo insmod kmod/ocsfs.ko
sudo mount -t ocsfs /dev/loop0 /mnt/ocsfs

echo "hello" | sudo tee /mnt/ocsfs/test.txt
sudo umount /mnt/ocsfs
sudo losetup -d /dev/loop0
sudo rmmod ocsfs
```

---

## Testbed setup (multi-node)

See [`docs/developer-guide.md`](docs/developer-guide.md) for the full
step-by-step guide. The minimum viable testbed:

- 2–3 nodes (VMs or bare metal) connected to a shared iSCSI LUN
- `targetcli-fb` / TrueNAS SCALE as the iSCSI target (`emulate_pr=1` for SCSI-3 PR)
- `open-iscsi` on each node

TrueNAS SCALE (LIO backend) is the recommended target when a NAS is available —
SCSI-3 PR works out of the box.

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

# Offline fsck
sudo python3 tools/ocsfs-fsck /dev/sdb
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
├── docs/           Developer guide, admin guide
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

The most impactful open task is running `xfstests quick+auto` on a 2-node
testbed with LIO iSCSI. SCSI CAW is implemented via BSG-direct
(`ocsfs_bsg_execute_cdb()`) — no kernel patch required.
