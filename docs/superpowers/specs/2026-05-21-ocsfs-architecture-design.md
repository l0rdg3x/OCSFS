# OCSFS — Architecture Design Spec

**Date:** 2026-05-21  
**Status:** Approved  
**Scope:** Documentation audit + gap analysis for OCSFS v0.1

## What we are building

Three documentation files for the OCSFS project:

1. `docs/developer-guide.md` — Technical reference for kernel developers
2. `docs/admin-guide.md` — Operational guide for Proxmox administrators
3. `docs/gap-analysis.md` — Bug tracker + test plan

## Architecture (existing system, documented)

OCSFS is a Linux kernel filesystem module targeting FC SAN shared storage for Proxmox VE clusters. Core design: on-disk distributed locking via SCSI CAW (currently soft-versioning), storage-path heartbeat, SCSI-3 PR fencing, per-node journaling.

## Decisions made during audit

- Doc audience: developers (technical) + Proxmox admins (operational) — separate docs
- Format: modular (3 files) + this spec doc
- Gap analysis is standalone file, not inline in developer guide
- README "all phases complete" is aspirational; documented actual state accurately

## Key findings

### Critical bugs
- BUG-001: inode struct size mismatch kmod vs userspace (492 vs 512 bytes)
- BUG-002: deadlock in ocsfs_lock_downgrade when downgrading to NL
- BUG-003: TOCTOU race in lock acquisition — needs real SCSI CAW

### Missing features
- SCSI CAW implementation (critical for multi-node)
- B+ tree in kmod for large directories and extent overflow
- KUnit tests (none exist)
- Multi-LUN spanning, encryption, deduplication (flags defined, no code)

## Test plan summary

- Level 0: existing userspace tests (pass)
- Level 1: single-node kmod + xfstests generic/quick
- Level 2: xfstests full suite
- Level 3: two-node VM simulation (after BUG-001/002/003 fixed)
- Level 4: real FC hardware with SCSI PR
- Level 5: fault injection + sustained load
- Level 6: KUnit (to be implemented)

## Sprint priorities

1. Fix BUG-001/002 → single-node stability
2. Implement SCSI CAW → multi-node correctness
3. Port btree.c to kmod → large directories + extent overflow
4. Proxmox HA callback → production readiness
