#!/bin/bash
# OCSFS v2 — POSIX namespace: symlink + hardlink + mknod + persistence gate.
# Usage: test_posix.sh <ocsfs2_dev>   (run ON a Proxmox node)
set -e
DEV="${1:?usage: test_posix.sh <ocsfs2_dev>}"
cd /root/OCSFS
cc -Wall -Werror -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -Wall -Werror -std=gnu11 tools2/fsck.c -o fsck.ocsfs2
rmmod ocsfs2 2>/dev/null || true
insmod kmod2/ocsfs2.ko
./mkfs.ocsfs2 -f -L posix "$DEV" >/dev/null
mkdir -p /mnt/o2; mount -t ocsfs2 "$DEV" /mnt/o2
M=/mnt/o2
P=0; FA=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: '$2' != '$3'"; FA=$((FA+1)); fi; }

# ── symlink ──
echo "hello-target" > $M/tgt
ln -s tgt $M/sym
ck "readlink target"        "$(readlink $M/sym)" "tgt"
ck "read via symlink"       "$(cat $M/sym)" "hello-target"
ln -s /abs/long/path/that/is/somewhat/long $M/symabs
ck "readlink absolute"      "$(readlink $M/symabs)" "/abs/long/path/that/is/somewhat/long"

# ── hardlink ──
echo "shared-data" > $M/f
ln $M/f $M/h
ck "hardlink same inode"    "$(stat -c %i $M/f)" "$(stat -c %i $M/h)"
ck "hardlink nlink 2"       "$(stat -c %h $M/f)" "2"
echo "updated" > $M/f
ck "read via hardlink sees update" "$(cat $M/h)" "updated"
rm $M/f
ck "after rm one link nlink 1" "$(stat -c %h $M/h)" "1"
ck "remaining link keeps data" "$(cat $M/h)" "updated"

# ── mknod (special files) ──
mkfifo $M/fifo
ck "fifo type"              "$(stat -c %F $M/fifo)" "fifo"
mknod $M/chr c 1 5
ck "chrdev type"           "$(stat -c %F $M/chr)" "character special file"
ck "chrdev major:minor"    "$(stat -c '%t:%T' $M/chr)" "1:5"
mknod $M/blk b 7 0
ck "blkdev type"           "$(stat -c %F $M/blk)" "block special file"

# ── persistence across remount ──
sync; umount $M; mount -t ocsfs2 "$DEV" $M
ck "symlink survives remount"   "$(readlink $M/sym)" "tgt"
ck "symlink abs survives"       "$(readlink $M/symabs)" "/abs/long/path/that/is/somewhat/long"
ck "hardlink data survives"     "$(cat $M/h)" "updated"
ck "fifo survives remount"      "$(stat -c %F $M/fifo)" "fifo"
ck "chrdev survives remount"    "$(stat -c '%t:%T' $M/chr)" "1:5"

umount $M
./fsck.ocsfs2 "$DEV"
rmmod ocsfs2
echo "=== dmesg tail ==="; dmesg | tail -4
echo "POSIX_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo POSIX_TEST_DONE || { echo POSIX_TEST_FAILED; exit 1; }
