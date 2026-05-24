#!/bin/bash
# check_no_split_journal.sh — Verify that no two nodes wrote to the same
# journal area. Each node occupies a fixed slot in the journal region;
# this script checks that journal headers have unique slot assignments.
#
# Usage: check_no_split_journal.sh <image_file> [block_size]
# Exit 0 = clean, 1 = split detected.

set -euo pipefail

IMAGE=${1:?Usage: $0 <image_file> [block_size]}
BLOCK_SIZE=${2:-4096}

JOURNAL_OFF=73728         # OCSFS_JOURNAL_OFF (from ocsfs.h)
JOURNAL_NODE_SIZE=5242880 # OCSFS_JOURNAL_SIZE_PER_NODE
JOURNAL_MAGIC=0x4F434A4C  # 'OCJL'

python3 - "$IMAGE" "$BLOCK_SIZE" "$JOURNAL_OFF" "$JOURNAL_NODE_SIZE" "$JOURNAL_MAGIC" <<'PYEOF'
import sys, struct

image, bs, joff, jnode_size, jmagic = \
    sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5], 0)

seen_slots = {}
errors = 0

with open(image, "rb") as f:
    slot = 0
    while True:
        slot_off = joff + slot * jnode_size
        f.seek(slot_off)
        hdr = f.read(bs)
        if len(hdr) < 16:
            break
        magic, node_slot, seq = struct.unpack_from("<IHxxQ", hdr)
        if magic != jmagic:
            break   # end of populated journal area
        print(f"  Journal slot {slot}: node_slot={node_slot}, seq={seq}")
        if node_slot in seen_slots:
            print(f"ERROR: node_slot {node_slot} appears in journal slots "
                  f"{seen_slots[node_slot]} and {slot} — split brain!")
            errors += 1
        else:
            seen_slots[node_slot] = slot
        slot += 1

if errors:
    print(f"FAIL: {errors} split-journal conflicts")
    sys.exit(1)
else:
    print(f"PASS: {len(seen_slots)} journal slots, no conflicts")
    sys.exit(0)
PYEOF
