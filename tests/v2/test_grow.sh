#!/bin/bash
# OCSFS v2 — Fase D2 autonomous online autogrow (single-node gate).
# Format only a 2 GiB prefix of a larger LUN, then (a) force-grow via ioctl and
# (b) let the autonomous watcher grow it; verify the fs span increases, an
# existing file survives, the new AGs are usable (write past the old capacity),
# fsck clean. Usage: test_grow.sh <dev>   (dev must be >= ~6 GiB)
set -e
DEV="${1:?usage: test_grow.sh <dev>}"
cd /root/OCSFS
P=0; FA=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: got '$2' want '$3'"; FA=$((FA+1)); fi; }
total() { df -P --block-size=1 /mnt/o2 | awk 'NR==2{print $2}'; }

cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -O2 -std=gnu11 tools2/fsck.c -o fsck.ocsfs2 2>/dev/null || true
cc -O2 -std=gnu11 tests/v2/growfs_tool.c -o growfs_tool
umount /mnt/o2 2>/dev/null || true; rmmod ocsfs2 2>/dev/null || true
insmod kmod2/ocsfs2.ko

echo "=== format a 2 GiB prefix, mount ==="
./mkfs.ocsfs2 -f -N 1 -s $((2*1024*1024*1024)) -L grow "$DEV" | grep ags=
mount -t ocsfs2 "$DEV" /mnt/o2
T0=$(total); echo "  span before: $((T0/1024/1024)) MiB"
dd if=/dev/urandom of=/mnt/o2/keep bs=1M count=100 status=none   # 100 MiB, real data
S0=$(sha256sum /mnt/o2/keep | awk '{print $1}')
sync

echo "=== force grow via ioctl ==="
./growfs_tool /mnt/o2
sync; T1=$(total); echo "  span after ioctl: $((T1/1024/1024)) MiB"
ck "fs span grew (ioctl)" "$([ "$T1" -gt "$T0" ] && echo yes || echo no)" "yes"
ck "keep intact after grow" "$(sha256sum /mnt/o2/keep | awk '{print $1}')" "$S0"
GREW=$(dmesg | grep -iE "ocsfs2: grew .* AGs" | tail -1); echo "  $GREW"
ck "kernel logged a grow" "$([ -n "$GREW" ] && echo yes || echo no)" "yes"

echo "=== new AGs are usable: write 3 GiB (past the old 2 GiB) ==="
if dd if=/dev/zero of=/mnt/o2/big bs=1M count=3072 status=none 2>/tmp/big.err; then
	BIGOK=yes; else BIGOK="no($(cat /tmp/big.err))"; fi
sync
ck "3 GiB write into grown space" "$BIGOK" "yes"
ck "keep still intact after big write" "$(sha256sum /mnt/o2/keep | awk '{print $1}')" "$S0"

echo "=== cleanup + fsck ==="
umount /mnt/o2; rmmod ocsfs2
./fsck.ocsfs2 "$DEV" 2>/dev/null | tail -1; FRC=${PIPESTATUS[0]}
ck "fsck clean after grow" "$FRC" "0"

echo "=== autonomous watcher (re-format prefix, wait, auto-grow) ==="
insmod kmod2/ocsfs2.ko
./mkfs.ocsfs2 -f -N 1 -s $((2*1024*1024*1024)) -L grow "$DEV" >/dev/null
mount -t ocsfs2 "$DEV" /mnt/o2
TA=$(total)
echo "  waiting 35s for the grow watcher..."
sleep 35
TB=$(total); echo "  span: $((TA/1024/1024)) -> $((TB/1024/1024)) MiB"
ck "watcher auto-grew the fs" "$([ "$TB" -gt "$TA" ] && echo yes || echo no)" "yes"
umount /mnt/o2; rmmod ocsfs2
./fsck.ocsfs2 "$DEV" 2>/dev/null | tail -1; FRC2=${PIPESTATUS[0]}
ck "fsck clean after autonomous grow" "$FRC2" "0"

echo "GROW_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo GROW_DONE || { echo GROW_FAILED; exit 1; }
