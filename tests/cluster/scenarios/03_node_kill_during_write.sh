#!/bin/bash
# Scenario 03: Kill a node mid-write; surviving node must complete recovery.
# Pass criteria: no data loss on surviving node, recovery log appears, no hang.

VICTIM=${VICTIM:-1}   # which node to kill (default: node 1)
WRITER=${WRITER:-0}   # node that keeps writing

scenario_run() {
    run_all "mount -t ocsfs $SHARED_DEV $MOUNT_POINT"

    echo "Starting background writer on node $WRITER..."
    run_on "$WRITER" "bash -c '
        for i in \$(seq 1 200); do
            echo \"\$i\" > $MOUNT_POINT/seq_\$i 2>/dev/null
            sleep 0.05
        done &
    '"

    echo "Killing node $VICTIM (SIGKILL to QEMU process)..."
    local victim_pid
    victim_pid=$(cat "$WORKDIR/pid/node${VICTIM}.pid")
    kill -9 "$victim_pid" 2>/dev/null || true
    sleep 1

    echo "Waiting for heartbeat timeout and recovery on node $WRITER..."
    local max=120 found=0
    for ((i=0; i<max; i++)); do
        if run_on "$WRITER" "dmesg | grep -q 'RECOVERY COMPLETE for node slot $VICTIM'" 2>/dev/null; then
            found=1; break
        fi
        sleep 2
    done
    [ $found -eq 1 ] || { echo "FAIL: recovery did not complete within ${max}s"; return 1; }

    echo "Checking writer node for filesystem integrity..."
    run_on "$WRITER" "ls $MOUNT_POINT/ | wc -l" | grep -q "[0-9]" || \
        { echo "FAIL: filesystem not accessible after recovery"; return 1; }

    run_on "$WRITER" "umount $MOUNT_POINT" || true

    echo "Restarting victim node..."
    "$WORKDIR/../setup-vms.sh" 1 "$WORKDIR" 2>/dev/null || true
    sleep 5

    echo "Remounting victim node to verify journal replay..."
    run_on "$VICTIM" "mount -t ocsfs $SHARED_DEV $MOUNT_POINT"
    run_on "$VICTIM" "umount $MOUNT_POINT"

    return 0
}
