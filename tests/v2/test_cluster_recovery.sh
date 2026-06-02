#!/bin/bash
# OCSFS v2 — cluster L5 recovery gate (2 nodes).
# n2 opens a file RW (holds an EX ownership lease) then its heartbeat is paused
# => n1 declares it DEAD, fences it (SCSI-PR preempt-abort) and runs recovery
# off the heartbeat path: becomes recovery leader, replays n2's per-node journal,
# and eagerly reclaims every lease n2 held. We then prove the reclaim happened
# *before* n1 even tries the file (eager, not lazy), that n1 can take the file
# RW, and that the volume is still fsck-clean. Run FROM n1. Usage: <dev> <n2_ip>
set -e
DEV="${1:?usage: test_cluster_recovery.sh <dev> <n2_ip>}"
N2="${2:?need n2 ip}"
cd /root/OCSFS
P=0; FA=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: got '$2' want '$3'"; FA=$((FA+1)); fi; }

cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -O2 -std=gnu11 tools2/fsck.c -o fsck.ocsfs2 2>/dev/null || true
# robust local + remote teardown: NON-lazy umount (lazy leaves the bdev held,
# so the next exclusive open fails with "Can't open blockdev"), retry rmmod.
CLEAN='[ -w /sys/module/ocsfs2/parameters/hb_pause ] && echo 0 > /sys/module/ocsfs2/parameters/hb_pause;
       umount /mnt/o2 2>/dev/null || true;
       for i in 1 2 3; do rmmod ocsfs2 2>/dev/null && break; sleep 1; done;
       true'
eval "$CLEAN"
ssh root@$N2 "$CLEAN" || true
insmod kmod2/ocsfs2.ko
scp -q kmod2/ocsfs2.ko root@$N2:/root/OCSFS/kmod2/ocsfs2.ko
ssh root@$N2 'insmod /root/OCSFS/kmod2/ocsfs2.ko'
./mkfs.ocsfs2 -f -N 3 -L clu "$DEV" >/dev/null

# pre-create the victim file single-node (namespace ops are L4b; here we only
# need a regular file each node can open)
mount -t ocsfs2 "$DEV" /mnt/o2
head -c 1048576 /dev/zero | tr '\0' 'A' > /mnt/o2/f1
sync
umount /mnt/o2
sleep 2                 # let the exclusive blockdev close settle before re-open

echo "=== mount both -o cluster ==="
for i in 1 2 3 4 5; do
	mount -t ocsfs2 -o cluster "$DEV" /mnt/o2 2>/tmp/mnt.err && break
	grep -q "Can't open blockdev" /tmp/mnt.err && { sleep 2; continue; }
	cat /tmp/mnt.err; break
done
ssh root@$N2 "mkdir -p /mnt/o2 && mount -t ocsfs2 -o cluster $DEV /mnt/o2"
sleep 6
dmesg -C >/dev/null 2>&1 || true   # clear n1 ring so we only match recovery lines

# ── n2 takes EX ownership of f1 (held fd) ──
ssh root@$N2 'nohup bash -c "exec 9<>/mnt/o2/f1; sleep 600" >/dev/null 2>&1 & echo started'
sleep 3
# sanity: while n2 holds EX, n1 RW-open of f1 is refused
N1BUSY=$(bash -c 'exec 9<>/mnt/o2/f1' 2>&1 | grep -ciE "busy|denied|error" || true)
ck "n1 RW-open blocked while n2 owns f1" "$([ "$N1BUSY" -ge 1 ] && echo blocked || echo allowed)" "blocked"

echo "=== simulate n2 death (pause heartbeat), wait out 30s window ==="
ssh root@$N2 'echo 1 > /sys/module/ocsfs2/parameters/hb_pause'
sleep 40   # > 30s death window + recovery work

# recovery must have run on n1 *on its own* (eager), before we touch the file
N1REC=$(dmesg | grep -iE "ocsfs2: recovery leader for dead slot" | tail -1)
N1DONE=$(dmesg | grep -iE "ocsfs2: recovery of slot .* complete" | tail -1)
ck "n1 became recovery leader"        "$([ -n "$N1REC" ] && echo yes || echo no)"  "yes"
ck "n1 finished recovery (eager)"     "$([ -n "$N1DONE" ] && echo yes || echo no)" "yes"

# ── after eager reclaim, n1 can take f1 RW ──
N1TAKE=$(bash -c 'exec 9<>/mnt/o2/f1; echo ok' 2>&1 | tail -1)
ck "n1 takes f1 RW after recovery" "$N1TAKE" "ok"
# and the data the dead node left is intact
ck "f1 content intact after recovery" "$(head -c 16 /mnt/o2/f1)" "AAAAAAAAAAAAAAAA"

# ── n1 can still write (lease genuinely reclaimed, not just opened) ──
head -c 65536 /dev/zero | tr '\0' 'Z' > /mnt/o2/f1
sync
ck "n1 writes f1 after reclaim" "$(head -c 8 /mnt/o2/f1)" "ZZZZZZZZ"

echo "=== cleanup ==="
ssh root@$N2 'pkill -f "sleep 600" 2>/dev/null; sleep 1' || true   # drop held fd
ssh root@$N2 "$CLEAN" || true
eval "$CLEAN"
./fsck.ocsfs2 "$DEV" 2>/dev/null | tail -2; FSCKRC=${PIPESTATUS[0]}
ck "fsck clean after recovery" "$FSCKRC" "0"
echo "CLUSTER_RECOVERY_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo CLUSTER_RECOVERY_DONE || { echo CLUSTER_RECOVERY_FAILED; exit 1; }
