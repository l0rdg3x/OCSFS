#!/bin/bash
# OCSFS v2 — cluster L4 single-writer ownership lease gate (2 nodes).
# Files are pre-created single-node (no namespace ops while clustered, since
# directory coherence is L4b). Tests: EX held on n1 blocks n2's open; after
# release n2 can open; write-then-handoff is coherent; concurrent writes to
# different files both land. Run FROM n1, drives n2. Usage: <dev> <n2_ip>
set -e
DEV="${1:?usage: test_cluster_lease.sh <dev> <n2_ip>}"
N2="${2:?need n2 ip}"
cd /root/OCSFS
P=0; FA=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: got '$2' want '$3'"; FA=$((FA+1)); fi; }

cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
umount /mnt/o2 2>/dev/null || true; rmmod ocsfs2 2>/dev/null || true
ssh root@$N2 'umount /mnt/o2 2>/dev/null; rmmod ocsfs2 2>/dev/null' || true
insmod kmod2/ocsfs2.ko
scp -q kmod2/ocsfs2.ko root@$N2:/root/OCSFS/kmod2/ocsfs2.ko
ssh root@$N2 'insmod /root/OCSFS/kmod2/ocsfs2.ko'
./mkfs.ocsfs2 -f -N 3 -L clu "$DEV" >/dev/null

# pre-create files single-node, then unmount
mount -t ocsfs2 "$DEV" /mnt/o2
head -c 1048576 /dev/zero | tr '\0' 'A' > /mnt/o2/f1
head -c 1048576 /dev/zero | tr '\0' 'B' > /mnt/o2/f2
umount /mnt/o2

echo "=== mount both -o cluster ==="
mount -t ocsfs2 -o cluster "$DEV" /mnt/o2
ssh root@$N2 "mount -t ocsfs2 -o cluster $DEV /mnt/o2"
sleep 5

# ── 1. EX exclusion: n1 holds f1 RW; n2 open must fail ──
nohup bash -c 'exec 9<>/mnt/o2/f1; sleep 12' >/dev/null 2>&1 &
sleep 2
N2OPEN=$(ssh root@$N2 'dd if=/mnt/o2/f1 of=/dev/null bs=1M count=1 2>&1 | grep -ciE "busy|denied|error" || true')
ck "n2 open blocked while n1 holds EX" "$([ "$N2OPEN" -ge 1 ] && echo blocked || echo allowed)" "blocked"

# ── 2. after n1 releases, n2 can open + read ──
wait   # n1's holder finishes (releases EX)
sleep 1
N2READ=$(ssh root@$N2 'head -c 16 /mnt/o2/f1' 2>/dev/null)
ck "n2 reads f1 after release" "$N2READ" "AAAAAAAAAAAAAAAA"

# ── 3. handoff coherence: n1 rewrites f1, n2 sees new data ──
head -c 1048576 /dev/zero | tr '\0' 'C' > /mnt/o2/f1   # open+write+close on n1 (flush+release)
sync
N2HAND=$(ssh root@$N2 'head -c 16 /mnt/o2/f1' 2>/dev/null)
ck "n2 sees n1's rewrite (handoff)" "$N2HAND" "CCCCCCCCCCCCCCCC"

# ── 4. concurrent writes to different files both land ──
( head -c 1048576 /dev/zero | tr '\0' 'X' > /mnt/o2/f1 ) &
ssh root@$N2 'head -c 1048576 /dev/zero | tr "\0" "Y" > /mnt/o2/f2'
wait
sync; ssh root@$N2 sync
ck "n1 wrote f1 (X)" "$(head -c 8 /mnt/o2/f1)" "XXXXXXXX"
ck "n2 wrote f2 (Y), n1 sees after handoff" "$(head -c 8 /mnt/o2/f2)" "YYYYYYYY"

echo "=== cleanup ==="
umount /mnt/o2 2>/dev/null || true; rmmod ocsfs2 2>/dev/null || true
ssh root@$N2 'umount /mnt/o2 2>/dev/null; rmmod ocsfs2 2>/dev/null' || true
./fsck.ocsfs2 "$DEV" 2>/dev/null | tail -1 || true
echo "CLUSTER_LEASE_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo CLUSTER_LEASE_DONE || { echo CLUSTER_LEASE_FAILED; exit 1; }
