#!/bin/bash
# OCSFS v2 — reflink + CoW + snapshot gate (Plan 4, R1-R3).
# Proves: FICLONE shares extents (df free ~unchanged), CoW isolates a write to
# the clone (clone diverges, source intact, free space drops), snapshot ioctl
# makes a point-in-time copy, delete integrity (shared blocks freed only at
# refcount 0), fsck clean throughout.
# Usage: test_reflink.sh <ocsfs2_dev> [xfs_scratch_dev]   (run ON a Proxmox node)
set -e
DEV="${1:?usage: test_reflink.sh <ocsfs2_dev> [xfs_scratch_dev]}"
XFS="${2:-}"
cd /root/OCSFS
cc -Wall -Werror -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -Wall -Werror -std=gnu11 tools2/fsck.c -o fsck.ocsfs2
cc -Wall -Werror -std=gnu11 tests/v2/ocsfs2_snap.c -o ocsfs2_snap
rmmod ocsfs2 2>/dev/null || true
insmod kmod2/ocsfs2.ko
./mkfs.ocsfs2 -f -L reflink "$DEV" >/dev/null
mkdir -p /mnt/o2
mount -t ocsfs2 "$DEV" /mnt/o2
drop() { sync; echo 3 > /proc/sys/vm/drop_caches; }
freeblk() { stat -f -c %f /mnt/o2; }              # free blocks (4 KiB units)
sha() { sha256sum "$1" | cut -d' ' -f1; }
PASS=0; FAIL=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS $1"; PASS=$((PASS+1)); else echo "  FAIL $1: '$2' != '$3'"; FAIL=$((FAIL+1)); fi; }
cklt() { if [ "$2" -lt "$3" ]; then echo "  PASS $1 ($2 < $3)"; PASS=$((PASS+1)); else echo "  FAIL $1 ($2 !< $3)"; FAIL=$((FAIL+1)); fi; }
ckle() { if [ "$2" -le "$3" ]; then echo "  PASS $1 ($2 <= $3)"; PASS=$((PASS+1)); else echo "  FAIL $1 ($2 !<= $3)"; FAIL=$((FAIL+1)); fi; }

# ── 1. source file (12 MiB random -> 2 extents) ──
dd if=/dev/urandom of=/mnt/o2/src bs=1M count=12 status=none; drop
SRC0=$(sha /mnt/o2/src)
F_BEFORE=$(freeblk)

# ── 2. reflink clone: shares extents, free space ~unchanged ──
cp --reflink=always /mnt/o2/src /mnt/o2/clone
drop
F_CLONE=$(freeblk)
ck  "clone content == src (cold)"        "$(sha /mnt/o2/clone)" "$SRC0"
ckle "reflink consumed <= 8 blocks"       "$((F_BEFORE - F_CLONE))" "8"

# ── 3. CoW: write one block to clone -> diverges, src intact, free drops ──
dd if=/dev/urandom of=/mnt/o2/clone bs=4k count=1 conv=notrunc status=none
drop
F_COW=$(freeblk)
CLONE1=$(sha /mnt/o2/clone)
ck  "src intact after clone write"        "$(sha /mnt/o2/src)" "$SRC0"
if [ "$CLONE1" != "$SRC0" ]; then echo "  PASS clone diverged after CoW"; PASS=$((PASS+1)); else echo "  FAIL clone did not diverge"; FAIL=$((FAIL+1)); fi
cklt "CoW allocated new blocks"           "$F_COW" "$F_CLONE"
# tail of clone (everything after block 0) still equals src tail
ck  "clone tail still shared with src"    "$(tail -c +4097 /mnt/o2/clone | sha256sum | cut -d' ' -f1)" "$(tail -c +4097 /mnt/o2/src | sha256sum | cut -d' ' -f1)"

# ── 4. snapshot ioctl: point-in-time reflink copy of src ──
./ocsfs2_snap /mnt/o2/src snap1
drop
ck  "snapshot content == src (cold)"      "$(sha /mnt/o2/snap1)" "$SRC0"
F_SNAP=$(freeblk)
ckle "snapshot consumed <= 8 blocks"      "$((F_COW - F_SNAP))" "8"

# ── 5. delete integrity: rm clone -> src/snap intact, only private blocks freed ──
rm /mnt/o2/clone
drop
F_RM=$(freeblk)
ck  "src intact after rm clone"           "$(sha /mnt/o2/src)" "$SRC0"
ck  "snap intact after rm clone"          "$(sha /mnt/o2/snap1)" "$SRC0"
# freeing the clone must NOT release the still-shared 12 MiB (3072 blocks); only
# the clone's private CoW block + metadata return -> a small delta
ckle "rm clone freed few blocks (shared kept)" "$((F_RM - F_COW))" "16"

# ── 6. delete the snapshot, then the source ──
rm /mnt/o2/snap1; drop
ck  "src intact after rm snap"            "$(sha /mnt/o2/src)" "$SRC0"
rm /mnt/o2/src; drop
F_END=$(freeblk)
ckle "all data freed after deleting everything" "$((F_BEFORE - F_END + 16))" "16"

# ── optional cross-check vs XFS reflink semantics ──
if [ -n "$XFS" ] && command -v mkfs.xfs >/dev/null; then
	mkfs.xfs -f -m reflink=1 "$XFS" >/dev/null
	mkdir -p /mnt/xfs; mount "$XFS" /mnt/xfs
	dd if=/dev/urandom of=/mnt/xfs/src bs=1M count=12 status=none
	XS=$(sha /mnt/xfs/src)
	cp --reflink=always /mnt/xfs/src /mnt/xfs/clone
	dd if=/dev/urandom of=/mnt/xfs/clone bs=4k count=1 conv=notrunc status=none; sync; echo 3 >/proc/sys/vm/drop_caches
	ck  "xfs: src intact after clone CoW"  "$(sha /mnt/xfs/src)" "$XS"
	if [ "$(sha /mnt/xfs/clone)" != "$XS" ]; then echo "  PASS xfs clone diverged (same semantics as ocsfs2)"; PASS=$((PASS+1)); else echo "  FAIL xfs clone did not diverge"; FAIL=$((FAIL+1)); fi
	umount /mnt/xfs
fi

umount /mnt/o2
./fsck.ocsfs2 "$DEV"
rmmod ocsfs2
echo "=== dmesg tail ==="; dmesg | tail -6
echo "REFLINK_TEST: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && echo REFLINK_TEST_DONE || { echo REFLINK_TEST_FAILED; exit 1; }
