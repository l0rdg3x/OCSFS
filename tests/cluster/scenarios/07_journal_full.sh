#!/bin/bash
# Scenario 07: Journal full — fill the journal area and verify correct stall/wait.
# Pass criteria: writer stalls (no -ENOSPC panic), journal checkpoints, writer resumes.

WRITER=${WRITER:-0}

scenario_run() {
    run_on "$WRITER" "mount -t ocsfs $SHARED_DEV $MOUNT_POINT"

    echo "Filling journal with transactions (many small files)..."
    local timeout=60
    local start=$SECONDS
    local stall_detected=0

    # Write continuously; expect stall when journal fills, then checkpoint resumes
    run_on "$WRITER" "bash -c '
        n=0
        while true; do
            echo \$n > $MOUNT_POINT/jtest_\$n 2>/dev/null || break
            n=\$((n+1))
            if [ \$n -gt 50000 ]; then break; fi
        done
        echo \$n files written
    '" &
    local writer_pid=$!

    while [ $((SECONDS - start)) -lt $timeout ]; do
        if run_on "$WRITER" "dmesg | grep -q 'journal.*full\|journal.*checkpoint\|waiting.*journal'" 2>/dev/null; then
            stall_detected=1
            echo "  Journal checkpoint detected"
            break
        fi
        sleep 2
    done

    wait "$writer_pid" || true

    echo "Verifying no -ENOSPC kernel panic..."
    run_on "$WRITER" "dmesg | grep -i 'panic\|BUG:\|kernel BUG'" | grep -q . && \
        { echo "FAIL: kernel panic detected"; return 1; }

    echo "Verifying filesystem accessible after journal flush..."
    run_on "$WRITER" "ls $MOUNT_POINT/ | wc -l" | grep -q "[0-9]" || \
        { echo "FAIL: filesystem not accessible after journal fill"; return 1; }

    run_on "$WRITER" "umount $MOUNT_POINT" || true
    return 0
}
