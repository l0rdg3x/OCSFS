#!/bin/bash
# Scenario 01: Concurrent mount — N nodes mount simultaneously.
# Pass criteria: no duplicate slot assignment (check Node Slot Table on disk).

scenario_run() {
    echo "Formatting shared device on node 0..."
    run_on 0 "mkfs.ocsfs -L ocsfs-test -n $N_NODES $SHARED_DEV"

    echo "Mounting all nodes simultaneously..."
    run_all "mount -t ocsfs $SHARED_DEV $MOUNT_POINT"

    echo "Checking for duplicate slot assignments..."
    local slots=()
    for ((node=0; node<N_NODES; node++)); do
        slot=$(run_on "$node" "cat /proc/mounts | grep ocsfs | awk '{print \$4}'" \
               || run_on "$node" "dmesg | grep 'ocsfs: mounted' | tail -1 | grep -oP 'slot=\K[0-9]+'")
        echo "  Node $node: slot=$slot"
        for s in "${slots[@]}"; do
            [ "$s" != "$slot" ] || { echo "FAIL: duplicate slot $slot"; return 1; }
        done
        slots+=("$slot")
    done
    echo "All slots unique: ${slots[*]}"

    echo "Unmounting all nodes..."
    run_all "umount $MOUNT_POINT" || true
    return 0
}
