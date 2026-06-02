#!/bin/bash
# OCSFS v2 — cluster L3 membership + death detection gate (2 nodes).
# Run FROM n1; it drives n2 over ssh. Both share the same iSCSI LUN.
# Usage: test_cluster.sh <dev> <n2_ip>
set -e
DEV="${1:?usage: test_cluster.sh <dev> <n2_ip>}"
N2="${2:?need n2 ip}"
cd /root/OCSFS
P=0; FA=0
ck() { if echo "$2" | grep -qiE "$3"; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1"; FA=$((FA+1)); fi; }

cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
umount /mnt/o2 2>/dev/null || true; rmmod ocsfs2 2>/dev/null || true
ssh root@$N2 'umount /mnt/o2 2>/dev/null; rmmod ocsfs2 2>/dev/null' || true
insmod kmod2/ocsfs2.ko
scp -q kmod2/ocsfs2.ko root@$N2:/root/OCSFS/kmod2/ocsfs2.ko
ssh root@$N2 'insmod /root/OCSFS/kmod2/ocsfs2.ko'
./mkfs.ocsfs2 -f -N 3 -L clu "$DEV" >/dev/null

echo "=== mount both -o cluster ==="
mount -t ocsfs2 -o cluster "$DEV" /mnt/o2
ssh root@$N2 "mkdir -p /mnt/o2 && mount -t ocsfs2 -o cluster $DEV /mnt/o2"
sleep 10

N1LOG=$(dmesg | grep -iE "ocsfs2.*(cluster up|alive|DEAD)" | tail -4)
N2LOG=$(ssh root@$N2 'dmesg | grep -iE "ocsfs2.*(cluster up|alive|DEAD)" | tail -4')
ck "n1 came up clustered"     "$N1LOG" "cluster up"
ck "n1 sees a peer alive"     "$N1LOG" "alive"
ck "n2 came up clustered"     "$N2LOG" "cluster up"
ck "n2 sees a peer alive"     "$N2LOG" "alive"

echo "=== simulate n2 death (pause its heartbeat) ==="
ssh root@$N2 'echo 1 > /sys/module/ocsfs2/parameters/hb_pause'
sleep 14   # > death window (8s) + a couple intervals
N1LOG2=$(dmesg | grep -iE "ocsfs2.*DEAD" | tail -2)
ck "n1 declared the paused peer DEAD" "$N1LOG2" "DECLARED DEAD"

echo "=== cleanup ==="
ssh root@$N2 'echo 0 > /sys/module/ocsfs2/parameters/hb_pause 2>/dev/null; umount /mnt/o2 2>/dev/null; rmmod ocsfs2 2>/dev/null' || true
umount /mnt/o2 2>/dev/null || true
rmmod ocsfs2 2>/dev/null || true
echo "CLUSTER_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo CLUSTER_TEST_DONE || { echo CLUSTER_TEST_FAILED; exit 1; }
