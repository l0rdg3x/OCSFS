#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# install.sh — Install OCSFS Proxmox VE storage plugin
#
# Installs the PVE storage plugin, mount helper, and CLI tools
# into their correct system paths on a Proxmox VE host.
#
# Usage: sudo ./install.sh
#

set -e

PROG="ocsfs-pve-install"

die() {
    echo "$PROG: ERROR: $*" >&2
    exit 1
}

info() {
    echo ":: $*"
}

# Must run as root
[ "$(id -u)" -eq 0 ] || die "must run as root"

# Check for Proxmox VE
if [ ! -d /usr/share/perl5/PVE/Storage ]; then
    die "Proxmox VE not found (missing /usr/share/perl5/PVE/Storage)"
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Install the Perl storage plugin
PLUGIN_SRC="$SCRIPT_DIR/OCSFSPlugin.pm"
PLUGIN_DST="/usr/share/perl5/PVE/Storage/OCSFSPlugin.pm"

if [ ! -f "$PLUGIN_SRC" ]; then
    die "OCSFSPlugin.pm not found at $PLUGIN_SRC"
fi

info "Installing storage plugin to $PLUGIN_DST"
install -m 0644 "$PLUGIN_SRC" "$PLUGIN_DST"

# Install mount helper
MOUNT_SRC="$SCRIPT_DIR/mount.ocsfs"
MOUNT_DST="/usr/sbin/mount.ocsfs"

if [ -f "$MOUNT_SRC" ]; then
    info "Installing mount helper to $MOUNT_DST"
    install -m 0755 "$MOUNT_SRC" "$MOUNT_DST"
fi

# Install CLI tools if built
if [ -f "$PROJECT_DIR/mkfs.ocsfs" ]; then
    info "Installing mkfs.ocsfs to /usr/sbin/"
    install -m 0755 "$PROJECT_DIR/mkfs.ocsfs" /usr/sbin/mkfs.ocsfs
fi

if [ -f "$PROJECT_DIR/ocsfs-tool" ]; then
    info "Installing ocsfs-tool to /usr/sbin/"
    install -m 0755 "$PROJECT_DIR/ocsfs-tool" /usr/sbin/ocsfs-tool
fi

# Install DKMS module source
KMOD_DIR="$PROJECT_DIR/kmod"
DKMS_SRC="/usr/src/ocsfs-0.1.0"

if [ -d "$KMOD_DIR" ] && [ -f "$KMOD_DIR/dkms.conf" ]; then
    info "Installing kernel module source to $DKMS_SRC"
    rm -rf "$DKMS_SRC"
    mkdir -p "$DKMS_SRC"
    cp -a "$KMOD_DIR"/* "$DKMS_SRC/"

    info "Registering with DKMS"
    dkms add -m ocsfs -v 0.1.0 2>/dev/null || true
    dkms build -m ocsfs -v 0.1.0 || die "DKMS build failed"
    dkms install -m ocsfs -v 0.1.0 || die "DKMS install failed"
fi

# Restart PVE services to pick up the new plugin
info "Restarting pvedaemon..."
systemctl restart pvedaemon 2>/dev/null || true

info "Restarting pvestatd..."
systemctl restart pvestatd 2>/dev/null || true

info ""
info "OCSFS Proxmox VE plugin installed successfully."
info ""
info "Add storage in /etc/pve/storage.cfg:"
info ""
info "  ocsfs: my-datastore"
info "    path /mnt/pve/my-datastore"
info "    device /dev/mapper/mpath-XXXX"
info "    content images,iso,vztmpl,backup,rootdir,snippets"
info "    maxnodes 16"
info "    thin 1"
info "    shared 1"
info ""
info "Or via pvesm:"
info "  pvesm add ocsfs my-datastore \\"
info "    --path /mnt/pve/my-datastore \\"
info "    --device /dev/mapper/mpath-XXXX \\"
info "    --content images,iso,vztmpl,backup,rootdir,snippets \\"
info "    --maxnodes 16 --thin 1 --shared 1"
