#!/bin/bash
# OCSFS v2 — format gate: mkfs then fsck-clean.
# Usage: test_format.sh <device>   (run ON a Proxmox node, repo at /root/OCSFS)
set -e
DEV="${1:?usage: test_format.sh <device>}"
cd /root/OCSFS

cc -Wall -Werror -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -Wall -Werror -std=gnu11 tools2/fsck.c -o fsck.ocsfs2

echo "== mkfs =="
./mkfs.ocsfs2 -L v2test -f "$DEV"
echo "== fsck =="
./fsck.ocsfs2 "$DEV"
echo "FORMAT_GATE_PASS"
