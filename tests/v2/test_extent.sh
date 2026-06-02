#!/bin/bash
# OCSFS v2 — extent B+tree (Plan 2b) gate: force a spill past 16 inline extents,
# verify data + fragmentation, then fsx-fuzz for deep integrity (heavy
# fragmentation = exercises the tree's insert/split/punch/truncate paths).
# Usage: test_extent.sh <ocsfs2_dev>   (run ON a Proxmox node)
set -e
DEV="${1:?usage: test_extent.sh <ocsfs2_dev>}"
cd /root/OCSFS
cc -Wall -Werror -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -Wall -Werror -std=gnu11 tools2/fsck.c -o fsck.ocsfs2
cc -O2 -Wall -std=gnu11 tests/ocsfs_fsx.c -o ocsfs_fsx
rmmod ocsfs2 2>/dev/null || true
insmod kmod2/ocsfs2.ko
./mkfs.ocsfs2 -f -L extent "$DEV" >/dev/null
mkdir -p /mnt/o2; mount -t ocsfs2 "$DEV" /mnt/o2
M=/mnt/o2
drop() { sync; echo 3 > /proc/sys/vm/drop_caches; }
P=0; FA=0
ck()   { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: '$2' != '$3'"; FA=$((FA+1)); fi; }
ckge() { if [ "$2" -ge "$3" ]; then echo "  PASS $1 ($2 >= $3)"; P=$((P+1)); else echo "  FAIL $1 ($2 < $3)"; FA=$((FA+1)); fi; }

# ── 1. force spill: 60 strided 4 KiB writes (gaps between) -> 60 extents ──
rm -f /tmp/ref
for i in $(seq 0 59); do
	off=$((i * 8192))
	xfs_io -f -c "pwrite -S 0xCC $off 4096" $M/frag >/dev/null
	xfs_io -f -c "pwrite -S 0xCC $off 4096" /tmp/ref >/dev/null
done
drop
EXT=$(filefrag -v $M/frag 2>/dev/null | grep -cE "^[[:space:]]+[0-9]+:")
ckge "frag file spilled to B+tree (>16 extents)" "${EXT:-0}" "17"
ck   "fragmented data matches reference"  "$(cmp -s $M/frag /tmp/ref && echo OK || echo DIFF)" "OK"

# ── 2. fill the holes (sequential rewrite) then verify ──
dd if=/dev/urandom of=/tmp/ref2 bs=1M count=1 status=none
cp /tmp/ref2 $M/seq; drop
ck   "sequential overwrite of spilled file" "$(cmp -s $M/seq /tmp/ref2 && echo OK || echo DIFF)" "OK"

# ── 3. punch + truncate on a spilled file ──
fallocate -p -o 16384 -l 8192 $M/frag        # punch 2 blocks in the middle
fallocate -p -o 16384 -l 8192 /tmp/ref
truncate -s 200000 $M/frag; truncate -s 200000 /tmp/ref
drop
ck   "punch+truncate on spilled file"     "$(cmp -s $M/frag /tmp/ref && echo OK || echo DIFF)" "OK"

# ── 4. delete a spilled file, fsck (frees all tree nodes + data) ──
rm $M/seq; drop
umount $M; ./fsck.ocsfs2 "$DEV" >/tmp/fsck1.txt 2>&1
ck   "fsck clean after spill ops"         "$(grep -c 'clean' /tmp/fsck1.txt)" "1"
mount -t ocsfs2 "$DEV" $M

# ── 5. fsx fuzzing (the integrity gate): heavy fragmentation, no mmap ──
FSXFAIL=0
for S in 1 2 3; do
	if ./ocsfs_fsx -N 30000 -l $((4*1024*1024)) -o 131072 -S $S -M $M/fsx_$S >/tmp/fsx_$S.log 2>&1; then
		echo "  PASS fsx seed=$S (30000 ops)"; P=$((P+1))
	else
		echo "  FAIL fsx seed=$S — tail:"; tail -8 /tmp/fsx_$S.log | sed 's/^/      /'; FA=$((FA+1)); FSXFAIL=1
	fi
done
# O_DIRECT fsx
if ./ocsfs_fsx -N 15000 -l $((4*1024*1024)) -o 131072 -S 7 -d -M $M/fsx_d >/tmp/fsx_d.log 2>&1; then
	echo "  PASS fsx O_DIRECT seed=7"; P=$((P+1))
else
	echo "  FAIL fsx O_DIRECT — tail:"; tail -8 /tmp/fsx_d.log | sed 's/^/      /'; FA=$((FA+1))
fi

umount $M
./fsck.ocsfs2 "$DEV"
rmmod ocsfs2
echo "=== dmesg tail ==="; dmesg | tail -5
echo "EXTENT_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo EXTENT_TEST_DONE || { echo EXTENT_TEST_FAILED; exit 1; }
