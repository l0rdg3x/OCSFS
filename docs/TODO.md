# OCSFS v2 — Open items & security analysis

Status as of 2026-06-02. The single-node data path is differentially validated
vs XFS (clean) and `fsck`-clean; cluster L3–L5 + maintenance services are
validated on 3 nodes. **[FIX]** = worth doing, **[DISCARD]** = out of scope.

### ✅ [CHORE · P2] Remove v1 leftovers from `main` — DONE (2026-06-03)
The v1 codebase is preserved on the **`v1-legacy`** branch; `main` now carries only
v2. Removed `kmod/ tools/ src/ include/ proxmox/ conf/ man/ debian/` + the v1 root
`Makefile` (75 files, −32 469 lines); README "Project structure" updated; verified
no v2 file references the v1 trees. (Minor residue kept on purpose under `tests/`
per instruction — a few loose v1 test files: `test_ocsfs.c`, `ocsfs_fsx.c`,
`repro13.c`, `cluster/`; harmless, not built by v2.)

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
- **A8** per-data-block checksums — **DONE** (opt-in `mkfs -C`, RO_COMPAT_DATACSUM):
  per-physical-block CRC32c in a per-AG region (`csum.c`); stored on every write
  (buffered via writeback_range, O_DIRECT via the dio submit hook), cluster-
  coherent via CAW; verified by the scrub (reads every data block — any SAN, any
  cache mode, cross-node). CoW/reflink-safe (checksum follows the physical block).
  Validated single-node + 2-node: `dd`-corrupt a block → scrub detects it; no
  regression on non-`-C` volumes.
- **A8-inline** read-time verification — **DONE** (2026-06-03): every data read
  now recomputes the block CRC and returns `-EIO` on mismatch instead of serving
  corruption, on **both** paths — buffered (custom `iomap_read_ops` synchronous
  `read_folio_range`) and **O_DIRECT** (the dio `submit_io` pre-reads the expected
  CRCs, the bio `end_io` compares them; `crc32c`+`kmap_local`, no sleeping). Fixed
  a latent bug it exposed: a freed block kept its stale CRC, so a later reuse that
  writes no data (`fallocate` preallocation) false-positived — now `ocsfs2_free_blocks`
  drops the CRC (`csum_clear_range`) and `copy_blocks` carries it (CoW/defrag stay
  verifiable). **Write-path accelerated** so `-C` is cheap: the per-block `sync`/CAW
  became one per checksum block (`csum_set_range`) — seqwrite `-C` 14.5→94.7 MB/s
  (−2% vs no-`-C`); and the cluster read verify batches the coherent CRC read
  (`csum_read_range`) — cluster seqread `-C` 12→32.5 MB/s/node (≈ no-`-C`).
  Validated single-node + 3-node `-o cluster` (cross-node `dd`-corrupt → n2 detects
  on both paths), fsx `-C` differential vs XFS clean, fsck clean.
- **P3a** checksum autogrow-added AGs — **DONE** (2026-06-03): `grow.c`
  `compute_ag_geom` now reserves the per-AG CRC region (identical layout to mkfs),
  `write_new_ag` zeroes it + writes `ag_csum_off/blocks`, `install_ag_incore`
  populates the in-core fields. Validated via dm-linear grow (3→23 AGs): a data
  block in an autogrow-added AG is verified inline (corrupt → `-EIO`) and the scrub
  finds exactly the corruption. **Also fixed a pre-existing scrub bug it exposed:**
  the scrub read DATA via `sb_bread` (buffer cache), incoherent with the iomap bio
  data path — it returned wrong bytes for grown regions → thousands of spurious
  "mismatches". `scrub.c` now reads data via a raw bio (`ocsfs2_cl_bio`, like the
  inline verify), batched 256 KiB/read (correct + much faster). grown clean scrub:
  2454→0 errors; 1 real corruption → exactly 1.
- **P3b** async checksum write (`-o csum_async`) — **DONE** (2026-06-03): skips the
  per-write checksum `sync` (deferred to writeback), lifting the pure-4 KiB-random-
  write ceiling: 3 356 → 12 192 IOPS (3.6×, ≈ no-`-C`), detection intact. Tradeoff:
  wider post-crash false-positive window (data may reach disk before its checksum;
  a rewrite/scrub clears it). Single-node only (in cluster the csum is the coherence
  CAW, always synchronous). Default off (crash-safe).
- **Buffered inline read perf** — **DONE** (2026-06-03): the inline read verify was
  a synchronous bio-per-folio (QD1) → buffered sequential read with `-C` was
  ~18 MB/s. Now async (pre-read CRCs, submit the bio without waiting, verify in the
  completion — like O_DIRECT) → pipelined: **117 MB/s** (line rate, 6.5×), detection
  intact. O_DIRECT read was already fast (~95 MB/s).
  Remaining (minor): the benign crash-mid-writeback false-positive window (a
  rewrite/scrub clears it).
- **S5** shared-disk single-trust-domain — **DISCARDED** (won't fix in the FS):
  inherent to shared-disk FSes; handled operationally — the LUN is exposed only
  to authenticated initiators (iSCSI CHAP / FC zoning + LUN masking).

The detailed analysis below is kept for reference.

---

## Part A — Open functional / correctness items

### A10. [DONE · 2026-06-03] Cluster allocator bitmap re-scan (the real #1 cluster cost)
`clustered_alloc` restarted its bitmap scan at block 0 every allocation,
coherently re-reading every full block — O(filled) cl_bio per alloc. A cluster
`qemu-img convert` spent **~89% of its I/O** there (32k/36k cl_bio). FIXED
(`3190d2e`): scan from `next_blk_hint` + wrap; cl_bio 68k→17k (4×), convert
~60s→47s; fsx + churn clean. This was masking the journal cost (below).

### A9. [FIX · P2] Extend Plan 5 journal deferral to cluster volumes (perf)
After A10, a cluster convert (47s) is dominated by the **synchronous journal**:
44k `sync_dirty_buffer` + 59k `blkdev_issue_flush` (per-op commit + checkpoint).
The single-node deferred path does 21/122/63 of those. So journal deferral would
take a cluster convert ~47s → ~17s (near single-node). **But it's the high-risk
cross-node change**: (1) the deferred path can't simply replace the synchronous
one — peers read home blocks coherently, and crash recovery (`replay_slot`)
needs the records ordered before the header, so naively dropping the per-op
flush/checkpoint risks torn cross-node metadata; (2) needs **checkpoint on every
metadata-lease release/downgrade** (`ocsfs2_lease_release`, lease.c — a single
missed site = stale cross-node read = corruption); (3) `replay_slot` must loop;
(4) full 3-node crash + coherence validation. Its own focused session.
Plan 5 (`50d2370`) makes the journal **batched + deferred** only on volumes that
can never join a cluster (`s_max_nodes <= 1`): `qemu-img convert` there went
52.4s → 12.7s (4.1×, at the data floor), crash-safe. **Cluster-capable volumes
(`-N>1`, i.e. the PVE storage) keep the synchronous per-op checkpoint** — correct
(peers read current home blocks coherently; dead-peer `replay_slot` sees the one
in-flight txn it expects) but `convert`/import/restore stay slow there. The
common cluster op (clone) is already instant via reflink (~0.08s), so this is a
*rare-op* optimization.
*To do it safely:* (1) allow the deferred path for cluster; (2) **checkpoint on
metadata-lease release** (`ocsfs2_lease_release`, lease.c — every EX-release site)
so a peer never reads a lagging home block — **a single missed site = stale
cross-node read = corruption**; (3) make `ocsfs2_journal_replay_slot` **loop**
(deferral means a dead node can leave many uncheckpointed txns, not one); (4) full
3-node crash + coherence validation. High risk for a rare-op gain → its own
focused session, not a tail-end change. (Single-node + cluster-synchronous paths
are both validated: see `docs/plans/2026-06-03-ocsfs-v2-plan5-journal-perf.md`.)

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
