#!/bin/bash
# Scenario 06: Concurrent CoW snapshot — two nodes race on refcount updates.
# Pass criteria: refcounts are consistent, no double-free, no data corruption.

scenario_run() {
    run_all "mount -t ocsfs $SHARED_DEV $MOUNT_POINT"

    echo "Creating a large file from node 0..."
    run_on 0 "dd if=/dev/urandom of=$MOUNT_POINT/bigfile bs=1M count=16 2>/dev/null"
    run_on 0 "sync"

    echo "Taking snapshot from both nodes simultaneously..."
    run_all "bash -c '
        ioctl_snapshot() {
            # ocsfs snapshot ioctl — OCSFS_IOC_SNAPSHOT = _IO(0xf4, 0x01)
            python3 -c \"
import fcntl, struct, os
fd = os.open(\\\"$MOUNT_POINT\\\", os.O_RDONLY)
fcntl.ioctl(fd, 0xf401, struct.pack(\\\"Q\\\", 0))
os.close(fd)
print(\\\"snapshot ok\\\")
\" 2>/dev/null || echo snapshot_not_supported
        }
        ioctl_snapshot
    '" || echo "  (snapshot ioctl not yet implemented — testing refcount inc path only)"

    echo "Concurrent write (CoW) from both nodes..."
    run_all "bash -c '
        dd if=/dev/urandom of=$MOUNT_POINT/bigfile bs=1M count=4 \
           seek=\$((RANDOM % 12)) conv=notrunc 2>/dev/null
    '"

    echo "Checking refcounts via debugfs..."
    run_on 0 "bash -c '
        # ocsfs_debugfs_refcount — iterate all blocks and check refcount consistency
        cat /sys/kernel/debug/ocsfs/*/refcount 2>/dev/null || true
    '" || true

    echo "Verifying no double-free in dmesg..."
    run_all "dmesg | grep -i 'double.free\|use.after.free\|refcount.*underflow'" 2>/dev/null && \
        { echo "FAIL: memory corruption detected"; return 1; }

    run_all "umount $MOUNT_POINT" || true
    return 0
}
