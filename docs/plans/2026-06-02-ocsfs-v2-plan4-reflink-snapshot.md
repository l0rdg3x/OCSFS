# OCSFS v2 — Plan 4: reflink (FICLONE) + CoW snapshots

**Goal:** Space-efficient cloning for Proxmox VM-disk workflows: `FICLONE` /
`copy_file_range` shares extents between files (no data copy); a write to a
shared block triggers copy-on-write so clones stay isolated. CoW file snapshots
(via ioctl) build on the same machinery. Single-node; multi-node sharing comes
with the cluster plan.

**Architecture:** A per-AG refcount B+tree (`ag_rc_btree_root`, already reserved
in the AG header) maps a physical block → reference count. A normal (sole-owner)
extent has no refcount entry (refcount implicitly 1). Sharing bumps the count and
sets `OCSFS2_EXT_SHARED` on both files' extents. On write to a `SHARED` block,
`iomap_begin` copies the block(s) to freshly-allocated space, decrements the old
block's refcount, and remaps the writer's extent — so the other sharer is
untouched. `ocsfs2_free_blocks` becomes refcount-aware: freeing a shared block
decrements the count and only releases to the bitmap at count 0. All refcount +
extent-map mutations are journaled (Plan 3 txns).

**Tasks:**
- **R1 `refcount.c`** — per-AG B+tree (reuse a generic block-btree node format):
  `ocsfs2_refcount_get(sb, phys) -> count`, `ocsfs2_refcount_inc(sb, phys, len)`,
  `ocsfs2_refcount_dec(sb, phys, len, *should_free)`, `ocsfs2_refcount_init_ag`.
  Tree nodes allocated from the AG; root in `ag_rc_btree_root`; all updates in a
  txn. `ocsfs2_needs_cow(sb, phys) = refcount_get(phys) > 1`. Refactor
  `ocsfs2_free_blocks` → refcount-aware (`ocsfs2_free_blocks_rc`): dec, free to
  bitmap only at 0. Test: inc/get/dec round-trips, fsck extension validates the
  tree.
- **R2 reflink** — `ocsfs2_remap_file_range` (FICLONE/clone): under EX on both
  inodes, share the source's extents into the dest (bump refcount per extent,
  set `OCSFS2_EXT_SHARED` on both), journaled. `iomap_begin` write path: if the
  target extent is `SHARED` and `needs_cow`, `ocsfs2_cow_extent` (alloc new,
  copy data, dec old refcount, remap, clear SHARED) before mapping for write.
  Wire `.remap_file_range = ocsfs2_remap_file_range` in `ocsfs2_file_fops`.
  Test: `cp --reflink`, write to clone, cold-read both — clone diverges, source
  intact (cross-check vs XFS reflink=1); `du` shows sharing; fsck clean.
- **R3 snapshot** — `OCSFS_IOC_SNAP_CREATE` ioctl: create a new file that
  reflinks the source (point-in-time copy). Source/clone isolation, delete
  integrity (delete one → other intact, blocks freed only at refcount 0), fsck
  clean.

**On-disk:** no new superblock fields (uses `ag_rc_btree_root`); add an
`OCSFS2_EXT_SHARED` extent flag (already defined). The refcount-btree node format
is a new metadata block type (magic + level + nr + [key,val] entries + crc) —
freeze it in `ocsfs.h` + mirror in `ocsfs_ondisk.h` so `fsck` can verify it.

**Tests:** `tests/v2/test_reflink.sh` — reflink isolation + du sharing + delete
integrity, cross-checked against XFS (`mkfs.xfs -m reflink=1` on a scratch LUN),
fsck clean after each. (The fsx integrity fuzzer with reflink/CoW churn lands
after the extent B+tree, Plan 2b, since heavy CoW fragments the extent map.)

**Exit:** reflink + CoW + snapshot correct and isolated, journaled (crash-safe),
fsck clean, zero warnings, cross-checked vs XFS.
