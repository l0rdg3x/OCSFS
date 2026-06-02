#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# install.sh — one-step OCSFS v2 installer for a Proxmox VE node.
#
# Builds and installs everything needed to use OCSFS v2 on THIS node:
#   1. the ocsfs2 kernel module  -> /lib/modules/$(uname -r)/extra + depmod
#   2. the on-disk tools         -> /usr/sbin/{mkfs,fsck}.ocsfs2
#   3. the mount helper          -> /sbin/mount.ocsfs2
#   4. the PVE storage plugin    -> PVE::Storage::Custom::OCSFS2Plugin (if PVE present)
#   5. loads the module
#
# Run it on EACH node that will mount the shared LUN.  Re-run after a kernel
# upgrade to rebuild the module.   Usage:  sudo ./proxmox2/install.sh
set -euo pipefail

say()  { echo ":: $*"; }
die()  { echo "install.sh: ERROR: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "must run as root"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
KREL="$(uname -r)"
KBUILD="/lib/modules/$KREL/build"

# 0. prerequisites (build toolchain + matching kernel headers). The out-of-tree
#    module must be built against THIS node's exact kernel. Auto-install on a
#    Debian/PVE node; otherwise tell the admin exactly what to install.
need_prereq=0
command -v cc >/dev/null   || need_prereq=1
[ -d "$KBUILD" ]           || need_prereq=1
if [ "$need_prereq" -eq 1 ]; then
    if command -v apt-get >/dev/null; then
        say "installing prerequisites (build-essential, pve-headers-$KREL)"
        apt-get update -qq 2>/dev/null || true
        DEBIAN_FRONTEND=noninteractive apt-get install -y \
            build-essential "pve-headers-$KREL" 2>/dev/null || \
            DEBIAN_FRONTEND=noninteractive apt-get install -y \
            build-essential "linux-headers-$KREL" 2>/dev/null || true
    fi
fi
[ -d "$KBUILD" ] || die "kernel headers for $KREL not found ($KBUILD). Install: apt install pve-headers-$KREL"
command -v cc >/dev/null || die "no C compiler. Install: apt install build-essential"

# 1. kernel module
say "building ocsfs2 kernel module for $KREL"
make -C "$KBUILD" M="$ROOT/kmod2" modules >/dev/null
DEST="/lib/modules/$KREL/extra"
install -d "$DEST"
install -m 0644 "$ROOT/kmod2/ocsfs2.ko" "$DEST/ocsfs2.ko"
depmod -a
say "installed module -> $DEST/ocsfs2.ko"

# 2. on-disk + online tools
say "building tools"
cc -O2 -std=gnu11 "$ROOT/tools2/mkfs.c"          -o /usr/sbin/mkfs.ocsfs2
cc -O2 -std=gnu11 "$ROOT/tools2/fsck.c"          -o /usr/sbin/fsck.ocsfs2
cc -O2 -std=gnu11 "$ROOT/tools2/ocsfs2_scrub.c"  -o /usr/sbin/ocsfs2-scrub
cc -O2 -std=gnu11 "$ROOT/tools2/ocsfs2_defrag.c" -o /usr/sbin/ocsfs2-defrag
cc -O2 -std=gnu11 "$ROOT/tools2/ocsfs2_tool.c"   -o /usr/sbin/ocsfs2-tool
chmod 0755 /usr/sbin/mkfs.ocsfs2 /usr/sbin/fsck.ocsfs2 \
           /usr/sbin/ocsfs2-scrub /usr/sbin/ocsfs2-defrag /usr/sbin/ocsfs2-tool
say "installed /usr/sbin/{mkfs,fsck}.ocsfs2, ocsfs2-{scrub,defrag,tool}"

# 3. mount helper
install -m 0755 "$HERE/mount.ocsfs2" /sbin/mount.ocsfs2
say "installed /sbin/mount.ocsfs2"

# 4. PVE storage plugin (optional — only if this is a PVE node)
if [ -d /usr/share/perl5/PVE/Storage ]; then
    install -d /usr/share/perl5/PVE/Storage/Custom
    install -m 0644 "$HERE/OCSFS2Plugin.pm" /usr/share/perl5/PVE/Storage/Custom/OCSFS2Plugin.pm
    say "installed PVE storage plugin (PVE::Storage::Custom::OCSFS2Plugin)"
    systemctl reload-or-restart pvedaemon pveproxy 2>/dev/null || \
        say "restart pvedaemon/pveproxy manually to load the plugin"
else
    say "PVE not detected — skipping storage plugin (module + tools still installed)"
fi

# 5. periodic online maintenance (scrub + defrag timers)
say "installing periodic maintenance services"
install -m 0755 "$HERE/ocsfs2-maint" /usr/sbin/ocsfs2-maint
if [ -d /run/systemd/system ]; then
    install -m 0644 "$HERE/systemd/ocsfs2-scrub.service"  /etc/systemd/system/
    install -m 0644 "$HERE/systemd/ocsfs2-scrub.timer"    /etc/systemd/system/
    install -m 0644 "$HERE/systemd/ocsfs2-defrag.service" /etc/systemd/system/
    install -m 0644 "$HERE/systemd/ocsfs2-defrag.timer"   /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable --now ocsfs2-scrub.timer ocsfs2-defrag.timer 2>/dev/null || \
        say "enable timers manually: systemctl enable --now ocsfs2-scrub.timer ocsfs2-defrag.timer"
    say "enabled ocsfs2-scrub.timer (weekly) + ocsfs2-defrag.timer (weekly)"
else
    say "systemd not detected — run 'ocsfs2-maint scrub|defrag' from cron instead"
fi

# 6. load the module now
modprobe ocsfs2 2>/dev/null || insmod "$DEST/ocsfs2.ko" 2>/dev/null || true
lsmod | grep -q '^ocsfs2 ' && say "module loaded" || say "module not loaded (load with: modprobe ocsfs2)"

cat <<EOF

OCSFS v2 installed on $(hostname).  Next steps:

  # format the shared LUN ONCE, from a single node (-N = max cluster nodes):
  mkfs.ocsfs2 -L vmstore -N 3 -f /dev/disk/by-id/<your-lun>

  # then, in /etc/pve/storage.cfg (or via the GUI once the plugin loads):
  #   ocsfs2: vmstore
  #       path /mnt/pve/vmstore
  #       device /dev/disk/by-id/<your-lun>
  #       content images,iso,vztmpl,backup,rootdir,snippets
  #       cluster 1
  #       shared 1

Run this installer on EVERY node sharing the LUN (and after each kernel upgrade).
EOF
