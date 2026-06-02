#!/bin/bash
# OCSFS v2 — cluster L4b directory coherence gate (2 nodes).
# Both nodes concurrently create files in the SAME directory; the global
# metadata lease must serialise + refresh so no entry is lost. Verified by a
# post-unmount single-node count (write coherence) + fsck. Run FROM n1.
# Usage: test_cluster_dir.sh <dev> <n2_ip>
set -e
DEV="${1:?usage: test_cluster_dir.sh <dev> <n2_ip>}"
N2="${2:?need n2 ip}"
cd /root/OCSFS
N=25   # files per node
P=0; FA=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: got '$2' want '$3'"; FA=$((FA+1)); fi; }

cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -O2 -std=gnu11 tools2/fsck.c -o fsck.ocsfs2
umount /mnt/o2 2>/dev/null || true; rmmod ocsfs2 2>/dev/null || true
ssh root@$N2 'umount /mnt/o2 2>/dev/null; rmmod ocsfs2 2>/dev/null' || true
insmod kmod2/ocsfs2.ko
scp -q kmod2/ocsfs2.ko root@$N2:/root/OCSFS/kmod2/ocsfs2.ko
ssh root@$N2 'insmod /root/OCSFS/kmod2/ocsfs2.ko'
./mkfs.ocsfs2 -f -N 3 -L clu "$DEV" >/dev/null

echo "=== mount both -o cluster ==="
mount -t ocsfs2 -o cluster "$DEV" /mnt/o2
ssh root@$N2 "mount -t ocsfs2 -o cluster $DEV /mnt/o2"
sleep 3

echo "=== concurrent create in shared dir ($N files/node) ==="
( for i in $(seq 0 $((N-1))); do echo n1 > /mnt/o2/a$i; done ) &
ssh root@$N2 "for i in \$(seq 0 $((N-1))); do echo n2 > /mnt/o2/b\$i; done"
wait
sync; ssh root@$N2 sync

echo "=== concurrent mkdir in shared dir ==="
( for i in $(seq 0 9); do mkdir /mnt/o2/da$i; done ) &
ssh root@$N2 "for i in \$(seq 0 9); do mkdir /mnt/o2/db\$i; done"
wait; sync; ssh root@$N2 sync

echo "=== unmount both, verify on a fresh single-node mount ==="
umount /mnt/o2; ssh root@$N2 'umount /mnt/o2'
mount -t ocsfs2 "$DEV" /mnt/o2
FILES=$(ls /mnt/o2 | grep -cE "^[ab][0-9]+$")
DIRS=$(ls /mnt/o2 | grep -cE "^d[ab][0-9]+$")
ck "all $((2*N)) files present (no lost entries)" "$FILES" "$((2*N))"
ck "all 20 dirs present"                          "$DIRS" "20"
# spot-check content
ck "a0 content" "$(cat /mnt/o2/a0)" "n1"
ck "b0 content" "$(cat /mnt/o2/b0)" "n2"
umount /mnt/o2

echo "=== fsck ==="
./fsck.ocsfs2 "$DEV" >/tmp/fk.txt 2>&1
ck "fsck clean" "$(grep -c 'clean' /tmp/fk.txt)" "1"
rmmod ocsfs2 2>/dev/null || true
ssh root@$N2 'rmmod ocsfs2 2>/dev/null' || true
echo "CLUSTER_DIR_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo CLUSTER_DIR_DONE || { echo CLUSTER_DIR_FAILED; exit 1; }
