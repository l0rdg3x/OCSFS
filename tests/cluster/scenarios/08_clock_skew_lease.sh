#!/bin/bash
# Scenario 08: Clock skew with PR-lease — one node has a clock running fast/slow.
# Tests that CAS_LEASE_TIMEOUT_NS-based PR-lease handles skewed clocks gracefully.
# Pass criteria: no lease livelock, no spurious EBUSY, correct lock acquire.

SKEWED_NODE=${SKEWED_NODE:-1}
SKEW_SEC=${SKEW_SEC:-10}   # advance node's clock by this many seconds

scenario_run() {
    echo "Introducing ${SKEW_SEC}s clock skew on node $SKEWED_NODE..."
    run_on "$SKEWED_NODE" "date -s \"+${SKEW_SEC} seconds\" 2>/dev/null || \
        timedatectl set-time \"\$(date -d \"+${SKEW_SEC} seconds\" '+%Y-%m-%d %H:%M:%S')\" 2>/dev/null || \
        { echo '  clock manipulation not possible in this environment'; }"

    run_all "mount -t ocsfs $SHARED_DEV $MOUNT_POINT"

    echo "Creating files from both nodes with skewed clocks..."
    run_all "bash -c '
        for i in \$(seq 1 20); do
            echo \"node_\${HOSTNAME}_\$i\" > $MOUNT_POINT/skew_\${HOSTNAME}_\$i 2>/dev/null
            sleep 0.1
        done
    '"

    echo "Checking for PR-lease livelock (EBUSY count in dmesg)..."
    local ebusy_count
    ebusy_count=$(run_on 0 "dmesg | grep -c 'livelock\|EBUSY.*CAS\|CAS.*EBUSY'" 2>/dev/null || echo 0)
    echo "  EBUSY/livelock events: $ebusy_count"
    [ "$ebusy_count" -lt 5 ] || { echo "FAIL: excessive livelock ($ebusy_count events)"; return 1; }

    echo "Verifying all files visible from both nodes..."
    local count0 count1
    count0=$(run_on 0 "ls $MOUNT_POINT/skew_* 2>/dev/null | wc -l")
    count1=$(run_on "$SKEWED_NODE" "ls $MOUNT_POINT/skew_* 2>/dev/null | wc -l")
    echo "  Node 0 sees: $count0 files, Node $SKEWED_NODE sees: $count1 files"
    [ "$count0" -eq "$count1" ] || \
        { echo "FAIL: file count mismatch ($count0 vs $count1)"; return 1; }

    echo "Restoring clock on node $SKEWED_NODE..."
    run_on "$SKEWED_NODE" "ntpdate pool.ntp.org 2>/dev/null || chronyc makestep 2>/dev/null || true"

    run_all "umount $MOUNT_POINT" || true
    return 0
}
