# OCSFS v2 — Open items & security analysis

Status as of 2026-06-02. The single-node data path is differentially validated
vs XFS (clean) and `fsck`-clean; cluster L3–L5 + maintenance services are
validated on 3 nodes. **[FIX]** = worth doing, **[DISCARD]** = out of scope.

### ✅ Resolved 2026-06-02 (all triaged items addressed)
- **A1** refcount cluster-coherence — FIXED (`d93036d`): coherent reads + CAW;
  cross-node reflink corruption repro now clean.
- **A2** extent B+tree delete-collapse — FIXED (`c3afa3b`): rebuild on emptied
  leaf; repro (`tests/v2/repro_a2.c`) 181/400 corrupt → 0/400.
- **A3** evict-time free coordination — VERIFIED already handled (clustered_free
  + cl_caw_record + A1); 3-node churn test clean.
- **A4** df free-count drift — FIXED (`6d59004`): statfs/unmount recompute from
  bitmap; df accurate cross-node.
- **A5** zero_range WARN — VERIFIED resolved (no repro after data-path hardening).
- **A6 / S4** AG geometry bound-check — FIXED (`174ef12`).
- **S1** snapshot permission bypass — FIXED (`a11a593`): inode_permission check.
- **S2** maintenance markers tamperable — FIXED (`a11a593`): root-only 0700 dir.
- **S3** result-struct stack leak — VERIFIED clean (already memset).
- **A7** inline compression — DISCARDED (out of scope).
- **A8** per-data-block checksums — DEFERRED (optional, format-v3).
- **S5** shared-disk single-trust-domain — documented assumption (by design).

The detailed analysis below is kept for reference.

---

## Part A — Open functional / correctness items

### A1. [FIX · P1] Per-AG refcount B+tree is not cluster-coherent
`kmod2/refcount.c` reads/writes the refcount tree with `sb_bread` +
`mark_buffer_dirty`/`sync_dirty_buffer` — the **single-node** path. It uses
**no coherent read** (`ocsfs2_meta_bread`), **no CAW**, and reflink/CoW take no
metadata lease. The refcount tree is **per-AG shared metadata** touched by
reflink / snapshot / dedup / CoW on *any* node. Two nodes operating in the same
AG can therefore lost-update the tree and read a stale refcount → a shared block
can be freed while another file still references it → **cross-node data
corruption** (the v1 "rc-coherence" bug, still present in v2).
*Why it slipped:* all reflink/dedup differential tests were single-node; the
single-writer ownership covers file **data**, not the shared per-AG refcount
metadata.
*Fix:* make refcount mutations cluster-safe like the bitmap/inode-table already
are — coherent reads (`ocsfs2_meta_bread`) + atomic update via SCSI CAW (or
serialise refcount ops in an AG under the global metadata lease). Add a
**multi-node reflink+CoW differential test** (clone on n1, write on n2, verify).

### A2. [FIX · P2] Extent B+tree has no delete-time collapse/rebalance
`ocsfs2_ext_tree_punch_range` / truncate remove leaf records but never merge
empty leaves or reduce tree height, and never re-route the internal nodes. On a
**multi-leaf** tree (> ~169 extents) a punch that empties a leaf leaves stale
internal routing; a later insert of a record spanning the freed key-range lands
in one leaf while reads past it resolve to the emptied leaf → **reads return a
hole** for mapped data. Online defrag avoids this by tearing down and rebuilding
(`free_all`), but `fallocate(PUNCH_HOLE)` on a very fragmented file (large VM
disk) can hit it directly.
*Why it slipped:* `fsx -l 500000` keeps files < 1 leaf, so the differential
suite never built a multi-leaf tree.
*Fix:* implement B+tree delete-time collapse (merge/rebalance leaves, drop a
level when the root has one child); rebuild the in-core map from the leaf chain
(`en_next`) which is reliable. Add an fsx case with a large file (`-l` big) to
force multi-leaf trees.

### A3. [FIX · P2] Concurrent evict-time free not coordinated (cluster)
Concurrent delete+alloc across nodes is not fully serialised at evict time
(known follow-up). Risk: a freed block reused by a peer before the free is
visible. *Fix:* route evict-time frees through the same coherent bitmap CAW path
and/or the metadata lease; add a churn test (delete on n1 while n2 allocates in
the same AG).

### A4. [FIX · P3] `df` free-count drift in cluster mode
The on-disk **bitmap is authoritative and correct** (`fsck` recomputes it;
allocation never returns a false ENOSPC), but the superblock's *cached*
`s_free_blocks` is not maintained cross-node, so `df` over-reports free space
until a remount. Cosmetic. *Fix:* recompute `s_free_blocks` from the bitmap at
mount and/or have the online scrub recompute + persist it (CAW on the super).

### A5. [FIX · P3] `iomap_zero_range` WARN under fsstress
`ocsfs2_fallocate` ZERO_RANGE issues a single `iomap_zero_range` over the whole
range and trips a kernel `WARN_ON_ONCE` (no data corruption — verified). *Fix:*
mirror v1's block-by-block zero with `round_down/up` flush, or correct the iomap
call so the warning is not tripped.

### A6. [FIX · P3] AG header geometry not bound-checked at mount
`read_ag_headers` trusts `bitmap_blocks` / `inode_table_off` / `inodes_per_ag`
from a CRC-valid AG header. A corrupted (CRC-recomputed) header with huge values
could drive a large `kvmalloc` (OOM/DoS) or out-of-AG reads (bounded by the block
layer, not memory-unsafe). Requires raw device write access. *Fix:* validate
each AG's geometry against the device size / AG region span at mount.

### A7. [DISCARD] Inline LZ4/ZSTD compression
Out of scope — breaks the iomap 1:1 logical↔physical mapping and O_DIRECT
(`cache=none`, the Proxmox default). Space savings come from dedup + thin +
discard. Already removed from the roadmap.

### A8. [DEFER · optional] Per-data-block checksums (format-v3)
Not done. Metadata is fully CRC32c-checksummed and the scrub verifies it. Data
checksums need a format-v3 per-AG region + write hooks on both buffered and
O_DIRECT paths; a half-correct version false-positives on every VM write. Only
worth it if silent-data-corruption protection becomes a requirement.

---

## Part B — Security analysis

Threat model: the LUN is a *shared SAN device trusted by the cluster*; the main
untrusted inputs are (1) **local users** issuing syscalls/ioctls on a mounted
volume, and (2) a **corrupted or hostile on-disk image** (defence-in-depth, since
writing it needs device/root access). The transport (SCSI PR/CAW), Perl plugin
and installer run as root.

Overall: the on-disk parse path is **well hardened** — magic + CRC32c + bounds
on super/AG/inode/extent-node/refcount-node/dirent/xattr/journal/lease; name
lengths checked; `parse_extents` clamps to 16 slots; `validate_layout` enforces
region ordering within the device (the v1 CRIT-O1 lesson). All privileged ioctls
(GROWFS/SCRUB/DEFRAG/FITRIM) gate on `CAP_SYS_ADMIN`. Findings:

### S1. [FIX · P1] Snapshot ioctl bypasses directory permission checks
`OCSFS_IOC_SNAP_CREATE` (`reflink.c:ocsfs2_ioc_snap_create`) calls
`dir->i_op->create()` **directly**, skipping `inode_permission()` /
`may_create()`. A user holding a *read-only* fd to a file can create a new file
(the snapshot) in its **parent directory regardless of write permission** on that
directory — a permission-model bypass. (The standard FICLONE path is safe; only
this custom ioctl is affected.)
*Fix:* before creating, `inode_permission(idmap, dir, MAY_WRITE | MAY_EXEC)` and
honour the mount's idmap; reject on a read-only mount (already does
`mnt_want_write_file`).

### S2. [FIX · P2] Maintenance marker files are user-tamperable (local DoS)
`ocsfs2-maint` writes `.ocsfs2-maint.<mode>.{lock,stamp}` at the **mount root**.
A non-root local user who can write there can pre-create a fresh `stamp` (to
**suppress** weekly scrub/defrag) or hold a `lock` (within the stale window).
Maintenance runs as root and trusts these files. *Fix:* keep markers in a
root-only-writable subdirectory (mode 0700, created/owned by root), or verify
the marker file is owned by root before trusting it.

### S3. [OK] Result structs are zeroed before copy_to_user
Verified: `ocsfs2_scrub()` and `ocsfs2_defrag_file()` both `memset` their result
to 0 before filling, and `fstrim_range` is round-tripped from userspace — so no
uninitialised kernel-stack bytes leak. Keep this invariant (memset any new result
struct) when adding future ioctls.

### S4. [FIX · P3] Hostile-image DoS hardening
A CRC-valid but malicious AG/super geometry can drive large `kvmalloc`s
(`s_ag_capacity` headroom up to 65536 entries; per-AG bitmap = `bitmap_blocks`
× 4 KiB) → memory exhaustion at mount. Bounded by needing device write access,
but mount of an untrusted device (e.g. a removable LUN) is a real surface. *Fix:*
sanity-cap geometry (A6) and use `kvmalloc(..., GFP_KERNEL | __GFP_NORETRY)` with
explicit limits for image-derived sizes.

### S5. [NOTE] Cluster trust model
Any node that can write the shared LUN can forge lease/membership records and
fence peers — the cluster is a single trust domain (as with GFS2/OCFS2). The
optional `-K` cluster-auth HMAC gates *joining*, not raw device writes. This is
inherent to shared-disk FSes; document it as an explicit assumption (the SAN/LUN
must be reachable only by trusted nodes).

### S6. [OK] Reviewed and clean
- Privileged ioctls correctly require `CAP_SYS_ADMIN`; snapshot/clone are user
  ops (modulo S1).
- `copy_from_user` sizes are fixed and buffers correctly sized
  (`name[OCSFS2_MAX_NAME+1]`, faults → `-EFAULT`).
- Proxmox `OCSFS2Plugin.pm` uses `run_command([...])` with argv arrays — **no
  shell injection**; `clone_image` likewise.
- `install.sh` runs as root, installs from trusted apt repos and ships files to
  root-owned paths.
- No 32-bit `.compat_ioctl` (functional gap, not a security issue) — add if
  32-bit userspace must drive the ioctls.

---

## Suggested order
P1 first (**A1 refcount cluster-coherence** — the only open *data-integrity*
issue; **S1 snapshot perms** — a real privilege bypass), then A2/A3/S2, then the
cosmetic/hardening P3s. A7 stays discarded; A8 deferred.
