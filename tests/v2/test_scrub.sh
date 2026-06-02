#!/bin/bash
# OCSFS v2 — Fase D5 online metadata scrub (single-node gate).
# A healthy fs scrubs with 0 errors; a deliberately corrupted inode checksum is
# detected by scrub. Usage: test_scrub.sh <dev>
set -e
DEV="${1:?usage: test_scrub.sh <dev>}"
cd /root/OCSFS
P=0; FA=0
ck() { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: got '$2' want '$3'"; FA=$((FA+1)); fi; }
field() { sed -n "s/.*$1=\([0-9]*\).*/\1/p"; }

cc -O2 -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -O2 -std=gnu11 tests/v2/scrub_tool.c -o scrub_tool
umount /mnt/o2 2>/dev/null || true; rmmod ocsfs2 2>/dev/null || true
insmod kmod2/ocsfs2.ko
./mkfs.ocsfs2 -f -N 1 -L sc "$DEV" >/dev/null
mount -t ocsfs2 "$DEV" /mnt/o2

echo "=== populate (inodes, dirs, xattr, reflink) ==="
mkdir -p /mnt/o2/d1/d2
for i in $(seq 1 20); do dd if=/dev/urandom of=/mnt/o2/d1/f$i bs=4k count=8 status=none; done
setfattr -n user.test -v hello /mnt/o2/d1/f1 2>/dev/null || echo "  (no setfattr; skipping xattr)"
cp --reflink=always /mnt/o2/d1/f2 /mnt/o2/d1/f2-clone 2>/dev/null || true
ln -s f1 /mnt/o2/d1/sym1
sync

echo "=== healthy scrub ==="
OUT=$(./scrub_tool /mnt/o2); echo "  $OUT"
CHK=$(echo "$OUT" | field checked); ERR=$(echo "$OUT" | field errors); INO=$(echo "$OUT" | field inodes)
ck "scrub clean (0 errors)" "$ERR" "0"
ck "scrub checked structures" "$([ "$CHK" -gt 20 ] && echo yes || echo no)" "yes"
ck "scrub counted inodes" "$([ "$INO" -ge 22 ] && echo yes || echo no)" "yes"

echo "=== inject corruption into a user inode, rescan ==="
umount /mnt/o2
python3 - "$DEV" <<'PY'
import sys, struct
dev = sys.argv[1]
f = open(dev, 'r+b')
sb = f.read(4096)                                  # superblock
ag_desc_off  = struct.unpack_from('<Q', sb, 232)[0]
inodes_per_ag = struct.unpack_from('<Q', sb, 264)[0]
f.seek(ag_desc_off); ag0 = f.read(4096)            # AG0 descriptor
itable_off = struct.unpack_from('<Q', ag0, 56)[0]  # ag_inode_table_off
f.seek(itable_off); itab = f.read(inodes_per_ag * 512)
off = -1
for s in range(0, len(itab), 512):                 # scan only AG0's inode table
    if itab[s:s+4] != b'2ONI':                     # le32 of INODE_MAGIC 0x494E4F32
        continue
    ino = struct.unpack_from('<Q', itab, s + 8)[0]
    if ino >= 64:                                  # a user inode, not root/reserved
        off = itable_off + s; break
if off < 0:
    print("NOFOUND"); sys.exit(1)
f.seek(off + 40); b = f.read(1)                    # flip a body byte (not magic/csum)
f.seek(off + 40); f.write(bytes([b[0] ^ 0xFF]))
f.flush(); f.close()
print("corrupted inode at byte", off)
PY
mount -t ocsfs2 "$DEV" /mnt/o2
OUT2=$(./scrub_tool /mnt/o2); echo "  $OUT2"
ERR2=$(echo "$OUT2" | field errors)
ck "scrub detects the corrupted inode" "$([ "$ERR2" -ge 1 ] && echo yes || echo no)" "yes"

echo "=== cleanup ==="
umount /mnt/o2; rmmod ocsfs2
echo "SCRUB_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo SCRUB_DONE || { echo SCRUB_FAILED; exit 1; }
