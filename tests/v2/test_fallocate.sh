#!/bin/bash
# OCSFS v2 — fallocate (preallocate UNWRITTEN / punch hole / zero range) +
# fiemap + SEEK_HOLE/SEEK_DATA gate. Run ON a Proxmox node.
# Usage: test_fallocate.sh <ocsfs2_dev>
set -e
DEV="${1:?usage: test_fallocate.sh <ocsfs2_dev>}"
cd /root/OCSFS
cc -Wall -Werror -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -Wall -Werror -std=gnu11 tools2/fsck.c -o fsck.ocsfs2
rmmod ocsfs2 2>/dev/null || true
insmod kmod2/ocsfs2.ko
./mkfs.ocsfs2 -f -L falloc "$DEV" >/dev/null
mkdir -p /mnt/o2; mount -t ocsfs2 "$DEV" /mnt/o2
M=/mnt/o2
drop() { sync; echo 3 > /proc/sys/vm/drop_caches; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
ZERO1M=$(head -c 1048576 /dev/zero | sha256sum | cut -d' ' -f1)
ZERO2M=$(head -c 2097152 /dev/zero | sha256sum | cut -d' ' -f1)
ZERO4M=$(head -c 4194304 /dev/zero | sha256sum | cut -d' ' -f1)
P=0; F=0
ck()   { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: '$2' != '$3'"; F=$((F+1)); fi; }
ckge() { if [ "$2" -ge "$3" ]; then echo "  PASS $1 ($2 >= $3)"; P=$((P+1)); else echo "  FAIL $1 ($2 < $3)"; F=$((F+1)); fi; }
cklt() { if [ "$2" -lt "$3" ]; then echo "  PASS $1 ($2 < $3)"; P=$((P+1)); else echo "  FAIL $1 ($2 !< $3)"; F=$((F+1)); fi; }
seg() { dd if="$1" bs=1M skip="$2" count="$3" status=none 2>/dev/null | sha256sum | cut -d' ' -f1; }

# ── 1. preallocate (UNWRITTEN): size set, blocks reserved, reads zero ──
fallocate -l 8388608 $M/pre
ck   "prealloc size 8M"            "$(stat -c %s $M/pre)" "8388608"
ckge "prealloc blocks reserved"    "$(stat -c %b $M/pre)" "16000"
drop
ck   "prealloc reads zero (8M)"    "$(seg $M/pre 0 8)" "$(head -c 8388608 /dev/zero|sha256sum|cut -d' ' -f1)"
echo "  fiemap(pre):"; filefrag -v $M/pre 2>/dev/null | grep -iE "unwritten|extent:" | head -3 | sed 's/^/    /'

# ── 2. write into preallocated region -> conversion, cold read ──
printf 'HELLO' | dd of=$M/pre bs=1 seek=1048576 conv=notrunc status=none
drop
ck   "prealloc write read-back"    "$(dd if=$M/pre bs=1 skip=1048576 count=5 status=none)" "HELLO"
ck   "prealloc head still zero(1M)" "$(seg $M/pre 0 1)" "$ZERO1M"

# ── 3. punch hole: middle reads zero, head/tail intact, blocks freed ──
dd if=/dev/urandom of=$M/f2 bs=1M count=16 status=none; drop
H0=$(seg $M/f2 0 4); T0=$(seg $M/f2 8 8); B0=$(stat -c %b $M/f2)
fallocate -p -o 4194304 -l 4194304 $M/f2; drop
ck   "punch middle reads zero"     "$(seg $M/f2 4 4)" "$ZERO4M"
ck   "punch head intact"           "$(seg $M/f2 0 4)" "$H0"
ck   "punch tail intact"           "$(seg $M/f2 8 8)" "$T0"
cklt "punch freed blocks"          "$(stat -c %b $M/f2)" "$B0"
ck   "punch keeps size"            "$(stat -c %s $M/f2)" "16777216"

# ── 4. zero range: range reads zero, rest intact ──
dd if=/dev/urandom of=$M/f3 bs=1M count=8 status=none; drop
Z_H=$(seg $M/f3 0 2); Z_T=$(seg $M/f3 4 4)
fallocate -z -o 2097152 -l 2097152 $M/f3; drop
ck   "zero-range reads zero"       "$(seg $M/f3 2 2)" "$ZERO2M"
ck   "zero-range head intact"      "$(seg $M/f3 0 2)" "$Z_H"
ck   "zero-range tail intact"      "$(seg $M/f3 4 4)" "$Z_T"

# ── 5. SEEK_HOLE / SEEK_DATA on a sparse file ──
xfs_io -f -c "pwrite 0 4096" -c "pwrite 8388608 4096" $M/sp >/dev/null; drop
SH=$(xfs_io -c "seek -h 0" $M/sp | awk 'NR==2{print $2}')
SD=$(xfs_io -c "seek -d 4096" $M/sp | awk 'NR==2{print $2}')
ck   "SEEK_HOLE from 0 -> 4096"    "$SH" "4096"
ck   "SEEK_DATA from 4096 -> 8388608" "$SD" "8388608"

umount $M
./fsck.ocsfs2 "$DEV"
rmmod ocsfs2
echo "=== dmesg tail ==="; dmesg | tail -5
echo "FALLOCATE_TEST: $P passed, $F failed"
[ "$F" -eq 0 ] && echo FALLOCATE_TEST_DONE || { echo FALLOCATE_TEST_FAILED; exit 1; }
