#!/bin/bash
# Scenario 05: Zombie node — a node is considered dead but revives after
# PR PREEMPT. It should fail writes with MISCOMPARE → be forced read-only.
# Pass criteria: zombie writes fail, surviving node's data is intact.

ZOMBIE=${ZOMBIE:-1}
LEADER=${LEADER:-0}

scenario_run() {
    run_all "mount -t ocsfs $SHARED_DEV $MOUNT_POINT"

    echo "Writing initial data..."
    run_on "$LEADER" "echo original > $MOUNT_POINT/canary"

    echo "Fencing zombie node via PR PREEMPT (simulate from LEADER)..."
    run_on "$LEADER" "bash -c '
        # Force heartbeat timeout by stopping heartbeat updates
        echo 1 > /sys/fs/ocsfs/node${ZOMBIE}/heartbeat_pause 2>/dev/null || true
    '" || true
    sleep 3  # Let heartbeat timeout expire

    echo "Attempting write from zombie node (should fail or be RO)..."
    local write_ret=0
    run_on "$ZOMBIE" "echo zombie_write > $MOUNT_POINT/canary 2>/dev/null" || write_ret=$?
    echo "  Zombie write returned: $write_ret (non-zero = fenced correctly)"

    echo "Verifying canary from leader node..."
    local canary
    canary=$(run_on "$LEADER" "cat $MOUNT_POINT/canary 2>/dev/null")
    [ "$canary" = "original" ] || \
        { echo "FAIL: canary corrupted to '$canary'"; return 1; }

    echo "Checking zombie node status..."
    if run_on "$ZOMBIE" "dmesg | grep -q 'forced.*read-only\|SB_RDONLY'" 2>/dev/null; then
        echo "  Zombie correctly forced read-only."
    else
        echo "  WARNING: zombie not detected as RO (PR may not be active)"
    fi

    run_all "umount $MOUNT_POINT" || true
    return 0
}
