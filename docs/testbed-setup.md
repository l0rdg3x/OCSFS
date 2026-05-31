# OCSFS Test Infrastructure Setup

**Version:** 0.1 — May 2026
**Status:** Alpha — research / development
**Audience:** Developers setting up a local multi-node OCSFS test environment

---

## 1. Architecture Overview

The reference testbed consists of three Debian 12 VMs running inside Proxmox VE, all
sharing a single iSCSI LUN exported by TrueNAS Scale. The Proxmox host provides
compute, the TrueNAS node provides shared block storage.

```
┌─────────────────────────────────────────────────────────────────────┐
│  Proxmox VE Host (8c/16t, 128 GB RAM, Gentoo hardened 7.0.10-p1)   │
│                                                                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │
│  │  ocsfs-node1 │  │  ocsfs-node2 │  │  ocsfs-node3 │              │
│  │  Debian 12   │  │  Debian 12   │  │  Debian 12   │              │
│  │  4 vCPU      │  │  4 vCPU      │  │  4 vCPU      │              │
│  │  8 GB RAM    │  │  8 GB RAM    │  │  8 GB RAM    │              │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘              │
│         │                 │                 │                       │
│         └─────────────────┴─────────────────┘                      │
│                           │ iSCSI (virtio NIC / bridge)             │
└───────────────────────────┼─────────────────────────────────────────┘
                            │
            ┌───────────────▼───────────────┐
            │  TrueNAS Scale (≥ 23.10)      │
            │  iSCSI target — SCSI-3 PR     │
            │  zvol: tank/ocsfs-test (50 GB)│
            └───────────────────────────────┘
```

Network layout used in this guide:

| Host | Management IP | iSCSI IP |
|---|---|---|
| TrueNAS | 192.168.10.10 | 192.168.20.10 |
| ocsfs-node1 | 192.168.10.21 | 192.168.20.21 |
| ocsfs-node2 | 192.168.10.22 | 192.168.20.22 |
| ocsfs-node3 | 192.168.10.23 | 192.168.20.23 |

Adjust addresses to match your environment. A dedicated iSCSI VLAN or bridge is
strongly recommended to isolate storage traffic.

---

## 2. TrueNAS Configuration

### 2.1 Create a zvol for OCSFS

Log into the TrueNAS Scale web UI or SSH into the TrueNAS host.

**Via SSH / shell:**

```bash
# Create a 50 GB zvol (thick provisioning for predictable latency)
zfs create -V 50G -s tank/ocsfs-test

# Enable SCSI Persistent Reservations on the zvol
zfs set scsi_pr=on tank/ocsfs-test

# Verify
zfs get scsi_pr tank/ocsfs-test
# NAME              PROPERTY  VALUE  SOURCE
# tank/ocsfs-test   scsi_pr   on     local
```

**Via TrueNAS Scale UI (alternative):**

1. Storage → Pools → your pool → vertical dots → Add Zvol.
2. Name: `ocsfs-test`, Size: `50 GiB`, Sync: `Standard`, Sparse: unchecked.
3. After creation: click the zvol → Edit → scroll to **SCSI Pr** → enable → Save.

> **Note:** TrueNAS Scale 23.10+ exposes `scsi_pr` as a zvol property, which
> maps to `pr_capable=1` in the kernel's zvol driver. Earlier versions do not
> support SCSI-3 PR on zvols; upgrade before attempting cluster mounts.

### 2.2 Configure iSCSI target with SCSI-3 PR support

**Via TrueNAS Scale UI:**

1. Sharing → iSCSI → Portals → Add:
   - IP: `192.168.20.10`, Port: `3260`
2. Sharing → iSCSI → Initiators → Add:
   - Leave blank (allow all) for the test environment, or add the IQNs of the
     three nodes explicitly for security.
3. Sharing → iSCSI → Extents → Add:
   - Name: `ocsfs-test-extent`
   - Extent Type: `Device`
   - Device: select `tank/ocsfs-test`
   - Logical Block Size: `512` (or `4096` if your kernel supports it — 512 is
     safer for initial testing)
   - **Disable Physical Block Size Reporting:** leave unchecked
4. Sharing → iSCSI → Targets → Add:
   - Name: `iqn.2024-01.net.truenas:ocsfs-test`
   - Portal Group: the portal created above
5. Sharing → iSCSI → Associated Targets → Add:
   - Target: `ocsfs-test`, Extent: `ocsfs-test-extent`, LUN ID: `0`
6. Sharing → iSCSI → Enable iSCSI service if not already running.

**Via targetcli (if running a manual Linux iSCSI target instead of TrueNAS):**

```bash
targetcli
/backstores/block create name=ocsfs-test dev=/dev/zvol/tank/ocsfs-test
/iscsi create iqn.2024-01.net.truenas:ocsfs-test
/iscsi/iqn.2024-01.net.truenas:ocsfs-test/tpg1/luns create \
    /backstores/block/ocsfs-test
/iscsi/iqn.2024-01.net.truenas:ocsfs-test/tpg1/acls create \
    iqn.1993-08.org.debian:node1
/iscsi/iqn.2024-01.net.truenas:ocsfs-test/tpg1/acls create \
    iqn.1993-08.org.debian:node2
/iscsi/iqn.2024-01.net.truenas:ocsfs-test/tpg1/acls create \
    iqn.1993-08.org.debian:node3
# Enable PR support explicitly in the kernel target
/backstores/block/ocsfs-test set attribute pr_enable=1
saveconfig
exit
```

### 2.3 Verify PR support from Linux

Run from any VM that has the device connected (see section 3.2):

```bash
# Install sg3-utils if missing
apt-get install -y sg3-utils

# Query Persistent Reservation keys — should succeed without error
sg_persist --in -k /dev/sdb

# Expected output on a clean LUN (no registrants yet):
# PR generation=0x0
# there are NO registered reservation keys

# If the target does not support PR, you will see:
# Persistent reservation in: command not supported
# In that case, verify zfs set scsi_pr=on and restart the iSCSI target service.

# Full inquiry including supported PR types
sg_persist --in --report-capabilities /dev/sdb
# Look for: "Persist Through Power Loss active (PTPL_A): 1"
# and: "Type Mask Description: 0x82" (Write Exclusive, Exclusive Access)
```

---

## 3. Proxmox VM Setup

### 3.1 Create 3 VMs

From the Proxmox host, create three identical VMs. The commands below use the
Proxmox CLI (`qm`); adjust IDs (101, 102, 103) and paths as needed.

```bash
# Download Debian 12 netinst ISO once
wget -P /var/lib/vz/template/iso/ \
    https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/debian-12.9.0-amd64-netinst.iso

# Create VM 101 (ocsfs-node1) — repeat with IDs 102, 103 for node2, node3
qm create 101 \
    --name ocsfs-node1 \
    --memory 8192 \
    --cores 4 \
    --sockets 1 \
    --cpu host \
    --ostype l26 \
    --boot order=ide2 \
    --ide2 local:iso/debian-12.9.0-amd64-netinst.iso,media=cdrom \
    --scsi0 local-lvm:32,ssd=1,discard=on \
    --scsihw virtio-scsi-pci \
    --net0 virtio,bridge=vmbr0 \
    --net1 virtio,bridge=vmbr1 \
    --agent enabled=1

qm start 101
```

- `vmbr0` = management network (192.168.10.0/24)
- `vmbr1` = iSCSI storage network (192.168.20.0/24)

Install Debian 12 with a minimal base system. Enable SSH, no desktop environment.
After installation remove the ISO and set the boot order to scsi0.

```bash
qm set 101 --ide2 none --boot order=scsi0
```

Repeat for VM 102 (ocsfs-node2) and VM 103 (ocsfs-node3).

### 3.2 Connect VMs to the shared iSCSI LUN

Perform the following steps on **all three VMs** unless noted otherwise.

```bash
# Install open-iscsi
apt-get install -y open-iscsi sg3-utils multipath-tools

# Set a unique initiator IQN (do this BEFORE starting iscsid)
# Replace N with 1, 2, or 3 on each node
echo "InitiatorName=iqn.1993-08.org.debian:node<N>" \
    > /etc/iscsi/initiatorname.iscsi

# Start iscsid
systemctl enable --now iscsid

# Discover targets on the TrueNAS iSCSI portal
iscsiadm -m discovery -t st -p 192.168.20.10:3260

# Expected output:
# 192.168.20.10:3260,1 iqn.2024-01.net.truenas:ocsfs-test

# Log in to the target (persistent across reboots)
iscsiadm -m node \
    -T iqn.2024-01.net.truenas:ocsfs-test \
    -p 192.168.20.10:3260 \
    --op update -n node.startup -v automatic

iscsiadm -m node \
    -T iqn.2024-01.net.truenas:ocsfs-test \
    -p 192.168.20.10:3260 \
    --login

# Verify session is up
iscsiadm -m session
# tcp: [1] 192.168.20.10:3260,1 iqn.2024-01.net.truenas:ocsfs-test (non-flash)
```

**Multipath note:** For the initial testing phase, disable multipath on the iSCSI
device to keep the setup simple. If you add a second path later:

```bash
# Minimal /etc/multipath.conf for TrueNAS zvol
cat > /etc/multipath.conf << 'EOF'
defaults {
    user_friendly_names yes
    find_multipaths yes
}
blacklist_exceptions {
    property "(SCSI_IDENT_.*|ID_WWN)"
}
EOF
systemctl enable --now multipathd
```

For single-path testing you can blacklist everything and use `/dev/sdb` directly.

### 3.3 Verify shared block device on all VMs

Run on each node after logging in to the iSCSI target:

```bash
# Check the new device appears
lsblk | grep -E 'NAME|sdb'
# NAME   MAJ:MIN RM  SIZE RO TYPE MOUNTPOINTS
# sdb      8:16   0   50G  0 disk

# Verify it is the same LUN (same WWN on all nodes)
sg_inq /dev/sdb | grep -i "unit serial"
# Unit serial number: ...

# Confirm SCSI-3 PR is advertised
sg_persist --in --report-capabilities /dev/sdb | grep -i "ptpl\|type mask"
```

The Unit Serial Number must be identical across all three nodes.

---

## 4. Build and Install OCSFS

### 4.1 Build kmod on Proxmox host

The kernel module is built against the currently running kernel. On the Proxmox
host (Gentoo hardened 7.0.10-p1):

```bash
cd /home/l0rdg3x/coding/OCSFS/kmod

# Ensure kernel headers are available
ls /lib/modules/$(uname -r)/build

# Build
make -j$(nproc)

# Expected: ocsfs.ko produced without errors
ls -lh ocsfs.ko
```

If `CONFIG_MODULE_SIG_FORCE=y` is set in the hardened Gentoo config, you must
sign the module before loading it:

```bash
# Check whether forced signing is active
grep -i "MODULE_SIG_FORCE\|MODULE_SIG_ALL" \
    /boot/config-$(uname -r) 2>/dev/null || \
    zcat /proc/config.gz | grep -i "MODULE_SIG"

# If signing is required, sign with the ephemeral key
/usr/src/linux-$(uname -r)/scripts/sign-file \
    sha256 \
    /usr/src/linux-$(uname -r)/certs/signing_key.pem \
    /usr/src/linux-$(uname -r)/certs/signing_key.x509 \
    ocsfs.ko
```

### 4.2 Install on VMs

**Option A — copy and insmod (quick, no DKMS):**

```bash
# From the Proxmox host, copy the module to all nodes
for node in 192.168.10.21 192.168.10.22 192.168.10.23; do
    scp /home/l0rdg3x/coding/OCSFS/kmod/ocsfs.ko root@${node}:/root/
done
```

On each VM:

```bash
# Install required kernel headers to satisfy depmod
apt-get install -y linux-headers-$(uname -r)

# Load the module
insmod /root/ocsfs.ko

# Persist across reboots (copy to the standard module path first)
cp /root/ocsfs.ko /lib/modules/$(uname -r)/extra/
depmod -a
echo "ocsfs" >> /etc/modules
```

**Option B — DKMS (recompiles on kernel upgrades):**

```bash
# On the Proxmox host: install DKMS and copy sources to VMs
for node in 192.168.10.21 192.168.10.22 192.168.10.23; do
    ssh root@${node} "apt-get install -y dkms linux-headers-\$(uname -r)"
    scp -r /home/l0rdg3x/coding/OCSFS/kmod/ \
        root@${node}:/usr/src/ocsfs-0.1/
done
```

On each VM:

```bash
# Add DKMS source tree descriptor
cat > /usr/src/ocsfs-0.1/dkms.conf << 'EOF'
PACKAGE_NAME="ocsfs"
PACKAGE_VERSION="0.1"
BUILT_MODULE_NAME[0]="ocsfs"
DEST_MODULE_LOCATION[0]="/extra"
AUTOINSTALL="yes"
MAKE[0]="make -j$(nproc) KVER=$kernelver"
CLEAN="make clean"
EOF

dkms add -m ocsfs -v 0.1
dkms build -m ocsfs -v 0.1
dkms install -m ocsfs -v 0.1
modprobe ocsfs
```

**Option C — build in-VM (most accurate, uses VM's exact kernel):**

```bash
# On each VM
apt-get install -y build-essential linux-headers-$(uname -r) git

git clone <your-ocsfs-repo> /usr/src/ocsfs
cd /usr/src/ocsfs/kmod
make -j$(nproc)
insmod ocsfs.ko
```

### 4.3 Verify module loads

Run on each node:

```bash
# Confirm the module is loaded
lsmod | grep ocsfs
# ocsfs   1234567  0

# Inspect kernel messages
dmesg | grep -i ocsfs | tail -20
# Should show: "ocsfs: module loaded" or similar init messages

# Verify filesystem type is registered
grep ocsfs /proc/filesystems
# nodev   ocsfs
```

---

## 5. First Format and Mount

### 5.1 Format (from one node only)

Format must be run on **exactly one node** while the device is not mounted on
any node. SSH into node1 for this step.

```bash
# Ensure the device is not in use
fuser /dev/sdb 2>/dev/null || echo "device free"

# Basic format (4 nodes maximum, 1 GB allocation groups, label "testcluster")
/path/to/OCSFS/tools/mkfs.ocsfs \
    --max-nodes 4 \
    --ag-size 1G \
    --label testcluster \
    /dev/sdb

# With compression enabled (optional for testing)
/path/to/OCSFS/tools/mkfs.ocsfs \
    --max-nodes 4 \
    --ag-size 1G \
    --label testcluster \
    --features compress \
    /dev/sdb

# Expected output ends with:
# Writing superblock... done
# Writing superblock mirror... done
# mkfs.ocsfs: /dev/sdb formatted successfully
```

**Important:** `mkfs.ocsfs` writes the superblock, node slot table, heartbeat
region, lock table, per-node journals, and AG descriptors. This is a destructive
operation. Double-check the device path before running.

### 5.2 Mount on all nodes

Mount on node1 first, then node2 and node3. The first mount initialises cluster
mode; subsequent mounts join the existing cluster.

```bash
# On each node — create the mount point
mkdir -p /mnt/ocsfs

# Mount (unauthenticated — for initial testing)
mount -t ocsfs /dev/sdb /mnt/ocsfs

# Mount with cluster authentication key (recommended even in test)
# The key is a 64-character hex string (256 bits)
CLUSTER_KEY=$(openssl rand -hex 32)
echo "Cluster key: $CLUSTER_KEY"   # save this — all nodes must use the same key

mount -t ocsfs /dev/sdb /mnt/ocsfs -o cluster_secret=${CLUSTER_KEY}
```

All three nodes must supply the same `cluster_secret` value.

**Persistent fstab entry (add to /etc/fstab on each node):**

```
/dev/sdb   /mnt/ocsfs   ocsfs   cluster_secret=<64hexkey>,_netdev   0 0
```

Use `_netdev` so the mount is deferred until the network (and therefore iSCSI)
is available.

### 5.3 Verify cluster mode is active

```bash
# Kernel messages on mount
dmesg | grep -i ocsfs | tail -30
# Look for:
#   ocsfs: claiming node slot 0 on /dev/sdb
#   ocsfs: cluster mode active, 1 node(s) present
#   ocsfs: heartbeat kthread started

# After all three nodes have mounted:
#   ocsfs: node 1 joined (slot 1)
#   ocsfs: node 2 joined (slot 2)

# Check registered PR keys (should show one entry per mounted node)
sg_persist --in -k /dev/sdb
# PR generation=0x3
# 3 registered reservation key(s) follow:
#   0x<sha256-of-node1-hostname-truncated>
#   0x<sha256-of-node2-hostname-truncated>
#   0x<sha256-of-node3-hostname-truncated>

# If /proc/fs/ocsfs is exported (implementation-dependent):
cat /proc/fs/ocsfs/sdb/cluster_state 2>/dev/null
```

---

## 6. Test Scenarios

### 6.1 Single-node baseline

Before testing cluster behaviour, establish single-node correctness. Unmount on
node2 and node3, keep only node1.

```bash
apt-get install -y pjdfstest bonnie++ fio

# POSIX conformance (takes ~5 minutes)
cd /mnt/ocsfs
prove -r /usr/local/share/pjdfstest/tests/ 2>&1 | tee /tmp/pjdfstest.log
grep -E "FAIL|ok" /tmp/pjdfstest.log | tail -20

# Sequential throughput (bonnie++)
bonnie++ -d /mnt/ocsfs -u root -s 4G -r 2048 2>&1 | tee /tmp/bonnie.log

# Random I/O (fio)
fio --name=randread \
    --directory=/mnt/ocsfs \
    --rw=randread \
    --bs=4k \
    --size=1G \
    --numjobs=4 \
    --iodepth=32 \
    --runtime=60 \
    --time_based \
    --group_reporting \
    --output=/tmp/fio-randread.log

fio --name=randrw \
    --directory=/mnt/ocsfs \
    --rw=randrw \
    --rwmixread=70 \
    --bs=4k \
    --size=1G \
    --numjobs=4 \
    --iodepth=32 \
    --runtime=60 \
    --time_based \
    --group_reporting \
    --output=/tmp/fio-randrw.log
```

### 6.2 Two-node basic cluster

Mount on node1 and node2. Leave node3 unmounted.

```bash
# --- Run on both nodes simultaneously (use tmux or parallel-ssh) ---

# Test 1: concurrent file creation
# node1
for i in $(seq 1 100); do
    touch /mnt/ocsfs/node1_file_${i}
done

# node2 (at the same time)
for i in $(seq 1 100); do
    touch /mnt/ocsfs/node2_file_${i}
done

# Verify from node1: should see 200 files
ls /mnt/ocsfs/ | wc -l   # expect 200

# Test 2: cross-node read-write consistency
# node1: write a file
dd if=/dev/urandom of=/mnt/ocsfs/shared_test bs=1M count=64 conv=fsync

# node2: read and compute checksum (should match node1's sha256)
sha256sum /mnt/ocsfs/shared_test

# node1
sha256sum /mnt/ocsfs/shared_test

# Test 3: concurrent append (stress DLM EX lock)
# node1
fio --name=append-node1 \
    --filename=/mnt/ocsfs/append_test \
    --rw=write \
    --bs=64k \
    --size=512M \
    --numjobs=1 &

# node2 (at the same time)
fio --name=append-node2 \
    --filename=/mnt/ocsfs/append_test_n2 \
    --rw=write \
    --bs=64k \
    --size=512M \
    --numjobs=1

wait
```

### 6.3 Crash recovery test

This test verifies the 5-phase recovery path (leader election → SCSI PR fencing
→ journal replay → lock recovery → slot cleanup).

```bash
# Setup: all three nodes mounted, some I/O in progress on node3
# On node3: start a background writer
fio --name=bg-write \
    --filename=/mnt/ocsfs/crash_test \
    --rw=write \
    --bs=4k \
    --size=2G \
    --numjobs=1 \
    --iodepth=8 &
FIO_PID=$!

# Wait a few seconds for I/O to be in flight
sleep 5

# From the Proxmox HOST: destroy node3's VM (equivalent to kernel panic / power cut)
# This is the correct way — do NOT use ACPI shutdown inside the guest
virsh destroy ocsfs-node3

# Observe recovery on node1 (the recovery leader)
dmesg -w | grep -i "ocsfs\|recovery\|fence\|replay" &
DMESG_PID=$!

# Wait for recovery to complete (typically 15–60 seconds)
# Expected sequence in dmesg:
#   ocsfs: node 2 heartbeat timeout (slot 2)
#   ocsfs: starting recovery for slot 2
#   ocsfs: SCSI PR PREEMPT_AND_ABORT for key 0x... — success
#   ocsfs: replaying journal for slot 2: N records
#   ocsfs: lock recovery for slot 2: M locks released
#   ocsfs: slot 2 marked DEAD — recovery complete

# Kill the dmesg watcher
kill $DMESG_PID

# Verify the filesystem is consistent on the surviving nodes
ls /mnt/ocsfs/crash_test          # file must still exist
sha256sum /mnt/ocsfs/crash_test   # must not error out

# Run fsck from node1 after unmounting (repair mode)
umount /mnt/ocsfs
python3 /path/to/OCSFS/tools/ocsfs-fsck --repair /dev/sdb
```

After recovery, restart node3's VM and rejoin:

```bash
virsh start ocsfs-node3
# SSH into node3
mount -t ocsfs /dev/sdb /mnt/ocsfs -o cluster_secret=${CLUSTER_KEY}
dmesg | grep ocsfs | tail -10
# ocsfs: node joined cluster, assigned slot 2
```

### 6.4 Network partition simulation

Simulates a SAN isolation event (iSCSI path dropped to one node). Because OCSFS
uses storage-path heartbeats (not a management network), blocking iSCSI traffic
is the correct partition primitive.

```bash
# On node3: block iSCSI traffic to/from TrueNAS
# This simulates the node losing access to the shared LUN
iptables -I INPUT  -s 192.168.20.10 -p tcp --sport 3260 -j DROP
iptables -I OUTPUT -d 192.168.20.10 -p tcp --dport 3260 -j DROP

# Expected behaviour:
# - node3: heartbeat writes fail → node evicts itself → remounts read-only or panics
# - node1+node2: detect node3 timeout → run recovery

# Monitor node1 (the expected recovery leader)
dmesg -w | grep -i ocsfs

# After recovery completes (check dmesg on node1/node2):
ls /mnt/ocsfs    # must work normally on node1 and node2

# Restore connectivity on node3
iptables -D INPUT  -s 192.168.20.10 -p tcp --sport 3260 -j DROP
iptables -D OUTPUT -d 192.168.20.10 -p tcp --dport 3260 -j DROP

# Rejoin node3
mount -t ocsfs /dev/sdb /mnt/ocsfs -o cluster_secret=${CLUSTER_KEY}
```

> **Note:** The current implementation (alpha) handles only one simultaneous
> recovery target. If node3's recovery is not complete when you restore
> connectivity, the second mount may race with the tail of recovery. Wait for
> the `recovery complete` dmesg entry before remounting.

---

## 7. xfstests Integration

### 7.1 Install xfstests

Build on each VM that will run the test suite (typically node1 only for generic
tests, which do not require a cluster).

```bash
apt-get install -y \
    git build-essential autoconf automake libtool \
    libacl1-dev libattr1-dev libaio-dev \
    xfslibs-dev uuid-dev libblkid-dev \
    e2fsprogs attr acl fio bc dump indent \
    quota gawk dbench

git clone https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git \
    /usr/local/src/xfstests
cd /usr/local/src/xfstests
make -j$(nproc)
make install
```

### 7.2 Configure for OCSFS

xfstests requires a TEST device (where tests run) and a SCRATCH device (wiped
between tests). OCSFS is a shared-disk filesystem, so both must be real block
devices: provide the shared iSCSI LUN as TEST (`/dev/sdb`) and a **second iSCSI
LUN** as SCRATCH (`/dev/sdc` — export a separate 20 GB zvol/LUN from the same
target and log in to it on node1).

```bash
# Confirm both LUNs are present after iSCSI login
lsblk /dev/sdb /dev/sdc
SCRATCH_DEV=/dev/sdc
```

**Create /usr/local/src/xfstests/local.config:**

```bash
cat > /usr/local/src/xfstests/local.config << 'EOF'
# OCSFS xfstests configuration
# Adjust paths to match your environment

TEST_DEV=/dev/sdb
TEST_DIR=/mnt/ocsfs
SCRATCH_DEV=/dev/sdc
SCRATCH_MNT=/mnt/scratch

FSTYP=ocsfs

# mkfs command used by xfstests to re-format TEST_DEV
MKFS_OPTIONS="--max-nodes 4 --ag-size 1G --label xfstests-test"
export MKFS_OPTIONS

# mount options applied to both TEST and SCRATCH mounts
MOUNT_OPTIONS=""
export MOUNT_OPTIONS

# Tell xfstests where the mkfs binary is
export MKFS_PROG=/path/to/OCSFS/tools/mkfs.ocsfs
export FSCK_PROG=/path/to/OCSFS/tools/ocsfs-fsck

# Scratch fs type — use ext4 for scratch until the ocsfs scratch path is stable
SCRATCH_FSTYPE=ext4
EOF
```

> **Tip:** Until a dedicated xfstests group for `ocsfs` exists, use `ext4` on
> SCRATCH to avoid cascading failures in tests that re-format scratch between
> runs. Set `SCRATCH_FSTYPE=ocsfs` only once the single-node scratch path is
> verified stable on the second LUN.

### 7.3 Run subset of tests

OCSFS does not yet have a dedicated xfstests group. Use `generic` tests,
excluding known-incompatible ones.

```bash
cd /usr/local/src/xfstests

# Run the auto group (quick smoke test, ~30 minutes)
./check -g auto -E /dev/null 2>&1 | tee /tmp/xfstests-auto.log

# Run generic tests only, skipping tests that require features not yet implemented
# Known exclusions for alpha OCSFS:
#   generic/083  — requires direct I/O to tmpfs (not applicable)
#   generic/263  — requires dm-error injection
#   generic/451  — requires quota support (not implemented)
#   generic/467  — requires reflink on scratch (partial support)
./check -g generic \
    -x generic/083 \
    -x generic/263 \
    -x generic/451 \
    -x generic/467 \
    2>&1 | tee /tmp/xfstests-generic.log

# Run just the quick tests first to catch obvious regressions
./check -g quick 2>&1 | tee /tmp/xfstests-quick.log

# Inspect failures
grep -E "^FAILED|Failures:" /tmp/xfstests-auto.log
```

Pass/fail results are written to `/usr/local/src/xfstests/results/`.

---

## 8. Kernel Debug Configuration

When debugging a crash or data corruption, rebuild the Gentoo kernel with
the following options enabled. These settings increase overhead significantly
— use a dedicated debug VM, not the Proxmox production host.

Edit `/usr/src/linux/.config` or run `make menuconfig`:

```
# Lock dependency validator — catches lock ordering violations
CONFIG_LOCKDEP=y
CONFIG_PROVE_LOCKING=y
CONFIG_DEBUG_LOCKDEP=y
CONFIG_LOCK_STAT=y

# Kernel Address Sanitizer — catches use-after-free, out-of-bounds
CONFIG_KASAN=y
CONFIG_KASAN_INLINE=y
# Use KASAN_GENERIC for best coverage, KASAN_SW_TAGS for lower overhead
CONFIG_KASAN_GENERIC=y

# Fault injection framework — inject I/O errors, memory allocation failures
CONFIG_FAULT_INJECTION=y
CONFIG_FAULT_INJECTION_DEBUG_FS=y
CONFIG_FAIL_MAKE_REQUEST=y   # injects block I/O errors
CONFIG_FAIL_PAGE_ALLOC=y     # injects memory allocation failures

# Slab debugging — detects use-after-free in kmalloc regions
CONFIG_SLUB_DEBUG=y
CONFIG_SLUB_DEBUG_ON=y

# Additional useful options
CONFIG_DEBUG_PAGE_REF=y
CONFIG_REFCOUNT_FULL=y
CONFIG_UBSAN=y
CONFIG_UBSAN_SANITIZE_ALL=y
CONFIG_DEBUG_LIST=y
CONFIG_DEBUG_PLIST=y
CONFIG_STACKTRACE=y
CONFIG_DEBUG_STACKOVERFLOW=y
```

Rebuild and install on the debug VM:

```bash
# On the debug VM (Debian 12)
apt-get install -y build-essential libssl-dev libelf-dev flex bison bc

# Fetch the kernel version matching the Proxmox host (optional — or use distro kernel)
wget https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.0.10.tar.xz
tar xf linux-7.0.10.tar.xz
cd linux-7.0.10

# Start from the current config and apply debug overrides
cp /boot/config-$(uname -r) .config
scripts/config \
    --enable LOCKDEP --enable PROVE_LOCKING --enable DEBUG_LOCKDEP \
    --enable KASAN --enable KASAN_GENERIC \
    --enable FAULT_INJECTION --enable FAULT_INJECTION_DEBUG_FS \
    --enable FAIL_MAKE_REQUEST \
    --enable SLUB_DEBUG --enable SLUB_DEBUG_ON \
    --enable UBSAN --enable UBSAN_SANITIZE_ALL
make olddefconfig
make -j$(nproc) bzImage modules
make modules_install
make install
```

> **Warning:** KASAN requires approximately 1/8 of physical memory as shadow
> memory. On an 8 GB VM, this leaves ~7 GB usable. Disable KASAN if the VM
> runs out of memory during testing.

**Enabling fault injection for block I/O (runtime):**

```bash
# Mount debugfs
mount -t debugfs none /sys/kernel/debug

# Inject block I/O errors at rate 1/100 requests
echo 1 > /sys/kernel/debug/fail_make_request/probability
echo 100 > /sys/kernel/debug/fail_make_request/interval
echo -1 > /sys/kernel/debug/fail_make_request/times    # unlimited
echo 1 > /sys/kernel/debug/fail_make_request/task-filter

# Enable for the ocsfs test process (PID)
echo 1 > /proc/<PID>/make-it-fail

# Disable when done
echo 0 > /sys/kernel/debug/fail_make_request/probability
```

---

## 9. Useful Debug Commands

### dmesg filtering

```bash
# Follow live kernel messages, filter for OCSFS
dmesg -w | grep -i ocsfs

# Show OCSFS messages with timestamps since last boot
dmesg -T | grep -i ocsfs

# Show only error/warning level messages
dmesg -l err,warn | grep -i ocsfs

# Filter for specific subsystems
dmesg | grep -E "ocsfs.*(lock|recovery|heartbeat|journal|pr)"
```

### ftrace — tracing OCSFS functions

```bash
# Mount debugfs if not already mounted
mount -t debugfs none /sys/kernel/debug

# List available OCSFS trace events (if CONFIG_FTRACE_EVENTS is set)
ls /sys/kernel/debug/tracing/events/ocsfs/ 2>/dev/null

# Enable function tracing for all ocsfs_* symbols
echo function > /sys/kernel/debug/tracing/current_tracer
echo 'ocsfs_*' > /sys/kernel/debug/tracing/set_ftrace_filter
echo 1 > /sys/kernel/debug/tracing/tracing_on

# Run your test, then capture the trace
cat /sys/kernel/debug/tracing/trace > /tmp/ocsfs-trace.txt

# Stop tracing
echo 0 > /sys/kernel/debug/tracing/tracing_on
echo nop > /sys/kernel/debug/tracing/current_tracer

# Trace only the lock subsystem
echo 'ocsfs_lock_* ocsfs_pr_*' > /sys/kernel/debug/tracing/set_ftrace_filter
```

### sg3_utils — SCSI Persistent Reservation diagnostics

```bash
# Show all registered PR keys on the LUN
sg_persist --in -k /dev/sdb

# Show current reservation holder (type + key)
sg_persist --in -r /dev/sdb

# Show full capabilities
sg_persist --in --report-capabilities /dev/sdb

# SCSI INQUIRY — general device info and supported features
sg_inq /dev/sdb

# SCSI INQUIRY — supported VPD pages
sg_inq --vpd /dev/sdb

# Read Unit Serial Number VPD page (for verifying same LUN across nodes)
sg_inq --vpd -i 0x80 /dev/sdb

# SCSI LOG SENSE — error counter and I/O stats
sg_logs /dev/sdb

# Reset error counters
sg_logs --reset /dev/sdb
```

### iSCSI session diagnostics

```bash
# Show active sessions
iscsiadm -m session -P 3

# Show session statistics
iscsiadm -m session --stats

# Check target configuration
iscsiadm -m node -T iqn.2024-01.net.truenas:ocsfs-test -P 3

# Reconnect a dropped session without rebooting
iscsiadm -m node -T iqn.2024-01.net.truenas:ocsfs-test \
    -p 192.168.20.10:3260 --op update -n node.session.timeo.replacement_timeout -v 120
```

### Filesystem state

```bash
# Show mounted OCSFS filesystems
mount | grep ocsfs

# Check superblock (read-only probe)
python3 /path/to/OCSFS/tools/ocsfs-fsck --dry-run /dev/sdb

# Dump on-disk node slot table (if ocsfs-fsck supports --dump-slots)
python3 /path/to/OCSFS/tools/ocsfs-fsck --dump-slots /dev/sdb

# Check I/O stats on the block device
iostat -xz 1 5 /dev/sdb
```

---

## 10. Known Limitations During Testing

The following scenarios are **outside the scope of alpha testing** and should
not be attempted without code changes to support them:

| Limitation | Details |
|---|---|
| **Multi-LUN spanning** | A single OCSFS volume cannot span multiple block devices. Do not attempt RAID-0 or dm-linear across two LUNs. |
| **More than 4 nodes simultaneously** | The test format uses `--max-nodes 4`. Mounting a fifth node will fail. Re-format with `--max-nodes N` (max 256) to increase the limit. |
| **Simultaneous dual-node failure** | Only one recovery target is tracked at a time (`s_recovery_target` is a single `u16`). If two nodes fail simultaneously, the second is not recovered until the first completes. |
| **Non-PR devices (degraded mode)** | Mounting on a target that does not support SCSI-3 PR (e.g., a basic iSCSI target without PR) is allowed in degraded mode, but fencing is skipped. Do not use degraded mode for split-brain correctness testing. |
| **Snapshot + cluster** | Snapshot creation (`OCSFS_IOC_SNAPSHOT`) is implemented but not tested in multi-node mode. Use snapshots only in single-node mode during this alpha. |
| **Dedup in cluster mode** | `OCSFS_IOC_DEDUP` ioctl acquires a per-inode EX lock but does not yet coordinate AG-level refcount updates across nodes. Avoid running dedup while other nodes are writing. |
| **Online resize** | `ocsfs_tool --resize` is not implemented. Resizing requires unmounting all nodes, using `ocsfs_tool` offline, then remounting. |
| **Kernel ≥ 7.1** | The module is written and tested against 7.0.x. The iomap API changes in 7.1 may break compilation. Pin to 7.0.x for now. |
| **32-bit architectures** | Not tested. All `__le64` fields assume the host can perform 64-bit atomic reads. |
| **FC SAN passthrough** | The guide covers iSCSI only. FC SAN passthrough to Proxmox VMs requires additional configuration (vHBA or SR-IOV) not documented here. |

---

*End of document.*
