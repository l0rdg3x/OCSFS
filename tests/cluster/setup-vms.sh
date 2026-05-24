#!/bin/bash
# setup-vms.sh — Spin up N QEMU nodes sharing a virtio-scsi disk with PR support.
# Usage: ./setup-vms.sh [N_NODES] [WORKDIR]
# Requirements: qemu-system-x86_64, qemu-pr-helper, ovmf (for UEFI), ssh-keygen
set -euo pipefail

N_NODES=${1:-2}
WORKDIR=${2:-/tmp/ocsfs-cluster}
SHARED_IMG="$WORKDIR/shared.raw"
PR_SOCK="$WORKDIR/pr-helper.sock"
BASE_PORT=10022  # SSH port for node 0; node k gets BASE_PORT+k
MONITOR_PORT=4444

die() { echo "ERROR: $*" >&2; exit 1; }

mkdir -p "$WORKDIR/ssh" "$WORKDIR/pid"

# Generate cluster SSH key
[ -f "$WORKDIR/ssh/id_ed25519" ] || \
    ssh-keygen -t ed25519 -N "" -f "$WORKDIR/ssh/id_ed25519" -q

# Shared block device: 10 GiB raw (virtio-scsi supports PR on raw)
if [ ! -f "$SHARED_IMG" ]; then
    echo "Creating 10 GiB shared block image..."
    truncate -s 10G "$SHARED_IMG"
fi

# Start qemu-pr-helper for Persistent Reservation emulation
if [ ! -S "$PR_SOCK" ]; then
    echo "Starting qemu-pr-helper on $PR_SOCK"
    qemu-pr-helper --socket "$PR_SOCK" --daemon \
        --pidfile "$WORKDIR/pid/pr-helper.pid" || \
        die "qemu-pr-helper failed to start; install qemu-tools"
fi

KERNEL=$(ls /boot/vmlinuz-* 2>/dev/null | sort -V | tail -1)
[ -f "$KERNEL" ] || die "No kernel found in /boot; set KERNEL env var"

echo "Using kernel: $KERNEL"

for ((i=0; i<N_NODES; i++)); do
    NODE_IMG="$WORKDIR/node${i}.qcow2"
    SSH_PORT=$((BASE_PORT + i))
    MON_PORT=$((MONITOR_PORT + i))
    PID_FILE="$WORKDIR/pid/node${i}.pid"

    # Per-node root disk: sparse 5 GiB clone of a base image
    if [ ! -f "$NODE_IMG" ]; then
        BASE_IMG=${BASE_QCOW2:-}
        [ -f "$BASE_IMG" ] || die "Set BASE_QCOW2 to a bootable qcow2 image"
        qemu-img create -f qcow2 -b "$BASE_IMG" -F qcow2 "$NODE_IMG" 5G
    fi

    echo "Starting node $i (SSH :$SSH_PORT, monitor :$MON_PORT)..."
    qemu-system-x86_64 \
        -name "ocsfs-node${i}" \
        -m 2G \
        -smp 2 \
        -drive file="$NODE_IMG",if=virtio,snapshot=on \
        -object pr-manager-helper,id=prman,connected-socket="$PR_SOCK" \
        -drive file="$SHARED_IMG",if=none,id=shared,format=raw,\
aio=native,cache.direct=on \
        -device virtio-scsi-pci,id=scsi0 \
        -device scsi-hd,bus=scsi0.0,drive=shared,share-rw=true,\
serial="OCSFSSHARED001",bus=scsi0.0,lun=0,\
"pr-manager=prman" \
        -netdev "user,id=net0,hostfwd=tcp::${SSH_PORT}-:22" \
        -device virtio-net-pci,netdev=net0 \
        -monitor "tcp:127.0.0.1:${MON_PORT},server,nowait" \
        -display none \
        -daemonize \
        -pidfile "$PID_FILE" \
        -append "root=/dev/vda1 console=ttyS0 quiet" \
        -kernel "$KERNEL" \
        2>"$WORKDIR/node${i}.log"

    echo "Node $i started (PID $(cat "$PID_FILE"))"
done

echo ""
echo "Cluster ready. SSH access:"
for ((i=0; i<N_NODES; i++)); do
    echo "  Node $i: ssh -p $((BASE_PORT+i)) -i $WORKDIR/ssh/id_ed25519 root@127.0.0.1"
done
echo ""
echo "To stop: kill \$(cat $WORKDIR/pid/node*.pid)"
echo "Shared disk: $SHARED_IMG"
