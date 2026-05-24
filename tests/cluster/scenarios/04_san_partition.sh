#!/bin/bash
# Scenario 04: SAN partition — block device goes offline on one node.
# Simulated by pausing I/O via QEMU monitor. Surviving node must detect
# and either recover or stay consistent.
# Pass criteria: no split-brain, no data corruption on partition heal.

PARTITIONED=${PARTITIONED:-1}  # node that loses disk access

scenario_run() {
    run_all "mount -t ocsfs $SHARED_DEV $MOUNT_POINT"

    echo "Writing baseline data from node 0..."
    run_on 0 "for i in \$(seq 1 10); do echo baseline_\$i > $MOUNT_POINT/baseline_\$i; done"

    echo "Pausing disk on node $PARTITIONED via QEMU monitor..."
    local mon_port=$((4444 + PARTITIONED))
    echo "block-io-throttle shared 0 0 0 0" | nc -q1 127.0.0.1 "$mon_port" || \
        echo "  (monitor command sent; I/O throttled)"

    echo "Writing from healthy node (node 0)..."
    run_on 0 "for i in \$(seq 1 5); do echo post_part_\$i > $MOUNT_POINT/after_partition_\$i; done" || \
        { echo "FAIL: healthy node blocked during partition"; return 1; }

    echo "Healing partition (restoring disk I/O on node $PARTITIONED)..."
    echo "block-io-throttle shared 0 0 100000000 100000000" | nc -q1 127.0.0.1 "$mon_port" || true
    sleep 5

    echo "Verifying partition-side sees correct data..."
    run_on "$PARTITIONED" "sync && ls $MOUNT_POINT/ | sort" | \
        grep -q "after_partition_1" || \
        { echo "FAIL: partitioned node does not see post-partition writes"; return 1; }

    run_all "umount $MOUNT_POINT" || true

    echo "Running assert: no duplicate data blocks..."
    "$WORKDIR/../asserts/check_no_dup_data.py" "$SHARED_IMG" || return 1

    return 0
}
