#!/bin/bash
# OCSFS v2 — xattr (user/trusted) + POSIX ACL (set/get + default inheritance) +
# persistence gate. Run ON a Proxmox node.
# Usage: test_xattr.sh <ocsfs2_dev>
set -e
DEV="${1:?usage: test_xattr.sh <ocsfs2_dev>}"
cd /root/OCSFS
cc -Wall -Werror -std=gnu11 tools2/mkfs.c -o mkfs.ocsfs2
cc -Wall -Werror -std=gnu11 tools2/fsck.c -o fsck.ocsfs2
rmmod ocsfs2 2>/dev/null || true
insmod kmod2/ocsfs2.ko
./mkfs.ocsfs2 -f -L xattr "$DEV" >/dev/null
mkdir -p /mnt/o2; mount -t ocsfs2 "$DEV" /mnt/o2
M=/mnt/o2
P=0; FA=0
ck()  { if [ "$2" = "$3" ]; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: '$2' != '$3'"; FA=$((FA+1)); fi; }
ckc() { if echo "$2" | grep -qF "$3"; then echo "  PASS $1"; P=$((P+1)); else echo "  FAIL $1: '$3' not in '$2'"; FA=$((FA+1)); fi; }

# ── user xattrs ──
touch $M/f
setfattr -n user.color -v blue $M/f
setfattr -n user.size  -v large $M/f
ck  "get user.color"        "$(getfattr -n user.color --only-values $M/f 2>/dev/null)" "blue"
ck  "get user.size"         "$(getfattr -n user.size --only-values $M/f 2>/dev/null)" "large"
ckc "list shows user.color" "$(getfattr -d $M/f 2>/dev/null)" "user.color"
ckc "list shows user.size"  "$(getfattr -d $M/f 2>/dev/null)" "user.size"
setfattr -x user.size $M/f
ck  "removed user.size"     "$(getfattr -n user.size --only-values $M/f 2>&1 >/dev/null | grep -c 'No such attribute')" "1"
ck  "user.color survives removal of other" "$(getfattr -n user.color --only-values $M/f 2>/dev/null)" "blue"

# ── trusted (root) ──
setfattr -n trusted.t1 -v secret $M/f
ck  "get trusted.t1"        "$(getfattr -n trusted.t1 --only-values $M/f 2>/dev/null)" "secret"

# ── big value (close to block) ──
BIG=$(head -c 2000 /dev/zero | tr '\0' 'X')
setfattr -n user.big -v "$BIG" $M/f
ck  "big xattr roundtrip len" "$(getfattr -n user.big --only-values $M/f 2>/dev/null | wc -c)" "2000"

# ── POSIX ACL: explicit set/get ──
touch $M/af
setfacl -m u:12345:rwx $M/af
ckc "acl user:12345:rwx"     "$(getfacl -c $M/af 2>/dev/null)" "user:12345:rwx"
setfacl -m g:54321:r-x $M/af
ckc "acl group:54321:r-x"    "$(getfacl -c $M/af 2>/dev/null)" "group:54321:r-x"

# ── POSIX ACL: default inheritance ──
mkdir $M/ad
setfacl -d -m u:12345:rwx $M/ad
touch $M/ad/child
ckc "child inherits default acl" "$(getfacl -c $M/ad/child 2>/dev/null)" "user:12345:rwx"
mkdir $M/ad/subdir
ckc "subdir inherits default acl" "$(getfacl -c -d $M/ad/subdir 2>/dev/null)" "user:12345:rwx"

# ── persistence across remount ──
sync; umount $M; mount -t ocsfs2 "$DEV" $M
ck  "user.color after remount"   "$(getfattr -n user.color --only-values $M/f 2>/dev/null)" "blue"
ck  "trusted.t1 after remount"   "$(getfattr -n trusted.t1 --only-values $M/f 2>/dev/null)" "secret"
ck  "big xattr after remount"    "$(getfattr -n user.big --only-values $M/f 2>/dev/null | wc -c)" "2000"
ckc "acl after remount"          "$(getfacl -c $M/af 2>/dev/null)" "user:12345:rwx"
ckc "inherited acl after remount" "$(getfacl -c $M/ad/child 2>/dev/null)" "user:12345:rwx"

umount $M
./fsck.ocsfs2 "$DEV"
rmmod ocsfs2
echo "=== dmesg tail ==="; dmesg | tail -4
echo "XATTR_TEST: $P passed, $FA failed"
[ "$FA" -eq 0 ] && echo XATTR_TEST_DONE || { echo XATTR_TEST_FAILED; exit 1; }
