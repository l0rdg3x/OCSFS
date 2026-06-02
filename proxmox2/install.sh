#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# install.sh — one-step OCSFS v2 installer for a Proxmox VE (or Debian) node.
#
# Installs everything needed to use OCSFS v2 on THIS node:
#   1. prerequisites          -> dkms, build-essential, matching kernel headers
#   2. the ocsfs2 kernel module via DKMS (auto-rebuilds on every kernel upgrade)
#   3. the on-disk / online tools -> /usr/sbin/{mkfs,fsck}.ocsfs2, ocsfs2-{scrub,defrag,tool}
#   4. the mount helper       -> /sbin/mount.ocsfs2
#   5. the PVE storage plugin -> PVE::Storage::Custom::OCSFS2Plugin (if PVE present)
#   6. periodic maintenance   -> ocsfs2-{scrub,defrag} systemd timers
#   7. loads the module
#
# Run it on EACH node that will mount the shared LUN.  Thanks to DKMS you do NOT
# need to re-run it after a kernel upgrade — the module is rebuilt automatically
# (provided the new kernel's headers are installed, which the meta-packages below
# pull in).   Usage:  sudo ./proxmox2/install.sh
set -euo pipefail

PKG=ocsfs2
VER=2.0                               # must match kmod2/dkms.conf PACKAGE_VERSION

say()  { echo ":: $*"; }
die()  { echo "install.sh: ERROR: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "must run as root"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
KREL="$(uname -r)"

# ── 1. prerequisites ────────────────────────────────────────────────────────
# dkms + a C toolchain + the headers for THIS kernel (and the meta-package so
# DKMS can rebuild for FUTURE kernels). The debug tools (bpftrace, xfsprogs, …)
# are NOT prerequisites — they are only for development.
if command -v apt-get >/dev/null; then
    say "installing prerequisites (dkms, build-essential, kernel headers)"
    apt-get update -qq 2>/dev/null || true
    DEBIAN_FRONTEND=noninteractive apt-get install -y dkms build-essential 2>/dev/null \
        || die "could not install dkms/build-essential"
    # headers for the running kernel — try Proxmox names first, then stock Debian
    DEBIAN_FRONTEND=noninteractive apt-get install -y "proxmox-headers-$KREL" 2>/dev/null \
        || DEBIAN_FRONTEND=noninteractive apt-get install -y "pve-headers-$KREL" 2>/dev/null \
        || DEBIAN_FRONTEND=noninteractive apt-get install -y "linux-headers-$KREL" 2>/dev/null \
        || true
    # meta-package so future kernel upgrades pull headers automatically (best effort)
    DEBIAN_FRONTEND=noninteractive apt-get install -y proxmox-default-headers 2>/dev/null \
        || DEBIAN_FRONTEND=noninteractive apt-get install -y pve-headers 2>/dev/null \
        || DEBIAN_FRONTEND=noninteractive apt-get install -y linux-headers-amd64 2>/dev/null \
        || true
else
    say "no apt-get — ensure dkms, a C compiler and kernel headers for $KREL are installed"
fi
command -v dkms >/dev/null || die "dkms not available (install: apt install dkms)"
command -v cc   >/dev/null || die "no C compiler (install: apt install build-essential)"
[ -d "/lib/modules/$KREL/build" ] || \
    die "kernel headers for $KREL not found. Install: apt install proxmox-headers-$KREL"

# ── 2. kernel module via DKMS ───────────────────────────────────────────────
SRC="/usr/src/$PKG-$VER"
say "staging module source -> $SRC"
# drop any previous registration/source for this version, then stage fresh
dkms remove -m "$PKG" -v "$VER" --all >/dev/null 2>&1 || true
rm -rf "$SRC"
install -d "$SRC/transport"
install -m 0644 "$ROOT/kmod2/"*.c "$ROOT/kmod2/"*.h "$ROOT/kmod2/Kbuild" \
        "$ROOT/kmod2/dkms.conf" "$SRC/"
install -m 0644 "$ROOT/kmod2/transport/"*.c "$SRC/transport/"

say "building + installing $PKG/$VER via DKMS for $KREL"
dkms add    -m "$PKG" -v "$VER"                 >/dev/null
dkms build  -m "$PKG" -v "$VER"                 || die "dkms build failed (see /var/lib/dkms/$PKG/$VER/build/make.log)"
dkms install -m "$PKG" -v "$VER" --force        >/dev/null
# remove any module left by a previous (pre-DKMS) install so only the DKMS copy
# in /updates/dkms remains
rm -f "/lib/modules/$KREL/extra/ocsfs2.ko" "/lib/modules/$KREL/extra/ocsfs2.ko.zst" 2>/dev/null || true
depmod -a
say "module installed via DKMS (auto-rebuilds on kernel upgrades)"

# ── 3. on-disk + online tools ───────────────────────────────────────────────
say "building tools"
cc -O2 -std=gnu11 "$ROOT/tools2/mkfs.c"          -o /usr/sbin/mkfs.ocsfs2
cc -O2 -std=gnu11 "$ROOT/tools2/fsck.c"          -o /usr/sbin/fsck.ocsfs2
cc -O2 -std=gnu11 "$ROOT/tools2/ocsfs2_scrub.c"  -o /usr/sbin/ocsfs2-scrub
cc -O2 -std=gnu11 "$ROOT/tools2/ocsfs2_defrag.c" -o /usr/sbin/ocsfs2-defrag
cc -O2 -std=gnu11 "$ROOT/tools2/ocsfs2_tool.c"   -o /usr/sbin/ocsfs2-tool
chmod 0755 /usr/sbin/mkfs.ocsfs2 /usr/sbin/fsck.ocsfs2 \
           /usr/sbin/ocsfs2-scrub /usr/sbin/ocsfs2-defrag /usr/sbin/ocsfs2-tool
say "installed /usr/sbin/{mkfs,fsck}.ocsfs2, ocsfs2-{scrub,defrag,tool}"

# ── 4. mount helper ─────────────────────────────────────────────────────────
install -m 0755 "$HERE/mount.ocsfs2" /sbin/mount.ocsfs2
say "installed /sbin/mount.ocsfs2"

# ── 5. PVE storage plugin (optional — only if this is a PVE node) ────────────
if [ -d /usr/share/perl5/PVE/Storage ]; then
    install -d /usr/share/perl5/PVE/Storage/Custom
    install -m 0644 "$HERE/OCSFS2Plugin.pm" /usr/share/perl5/PVE/Storage/Custom/OCSFS2Plugin.pm
    say "installed PVE storage plugin (PVE::Storage::Custom::OCSFS2Plugin)"
    systemctl reload-or-restart pvedaemon pveproxy 2>/dev/null || \
        say "restart pvedaemon/pveproxy manually to load the plugin"
else
    say "PVE not detected — skipping storage plugin (module + tools still installed)"
fi

# ── 6. periodic online maintenance (scrub + defrag timers) ──────────────────
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

# ── 7. load the module now ──────────────────────────────────────────────────
modprobe ocsfs2 2>/dev/null || true
lsmod | grep -q '^ocsfs2 ' && say "module loaded" || say "module not loaded (load with: modprobe ocsfs2)"

cat <<EOF

OCSFS v2 installed on $(hostname) via DKMS.  Next steps:

  # format the shared LUN ONCE, from a single node.
  #   -N = max cluster nodes baked into the layout (default $((32)); 1 = single-node).
  #   -C = enable per-data-block checksums (silent-corruption detection on any SAN).
  mkfs.ocsfs2 -L vmstore -N 32 -C -f /dev/disk/by-id/<your-lun>

  # mount with the 'cluster' option on EVERY node (required for cross-node coherence):
  mount -t ocsfs2 -o cluster /dev/disk/by-id/<your-lun> /mnt/pve/vmstore

  # or, in /etc/pve/storage.cfg (GUI once the plugin loads):
  #   ocsfs2: vmstore
  #       path /mnt/pve/vmstore
  #       device /dev/disk/by-id/<your-lun>
  #       content images,iso,vztmpl,backup,rootdir,snippets
  #       cluster 1
  #       shared 1

Run this installer once on EVERY node sharing the LUN.  DKMS rebuilds the module
automatically on future kernel upgrades — no need to re-run after an upgrade.
EOF
