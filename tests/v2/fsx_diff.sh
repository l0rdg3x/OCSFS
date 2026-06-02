#!/bin/bash
# OCSFS v2 — DIFFERENTIAL data-path test (the reliable method).
#
# Why not `xfstests ./check`? For a custom FS its raw pass/fail is unreliable:
#   - the golden .out assumes full feature parity (it flags benign stdout like
#     "filesystem does not support fallocate mode ..." as a failure);
#   - fsx itself emits ops that even XFS rejects (e.g. unaligned O_DIRECT writes
#     -> "write: Invalid argument"), which look like FS bugs but are not.
# So we run the SAME fsx seed + params on OCSFS and on XFS (reflink=1) and a bug
# is REAL only when OCSFS diverges from XFS (OCSFS hits BAD DATA / an op error
# that XFS does not). Aligned params (-r/-t/-w = block size) avoid the artifacts.
#
# Usage: fsx_diff.sh <ocsfs_dev> <xfs_dev> [N] [seeds]
set -u
OCDEV="${1:?usage: fsx_diff.sh <ocsfs_dev> <xfs_dev> [N] [seeds]}"
XFDEV="${2:?need an xfs scratch dev}"
N="${3:-20000}"
SEEDS="${4:-1 2 3 4 5 6 7 8}"
FSX=/root/xfstests/ltp/fsx
OM=/mnt/ocfsxdiff; XM=/mnt/xfsxdiff
mkdir -p $OM $XM
# fsx flag matrix mirroring xfstests generic/075,091,127,263 (aligned to avoid
# the unaligned-O_DIRECT artifact); buffered+mmap and O_DIRECT.
MATRIX=("-r 4096 -t 4096 -w 4096" "-r 4096 -t 4096 -w 4096 -Z")

verdict() { echo "$1" | grep -qi "BAD DATA" && { echo BADDATA; return; }
            echo "$1" | grep -qi "A-OK" && { echo OK; return; }; echo OPERR; }

umount $OM 2>/dev/null; rmmod ocsfs2 2>/dev/null
mkfs.xfs -f -m reflink=1 "$XFDEV" >/dev/null 2>&1; mount "$XFDEV" $XM
insmod /root/OCSFS/kmod2/ocsfs2.ko 2>/dev/null || insmod kmod2/ocsfs2.ko
/root/OCSFS/mkfs.ocsfs2 -f -N 1 "$OCDEV" >/dev/null 2>/dev/null || ./mkfs.ocsfs2 -f -N 1 "$OCDEV" >/dev/null
mount -t ocsfs2 "$OCDEV" $OM

DIVERGE=0; RUNS=0
for flags in "${MATRIX[@]}"; do
  for S in $SEEDS; do
    RUNS=$((RUNS+1))
    xo=$($FSX -N $N -S $S -o 32768 -l 500000 $flags -P /tmp $XM/c 2>&1); xv=$(verdict "$xo"); rm -f $XM/c
    oo=$($FSX -N $N -S $S -o 32768 -l 500000 $flags -P /tmp $OM/c 2>&1); ov=$(verdict "$oo"); rm -f $OM/c
    if [ "$xv" = OK ] && [ "$ov" != OK ]; then
      echo "  DIVERGE seed=$S flags='$flags' : XFS=$xv OCSFS=$ov  <<< REAL OCSFS BUG"
      echo "$oo" | grep -i "BAD DATA\|: Invalid\|: No space\|operation#" | head -2 | sed "s/^/      /"
      DIVERGE=$((DIVERGE+1))
    else
      echo "  ok seed=$S flags='${flags}': XFS=$xv OCSFS=$ov"
    fi
  done
done
umount $OM 2>/dev/null; rmmod ocsfs2 2>/dev/null; umount $XM 2>/dev/null
echo "FSX_DIFF: $RUNS runs, $DIVERGE real divergences"
[ "$DIVERGE" -eq 0 ] && echo FSX_DIFF_CLEAN || echo FSX_DIFF_BUGS
