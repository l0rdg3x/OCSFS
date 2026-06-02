#!/bin/bash
# OCSFS v2 — crash recovery test (REAL power loss via sysrq-b). Two phases,
# because phase 1 hard-resets the node. Run ON a Proxmox TEST node.
#
#   ssh root@NODE 'bash test_crash.sh phase1 /dev/disk/by-path/...lun-3'   # crashes
#   ... wait for the node to reboot, re-login iSCSI ...
#   ssh root@NODE 'bash test_crash.sh phase2 /dev/disk/by-path/...lun-3'   # verifies
#
# phase1 commits a metadata op to the journal with the checkpoint SKIPPED
# (module param crash_after_commit=1), then hard-crashes. phase2 remounts,
# which replays the committed transaction, and verifies the op was recovered
# and fsck reports no corruption.
set -e
PHASE="${1:?phase1|phase2}"
DEV="${2:?device}"
cd /root/OCSFS

if [ "$PHASE" = phase1 ]; then
	cc -Wall -Werror -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
	umount /mnt/o2 2>/dev/null || true
	rmmod ocsfs2 2>/dev/null || true
	insmod kmod2/ocsfs2.ko
	./mkfs.ocsfs2 -f -L crash "$DEV" >/dev/null
	mkdir -p /mnt/o2
	mount -t ocsfs2 "$DEV" /mnt/o2
	mkdir /mnt/o2/precrash          # normal: committed + checkpointed
	sync
	echo 1 > /sys/module/ocsfs2/parameters/crash_after_commit
	mkdir /mnt/o2/REPLAYME          # committed to journal, checkpoint skipped
	echo "pre-crash tree: $(ls /mnt/o2)"
	echo "CRASHING"; echo b > /proc/sysrq-trigger; sleep 30
elif [ "$PHASE" = phase2 ]; then
	cc -Wall -Werror -std=gnu11 tools2/fsck.c -o fsck.ocsfs2
	rmmod ocsfs2 2>/dev/null || true
	insmod kmod2/ocsfs2.ko          # fresh load: crash_after_commit back to 0
	mount -t ocsfs2 "$DEV" /mnt/o2  # triggers replay
	echo "post-replay tree (expect precrash AND REPLAYME): $(ls /mnt/o2)"
	dmesg | grep -iE "ocsfs2.*replayed" | tail -1
	umount /mnt/o2
	./fsck.ocsfs2 "$DEV"            # exit 0 = no corruption (free-count hint may warn)
	rmmod ocsfs2
	echo CRASH_TEST_DONE
fi
