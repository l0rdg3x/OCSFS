# OCSFS v2 — Plan 3: Journaling (WAL redo) + crash recovery

**Goal:** Metadata operations are crash-atomic. A per-node write-ahead redo log
records the after-images of the metadata blocks a transaction changes; COMMIT is
the durability point; mount replays committed transactions. A power loss
(`sysrq-b`) mid-operation leaves the FS consistent after remount, `fsck` clean.

**Architecture (single-node, node slot 0):** Reuse the reserved journal region
(`s_journal_off`, `s_journal_size`). Redo-only (after-images), ordered-data:
file data blocks reach disk (via the iomap write path / `filemap_write_and_wait`
on fsync) before the metadata that references them is committed — so the journal
never needs data blocks, and replay never points metadata at uninitialised data.

Record stream in the ring: `BEGIN(seq) | METADATA(bref + block)* | COMMIT(seq,crc)`.
Each record is crc32c-guarded. Replay scans from `tail`; a transaction whose
COMMIT is present and valid is re-applied (after-images copied to home blocks);
the first torn/absent record stops replay cleanly (jbd2-style). After replay the
journal is reset empty.

**Tasks:**
- **J1 `journal.c`** — `ocsfs2_journal_init/exit` (read/validate journal header,
  set head/tail/seq), `ocsfs2_txn_begin(sb)`, `ocsfs2_txn_get(txn, bh)` (register
  a metadata buffer; dup-coalesced), `ocsfs2_txn_commit(txn)` (write BEGIN +
  METADATA after-images + COMMIT to the ring, `blkdev_issue_flush`, then
  checkpoint: write home blocks, flush, advance tail + persist header),
  `ocsfs2_txn_abort(txn)`. Ring wrap via monotonic seq; refuse a txn larger than
  the journal. Test: mount/unmount with journal init, `fsck` clean.
- **J2 `journal_replay.c`** — `ocsfs2_journal_replay(sb)` at mount before the root
  iget: forward scan, validate crcs, re-apply committed txns, stop on torn record,
  reset header. Test: hand-craft a dirty journal (write records, skip checkpoint)
  → remount → replay → home blocks updated → `fsck` clean.
- **J3 integration** — wrap metadata mutations in transactions: `create`/`mkdir`/
  `unlink`/`rmdir`/`rename` and the inode/bitmap/dirent block writes they touch go
  through `txn_get` instead of bare `mark_buffer_dirty`. Inode-number reserve and
  block bitmap updates join the same txn as the dirent + inode writes so a crash
  can't half-apply a create. Data path stays ordered (no change). Test: namespace
  gate still passes, `fsck` clean.
- **J4 crash test** — on a node: start a metadata-heavy loop, `echo b >
  /proc/sysrq-trigger` (hard crash), reboot, remount (replay), verify no partial
  ops / no orphans / `fsck` clean. Also: abort path, wrapped journal, torn tail.

**Exit:** crash mid-metadata-op → consistent after remount, `fsck` clean, zero
warnings. Note: this is single-node crash recovery; multi-node recovery (a peer
replaying a dead node's journal after fencing) is Plan 5.
