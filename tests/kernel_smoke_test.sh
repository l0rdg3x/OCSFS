#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# kernel_smoke_test.sh — single-node loopback smoke test for the OCSFS kernel
# module.  Requires root (insmod / mount / losetup).  This is the definitive
# check that mkfs-formatted volumes mount and round-trip data through the actual
# kernel module — it exercises the Sprint R layout fix and the CRC32C convention
# fix end to end.
#
# Usage:  sudo bash tests/kernel_smoke_test.sh
#
# It is non-destructive to the host: it works on a loopback-backed image in
# /tmp and cleans up after itself.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG=/tmp/ocsfs_smoke.img
MNT=/tmp/ocsfs_smoke_mnt
KO="$ROOT/kmod/ocsfs.ko"
MKFS="$ROOT/mkfs.ocsfs"
LOOP=""
FAIL=0

log()  { echo ":: $*"; }
ok()   { echo "  PASS: $*"; }
bad()  { echo "  FAIL: $*"; FAIL=1; }

cleanup() {
    mountpoint -q "$MNT" && umount "$MNT" 2>/dev/null
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
    rmmod ocsfs 2>/dev/null
    rm -f "$IMG"; rmdir "$MNT" 2>/dev/null
}
trap cleanup EXIT

[ "$(id -u)" -eq 0 ] || { echo "must run as root (sudo)"; exit 1; }
[ -f "$KO" ]   || { echo "kernel module not built: $KO (run: cd kmod && make)"; exit 1; }
[ -x "$MKFS" ] || { echo "mkfs not built: $MKFS (run: make mkfs.ocsfs)"; exit 1; }

log "format 2 GiB image (4 nodes)"
rm -f "$IMG"; truncate -s 2G "$IMG"
"$MKFS" -L smoke -N 4 -f "$IMG" >/dev/null || { bad "mkfs"; exit 1; }

log "load module + attach loopback"
rmmod ocsfs 2>/dev/null
# insmod does not pull in dependencies; load them first (the module references
# LZ4_compress_default from lz4_compress).
modprobe lz4_compress 2>/dev/null
insmod "$KO" || { bad "insmod (check: dmesg | tail)"; dmesg | tail -5; exit 1; }
LOOP="$(losetup -f --show "$IMG")" || { bad "losetup"; exit 1; }
mkdir -p "$MNT"

log "mount (single node, no cluster secret)"
if mount -t ocsfs -o degraded "$LOOP" "$MNT"; then
    ok "mount succeeded — superblock CRC accepted by kernel"
else
    bad "mount failed — check 'dmesg | tail' (CRC/layout mismatch?)"
    dmesg | tail -15
    exit 1
fi

log "data integrity round-trip (8 MiB)"
dd if=/dev/urandom of=/tmp/ocsfs_smoke.src bs=1M count=8 status=none
cp /tmp/ocsfs_smoke.src "$MNT/data.bin"
S1=$(sha256sum < /tmp/ocsfs_smoke.src | cut -d' ' -f1)
S2=$(sha256sum < "$MNT/data.bin"     | cut -d' ' -f1)
[ "$S1" = "$S2" ] && ok "8 MiB sha256 match" || bad "data corrupted"

log "metadata ops"
mkdir -p "$MNT/a/b/c" && echo deep > "$MNT/a/b/c/leaf" && ok "nested mkdir+write" || bad "nested"
for i in $(seq 1 30); do echo "c$i" > "$MNT/f$i"; done
[ "$(ls "$MNT" | grep -c '^f')" = 30 ] && ok "30 files created" || bad "file create"
for i in $(seq 1 2 30); do rm "$MNT/f$i"; done
ls "$MNT" >/dev/null 2>&1 && ok "readdir after removals (no EIO)" || bad "readdir EIO"
truncate -s 4 "$MNT/data.bin"; [ "$(stat -c %s "$MNT/data.bin")" = 4 ] && ok "truncate" || bad "truncate"

log "fsync + remount persistence"
sync
umount "$MNT"
mount -t ocsfs -o degraded "$LOOP" "$MNT" || { bad "remount"; dmesg | tail -15; exit 1; }
[ "$(cat "$MNT/a/b/c/leaf" 2>/dev/null)" = deep ] && ok "nested persisted" || bad "nested lost"
[ "$(ls "$MNT" | grep -c '^f')" = 15 ] && ok "15 files persisted" || bad "file count after remount"
umount "$MNT"

log "offline fsck"
if python3 "$ROOT/tools/ocsfs-fsck" "$IMG" 2>&1 | tail -1 | grep -q clean; then
    ok "fsck clean"
else
    bad "fsck reported errors"
fi

echo
if [ "$FAIL" = 0 ]; then
    echo "==== KERNEL SMOKE TEST: ALL PASS ===="
else
    echo "==== KERNEL SMOKE TEST: FAILURES (see above + 'dmesg | tail') ===="
fi
exit $FAIL
