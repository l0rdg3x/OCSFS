#!/bin/bash
# run-test.sh — Execute a cluster scenario on all QEMU nodes via SSH.
# Usage: ./run-test.sh <scenario_number> [WORKDIR] [N_NODES]
# Example: ./run-test.sh 01 /tmp/ocsfs-cluster 2
set -euo pipefail

SCENARIO=${1:-}
WORKDIR=${2:-/tmp/ocsfs-cluster}
N_NODES=${3:-2}
BASE_PORT=10022
SSH_KEY="$WORKDIR/ssh/id_ed25519"
SCENARIO_DIR="$(dirname "$0")/scenarios"
ASSERT_DIR="$(dirname "$0")/asserts"
MOUNT_POINT="/mnt/ocsfs"
SHARED_DEV="/dev/sda"   # virtio-scsi disk in guest
LOG="$WORKDIR/test-${SCENARIO}.log"

[ -n "$SCENARIO" ] || { echo "Usage: $0 <scenario_number>"; exit 1; }

# Find scenario script
SCRIPT=$(ls "$SCENARIO_DIR/${SCENARIO}_"*.sh 2>/dev/null | head -1)
[ -f "$SCRIPT" ] || { echo "No scenario '$SCENARIO' found in $SCENARIO_DIR"; exit 1; }

SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10 -i $SSH_KEY"

# Wait for all nodes to be reachable
wait_ssh() {
    local max=60 node port
    for ((node=0; node<N_NODES; node++)); do
        port=$((BASE_PORT + node))
        echo -n "Waiting for node $node (port $port)..."
        for ((i=0; i<max; i++)); do
            ssh $SSH_OPTS -p "$port" root@127.0.0.1 true 2>/dev/null && break
            sleep 2
        done
        [ $i -lt $max ] || { echo " TIMEOUT"; exit 1; }
        echo " OK"
    done
}

# Run a command on one node
run_on() {
    local node=$1; shift
    local port=$((BASE_PORT + node))
    ssh $SSH_OPTS -p "$port" root@127.0.0.1 "$@"
}

# Run a command on all nodes in parallel, wait for all
run_all() {
    local pids=() node
    for ((node=0; node<N_NODES; node++)); do
        run_on "$node" "$@" &
        pids+=($!)
    done
    local ok=0
    for pid in "${pids[@]}"; do
        wait "$pid" || ok=1
    done
    return $ok
}

# Install/load the kernel module on all nodes
setup_module() {
    local mod_dir
    mod_dir="$(dirname "$0")/../../kmod"
    for ((node=0; node<N_NODES; node++)); do
        scp $SSH_OPTS -P $((BASE_PORT+node)) "$mod_dir/ocsfs.ko" \
            root@127.0.0.1:/tmp/ocsfs.ko
        run_on "$node" "insmod /tmp/ocsfs.ko 2>/dev/null || true"
    done
}

echo "=== OCSFS Cluster Test: scenario $SCENARIO ===" | tee "$LOG"
echo "Nodes: $N_NODES, Workdir: $WORKDIR" | tee -a "$LOG"
date | tee -a "$LOG"

wait_ssh
setup_module

# Source and execute the scenario
# shellcheck source=/dev/null
source "$SCRIPT"

echo ""
echo "=== Running scenario: $(basename "$SCRIPT") ===" | tee -a "$LOG"
scenario_run 2>&1 | tee -a "$LOG"
RESULT=${PIPESTATUS[0]}

echo ""
if [ $RESULT -eq 0 ]; then
    echo "PASS: $SCENARIO" | tee -a "$LOG"
else
    echo "FAIL: $SCENARIO (exit $RESULT)" | tee -a "$LOG"
fi

exit $RESULT
