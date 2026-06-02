#!/bin/bash
# OCSFS v2 — Fase D4 discard/TRIM thin-reclaim (single-node gate).
# fstrim reports trimmed bytes; live data is never discarded (a kept file stays
# intact, new writes work); fsck clean. Usage: test_discard.sh <dev>
set -e
DEV="${1:?usage: test_discard.sh <dev>}"
cd /root/OCSFS
P=0; FA=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: got '$2' want '$3'"; FA=$((FA+1)); fi; }

cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -O2 -std=gnu11 tools2/fsck.c -o fsck.ocsfs2 2>/dev/null || true
umount /mnt/o2 2>/dev/null || true; rmmod ocsfs2 2>/dev/null || true
insmod kmod2/ocsfs2.ko
./mkfs.ocsfs2 -f -N 1 -L tr "$DEV" >/dev/null
mount -t ocsfs2 "$DEV" /mnt/o2

echo "=== keep one file, churn another, then trim ==="
head -c 4194304 /dev/zero | tr '\0' 'K' > /mnt/o2/keep      # 4 MiB, must survive
S0=$(sha256sum /mnt/o2/keep | awk '{print $1}')
head -c 209715200 /dev/zero | tr '\0' 'B' > /mnt/o2/churn   # 200 MiB
sync
rm -f /mnt/o2/churn
sync; sleep 1

TR=$(fstrim -v /mnt/o2 2>&1); echo "  $TR"
BYTES=$(echo "$TR" | grep -oE '[0-9]+ bytes' | grep -oE '[0-9]+' | head -1)
ck "fstrim trimmed >= 200 MiB" "$([ -n "$BYTES" ] && [ "$BYTES" -ge 209715200 ] && echo yes || echo "no($BYTES)")" "yes"

echo "=== kept file survived the trim ==="
ck "keep content intact" "$(sha256sum /mnt/o2/keep | awk '{print $1}')" "$S0"

echo "=== fs still writable after trim ==="
head -c 1048576 /dev/zero | tr '\0' 'N' > /mnt/o2/newf
sync
ck "new file written + read" "$(head -c 8 /mnt/o2/newf)" "NNNNNNNN"
ck "keep still intact after new writes" "$(sha256sum /mnt/o2/keep | awk '{print $1}')" "$S0"

echo "=== a second trim is a near-noop (already trimmed) ==="
fstrim -v /mnt/o2 >/dev/null 2>&1
ck "second fstrim ok" "$?" "0"

echo "=== cleanup ==="
umount /mnt/o2; rmmod ocsfs2
./fsck.ocsfs2 "$DEV" 2>/dev/null | tail -1; FRC=${PIPESTATUS[0]}
ck "fsck clean" "$FRC" "0"
echo "DISCARD_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo DISCARD_DONE || { echo DISCARD_FAILED; exit 1; }
