#!/bin/bash
# OCSFS v2 — Fase D6 multinode performance (2 nodes, single-writer ownership).
# Each node drives fio O_DIRECT (the Proxmox cache=none workload) against its
# OWN file on the shared clustered fs; report per-node throughput for the
# single-node baseline and the 2-node concurrent run. Run FROM n1.
# Usage: test_cluster_perf.sh <dev> <n2_ip>
set -e
DEV="${1:?usage: test_cluster_perf.sh <dev> <n2_ip>}"
N2="${2:?need n2 ip}"
cd /root/OCSFS
SZ=2G; RT=20

CLEAN='{ [ -w /sys/module/ocsfs2/parameters/hb_pause ] && echo 0 >/sys/module/ocsfs2/parameters/hb_pause; } 2>/dev/null;
       pkill -f "fio --name" 2>/dev/null || true; umount /mnt/o2 2>/dev/null || true;
       for i in 1 2 3; do rmmod ocsfs2 2>/dev/null && break; sleep 1; done; true'
cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -O2 -std=gnu11 tools2/fsck.c -o fsck.ocsfs2 2>/dev/null || true
eval "$CLEAN"; ssh root@$N2 "$CLEAN" || true
insmod kmod2/ocsfs2.ko
scp -q kmod2/ocsfs2.ko root@$N2:/root/OCSFS/kmod2/ocsfs2.ko
ssh root@$N2 'insmod /root/OCSFS/kmod2/ocsfs2.ko'
./mkfs.ocsfs2 -f -N 3 -L perf "$DEV" >/dev/null
mount -t ocsfs2 -o cluster "$DEV" /mnt/o2
ssh root@$N2 "mkdir -p /mnt/o2 && mount -t ocsfs2 -o cluster $DEV /mnt/o2"
sleep 6
echo "cluster up"

# fio one job -> one human line (IOPS + BW); $1 file $2 rw $3 bs $4 iodepth
fio_local() {
	fio --name=j --filename="$1" --rw="$2" --bs="$3" --iodepth="$4" --direct=1 \
	    --ioengine=libaio --runtime=$RT --time_based --group_reporting --size=$SZ \
	    2>/dev/null | grep -iE "^[[:space:]]*(read|write):" | head -1 | sed "s/^[[:space:]]*//"
}
fio_remote() {
	ssh root@$N2 "fio --name=j --filename=$1 --rw=$2 --bs=$3 --iodepth=$4 --direct=1 \
	    --ioengine=libaio --runtime=$RT --time_based --group_reporting --size=$SZ 2>/dev/null" \
	    | grep -iE "^[[:space:]]*(read|write):" | head -1 | sed "s/^[[:space:]]*//"
}

echo "=== single-node baseline (n1) ==="
for j in "write 1M 16" "randwrite 4k 32" "randread 4k 32"; do
	set -- $j; echo "  $1/$2: $(fio_local /mnt/o2/p1 "$1" "$2" "$3")"
done

echo "=== 2-node concurrent (each its own file) ==="
for j in "write 1M 16" "randwrite 4k 32" "randread 4k 32"; do
	set -- $j
	fio_remote /mnt/o2/p2 "$1" "$2" "$3" >/tmp/n2.out 2>/dev/null &
	R1=$(fio_local /mnt/o2/p1 "$1" "$2" "$3")
	wait
	echo "  $1/$2:"
	echo "     n1: $R1"
	echo "     n2: $(cat /tmp/n2.out)"
done

echo "=== cleanup + fsck ==="
ssh root@$N2 "$CLEAN" || true
eval "$CLEAN"
./fsck.ocsfs2 "$DEV" 2>/dev/null | tail -1
echo "PERF_DONE"
