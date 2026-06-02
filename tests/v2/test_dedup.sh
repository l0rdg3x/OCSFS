#!/bin/bash
# OCSFS v2 — Fase D1 cross-file dedup (FIDEDUPERANGE). Single-node integrity gate.
# Identical files -> shared storage (df drops); content preserved; a write to one
# CoWs and diverges (df rises, the other is untouched); non-identical files dedup
# nothing; fsck clean. Usage: test_dedup.sh <dev>
set -e
DEV="${1:?usage: test_dedup.sh <dev>}"
cd /root/OCSFS
P=0; FA=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: got '$2' want '$3'"; FA=$((FA+1)); fi; }
used() { df -P /mnt/o2 | awk 'NR==2{print $3}'; }   # used KiB

cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -O2 -std=gnu11 tools2/fsck.c -o fsck.ocsfs2 2>/dev/null || true
cc -O2 -std=gnu11 tests/v2/dedup_tool.c -o dedup_tool
umount /mnt/o2 2>/dev/null || true; rmmod ocsfs2 2>/dev/null || true
insmod kmod2/ocsfs2.ko
./mkfs.ocsfs2 -f -N 1 -L dd "$DEV" >/dev/null
mount -t ocsfs2 "$DEV" /mnt/o2

echo "=== identical files ==="
head -c 4194304 /dev/zero | tr '\0' 'A' > /mnt/o2/a
head -c 4194304 /dev/zero | tr '\0' 'A' > /mnt/o2/b
sync; U0=$(used)
./dedup_tool /mnt/o2/a /mnt/o2/b 4194304 | tee /tmp/dd.out
DED=$(awk -F'[= ]' '{print $2}' /tmp/dd.out)
STA=$(awk -F'[= ]' '{print $4}' /tmp/dd.out)
ck "dedup reported 4 MiB shared" "$DED" "4194304"
ck "dedup status ok" "$STA" "0"
sync; sleep 1; U1=$(used)
# used should drop by ~4 MiB (4096 KiB), allow slack for metadata rounding
DROP=$((U0 - U1))
ck "space reclaimed (~4MiB)" "$([ "$DROP" -ge 3500 ] && echo yes || echo "no($DROP)")" "yes"
ck "b content intact (A)" "$(head -c 8 /mnt/o2/b)" "AAAAAAAA"
ck "a content intact (A)" "$(head -c 8 /mnt/o2/a)" "AAAAAAAA"

echo "=== CoW divergence: write to b ==="
echo -n "ZZZZZZZZ" | dd of=/mnt/o2/b bs=1 count=8 conv=notrunc 2>/dev/null
sync; sleep 1; U2=$(used)
ck "b now starts ZZZ (diverged)" "$(head -c 8 /mnt/o2/b)" "ZZZZZZZZ"
ck "a still AAA (isolated by CoW)" "$(head -c 8 /mnt/o2/a)" "AAAAAAAA"
ck "space grew back after CoW" "$([ "$U2" -gt "$U1" ] && echo yes || echo no)" "yes"

echo "=== non-identical files dedup nothing ==="
head -c 1048576 /dev/zero | tr '\0' 'X' > /mnt/o2/c
head -c 1048576 /dev/zero | tr '\0' 'Y' > /mnt/o2/d
sync
./dedup_tool /mnt/o2/c /mnt/o2/d 1048576 | tee /tmp/dd2.out
ND=$(awk -F'[= ]' '{print $2}' /tmp/dd2.out)
ck "no bytes deduped for differing files" "$([ "$ND" = 0 ] && echo yes || echo no)" "yes"
ck "d content untouched (Y)" "$(head -c 8 /mnt/o2/d)" "YYYYYYYY"

echo "=== cleanup ==="
umount /mnt/o2; rmmod ocsfs2
./fsck.ocsfs2 "$DEV" 2>/dev/null | tail -1; FRC=${PIPESTATUS[0]}
ck "fsck clean" "$FRC" "0"
echo "DEDUP_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo DEDUP_DONE || { echo DEDUP_FAILED; exit 1; }
