#!/usr/bin/env python3
"""
check_no_dup_data.py — Scan a raw OCSFS image for duplicate data blocks.

Reads the block bitmap from each AG and verifies that no physical block
is allocated in more than one AG (overlap check). Also hashes each
allocated block and reports duplicates with identical content at
different physical locations (potential CoW aliasing bug).

Usage: check_no_dup_data.py <image_file> [--block-size 4096]
Exit 0 = clean, 1 = duplicates/overlap found.
"""

import sys
import struct
import hashlib
import argparse
from pathlib import Path

OCSFS_MAGIC = 0x4F435346  # 'OCSF'
DEFAULT_BLOCK_SIZE = 4096

# Superblock offsets (block 0)
SB_MAGIC_OFF     = 0
SB_BLOCK_SIZE_OFF = 8
SB_AG_COUNT_OFF  = 16
SB_AG_SIZE_OFF   = 24
SB_AG_DESC_OFF   = 80


def read_block(f, block_no, block_size):
    f.seek(block_no * block_size)
    return f.read(block_size)


def iter_bits(bitmap_bytes):
    for byte_idx, byte in enumerate(bitmap_bytes):
        for bit in range(8):
            if byte & (1 << bit):
                yield byte_idx * 8 + bit


def main():
    parser = argparse.ArgumentParser(description="OCSFS duplicate block checker")
    parser.add_argument("image", help="Path to raw OCSFS image")
    parser.add_argument("--block-size", type=int, default=DEFAULT_BLOCK_SIZE)
    args = parser.parse_args()

    path = Path(args.image)
    if not path.exists():
        print(f"ERROR: {args.image} not found", file=sys.stderr)
        sys.exit(2)

    bs = args.block_size
    errors = 0

    with open(path, "rb") as f:
        sb = read_block(f, 0, bs)
        magic = struct.unpack_from("<I", sb, SB_MAGIC_OFF)[0]
        if magic != OCSFS_MAGIC:
            print(f"ERROR: bad magic 0x{magic:08x} (expected 0x{OCSFS_MAGIC:08x})")
            sys.exit(2)

        ag_count = struct.unpack_from("<I", sb, SB_AG_COUNT_OFF)[0]
        ag_size  = struct.unpack_from("<Q", sb, SB_AG_SIZE_OFF)[0]
        ag_desc_off = struct.unpack_from("<Q", sb, SB_AG_DESC_OFF)[0]

        print(f"OCSFS image: {ag_count} AGs, AG size {ag_size} blocks, bs={bs}")

        allocated_blocks = set()
        content_hashes = {}  # hash -> first block number

        for ag_no in range(ag_count):
            ag_desc_block = ag_desc_off // bs + ag_no
            ag = read_block(f, ag_desc_block, bs)
            # ocsfs_disk_ag: block_start(u64), block_count(u64), bitmap_off(u64), ...
            block_start  = struct.unpack_from("<Q", ag, 0)[0]
            block_count  = struct.unpack_from("<Q", ag, 8)[0]
            bitmap_off   = struct.unpack_from("<Q", ag, 16)[0]

            bitmap_blocks = (block_count + bs * 8 - 1) // (bs * 8)
            bitmap = b""
            for bm_block in range(int(bitmap_blocks)):
                bm_lba = bitmap_off // bs + bm_block
                bitmap += read_block(f, bm_lba, bs)

            for local_bit in iter_bits(bitmap):
                if local_bit >= block_count:
                    break
                phys = block_start + local_bit
                if phys in allocated_blocks:
                    print(f"ERROR: block {phys} allocated in multiple AGs (AG {ag_no})")
                    errors += 1
                else:
                    allocated_blocks.add(phys)

                data = read_block(f, phys, bs)
                h = hashlib.sha256(data).hexdigest()
                if h in content_hashes and content_hashes[h] != phys:
                    print(f"WARN: block {phys} has same content as block "
                          f"{content_hashes[h]} (possible CoW aliasing)")
                else:
                    content_hashes[h] = phys

        print(f"Checked {len(allocated_blocks)} allocated blocks across {ag_count} AGs")
        if errors:
            print(f"FAIL: {errors} overlap errors found")
            sys.exit(1)
        else:
            print("PASS: no duplicate/overlapping blocks")
            sys.exit(0)


if __name__ == "__main__":
    main()
