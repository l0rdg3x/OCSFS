# OCSFS v2 — Plan 1: Bootable Skeleton (build → format → mount → namespace)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A single-node OCSFS v2 volume that can be formatted (`mkfs.ocsfs2`),
checked clean (`fsck.ocsfs2`), mounted on a real iSCSI LUN, support directory and
file *namespace* operations (mkdir / create / lookup / unlink / rmdir / rename /
readdir), persist across remount, and pass `fsck` clean. **No file data path yet**
(write/read of file *content* is Plan 2) — files exist as empty inodes.

**Architecture:** New tree `kmod2/`. XFS-like allocation groups (sound part of v1,
kept). Self-describing checksummed metadata blocks. Single-node only — zero cluster
code in this plan (the cluster coordination regions are *reserved on disk* by mkfs
but not used). On-disk format frozen here becomes the contract for all later plans.

**Tech Stack:** Linux kernel module (C, kernel 7.0.6-2-pve), VFS, buffer_head for
metadata I/O, crc32c. Userspace `mkfs`/`fsck` in C. Salvaged `scsi_pr.c` compiles
but is dormant until the cluster plan.

**Test environment (from probe):**
- Build/test node: `pve01-test` = `ssh root@192.168.1.48` (kernel 7.0.6-2-pve, repo `/root/OCSFS`).
- Dedicated single-node test LUN: a 30 GiB iSCSI LUN. **Resolve it by stable path**,
  not `/dev/sdX` (iSCSI letters are not stable across nodes/reboots):
  `ls -l /dev/disk/by-path/*iscsi*` → pick one 30G LUN, export as `$LUN`.
- **NO loopback** (user constraint). All mount tests on the real LUN.
- Dev loop: edit on workstation → `rsync -a --delete kmod2/ tools2/ root@192.168.1.48:/root/OCSFS/` → build + test on node.
- Cross-check correctness against **ext4** on an equivalent LUN with identical ops.

---

## File structure (created by this plan)

```
kmod2/
  Kbuild                  module build rules
  ocsfs.h                 on-disk + in-memory types, constants, static_asserts (THE contract)
  transport/
    scsi_pr.c             SALVAGED from kmod/scsi_pr.c (compiles, dormant)
  super.c                 module init/exit, fs_type, fill_super, kill_sb, statfs
  inode.c                 ocsfs_iget, flush, new_inode, evict, alloc/free inode number
  dir.c                   dirent add/del/find, lookup, create, mkdir, unlink, rmdir, readdir
  rename.c                ocsfs_rename
  bitmap.c                per-AG block + inode bitmap alloc/free (block alloc used by dir data blocks)
tools2/
  mkfs.c                  mkfs.ocsfs2 — authoritative formatter
  fsck.c                  fsck.ocsfs2 — authoritative checker
  ocsfs_ondisk.h          shared on-disk struct defs for userspace (mirror of kmod2/ocsfs.h on-disk part)
tests/v2/
  test_format.sh          mkfs → fsck-clean gate
  test_namespace.sh       mount → mkdir/touch/ls/rm/rename → remount → fsck gate
  fsx-namespace cross-check helpers
```

Files stay < 500 lines (CLAUDE.md). Split when a file outgrows its single purpose.

---

## On-disk format v2 (frozen by this plan)

Block size 4096. Little-endian. Every metadata block begins with a common 16-byte
header `ocsfs_blk_hdr { __le32 h_magic; __le16 h_type; __le16 h_flags; __le32 h_seq;
__le32 h_crc; }` where `h_crc = crc32c(~0, block_after_h_crc_zeroed)` over the whole
block. (Superblock and inode keep their existing dedicated layout with a trailing
crc field instead, for compactness — see structs.)

Region layout — **mkfs computes all offsets and stores them in the superblock**; the
kernel validates non-overlap at mount. Order on disk:
`SB | SB-mirror | node-table | heartbeat | lease-table | recovery-blk | journal[max_nodes] | AG[0..ag_count)`
Each AG: `ag-header-blk | block-bitmap | inode-table | data-blocks`.

The cluster regions (node/heartbeat/lease/recovery, per-node journals) are written
zeroed-with-valid-headers by mkfs but **not read by this plan's kernel code**.

---

## Phase A — Scaffolding & transport

### Task A1: kmod2 skeleton that builds and loads

**Files:**
- Create: `kmod2/Kbuild`, `kmod2/ocsfs.h` (minimal), `kmod2/super.c` (minimal)
- Create: `Makefile` target or `kmod2/Makefile` wrapper for out-of-tree build

- [ ] **Step 1: Write minimal `kmod2/super.c`** — module that registers nothing yet, just init/exit printing a banner.

```c
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>

static int __init ocsfs2_init(void)
{
	pr_info("ocsfs2: module loaded (v2 skeleton)\n");
	return 0;
}
static void __exit ocsfs2_exit(void)
{
	pr_info("ocsfs2: module unloaded\n");
}
module_init(ocsfs2_init);
module_exit(ocsfs2_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OCSFS v2 — clustered shared-disk filesystem");
```

- [ ] **Step 2: Write `kmod2/Kbuild`**

```make
obj-m += ocsfs2.o
ocsfs2-y := super.o transport/scsi_pr.o
ccflags-y := -I$(src) -Wall
```
(Until A2 adds scsi_pr.c, temporarily `ocsfs2-y := super.o`.)

- [ ] **Step 3: Build on the node**

Run (from workstation):
```bash
rsync -a kmod2/ root@192.168.1.48:/root/OCSFS/kmod2/
ssh root@192.168.1.48 'make -C /lib/modules/$(uname -r)/build M=/root/OCSFS/kmod2 modules'
```
Expected: builds `ocsfs2.ko`, no warnings.

- [ ] **Step 4: Load/unload**

Run: `ssh root@192.168.1.48 'insmod /root/OCSFS/kmod2/ocsfs2.ko && dmesg | tail -2 && rmmod ocsfs2 && dmesg | tail -1'`
Expected: "module loaded (v2 skeleton)" then "module unloaded".

- [ ] **Step 5: Commit**
```bash
git add kmod2/ && git commit -m "feat(v2): kmod2 skeleton builds and loads"
```

### Task A2: Salvage SCSI PR/CAW transport

**Files:**
- Create: `kmod2/transport/scsi_pr.c` (copied from `kmod/scsi_pr.c`)
- Modify: `kmod2/ocsfs.h` — add the minimal decls/struct fields scsi_pr.c needs
  (`struct ocsfs_pr_info { u64 pr_key; bool pr_registered; }`, the OCSFS_PR_* and
  CAW prototypes, `OCSFS_SB()` returning a stub sbi with `s_pr` + `s_bdev`).

- [ ] **Step 1:** Copy `kmod/scsi_pr.c` → `kmod2/transport/scsi_pr.c`, change include to `"../ocsfs.h"`.
- [ ] **Step 2:** Add to `kmod2/ocsfs.h` the exact subset of types/macros referenced by scsi_pr.c (audit the file: `struct ocsfs_sb_info` needs `s_pr`, `s_bdev` via sb; the `OCSFS_PR_*`/`OCSFS_PR_TYPE_*` macros; CAW prototypes). Define `static inline struct ocsfs_sb_info *OCSFS_SB(struct super_block *sb){ return sb->s_fs_info; }`.
- [ ] **Step 3:** Enable `transport/scsi_pr.o` in Kbuild. Build on node (Step 3 of A1). Expected: clean build.
- [ ] **Step 4: Commit** `git commit -m "feat(v2): salvage SCSI PR+CAW transport (dormant)"`

---

## Phase B — On-disk format + mkfs/fsck

### Task B1: Freeze the on-disk format (`ocsfs.h` + `ocsfs_ondisk.h`)

**Files:**
- Modify: `kmod2/ocsfs.h` — add all on-disk structs with `static_assert` size checks.
- Create: `tools2/ocsfs_ondisk.h` — userspace mirror (same struct layout, `uint*_t` types).

- [ ] **Step 1: Define on-disk structs** (identical layout kernel vs userspace; kernel uses `__le*`). Key structs and frozen sizes:

```c
#define OCSFS2_MAGIC        0x4F435332u   /* 'OCS2' */
#define OCSFS2_INODE_MAGIC  0x494E4F32u   /* 'INO2' */
#define OCSFS2_AG_MAGIC     0x41474732u   /* 'AGG2' */
#define OCSFS2_BLOCK_SIZE   4096
#define OCSFS2_INODE_SIZE   512
#define OCSFS2_ROOT_INO     2
#define OCSFS2_FIRST_USER_INO 64
#define OCSFS2_INLINE_EXTENTS 16
#define OCSFS2_MAX_NAME 255

struct ocsfs2_disk_super {           /* 4096 B */
	__le32 s_magic; __le16 s_major; __le16 s_minor;
	__u8   s_uuid[16]; __u8 s_label[64];
	__le32 s_block_size; __le32 s_inode_size;
	__le64 s_total_blocks; __le64 s_free_blocks;
	__le64 s_total_inodes; __le64 s_free_inodes;
	__le32 s_ag_count; __le64 s_ag_size;        /* inodes-per-AG span */
	__le64 s_ag_blocks;                         /* blocks per AG */
	__le16 s_max_nodes; __le16 s_pad0;
	__le64 s_feat_compat; __le64 s_feat_incompat; __le64 s_feat_ro_compat;
	/* region offsets (byte offsets), computed by mkfs */
	__le64 s_node_table_off; __le64 s_heartbeat_off;
	__le64 s_lease_table_off; __le64 s_lease_count;
	__le64 s_recovery_off; __le64 s_journal_off; __le64 s_journal_size;
	__le64 s_ag_desc_off;   /* first AG header */
	__le64 s_data_off;      /* informational: first data byte of AG0 */
	__le64 s_mkfs_time; __le64 s_mount_count;
	__u8   s_reserved[/* pad to 4092 */];
	__le32 s_checksum;      /* crc32c over [0..4091] */
} __packed;
static_assert(sizeof(struct ocsfs2_disk_super) == 4096, "super 4096");

struct ocsfs2_disk_inode {           /* 512 B */
	__le32 i_magic; __le32 i_pad;
	__le64 i_ino; __le16 i_mode; __le16 i_nlink;
	__le32 i_uid; __le32 i_gid;
	__le64 i_size; __le64 i_blocks;
	__le64 i_atime; __le64 i_mtime; __le64 i_ctime;
	__le32 i_flags; __le16 i_extent_count; __le16 i_pad2;
	__le64 i_extent_tree_root;
	__u8   i_inline_extents[OCSFS2_INLINE_EXTENTS * 24]; /* 384 B */
	__le64 i_dir_btree_root; __le32 i_dirent_count; __le32 i_pad3;
	__le64 i_xattr_block;
	__u8   i_reserved[/* pad to 508 */];
	__le32 i_checksum;
} __packed;
static_assert(sizeof(struct ocsfs2_disk_inode) == 512, "inode 512");

struct ocsfs2_disk_extent {          /* 24 B */
	__le64 e_logical; __le64 e_physical; __le32 e_length;
	__le16 e_flags; __le16 e_pad;
} __packed;
static_assert(sizeof(struct ocsfs2_disk_extent) == 24, "extent 24");

struct ocsfs2_disk_ag {              /* 4096 B header block */
	__le32 ag_magic; __le32 ag_number;
	__le64 ag_block_start; __le64 ag_block_count;
	__le64 ag_free_blocks; __le64 ag_free_inodes;
	__le64 ag_bitmap_off; __le64 ag_bitmap_blocks;
	__le64 ag_inode_table_off; __le64 ag_inodes_per_ag;
	__le64 ag_data_off; __le64 ag_rc_btree_root;
	__u8   ag_reserved[/* pad to 4092 */]; __le32 ag_checksum;
} __packed;
static_assert(sizeof(struct ocsfs2_disk_ag) == 4096, "ag 4096");

struct ocsfs2_disk_dirent {          /* variable, but fixed-stride here for simplicity */
	__le32 de_magic; __le64 de_ino; __le64 de_name_hash;
	__u8 de_file_type; __u8 de_name_len; __u8 de_name[OCSFS2_MAX_NAME + 1];
	__le16 de_rec_len; __le16 de_checksum;
} __packed; /* stride = sizeof(); keep a static_assert documenting it */
```

  Plus *reserved* cluster structs (defined but unused this plan): `ocsfs2_disk_node_slot` (256 B), `ocsfs2_disk_heartbeat` (1024 B), `ocsfs2_disk_lease` (64 B), `ocsfs2_disk_recovery` (one block), `ocsfs2_disk_journal_hdr`/`_txn`. Add `static_assert` for each.

- [ ] **Step 2: Userspace mirror test** — `tools2/ocsfs_ondisk.h` with the same structs (`uint32_t` etc., `_Static_assert`). Write `tools2/t_sizes.c`:
```c
#include "ocsfs_ondisk.h"
#include <stdio.h>
int main(void){ printf("super=%zu inode=%zu ag=%zu extent=%zu\n",
  sizeof(struct ocsfs2_disk_super), sizeof(struct ocsfs2_disk_inode),
  sizeof(struct ocsfs2_disk_ag), sizeof(struct ocsfs2_disk_extent)); return 0; }
```
Run: `cc -Wall -Werror tools2/t_sizes.c -o /tmp/t_sizes && /tmp/t_sizes`
Expected: `super=4096 inode=512 ag=4096 extent=24` and **compiles** (the `_Static_assert`s pass).

- [ ] **Step 3: Commit** `git commit -m "feat(v2): freeze on-disk format (structs + size asserts)"`

### Task B2: `mkfs.ocsfs2`

**Files:** Create `tools2/mkfs.c`. Build: `cc -Wall -Werror tools2/mkfs.c -o mkfs.ocsfs2`.

mkfs flow (write directly to the block device, O_DIRECT or buffered+fsync):
1. Parse args: device, `-L label`, `-N max_nodes` (default 8), `-b blocksize` (4096),
   `--ag-size` (default 1 GiB worth of blocks), `-f` force.
2. Query device size (`BLKGETSIZE64`). Reject if < a sane minimum.
3. Compute region offsets in order (SB, mirror, node-table, heartbeat, lease-table,
   recovery, journals, AGs). `lease_count` default 65536, entry 64 B. `journal_size`
   default 16 MiB × max_nodes.
4. Compute AG geometry: `ag_blocks` = ag-size/bs; `ag_count` = remaining/ag_blocks
   (≥1). For each AG: header block, bitmap (ceil(ag_blocks/8/bs) blocks), inode table
   (`inodes_per_ag` × 512; pick inodes_per_ag e.g. ag_blocks/4), data blocks.
5. Write zeroed regions, then valid headers: superblock(+mirror) with all offsets,
   each AG header with crc, each AG bitmap with metadata blocks (header+bitmap+itable)
   marked allocated, each inode-table zeroed.
6. Create **root inode** (ino=2) in AG0: mode `S_IFDIR|0755`, nlink=2, size=block,
   one data block allocated for `.`/`..` dirents; mark that block in AG0 bitmap; write
   the two dirents; set inode crc. Mark inode 2 used in AG0 inode bitmap (inode
   allocation uses the AG block-bitmap region? No — inodes tracked by a separate
   scheme: simplest = an inode is "used" iff `i_magic`/`i_nlink`!=0. For v1-parity use
   a per-AG inode bitmap embedded at the start of the inode-table region, OR scan.
   **Decision:** use `i_links_count!=0 && i_magic==MAGIC` as "in use"; allocation scans
   the inode table for a free slot. Simple, correct, fine for v1; optimize later.)
7. `fsync`, close.

- [ ] **Step 1:** Implement region/geometry computation as pure helper `compute_layout(dev_size, opts, struct layout *out)`; unit-test it with a fake size.
- [ ] **Step 2:** Implement device writing.
- [ ] **Step 3: Test on node** (resolve `$LUN` by-path first):
```bash
ssh root@192.168.1.48 'cd /root/OCSFS && cc -Wall -Werror tools2/mkfs.c -o mkfs.ocsfs2 && \
  LUN=$(ls /dev/disk/by-path/*iscsi*lun-2 2>/dev/null | head -1); echo LUN=$LUN; \
  ./mkfs.ocsfs2 -L test $LUN && \
  dd if=$LUN bs=4096 count=1 2>/dev/null | xxd | head -4'
```
Expected: prints magic `5343 4f32` (LE 'OCS2') at offset 0; exit 0.
- [ ] **Step 4: Commit** `git commit -m "feat(v2): mkfs.ocsfs2 — format a volume"`

### Task B3: `fsck.ocsfs2` (read-only verifier)

**Files:** Create `tools2/fsck.c`. Build `cc -Wall -Werror tools2/fsck.c -o fsck.ocsfs2`.

Checks: SB magic+crc (fall back to mirror); region offsets in-range & non-overlapping;
each AG header magic+crc; bitmap consistency (metadata blocks marked used); root inode
present, valid mode, crc ok, its `.`/`..` dirents valid; every used inode crc ok and
its extents in-range & non-overlapping (no extents yet, so trivially ok); free-counter
sanity. Print `clean` / list errors; exit 0 clean, non-zero on error.

- [ ] **Step 1:** Implement; reuse `compute_layout`-style validation against stored offsets.
- [ ] **Step 2: Test gate** (`tests/v2/test_format.sh`): mkfs then fsck.
```bash
ssh root@192.168.1.48 'cd /root/OCSFS && cc -Wall -Werror tools2/fsck.c -o fsck.ocsfs2 && \
  LUN=$(ls /dev/disk/by-path/*iscsi*lun-2|head -1) && ./fsck.ocsfs2 $LUN; echo "exit=$?"'
```
Expected: `clean`, `exit=0`.
- [ ] **Step 3: Commit** `git commit -m "feat(v2): fsck.ocsfs2 — verify a volume clean"`

---

## Phase C — Mount

### Task C1: `fill_super` + mount/unmount

**Files:** Modify `kmod2/super.c`; add `kmod2/ocsfs.h` in-memory `ocsfs2_sb_info`,
`ocsfs2_ag_info`, `ocsfs2_inode_info`.

Contract:
- `ocsfs2_fs_type` with `init_fs_context` (modern API) → `get_tree_bdev`.
- `fill_super`: `sb_set_blocksize(sb, 4096)`; read block 0 via `sb_bread`; validate
  magic+crc (fallback mirror); populate `ocsfs2_sb_info` (cache geometry + region
  offsets); read AG headers into `s_ags[]`; **reject** if any region overlaps
  (the CRIT-O1 guard); set `sb->s_op`; `ocsfs2_iget(sb, ROOT_INO)`; `d_make_root`.
- `kill_sb` / `put_super`: free `s_ags`, brelse buffers.
- `statfs`: report total/free blocks+inodes from cached counters.
- Inode cache: `kmem_cache` for `ocsfs2_inode_info`; `alloc_inode`/`free_inode` ops.

- [ ] **Step 1:** Define in-memory structs + sb_info; implement init/exit registering `ocsfs2_fs_type`.
- [ ] **Step 2:** Implement `fill_super` (root iget depends on Task D1's `ocsfs2_iget`; if doing C before D, stub iget to build a minimal root inode inline, then replace in D1).
- [ ] **Step 3: Test — mount/unmount** (requires D1 for a real root; gate runs after D1):
```bash
ssh root@192.168.1.48 'cd /root/OCSFS && make -C /lib/modules/$(uname -r)/build M=$PWD/kmod2 modules && \
  insmod kmod2/ocsfs2.ko && LUN=$(ls /dev/disk/by-path/*iscsi*lun-2|head -1) && \
  mkdir -p /mnt/o2 && mount -t ocsfs2 $LUN /mnt/o2 && mount | grep o2 && \
  ls -la /mnt/o2 && umount /mnt/o2 && rmmod ocsfs2 && ./fsck.ocsfs2 $LUN; echo exit=$?'
```
Expected: mounts; `ls -la /mnt/o2` shows `.`/`..`; unmounts; `fsck` clean.
- [ ] **Step 4: Commit** `git commit -m "feat(v2): mount/unmount a volume, root dir visible"`

---

## Phase D — Inode & directory namespace

### Task D1: Inode read/write/alloc

**Files:** Create `kmod2/inode.c`, `kmod2/bitmap.c`.

Functions + contracts:
- `struct inode *ocsfs2_iget(sb, ino)`: `iget_locked`; if new, read on-disk inode at
  `inode_disk_off(ino)`, validate magic+crc, fill VFS inode (mode, nlink, uid/gid,
  size, times), copy inline extents into `ocsfs2_inode_info`, set i_op/i_fop by type,
  `unlock_new_inode`.
- `int ocsfs2_write_inode_block(inode)`: serialize in-memory → on-disk inode, recompute
  crc, write its buffer_head (dirty + sync per wbc).
- `int ocsfs2_alloc_inode_num(sb, ag_hint, u64 *ino)`: scan AG inode table for a free
  slot (i_magic==0 / nlink==0), under `ag_lock`; return ino. `ocsfs2_free_inode_num`.
- `struct inode *ocsfs2_new_inode(dir, mode)`: alloc ino, init inode, mark used.
- `bitmap.c`: `ocsfs2_alloc_blocks(sb, ag_hint, count, u64 *blk)` /
  `ocsfs2_free_blocks(sb, blk, count)` — per-AG bitmap, under `ag_lock`, update
  free counters.

- [ ] **Step 1:** Implement bitmap alloc/free; **test indirectly** via dir block alloc in D2.
- [ ] **Step 2:** Implement iget/write/alloc inode.
- [ ] **Step 3:** Wire into `fill_super` root iget (replace C1 stub). Run C1's mount gate. Expected: root shows `.`/`..`, fsck clean.
- [ ] **Step 4: Commit** `git commit -m "feat(v2): inode get/write/alloc + block bitmap"`

### Task D2: Directory operations

**Files:** Create `kmod2/dir.c`. Modify `kmod2/super.c`/`inode.c` to wire `i_op`/`i_fop`.

Functions:
- `ocsfs2_add_dirent(dir, name, ino, ft)` / `__del_dirent` / `find_dirent` — scan dir
  data blocks (via inline extents → physical block → `sb_bread`), append/remove a
  fixed-stride dirent, recompute dirent crc, bump `i_dirent_count`/`i_size`, allocate a
  new dir data block when full (via bitmap + inline extent append).
- `ocsfs2_lookup`, `ocsfs2_create`, `ocsfs2_mkdir`, `ocsfs2_unlink`, `ocsfs2_rmdir`,
  `ocsfs2_empty_dir`, `ocsfs2_readdir` (`iterate_shared`).
- `inode_operations` for dir; `file_operations` for dir (readdir).

- [ ] **Step 1:** Implement dirent primitives + lookup/readdir.
- [ ] **Step 2:** Implement create/mkdir/unlink/rmdir (with nlink bookkeeping, `.`/`..`).
- [ ] **Step 3: Test — namespace** (`tests/v2/test_namespace.sh`), cross-checked vs ext4:
```bash
ssh root@192.168.1.48 'set -e; cd /root/OCSFS; LUN=$(ls /dev/disk/by-path/*iscsi*lun-2|head -1)
 ./mkfs.ocsfs2 -L t $LUN; insmod kmod2/ocsfs2.ko; mount -t ocsfs2 $LUN /mnt/o2
 mkdir /mnt/o2/d1 /mnt/o2/d2; touch /mnt/o2/d1/a /mnt/o2/d1/b; echo hi>/mnt/o2/note 2>/dev/null||true
 ls -R /mnt/o2 > /tmp/before.txt; mv /mnt/o2/d1/a /mnt/o2/d2/a2; rm /mnt/o2/d1/b
 umount /mnt/o2; mount -t ocsfs2 $LUN /mnt/o2; ls -R /mnt/o2 > /tmp/after.txt
 cat /tmp/after.txt; umount /mnt/o2; rmmod ocsfs2; ./fsck.ocsfs2 $LUN; echo exit=$?'
```
Expected: after remount the tree shows `d1/`, `d2/a2`, `note`; `b` gone; `fsck` clean,
`exit=0`. (Note: `echo hi>note` may fail until Plan 2's data path — creating the empty
file must still succeed; writing content is allowed to ENOSPC/short until Plan 2.)
- [ ] **Step 4: Commit** `git commit -m "feat(v2): directory namespace ops + readdir"`

### Task D3: Rename

**Files:** Create `kmod2/rename.c`.

- `ocsfs2_rename` (handle `RENAME_NOREPLACE`; same-dir and cross-dir; update `..` on
  dir moves; nlink bookkeeping; reject non-empty target dir).

- [ ] **Step 1:** Implement.
- [ ] **Step 2: Test** — extend `test_namespace.sh` with cross-dir rename of a dir and
  a rename-over-existing-file; remount; `fsck` clean.
- [ ] **Step 3: Commit** `git commit -m "feat(v2): rename (same-dir, cross-dir, dir moves)"`

---

## Exit criteria for Plan 1 (the gate to Plan 2)
- `mkfs.ocsfs2` + `fsck.ocsfs2` build `-Werror` clean.
- Module builds with **zero warnings** on the node, loads/unloads cleanly.
- Mount a freshly-formatted 30 GiB iSCSI LUN; full namespace lifecycle
  (mkdir/create/lookup/unlink/rmdir/rename/readdir) works and **persists across
  remount**; `fsck` reports clean after every scenario.
- Behaviour cross-checked against ext4 for the same namespace op sequence.
- **No cluster code executed** (regions reserved only).

## Self-review notes (coverage vs spec §6, §9)
- On-disk format §6 → Tasks B1/B2 (format frozen; lease/journal regions reserved).
- L1 mkfs/fsck §9 → B2/B3. L1→VFS mount §3 → C1. L2 inode/dir namespace §9 → D1–D3.
- Data path (§9 iomap/thin/reflink/snapshot), journaling (§5), cluster (§4,§8) →
  **explicitly deferred to Plan 2+** (each its own plan that produces testable software).
