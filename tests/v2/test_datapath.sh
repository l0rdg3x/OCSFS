#!/bin/bash
# OCSFS v2 — data-path gate: buffered + O_DIRECT sha256 round-trip across
# drop_caches, sparse files, sub-block RMW, truncate grow/shrink, fsck clean.
# Usage: test_datapath.sh <ocsfs2_dev>   (run ON a Proxmox node)
set -e
DEV="${1:?usage: test_datapath.sh <ocsfs2_dev>}"
cd /root/OCSFS
cc -Wall -Werror -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -Wall -Werror -std=gnu11 tools2/fsck.c -o fsck.ocsfs2
rmmod ocsfs2 2>/dev/null || true
insmod kmod2/ocsfs2.ko
./mkfs.ocsfs2 -f -L v2 "$DEV" >/dev/null
mkdir -p /mnt/o2
mount -t ocsfs2 "$DEV" /mnt/o2
drop() { sync; echo 3 > /proc/sys/vm/drop_caches; }

dd if=/dev/urandom of=/tmp/src bs=1M count=64 status=none
SRC=$(sha256sum /tmp/src | cut -d' ' -f1)

# 1. buffered 64 MiB round-trip
cp /tmp/src /mnt/o2/big; drop
echo "1 buffered : $([ "$(sha256sum /mnt/o2/big|cut -d' ' -f1)" = "$SRC" ] && echo MATCH || echo MISMATCH)"

# 2. O_DIRECT 64 MiB round-trip
dd if=/tmp/src of=/mnt/o2/bigd bs=1M count=64 oflag=direct status=none; drop
dd if=/mnt/o2/bigd of=/tmp/outd bs=1M iflag=direct status=none
echo "2 o_direct : $([ "$(sha256sum /tmp/outd|cut -d' ' -f1)" = "$SRC" ] && echo MATCH || echo MISMATCH)"

# 3. sparse: 4 KiB at offset 8 MiB; leading 8 MiB reads zero, thin
dd if=/dev/urandom of=/mnt/o2/sparse bs=4k count=1 seek=2048 status=none; drop
SZ=$(stat -c %s /mnt/o2/sparse); BLK=$(stat -c %b /mnt/o2/sparse)
LEAD=$(head -c $((8*1024*1024)) /mnt/o2/sparse | sha256sum | cut -d' ' -f1)
ZERO=$(head -c $((8*1024*1024)) /dev/zero | sha256sum | cut -d' ' -f1)
echo "3 sparse   : size=$SZ(exp 8392704) blocks=$BLK(thin, exp small) leadzero=$([ "$LEAD" = "$ZERO" ] && echo yes || echo NO)"

# 4. sub-block RMW: 4096 'A', then overwrite 100 'B' at offset 50, cold read
head -c 4096 /dev/zero | tr '\0' 'A' > /mnt/o2/rmw; drop
head -c 100 /dev/zero | tr '\0' 'B' | dd of=/mnt/o2/rmw bs=1 seek=50 conv=notrunc status=none; drop
{ head -c 50 /dev/zero|tr '\0' A; head -c 100 /dev/zero|tr '\0' B; head -c 3946 /dev/zero|tr '\0' A; } > /tmp/rmw_exp
echo "4 rmw      : $(cmp -s /mnt/o2/rmw /tmp/rmw_exp && echo OK || echo FAIL)"

# 5. truncate shrink then grow (sparse tail reads zero)
dd if=/dev/urandom of=/mnt/o2/t bs=1M count=4 status=none
truncate -s 1M /mnt/o2/t
truncate -s 8M /mnt/o2/t
drop
TL=$(tail -c $((7*1024*1024)) /mnt/o2/t | sha256sum | cut -d' ' -f1)
Z7=$(head -c $((7*1024*1024)) /dev/zero | sha256sum | cut -d' ' -f1)
echo "5 truncate : size=$(stat -c %s /mnt/o2/t)(exp 8388608) tailzero=$([ "$TL" = "$Z7" ] && echo yes || echo NO)"

umount /mnt/o2
./fsck.ocsfs2 "$DEV"
rmmod ocsfs2
echo "=== dmesg tail ==="; dmesg | tail -4
echo DATAPATH_TEST_DONE
