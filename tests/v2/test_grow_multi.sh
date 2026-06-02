#!/bin/bash
# OCSFS v2 — Fase D2 REPEATED online grow (unlike v1's one-shot grow).
# A dm-linear target over the real iSCSI LUN lets us present the same backing
# store at growing sizes (2 GiB -> 6 GiB -> 14 GiB -> full): we grow at each
# step, several times in one mount, data surviving throughout. Not loopback —
# the backing store is the real LUN, only its visible size is stepped via dm.
# Usage: test_grow_multi.sh <dev>   (>= ~16 GiB)
set -e
DEV="${1:?usage: test_grow_multi.sh <dev>}"
cd /root/OCSFS
DM=ocsfs2grow
DMDEV=/dev/mapper/$DM
P=0; FA=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: got '$2' want '$3'"; FA=$((FA+1)); fi; }
total() { df -P --block-size=1 /mnt/o2 | awk 'NR==2{print $2}'; }
secs() { echo $(( $1 * 1024 * 1024 * 1024 / 512 )); }   # GiB -> 512B sectors
mklinear() { dmsetup "$1" $DM --table "0 $(secs $2) linear $DEV 0"; }

cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -O2 -std=gnu11 tools2/fsck.c -o fsck.ocsfs2 2>/dev/null || true
cc -O2 -std=gnu11 tests/v2/growfs_tool.c -o growfs_tool
umount /mnt/o2 2>/dev/null || true; rmmod ocsfs2 2>/dev/null || true
dmsetup remove $DM 2>/dev/null || true
insmod kmod2/ocsfs2.ko

echo "=== present LUN as 2 GiB via dm-linear, format, mount ==="
mklinear create 2
./mkfs.ocsfs2 -f -N 1 -L mg "$DMDEV" | grep ags=
mount -t ocsfs2 "$DMDEV" /mnt/o2
dd if=/dev/urandom of=/mnt/o2/keep bs=1M count=50 status=none   # 50 MiB, real data
S0=$(sha256sum /mnt/o2/keep | awk '{print $1}')
sync
T_PREV=$(total); echo "  span: $((T_PREV/1024/1024)) MiB"

grow_step() {   # $1 = new GiB size
	echo "=== grow step -> $1 GiB ==="
	mklinear reload "$1"
	dmsetup resume $DM
	./growfs_tool /mnt/o2 >/dev/null
	sync
	local T=$(total)
	ck "span grew at $1 GiB step" "$([ "$T" -gt "$T_PREV" ] && echo yes || echo no)" "yes"
	ck "keep intact after $1 GiB grow" "$(sha256sum /mnt/o2/keep | awk '{print $1}')" "$S0"
	echo "  span: $((T_PREV/1024/1024)) -> $((T/1024/1024)) MiB"
	T_PREV=$T
}

grow_step 6
grow_step 14
grow_step 28

echo "=== new space usable: write 5 GiB into grown AGs ==="
dd if=/dev/zero of=/mnt/o2/big bs=1M count=5120 status=none 2>/tmp/big.err && BIGOK=yes || BIGOK="no($(cat /tmp/big.err))"
sync
ck "5 GiB write across grown AGs" "$BIGOK" "yes"
ck "keep still intact" "$(sha256sum /mnt/o2/keep | awk '{print $1}')" "$S0"

echo "=== cleanup + fsck ==="
umount /mnt/o2; rmmod ocsfs2
./fsck.ocsfs2 "$DMDEV" 2>/dev/null | tail -1; FRC=${PIPESTATUS[0]}
ck "fsck clean after 3 grows" "$FRC" "0"
dmsetup remove $DM
echo "GROW_MULTI_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo GROW_MULTI_DONE || { echo GROW_MULTI_FAILED; exit 1; }
