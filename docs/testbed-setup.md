# OCSFS v2 — Testbed Setup

How to stand up the 3-node + shared-LUN testbed used to validate OCSFS v2, and
how to run the single-node and cluster tests on it. The filesystem is designed
for **high availability — it must keep serving through node failures and never be
taken offline for maintenance**, so the testbed exists to prove exactly that:
online checks, online grow/defrag, and survival of a node crash.

---

## 1. Topology

```
        TrueNAS (SCST iSCSI target, SCSI-3 PR + Compare-and-Write)
        192.168.1.47  — exports LUNs lun-0..lun-5
                 |  iSCSI (1/10 GbE)
   +-------------+-------------+-------------+
   | node n1     | node n2     | node n3     |   Proxmox VE, identical kernel
   | .48         | .49         | .45         |   (build the module on these)
   +-------------+-------------+-------------+
        all three open the SAME LUN as /dev/sdX
```

- **Real iSCSI LUNs only** — never loopback files (loopback hides the SCSI PR/CAW
  semantics OCSFS relies on and the real flush/cache behaviour).
- Each LUN appears as a plain `/dev/sdX`; map it per node with
  `ls -l /dev/disk/by-path/ | grep 192.168.1.47` (the `lun-N → ../../sdX` link).
  **The same LUN can be a different `/dev/sdX` on each node** — always map by
  `by-path`/`by-id`, never assume the letter.

> Safety: identify the **host OS disk** first (`lsblk -o NAME,SIZE,TYPE,MOUNTPOINT`)
> and never `mkfs` it. Run `findmnt -S /dev/sdX` on every node before formatting a
> LUN. Never `fuser -km` a mountpoint on a PVE node — it can kill `init`.

---

## 2. One-time node prep

On every node (matching kernel):

```bash
apt-get install -y build-essential pve-headers-$(uname -r) \
                   open-iscsi xfsprogs bpftrace
# discover + log in to the target (persists across reboot if node.startup=automatic):
iscsiadm -m discovery -t sendtargets -p 192.168.1.47
iscsiadm -m node -p 192.168.1.47 --login
```

Build + install OCSFS on each node:

```bash
cd OCSFS
make -C /lib/modules/$(uname -r)/build M=$PWD/kmod2 modules   # build the module
cc -O2 -Wall -o mkfs.ocsfs2 tools2/mkfs.c                     # build the tools
cc -O2 -Wall -o fsck.ocsfs2 tools2/fsck.c
# or just: cd proxmox2 && ./install.sh   (module + tools + plugin + timers)
```

The xfstests `fsx` binary (used by the differential harness) lives at
`/root/xfstests/ltp/fsx`; build xfstests once per node if absent.

---

## 3. Create & mount the cluster filesystem

Pick a free LUN (e.g. `lun-1`). On **one** node:

```bash
insmod kmod2/ocsfs2.ko                     # or modprobe ocsfs2
./mkfs.ocsfs2 -f -N 3 /dev/disk/by-path/...lun-1   # 3 node slots
```

On **each** node:

```bash
insmod kmod2/ocsfs2.ko
mkdir -p /mnt/clu
mount -t ocsfs2 -o cluster /dev/disk/by-path/...lun-1 /mnt/clu
```

`-o cluster` turns on membership, fencing, leases and recovery. Confirm all three
joined: each node took a heartbeat slot (`dmesg | grep ocsfs2`).

---

## 4. Single-node data-path validation (do this first)

The whole local engine is proven before trusting the cluster. The reliable
method is **differential vs XFS** (a divergence counts as a bug only if XFS is
correct on the identical op stream):

```bash
# args: <ocsfs_dev> <xfs_dev> [N] ["seeds"]
tests/v2/fsx_diff.sh /dev/sdc /dev/sdd 20000 "1 2 3 4 5 6 7 8"
# -> FSX_DIFF_CLEAN  (buffered + O_DIRECT + clone, all match XFS)
```

Use a second free LUN for the XFS reference (`mkfs.xfs -m reflink=1`). Feature
scripts (run on one node): `test_datapath.sh`, `test_extent.sh`,
`test_reflink.sh`, `test_fallocate.sh`, `test_xattr.sh`, `test_posix.sh`,
`test_dedup.sh`, `test_discard.sh`, `test_grow.sh`, `test_scrub.sh`.
Finish with `fsck.ocsfs2 <dev>` (unmounted) → `clean`.

---

## 5. Cluster validation (2–3 nodes)

```bash
tests/v2/test_cluster.sh           # basic cross-node create/read/delete coherence
tests/v2/test_cluster_dir.sh       # concurrent directory ops on a shared dir
tests/v2/test_cluster_lease.sh     # ownership lease handoff (open/close)
tests/v2/test_cluster_perf.sh      # per-node + aggregate throughput
tests/v2/test_grow_multi.sh        # online grow seen by all nodes
```

Cross-node coherence is checked with **buffered** I/O (not just O_DIRECT): write
on n1, read on n2, verify; ping-pong; `fio --verify`.

---

## 6. High-availability validation (the point of the project)

Prove the FS keeps serving and never needs to be stopped:

**Online maintenance (no unmount):**
```bash
ocsfs2-scrub  /mnt/clu                 # online metadata check (= online fsck)
fsck.ocsfs2   /mnt/clu                 # online check via the scrub ioctl
ocsfs2-defrag -r /mnt/clu              # online defragmentation
ocsfs2-tool   growfs /mnt/clu          # online, repeatable grow
```
Run each while a `fio` load runs on the same mount — it must complete without
errors and without interrupting the load.

**Node-failure survival (HA):**
```bash
# on n1: start a sustained write to a file it owns
fio --name=load --filename=/mnt/clu/vm.raw --rw=randwrite --bs=4k \
    --ioengine=libaio --direct=1 --runtime=120 --time_based &
# hard-kill n1 (power reset / `echo b > /proc/sysrq-trigger`)
# on n2/n3: within a few seconds the survivors fence n1, replay its journal,
#   reclaim its leases; n1's files become writable again with content intact.
tests/v2/test_cluster_recovery.sh      # automates fence + replay + reclaim
fsck.ocsfs2 /mnt/clu                    # online check after recovery -> clean
```
Expected: surviving nodes keep serving their own files throughout; the dead
node's files are unavailable only for the few seconds of fencing+replay, then
recover with no data loss. Rejoin n1 by re-mounting.

> Crash-recovery torture (`test_crash.sh`) crashes mid-reflink / mid-journal-wrap
> and confirms the volume remounts and replays cleanly.

---

## 7. Debugging on the testbed

- **Replay-bisection**: `fsx --record-ops=f.ops <seed>`, then binary-search the
  smallest `head -K f.ops` whose replay makes the OCSFS and XFS files differ
  (`cmp`) — line *K* is the corrupting op.
- **FIEMAP**: `filefrag -v <file>` to see the extent → physical mapping.
- **bpftrace** (installed in §2): probe module functions by name (scalar args
  need no BTF), e.g. trace which code writes a victim block:
  ```bash
  bpftrace -e 'tracepoint:block:block_bio_queue
     /args->sector>=SEC && args->sector<SEC+8/
     { printf("%s %llu %s\n", args->rwbs, args->sector, kstack(5)); }'
  ```
- Always reproduce a suspected bug on XFS/ext4 with the same seed **first**; if it
  fails there too it is a tester artifact, not an OCSFS bug.

---

## 8. Node / LUN cheat-sheet

| Host | Address |
|---|---|
| n1 | `root@192.168.1.48` |
| n2 | `root@192.168.1.49` |
| n3 | `root@192.168.1.45` |
| TrueNAS | `truenas_admin@192.168.1.47` (`midclt` for LUN ops) |

```bash
# map LUNs on a node:
ls -l /dev/disk/by-path/ | grep 192.168.1.47        # lun-N -> ../../sdX
# clean teardown on a node:
umount /mnt/clu 2>/dev/null; rmmod ocsfs2 2>/dev/null
```

`/tmp` on a node is wiped on reboot; iSCSI may need re-login after a reboot
(`iscsiadm -m node --login`).
