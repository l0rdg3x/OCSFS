# OCSFS v2 — Plan 5: Journal performance redesign (jbd2-lite)

**Goal:** make metadata-heavy write workloads (notably `qemu-img convert` —
disk import, backup restore, template materialization) fast *without* weakening
crash consistency. Today these run ~4× slower than raw sequential write because
the Plan 3 WAL commits **one synchronous transaction per metadata op** and
**checkpoints (writes home blocks) synchronously inside every commit**. Reflink
clones (`cp --reflink`, the `qm clone` path) are already optimal (~0.08s) and are
**out of scope** — this plan only touches the journal write path.

---

## Measured bottleneck (2026-06-03, n1, LUN /dev/sdc, `-C`)

`qemu-img convert -t none` of a 323 MB image = **52 s** (~18 MB/s vs ~76 MB/s raw):

- **120 281 `sync_dirty_buffer` calls** (bpftrace), device busy writing 49/52 s
  → I/O-bound on serial iSCSI round-trips (~0.4 ms each ⇒ ~48 s; matches).
- **write amplification 4.2×** (1369 MB written for 323 MB of data).
- attribution (kstack): **~6 serial syncs per transaction, one transaction per
  block allocation** (15 328 allocs). `ocsfs2_txn_commit ← ext_tree_insert ←
  extent_insert ← iomap_begin` dominates (~70%); + 1 bitmap sync + 1 csum sync
  per write. `nr≈1` block per txn.

Per `ocsfs2_txn_commit` (today): `write_block(desc)` + `write_block(after-image)*nr`
+ `write_block(commit)` → flush → **checkpoint: `sync_dirty_buffer(home)*nr`**
(rewrites the *same* growing btree leaf / inode every op) → flush → 2×
`persist_header`. = `2·nr + 4` serial round-trips + 4 flushes **per op**.

The two structural costs: **(1)** one commit per op (no batching) and **(2)**
synchronous immediate checkpoint (repeated home rewrites = the 4.2×).

---

## Design principles (the jbd2-lite contract)

1. **The journal owns metadata writeback.** A metadata block that has been
   modified but not yet checkpointed is **never** written to its home location by
   the kernel's dirty-page writeback. Uncommitted/uncheckpointed metadata reaches
   home *only* through a controlled journal checkpoint. This is the prerequisite
   that makes deferral safe.
2. **Checkpoint = replay-from-ring.** The home content written at checkpoint is
   sourced from the **immutable after-image in the journal ring**, never from the
   live cached buffer. This is byte-identical to crash replay, so concurrent
   modification or eviction of the live buffer can never corrupt a checkpoint.
3. **Commit is the durability point; checkpoint is lazy.** After a txn's records
   (desc + after-images + commit) are durable, the change survives a crash (redo
   on replay). Writing the home blocks can happen much later, coalesced.
4. **Crash semantics unchanged at the op level:** a crash mid-op leaves the FS
   consistent (the op atomically did or did not happen); ordered-data still holds
   (file data reaches disk before the metadata referencing it is committed); a
   non-fsync'd metadata op may be lost on crash (ext4-default behavior) but never
   leaves torn metadata. `fsck` clean after every crash.

---

## On-disk format

The journal region layout (`s_journal_off`, per-slot `s_journal_size`, header at
block 0, ring after) is **unchanged**. Two additive changes, both gated by a new
RO_COMPAT feature bit `OCSFS2_FEATURE_RO_COMPAT_JOURNAL2` so a Plan-5 volume is
refused read-write by a Plan-3 kernel (and vice-versa the new kernel replays a
Plan-3 dirty journal on first mount, then writes Plan-5 records):

- **Commit block carries a txn CRC** over `desc || all after-images` (J5-C). Lets
  desc+after-images+commit be written in one unordered batch with a single flush;
  a torn commit is detected by CRC, replacing the after-image-before-commit
  ordering barrier.
- **Header `jh_head/jh_tail` track a multi-txn window** (J5-B): they are advanced
  at commit (head) and checkpoint (tail) instead of being reset empty every op.

---

## Tasks (incremental — each phase ships and is crash-validated on its own)

### J5-A — Journal owns metadata writeback + checkpoint-from-ring  *(foundation; behavior-neutral)*
Make principles 1–2 true while keeping checkpoint **immediate and synchronous**
(so observable behavior is unchanged — pure refactor, validated before any
deferral).
- Audit every metadata modify site (`node_finish` extent_btree.c, dir.c dirent
  writes, bitmap.c, xattr.c, refcount/snapshot.c, `ocsfs2_write_inode_block`):
  replace bare `mark_buffer_dirty` on a journaled block with `txn_get` enrolment
  only — the buffer stays **uptodate-but-not-dirty** in cache (kernel won't write
  it back); the journal writes its home at checkpoint.
- Rewrite `txn_commit`'s checkpoint loop to **read each after-image back from the
  ring and `write_block` it to its home** (drop `sync_dirty_buffer(tb->bh)`).
- Pin enrolled buffers against eviction until checkpointed (hold the `get_bh` ref
  taken in `txn_get`; release at checkpoint) so an uncommitted block is never
  dropped and re-read stale from home.
- **Risk:** a missed site (still `mark_buffer_dirty` → kernel writes uncommitted
  metadata home) or a missed enrolment (block never written → lost metadata).
  Mitigation: a debug `WARN` if a dirty buffer on the bdev belongs to a journaled
  block region; fsx + crash + fsck must stay clean.
- **Win:** none yet (foundation). **Exit:** fsx, 3-node coherence, crash, fsck all
  clean — identical to Plan 3.

### J5-B — Deferred + coalesced checkpoint + multi-txn ring + looping replay
- `txn_commit` writes records, advances `j_head`, persists header (publish) — and
  **returns without checkpointing**. The ring now holds `[tail, head)` committed
  txns.
- New `ocsfs2_journal_checkpoint(sb)`: under `j_lock`, scan `[tail, head)`, build a
  **coalesced** home→latest-after-image map (last writer wins), write each unique
  home once (batched async + single wait), flush, set `tail = head`, persist
  header. Triggered on: ring pressure (commit needs space), `->sync_fs`,
  `->fsync`, `put_super`.
- `ocsfs2_journal_replay` and `ocsfs2_journal_replay_slot` (dead-peer recovery)
  **loop** over all committed txns in `[tail, head)` in order (currently apply
  one); stop at the first torn/invalid record.
- **Win:** removes the synchronous checkpoint + 2nd header from every op and
  collapses the 4.2× write-amp (the repeated leaf/inode home writes become one per
  checkpoint). Est. **~1.5–2×** on convert.
- **Exit:** crash mid-burst replays *all* committed txns; crash mid-checkpoint
  redoes from ring (idempotent); 3-node dead-peer recovery replays a multi-txn
  slot; fsx + fsck clean.

### J5-C — Single-flush commit (txn-CRC) + log-scanning replay
- Write `desc + after-images + commit` as one contiguous async batch; the commit
  block's CRC covers the whole txn → a torn write is caught by CRC (no
  after-image-before-commit barrier). One flush per commit.
- Stop persisting the header on every commit: replay **scans** the ring forward
  from `tail` using `seq` + commit-CRC until the first gap (header `head` becomes
  a hint only, refreshed at checkpoint). Removes the per-commit header write.
- **Win:** per-op journal cost ≈ **1 sync + 1 flush** (down from 6 + 4). Est. a
  further **~1.5–2×** (convert now bounded by the remaining bitmap + csum sync per
  write and raw bandwidth).
- **Exit:** torn-commit (CRC) and torn-tail handled; replay finds all committed
  txns by scan; fsck clean.

### J5-D — Lazy batched commit (running deferral)  *(biggest win, highest risk)*
Single-threaded `convert` still pays one commit barrier per op (J5-C made it
cheap but not free). Amortize it by **deferring commit across ops**:
- Each op still builds its **own** txn with before-images (per-op abort stays
  simple — failures revert in place and never enqueue, FS does **not** go
  read-only). On success the op **snapshots its after-images** (copy, since a
  later op may re-modify the same buffer) and enqueues the txn; it returns without
  committing.
- A committer (commit on: `j_size`/age threshold, `->sync_fs`, `->fsync`, ring
  pressure, a deadline workqueue ~5 s) writes the queued txns' records in one
  batch with a single flush, advances head.
- Enrolled buffers are **pinned until checkpoint** (J5-A) so an uncommitted block
  is never evicted and re-read stale.
- Fold the per-op **bitmap allocation into the same txn** (drop the separate
  `flush_bitmap_range` sync — bitmap becomes atomic with the extent insert,
  *more* correct and one fewer sync/op).
- **Win:** thousands of commits → a handful. Est. **5–8×** on convert (approaches
  raw bandwidth).
- **Risk (highest):** buffer pinning vs. memory reclaim; after-image snapshot
  memory bound; interaction with cluster lease/CAW coherence (a committed txn must
  be durable before the metadata lease is handed to a peer — `fsync`/lease-release
  must force-commit). Validate hardest here.
- **Exit:** crash at any queue depth → consistent + fsck clean; lease handoff
  cross-node forces commit (no stale peer read); fsx + 3-node coherence clean.

### J5-E — Validation, perf gate, docs
- Crash matrix on a node (`echo b > /proc/sysrq-trigger`) at each phase: mid-op,
  mid-commit, mid-checkpoint, ring-wrapped, torn-tail → remount replay → fsck
  clean, no orphans, fio-verify err=0.
- 3-node: coherence churn (create/delete/rename), dead-peer recovery of a
  multi-txn slot, lease handoff forces commit.
- Perf gate: `qemu-img convert` time per phase + `sync_dirty_buffer`/write-amp via
  bpftrace + diskstats; record in the perf table.
- Update `design-v2.md` (journal section), `developer-guide.md`, `TODO.md`.

---

## Crash-safety invariants (must hold after every phase)

- **I1 (ordered data):** file data is durable before the metadata that references
  it is committed (unchanged from Plan 3).
- **I2 (atomic op):** after replay, every committed txn is fully applied and every
  uncommitted txn is fully absent — never partially.
- **I3 (no uncommitted home write):** a metadata block modified by an
  uncommitted/uncheckpointed txn is never on its home location on disk (J5-A).
- **I4 (checkpoint idempotent):** checkpoint writes home from the ring; replaying
  it any number of times yields the same home content (J5-B).
- **I5 (durable-before-handoff):** in cluster mode, a metadata change is committed
  (durable) before its lease is released to a peer (J5-D).

---

## Risks & fallback

- The risky core is **J5-A** (touches every metadata path) and **J5-D** (pinning +
  lazy commit + cluster lease interaction). J5-B/J5-C are contained to `journal.c`.
- **Fallback (Strada 1):** if a phase hits an insurmountable correctness problem,
  fall back to the *safe contained* optimization — batch the serial contiguous
  writes inside the existing synchronous `txn_commit` (desc+after-images in one
  async submit + one wait; home blocks batched), preserving **all** ordering
  barriers. Zero crash-safety change, ~1.2–1.5×. This is a strict subset of J5-C's
  batching and can ship independently at any time.

## Exit (whole plan)

`qemu-img convert` within ~2× of raw sequential write (target ≤ ~15 s for the
323 MB image), **zero** crash-consistency regression (full crash matrix + 3-node
recovery + fsx + fsck clean), reflink clone unaffected.

---

## Progress — J5-A + J5-B (2026-06-03, WIP, not committed)

Implemented J5-A (journal owns metadata writeback: 15 `mark_buffer_dirty` sites
gated `!ocsfs2_current_txn()`) and J5-B (deferred checkpoint, single-node only —
gate `s_max_nodes <= 1`; cluster stays synchronous so peers reading home blocks
coherently always see current metadata, and `replay_slot` keeps its single
in-flight txn). `journal.c`: `j_ckpt` list pinning committed txns' buffers,
`checkpoint_locked` (coalesced, sourced from the ring via `ocsfs2_cl_bio` private
bio), looping `ocsfs2_journal_replay`, `txn_forget` forces a checkpoint before
block reuse. Plus a required fix: `ocsfs2_write_inode_block` wraps a private txn
in deferred mode so the inode block is *always* journaled (else a create's empty
after-image is replayed over a later non-journaled fsync write).

Validation (n1, LUN /dev/sdc, real `echo b` crashes):
- Build clean; fsx **80k buffered + 80k O_DIRECT** (`-M -C -N1`) ALL verified OK.
- Core deferred journal **correct**: crash **without `-C`** (65 files) → 0 fail,
  replay looped 131 txns, fsck clean. Crash **`-C` at 50 files** → 0 fail.
- Perf: convert `-N1 -C` **52.4 s → 41.9 s (~1.25×)**, `sync_dirty_buffer`
  120k → 90k, checkpoint coalesced to 56 `cl_bio`. Modest — the 4 remaining
  journal syncs/txn + bitmap + csum cap it; the big win needs **J5-C** (single-
  flush commit + log-scan replay) and **bitmap-in-txn**.

The "open bug" (`-C` + crash ~9 fail) was a **verification-script artifact** (nested-ssh quoting) + the pre-inode-fix i_size bug; with the inode fix and clean
`md5sum -c` verification, every crash test passes. J5-B validated correct.

## Progress — J5-C + C1 + flush-deferral (2026-06-03, WIP, not committed)

Built on J5-B:
- **J5-C batched commit**: desc + after-images + commit written as ONE async
  batch (`write_records_batched` via `write_dirty_buffer`), single flush barrier.
  Per-after-image `je_crc` lets replay reject a torn batch whole (no intra-batch
  ordering). **Deferred drops the per-commit header**; mount recovers head by
  **log-scanning** the ring from tail (seq-continuity), so a commit costs no
  synchronous header write. `replay_one()` validates desc+commit+**every**
  after-image and applies atomically; replay branches deferred(log-scan) vs
  cluster(head-bounded).
- **C1 bitmap-in-txn**: `iomap_begin` binds the bitmap allocation and the extent
  insert into one txn (deferred), so the bitmap is journaled (no separate sync)
  and the two are crash-atomic.
- **flush-deferral**: no `blkdev_issue_flush` per commit. Barriers are checkpoint
  *start* (records durable before any home write), `->fsync`, and sync. Non-
  fsync'd metadata is durable only at the next barrier — consistent (fsck-clean),
  fsync'd is durable.

Validation (n1, /dev/sdc, real `echo b` crashes, clean `md5sum -c`):
- fsx 60k buffered + 60k O_DIRECT (`-C`) ALL OK; fsck clean.
- Crash (fsync survivors + churn + reuse): every survivor OK, log-scan replay
  300–450 txns, fsck clean. flush-deferral crash: fsync'd 65/65 OK, non-fsync'd
  consistent, fsck clean.
- **Perf: convert `-N1 -C` 52.4s → 23.8s (~2.2×)**. `sync_dirty_buffer`
  120k → 15.6k (only csum left), `blkdev_issue_flush` 14.9k → 63, device 93%
  busy (I/O-bound). Write volume 1198MB for a 948MB qcow2 → ~250MB journal
  overhead (60.9k record-writes) is the remaining reducible cost.

## DONE — J5-D + csum-in-batch (2026-06-03) → convert 4.1×, at the data floor

- **J5-D running transaction**: data-path ops (alloc+extent in `iomap_begin`,
  inode writeback, and the data csum writes) JOIN a shared `j_running` batch
  (`ocsfs2_run_begin/_end`) instead of each opening a private txn. The batch
  commits when it reaches `OCSFS2_RUN_MAX_BUFS` (192) distinct buffers and the
  last in-flight op has left, or at a barrier (`->fsync`, sync, an explicit
  non-data op via `ocsfs2_txn_begin`, checkpoint, unmount). A **handle count**
  (`j_run_handles`) is the commit barrier: the batch is committed only when
  `handles==0` (no op mid-modify), so its coalesced after-images are consistent;
  `ocsfs2_run_flush` waits for in-flight ops. A repeatedly-appended btree leaf /
  bitmap / inode is now journaled once per batch. Ops never abort the shared txn
  — they undo partial work with normal ops (extent insert only fails at
  node_alloc, before touching the btree); a rare `txn_get` failure reverts the
  whole batch (the non-fsync'd in-flight ops are lost, consistency kept).
- **csum-in-batch**: the data checksum writes (`csum_bio` O_DIRECT, the writeback
  `csum_folio_range`) run inside a run handle, so the per-AG csum blocks are
  journaled and coalesced with the batch instead of one synchronous write each.

Validation (n1, /dev/sdc, real crashes, clean `md5sum -c` + `/root/ocsfs_verify.sh`):
- **Perf: convert `-N1 -C` 52.4s → 12.7s (4.1×)** — at the ~12s data floor (948MB
  qcow2 @ ~78MB/s). `sync_dirty_buffer` 120k → 21; `write_dirty_buffer` 60.9k →
  122; write volume 1198MB → 960MB (≈ data, journal redo ~gone).
- Crash (fsync survivors + churn + reuse, `-C`): every survivor OK, EIO=0 (csum
  durable via journal), replay 240–290 txns, fsck clean. Non-fsync'd consistent.
- fsx: 120k single + 3×40k concurrent + 100k single (all `-C`, buffered+O_DIRECT)
  ALL verified OK — running-txn concurrency (commit barrier) correct.
- Cluster (synchronous) path unaffected: `-N4` crash recovery clean.

**Remaining:** 3-node cluster validation of the refactored synchronous path
(single-node clean); checkpoint-on-lease-release to extend deferral to cluster
volumes (then cluster converts also batch). The deadline-timer commit (age-bound
durability) is optional — the threshold + barriers already bound it.

## Was: Next — J5-D (the remaining lever)

Convert is I/O-bound on **journal write volume** (~244MB of redo records, one
desc+commit per op). The data floor (948MB qcow2 ≈ 12s) caps the win; J5-D would
take ~24s → ~14s. J5-D = **transaction batching** (a journal-level running
transaction many ops join, committed on a buffer-count/age threshold + fsync +
sync + checkpoint), so N ops share one desc+commit and a repeatedly-touched
btree leaf / inode after-image is written once per batch. Plus per-AG **csum**
left as the only `sync_dirty_buffer` source. Requires the handle model and an
error-path audit (ops must undo partial work via normal ops, not a txn abort,
since the running txn is shared) — a focused effort with its own crash matrix.
Also: checkpoint-on-lease-release to extend deferral safely to cluster volumes.
