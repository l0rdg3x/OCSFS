#!/bin/bash
# OCSFS v2 — namespace gate: mkdir/create/rename/unlink/rmdir, persistence
# across remount, fsck clean, cross-checked against ext4 with identical ops.
# Usage: test_namespace.sh <ocsfs2_dev> <ext4_dev>   (run ON a Proxmox node)
set -e
OCSDEV="${1:?usage: test_namespace.sh <ocsfs2_dev> <ext4_dev>}"
EXT4DEV="${2:?need an ext4 scratch device for cross-check}"
cd /root/OCSFS

cc -Wall -Werror -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -Wall -Werror -std=gnu11 tools2/fsck.c -o fsck.ocsfs2
rmmod ocsfs2 2>/dev/null || true
insmod kmod2/ocsfs2.ko

run_seq() {   # $1 = mountpoint
	local M="$1"
	mkdir "$M/d1" "$M/d1/sub" "$M/d2"
	touch "$M/d1/a" "$M/d1/sub/b" "$M/d2/c" "$M/f0"
	mv "$M/d1/a" "$M/d2/a2"        # cross-dir file rename
	mv "$M/d1/sub" "$M/d2/sub2"    # cross-dir dir rename (.. fixup)
	rm "$M/d2/c"                   # unlink
	rmdir "$M/d1"                  # rmdir now-empty dir
}
tree() { ( cd "$1" && find . -printf '%y %P\n' | sort ); }

# ── OCSFS2 ──
./mkfs.ocsfs2 -f -L v2 "$OCSDEV" >/dev/null
mkdir -p /mnt/o2
mount -t ocsfs2 "$OCSDEV" /mnt/o2
run_seq /mnt/o2
tree /mnt/o2 > /tmp/ocs_before.txt
umount /mnt/o2
mount -t ocsfs2 "$OCSDEV" /mnt/o2     # remount: persistence check
tree /mnt/o2 > /tmp/ocs_after.txt
umount /mnt/o2
./fsck.ocsfs2 "$OCSDEV"

# ── ext4 ground truth ──
mkfs.ext4 -F -q "$EXT4DEV"
mkdir -p /mnt/e4
mount "$EXT4DEV" /mnt/e4
run_seq /mnt/e4
tree /mnt/e4 > /tmp/e4.txt
umount /mnt/e4

echo "=== persistence (before vs after remount) ==="
diff /tmp/ocs_before.txt /tmp/ocs_after.txt && echo "PERSIST_OK"
echo "=== structure vs ext4 ==="
diff /tmp/ocs_after.txt /tmp/e4.txt && echo "MATCH_EXT4"
echo "=== final tree ==="
cat /tmp/ocs_after.txt
rmmod ocsfs2
echo NAMESPACE_TEST_DONE
