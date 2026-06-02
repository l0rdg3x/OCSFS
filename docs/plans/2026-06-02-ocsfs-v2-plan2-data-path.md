# OCSFS v2 — Plan 2: Data path (file content I/O)

**Goal:** Regular files hold data. Buffered + O_DIRECT read/write via iomap,
sparse files (holes read as zero), sub-block partial writes (RMW), append,
truncate grow/shrink. Single-node; inline extents only (≤16; B+tree spill is
Plan 2b). Integrity gate: sha256 round-trip across `drop_caches`, sparse + partial
patterns, all cross-checked against ext4 on a real iSCSI LUN.

**Architecture:** Single-writer makes this simple — no DLM, no cache-coherence,
no UNWRITTEN-conversion dance. `iomap_begin` maps an existing extent, or on
IOMAP_WRITE allocates a contiguous run and inserts it as `IOMAP_MAPPED |
IOMAP_F_NEW` (iomap zeroes partial blocks), or on read/zero returns a hole
**clamped to the next allocated extent** (the v1 #13 lesson: never let a hole
swallow following mapped blocks). Fresh blocks are written through the page
cache / DIO; no unwritten extents in Plan 2 (fallocate is later).

**Tasks (E1–E3):**
- **E1** `inode.c`: `ocsfs2_extent_find(inode, lblk, *cover, *next_logical)`,
  `ocsfs2_extent_insert(inode, logical, phys, len, flags)` (sorted + merge
  contiguous), `ocsfs2_extent_truncate_from(inode, from_block)` (free blocks ≥
  from_block, trim/drop extents). Reuse for `ocsfs2_bmap` / dir `append_block`.
  Caller holds `i_meta_lock`.
- **E2** `iomap.c`: `ocsfs2_iomap_begin/end`, `ocsfs2_file_aops`
  (`read_folio`=`iomap_bio_read_folio`, `readahead`=`iomap_bio_readahead`,
  `writepages` via `iomap_writepage_ctx`+`writeback_range`/`writeback_submit`),
  `ocsfs2_file_read_iter`/`write_iter` (buffered: `filemap_read` /
  `iomap_file_buffered_write`), `ocsfs2_file_fops` (moved here). Wire
  `i_mapping->a_ops` in `iget`/`new_inode` for regular files. Alloc under
  `i_meta_lock` + `memalloc_nofs` (lock order i_meta_lock→ag_lock).
- **E3**: O_DIRECT (`iomap_dio_rw`) in the iters; `ocsfs2_setattr` ATTR_SIZE →
  `truncate_setsize` + `ocsfs2_extent_truncate_from` on shrink.

**Tests (`tests/v2/test_datapath.sh`, on a node, vs ext4):**
1. 64 MiB write, `sync`, `echo 3>drop_caches`, read back, sha256 match (buffered).
2. Same with `dd oflag=direct iflag=direct` (O_DIRECT).
3. Sparse: write 4 KiB at offset 8 MiB; file size 8 MiB+4 KiB; `[0,8MiB)` reads
   zero; `du` shows ~1 block (thin).
4. Sub-block partial overwrite: write 100 bytes at offset 50 in a block, cold
   read, compare to ext4 (RMW correctness — v1's bug area).
5. Truncate grow (sparse tail reads zero) and shrink (blocks freed, `df` drops),
   then `fsck` clean.
6. `fsck` clean after every scenario; cross-check the byte image vs ext4.

**Exit:** all six pass, `fsck` clean, zero kernel warnings. Heavy fragmentation
(fsx) and fallocate/punch deferred to Plan 2b (extent B+tree).
