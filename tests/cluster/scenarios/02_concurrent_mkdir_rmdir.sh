#!/bin/bash
# Scenario 02: Concurrent metadata stress — each node creates and removes dirs.
# Pass criteria: no kernel oops, consistent directory tree on all nodes after stop.

N_ITERS=${OCSFS_ITERS:-100}

scenario_run() {
    run_all "mount -t ocsfs $SHARED_DEV $MOUNT_POINT"

    echo "Running $N_ITERS mkdir/rmdir iterations per node in parallel..."
    run_all "bash -c '
        for i in \$(seq 1 $N_ITERS); do
            dir=\"$MOUNT_POINT/node\${HOSTNAME}_\$i\"
            mkdir -p \"\$dir\" 2>/dev/null
            echo \"\$i\" > \"\$dir/seq\"
            rmdir \"\$dir\" 2>/dev/null || true
        done
    '"

    echo "Checking for kernel warnings..."
    if run_all "dmesg | grep -E 'BUG:|WARNING:|Oops:'" 2>/dev/null | grep -q .; then
        echo "FAIL: kernel warnings detected"
        return 1
    fi

    echo "Verifying filesystem consistency (ls on all nodes)..."
    local ref
    ref=$(run_on 0 "ls $MOUNT_POINT/ | sort")
    for ((node=1; node<N_NODES; node++)); do
        local view
        view=$(run_on "$node" "ls $MOUNT_POINT/ | sort")
        [ "$ref" = "$view" ] || { echo "FAIL: node $node sees different tree"; return 1; }
    done

    run_all "umount $MOUNT_POINT" || true
    return 0
}
