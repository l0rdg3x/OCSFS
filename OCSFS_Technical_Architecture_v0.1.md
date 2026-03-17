# OCSFS

Open Cluster Shared FileSystem

### Technical Architecture & Design Specification

A VMFS-class Open-Source Cluster Filesystem for Linux

Designed for Fibre Channel SAN, Proxmox VE & KVM Environments

Version 0.1 — March 2026

## Table of Contents

## 1. Executive Summary

### 1.1 The Problem

Broadcom’s acquisition of VMware has resulted in dramatic license cost increases—up to 5x—forcing organizations worldwide to reconsider their virtualization strategy. Proxmox VE has emerged as the leading open-source alternative, but it lacks a critical capability: a true cluster-aware shared filesystem for Fibre Channel SAN storage. VMware’s VMFS has been the gold standard for shared block-level storage in virtualized environments for over 15 years, and no open-source equivalent exists.

Currently, Proxmox users with FC SAN infrastructure are limited to LVM with lvmlockd—a volume manager, not a filesystem. This approach provides no file-level access, no thin provisioning, and a cumbersome management experience. GFS2 and OCFS2 exist in the mainline kernel but are designed for general-purpose clustered file access, not optimized for VM disk image I/O patterns, and have no Proxmox integration.

### 1.2 The Solution: OCSFS

OCSFS (Open Cluster Shared FileSystem) is a purpose-built, open-source, Linux kernel filesystem designed specifically for shared block storage in virtualized environments. It targets FC SAN LUNs as its primary storage backend and is engineered from the ground up to support concurrent access from multiple hypervisor nodes with minimal coordination overhead.

Key design principles: on-disk distributed locking with no external dependencies, extent-based allocation optimized for large sequential I/O (VM disk images), SCSI-3 Persistent Reservations for hardware-level fencing, per-node metadata journaling, native thin provisioning and space reclamation, and first-class Proxmox VE integration as a storage plugin.

### 1.3 Target Use Cases

- **Enterprise Migration:** Organizations migrating from VMware vSphere to Proxmox VE that need to leverage their existing FC SAN investment without re-architecting storage.

- **Shared Datastore:** Multiple Proxmox nodes accessing a common pool of VM disk images, ISOs, templates, and container images on a single FC LUN.

- **Live Migration:** Seamless VM live migration between nodes without storage migration, as all nodes see the same filesystem.

- **High Availability:** HA failover of VMs between cluster nodes with immediate access to VM disks from any surviving node.

## 2. VMFS Deep Analysis — What We Must Match

### 2.1 VMFS Architecture Overview

Understanding VMFS deeply is essential because OCSFS must match or exceed its capabilities for VM workloads. VMFS (Virtual Machine File System) is a clustered filesystem developed by VMware since 2004. It has gone through several major versions, with VMFS-6 being the current iteration. Its design is fundamentally optimized for one use case: storing and serving virtual machine disk images from shared block storage.

#### 2.1.1 On-Disk Locking

VMFS uses on-disk locking exclusively—no external lock manager is required. This is achieved through SCSI-3 Persistent Reservations (PR) and ATS (Atomic Test and Set) operations. Every metadata update acquires a lock directly on the LUN using SCSI commands. This eliminates any dependency on a network-based lock manager (unlike GFS2’s DLM) and means the filesystem continues to function correctly even if the cluster’s management network is partitioned.

Lock types in VMFS: file-level locks for exclusive VM ownership (one node “owns” a VMDK while a VM is running), metadata locks for structural changes (allocation, deletion), and a distributed lock that serializes access to the on-disk lock table using SCSI-2 reservations as a fallback.

#### 2.1.2 Resource Allocation

VMFS uses a hierarchical allocation model. The LUN is divided into a small number of large resource groups (typically 256 MB each). Each resource group has its own bitmap for block allocation. When a node needs to allocate space, it acquires a lock on a specific resource group rather than a global allocation lock. This dramatically reduces contention—nodes can allocate space concurrently as long as they’re working in different resource groups.

Sub-block allocation (for small files like descriptors) uses 1KB sub-blocks carved from a dedicated pool, avoiding wasted space for the many small metadata files that accompany VM disk images.

#### 2.1.3 Heartbeat Mechanism

Each VMFS volume contains a heartbeat region—a reserved area on disk where each node periodically writes a timestamp. This serves dual purposes: it proves the node is still alive and has I/O path connectivity to the LUN. If a node’s heartbeat goes stale (typically after 16 seconds), other nodes can reclaim its locks. This is fundamentally more reliable than network-based heartbeats because it tests the actual storage path, not just network connectivity.

### 2.2 VMFS Strengths We Must Replicate

| **Feature**                   | **VMFS Approach**                            | **Why It Matters**                                  |
|-------------------------------|----------------------------------------------|-----------------------------------------------------|
| On-disk locking               | SCSI-3 PR + ATS                              | No external dependencies, storage-path awareness    |
| Per-resource-group allocation | 256 MB groups with per-group bitmaps         | Minimal contention under concurrent allocation      |
| Large extent sizes            | 1 MB default block size                      | Optimal for sequential VM disk I/O                  |
| Heartbeat on disk             | Dedicated heartbeat region                   | Tests actual storage connectivity, not just network |
| Thin provisioning             | Lazy allocation + zeroed-thick + eager-thick | Space efficiency without admin overhead             |
| ATS for metadata              | Hardware atomic compare-and-swap on disk     | Lock-free metadata updates where possible           |
| Sub-block allocation          | 1 KB sub-blocks for small files              | Efficient storage of descriptor files               |
| Online grow                   | Extent spanning across LUNs                  | Non-disruptive capacity expansion                   |

### 2.3 VMFS Limitations We Can Improve

- **Single vendor lock-in:** Proprietary, only runs on ESXi. OCSFS will be GPLv2, running on any Linux kernel.

- **No snapshots at filesystem level:** VMFS relies on VMDK-level snapshots. OCSFS can integrate filesystem-level CoW snapshots.

- **Limited scalability:** VMFS-6 supports up to 64 nodes. OCSFS targets 64+ nodes with a design that scales further.

- **No compression/deduplication:** VMFS has none. OCSFS can add optional inline compression.

- **Block size inflexibility:** VMFS-6 fixed at 1 MB. OCSFS supports variable extent sizes (64 KB – 64 MB).

## 3. On-Disk Layout Architecture

### 3.1 Volume Geometry

An OCSFS volume occupies an entire block device (FC LUN). The volume is divided into fixed regions, laid out sequentially from the beginning of the device. All multi-byte integers are stored in little-endian format. All offsets and sizes are in bytes unless otherwise noted.

| **Region**                   | **Offset**           | **Size**  | **Description**                                     |
|------------------------------|----------------------|-----------|-----------------------------------------------------|
| Superblock                   | 0                    | 4 KB      | Volume identity, geometry, feature flags            |
| Superblock Mirror            | 4 KB                 | 4 KB      | Redundant copy for recovery                         |
| Node Slot Table              | 8 KB                 | 64 KB     | Up to 256 node registration slots                   |
| Heartbeat Region             | 72 KB                | 256 KB    | Per-node heartbeat sectors (1 KB each, 256 max)     |
| Lock Table                   | 328 KB               | 1 MB      | On-disk distributed lock entries                    |
| Journal Region               | ~1.3 MB              | N × 32 MB | Per-node journals (N = max_nodes, default 64)       |
| Allocation Group Descriptors | After journals       | Variable  | AG metadata headers                                 |
| Data Region                  | After AG descriptors | Remainder | Allocation Groups containing file data and metadata |

### 3.2 Superblock

The superblock is the root of the filesystem’s metadata. It is written at offset 0 and mirrored at offset 4096. The superblock contains:

- **Magic number:** 0x4F435346 (‘OCSF’) — identifies the filesystem type.

- **Version:** Major.minor version for on-disk format compatibility.

- **Volume UUID:** 128-bit UUID generated at mkfs time, used for identification.

- **Volume label:** UTF-8 string up to 64 bytes.

- **Block size:** Minimum allocation unit (default 4 KB, matching underlying device sector alignment).

- **Extent size:** Default extent allocation granularity (default 1 MB, configurable 64 KB – 64 MB).

- **Total blocks/Free blocks:** Volume capacity tracking.

- **AG count/AG size:** Number and size of Allocation Groups.

- **Max nodes:** Maximum cluster nodes supported (set at mkfs, default 64, max 256).

- **Feature flags:** Bitfield for optional features (compression, dedup, encryption, thin provisioning).

- **Heartbeat timeout:** Configurable staleness threshold (default 15 seconds).

- **Checksum:** CRC32C of the superblock for integrity validation.

```c
struct ocsfs_superblock {
__le32 s_magic; /* 0x4F435346 */
__le16 s_version_major;
__le16 s_version_minor;
__u8 s_uuid[16];
__u8 s_label[64];
__le32 s_block_size; /* bytes, power of 2 */
__le32 s_extent_size; /* bytes, multiple of block_size */
__le64 s_total_blocks;
__le64 s_free_blocks;
__le32 s_ag_count;
__le64 s_ag_size; /* blocks per AG */
__le16 s_max_nodes;
__le64 s_feature_flags;
__le32 s_heartbeat_timeout; /* milliseconds */
__le32 s_journal_size; /* blocks per journal */
__le64 s_lock_table_offset;
__le64 s_journal_offset;
__le64 s_ag_desc_offset;
__le64 s_data_offset;
__le32 s_checksum; /* CRC32C */
};
```

### 3.3 Node Slot Table

Each node that mounts the volume must claim a slot in the Node Slot Table. A slot contains:

- **Node UUID:** Unique identifier of the node (derived from machine-id or configured).

- **Node name:** Human-readable hostname (informational).

- **Slot state:** FREE (0x00), ACTIVE (0x01), EVICTING (0x02), DEAD (0xFF).

- **Mount generation:** Monotonically increasing counter, incremented on each mount. Used to detect stale locks from a previous mount of the same node.

- **Last heartbeat timestamp:** Nanosecond-precision timestamp of last heartbeat write.

Slot claiming uses SCSI-3 PR (Write Exclusive – Registrants Only) to ensure atomicity. A node first registers its PR key with the LUN, then performs a compare-and-write on the slot to claim it. If two nodes race for the same slot, SCSI guarantees only one succeeds.

### 3.4 Heartbeat Region

The heartbeat region contains 256 sectors (one per possible node slot), each 1 KB. Every heartbeat_interval (default 5 seconds), each active node writes its current timestamp and a monotonic sequence number to its heartbeat sector using a direct SCSI WRITE command. Other nodes periodically read the heartbeat region to detect failures.

A node is considered failed if its heartbeat has not been updated for heartbeat_timeout (default 15 seconds = 3 missed intervals). Upon detecting a failed node, the recovery leader initiates lock recovery (Section 5). This heartbeat is fundamentally superior to network-based heartbeats because it validates the actual I/O path to the shared storage device.

```c
struct ocsfs_heartbeat {
__le16 hb_node_slot;
__le16 hb_state; /* mirrors slot state */
__le64 hb_timestamp; /* nanoseconds since epoch */
__le64 hb_sequence; /* monotonic counter */
__le32 hb_mount_gen; /* must match node slot */
__le32 hb_checksum; /* CRC32C */
};
```

### 3.5 Allocation Groups

The data region is divided into Allocation Groups (AGs), each of configurable size (default 1 GB, minimum 256 MB, maximum 64 GB). Each AG is an independent unit of space management, containing:

- **AG Header:** AG identifier, free block count, free extent count, flags.

- **Block Bitmap:** Bit-per-block allocation bitmap (1 bit = 1 filesystem block).

- **Extent B-tree:** B+ tree indexing free extents by size and offset for fast allocation.

- **Inode Table:** Fixed-size array of inode structures within each AG.

- **Inode B-tree:** B+ tree mapping inode numbers to their location within the AG.

The AG design is the cornerstone of concurrency: each AG can be locked independently, so nodes allocating in different AGs never contend. An affinity algorithm assigns a “home AG” to each node based on its slot number, and nodes preferentially allocate from their home AG. When the home AG is full, the node moves to the next AG in a round-robin fashion with backoff.

#### 3.5.1 Inode Structure

Each file and directory is represented by an inode. OCSFS inodes are 512 bytes to allow inline extent lists for typical VM disk files.

```c
struct ocsfs_inode {
__le32 i_magic; /* 0x494E4F44 ('INOD') */
__le64 i_ino; /* inode number */
__le16 i_mode; /* file type + permissions */
__le16 i_nlink;
__le32 i_uid, i_gid;
__le64 i_size; /* file size in bytes */
__le64 i_blocks; /* allocated blocks */
__le64 i_atime, i_mtime, i_ctime;
__le32 i_flags; /* immutable, append, thin, compressed */
__le16 i_extent_count; /* number of inline extents */
__le16 i_extent_max; /* max inline extents (depends on inode size) */
struct ocsfs_extent i_extents[28]; /* inline extent list */
__le64 i_extent_tree_root; /* overflow B+tree root block, 0 if inline */
__le64 i_thin_allocated; /* actually written bytes (thin prov.) */
__le32 i_checksum;
};
struct ocsfs_extent {
__le64 e_logical_block; /* file-relative offset */
__le64 e_physical_block; /* volume-absolute offset */
__le32 e_length; /* extent length in blocks */
__le16 e_flags; /* WRITTEN, UNWRITTEN (hole/thin), COMPRESSED */
__le16 e_checksum;
};
```

#### 3.5.2 Directory Structure

Directories use a B+ tree keyed on filename hash (XXH3-64) with collision chains. Each directory entry contains: filename hash (8 bytes), inode number (8 bytes), entry type (1 byte: file, directory, symlink), name length (1 byte), and the actual filename (up to 255 bytes). For small directories (fewer than ~20 entries), entries are stored inline in the directory inode to avoid B+ tree overhead.

## 4. Distributed Locking Subsystem

### 4.1 Design Philosophy

OCSFS’s locking subsystem is entirely on-disk, requiring no external lock manager daemon, no network communication between nodes, and no dependency on the cluster management network. All lock state is stored in the Lock Table region of the volume and manipulated using SCSI commands. This is the single most critical design decision in OCSFS and what differentiates it from GFS2/OCFS2.

The rationale is fundamental: in a SAN environment, the storage fabric IS the shared resource. If a node can’t reach the SAN, it shouldn’t be participating in the filesystem regardless of network status. Conversely, if the management network fails but SAN connectivity remains, nodes should continue operating—unlike GFS2, which would fence nodes or degrade when the DLM can’t communicate.

### 4.2 Lock Types

| **Lock Type**         | **Scope**         | **SCSI Mechanism**                      | **Use Case**                                      |
|-----------------------|-------------------|-----------------------------------------|---------------------------------------------------|
| Null (NL)             | Advisory          | None                                    | Intent declaration, no blocking                   |
| Shared (SH)           | Read access       | PR: Read Shared                         | Reading file metadata, reading allocation bitmaps |
| Exclusive (EX)        | Write access      | PR: Write Exclusive                     | Modifying file content, allocation changes        |
| Concurrent Write (CW) | Multi-writer data | PR: Exclusive Access – Registrants Only | VM disk I/O where node holds file-level EX        |

### 4.3 Lock Table Architecture

The Lock Table is a 1 MB region containing 4096 lock entries of 256 bytes each. Each lock entry represents a lockable resource (file, AG metadata, journal, etc.) and contains:

```c
struct ocsfs_lock_entry {
__le64 le_resource_id; /* hash of resource name */
__le32 le_resource_type; /* INODE, AG, JOURNAL, RENAME */
__le16 le_mode; /* NL, SH, EX, CW */
__le16 le_holder_slot; /* node slot of current holder */
__le32 le_holder_gen; /* mount generation of holder */
__le64 le_grant_time; /* when lock was granted */
__le64 le_waiters; /* bitmask of waiting node slots */
__le16 le_waiter_modes[256]; /* requested mode per waiter */
__le32 le_version; /* lock version for CAS operations */
__le32 le_checksum;
};
```

#### 4.3.1 Lock Acquisition Protocol

Lock acquisition uses a multi-step protocol leveraging SCSI Compare-And-Write (CAW) for atomicity:

- **Step 1 — Hash and Locate:** The resource (e.g., inode number) is hashed to find its lock entry in the Lock Table. Consistent hashing with linear probing handles collisions.

- **Step 2 — Read Current State:** Read the lock entry via SCSI READ.

- **Step 3 — Evaluate Compatibility:** Check if the requested mode is compatible with the current holder (SH+SH = compatible, SH+EX = conflict, EX+anything = conflict).

- **Step 4a — Compatible:** Use SCSI Compare-And-Write to atomically update the entry (add self as co-holder or upgrade lock). If CAW fails (another node modified the entry), retry from Step 2.

- **Step 4b — Conflict:** Set waiter bit in le_waiters via CAW, then poll the lock entry periodically, backing off exponentially from 1ms to 100ms.

- **Step 5 — Grant:** When the holder releases or downgrades, it checks waiters and updates the entry to grant to the highest-priority waiter (EX requests take priority over SH for starvation avoidance).

#### 4.3.2 Lock Release Protocol

Lock release is simpler: the holder reads the lock entry, clears its holder fields, checks the waiter bitmask, and if waiters exist, promotes the next waiter to holder—all in a single CAW operation. If no waiters, the entry is set to NL mode.

### 4.4 SCSI-3 Persistent Reservations Integration

OCSFS uses SCSI-3 PR extensively for two purposes:

**1. Volume-level fencing:** When a node mounts, it registers a PR key (derived from node UUID + mount generation) with the LUN. All I/O uses this registration. If a node must be fenced (heartbeat failure + lock recovery), other nodes issue a PR PREEMPT AND ABORT with the failed node’s key, which causes the SCSI target to abort all pending I/O from the fenced node and prevent future I/O until re-registration.

**2. Metadata serialization fallback:** For operations that cannot be expressed as a single CAW (e.g., multi-entry updates), a short-lived SCSI-2 RESERVE/RELEASE is used as a coarse-grained mutex. This is rare and only used for superblock updates and journal recovery.

### 4.5 Comparison with GFS2/OCFS2 Locking

| **Aspect**                 | **OCSFS**                    | **GFS2**                       | **OCFS2**                |
|----------------------------|------------------------------|--------------------------------|--------------------------|
| Lock Manager               | On-disk (SCSI CAW/PR)        | DLM (network-based)            | DLM (network-based)      |
| External Dependencies      | None (SAN fabric only)       | Corosync + DLM daemon          | Cluster stack + DLM      |
| Network Partition Behavior | Continues if SAN works       | Fences/degrades                | Fences/degrades          |
| Lock Granularity           | Per-resource (4096 slots)    | Per-resource (in-memory)       | Per-resource (in-memory) |
| Lock Persistence           | Survives node reboot         | Lost on daemon restart         | Lost on daemon restart   |
| Storage Path Validation    | Implicit (heartbeat on disk) | Separate (requires stonith)    | Separate                 |
| Scalability                | Limited by SCSI CAW latency  | Limited by DLM network traffic | Limited by DLM           |

## 5. Journaling & Crash Recovery

### 5.1 Per-Node Journaling

OCSFS uses per-node write-ahead journals rather than a single shared journal. Each node has a dedicated 32 MB journal region (configurable). This eliminates journal contention entirely—nodes never compete for journal space, and journal writes are purely sequential within each node’s region.

The journal uses a circular buffer with head/tail pointers stored in the journal header. Each journal transaction contains: transaction ID (monotonic), list of modified block addresses, before-images of metadata blocks (for undo), after-images of metadata blocks (for redo), and a commit record with CRC32C.

#### 5.1.1 Transaction Lifecycle

- **Begin:** Allocate a transaction ID, reserve journal space.

- **Log:** Write before-images and after-images of all metadata blocks to be modified. Data blocks are NOT journaled (only metadata).

- **Commit:** Write the commit record. At this point, the transaction is durable.

- **Checkpoint:** Write the actual metadata blocks to their final on-disk locations.

- **Release:** Advance the journal tail past the checkpointed transaction, freeing journal space.

Ordered-data mode is the default: file data is flushed to disk before the metadata transaction commits, ensuring that a crash never exposes stale data blocks in a newly allocated extent. This matches ext4’s default behavior and is critical for security in multi-tenant VM environments.

### 5.2 Crash Recovery Protocol

When a node fails (detected via heartbeat timeout), recovery proceeds in phases:

#### 5.2.1 Phase 1 — Leader Election

The surviving node with the lowest slot number becomes the recovery leader. Leadership is claimed by setting a special recovery lock in the Lock Table via CAW. If multiple nodes detect the failure simultaneously, CAW atomicity ensures only one wins.

#### 5.2.2 Phase 2 — Fencing

The leader issues a SCSI-3 PR PREEMPT AND ABORT using the failed node’s registration key. This is a hardware-level fence: the SAN fabric itself will reject any subsequent I/O from the failed node’s HBA until it re-registers. This is critical because it eliminates the possibility of a “zombie” node (one that is slow/hung but not truly dead) from corrupting the filesystem after recovery begins.

#### 5.2.3 Phase 3 — Journal Replay

The leader reads the failed node’s journal and replays any committed but uncheckpointed transactions. The replay is idempotent—if the failed node had partially checkpointed a transaction, replaying it simply overwrites the same blocks with the same data. Uncommitted transactions (no commit record) are discarded.

#### 5.2.4 Phase 4 — Lock Recovery

The leader scans the Lock Table for any entries held by the failed node (matching holder_slot and holder_gen). These locks are released or downgraded as appropriate. File-level exclusive locks on VM disk images trigger a special notification to the Proxmox HA manager so it can restart the affected VM on another node.

#### 5.2.5 Phase 5 — Slot Cleanup

The failed node’s slot is marked DEAD in the Node Slot Table. The slot can be reused when the node re-mounts (it will see its slot as DEAD, clear it, and re-register with a new mount generation).

### 5.3 Split-Brain Prevention

Split-brain is the most dangerous failure mode for a clustered filesystem. OCSFS prevents it through multiple layers:

- **SCSI-3 PR Fencing:** Hardware-level I/O barrier prevents fenced nodes from writing.

- **Mount Generation Counters:** A stale mount (old generation) cannot acquire locks—the Lock Table rejects requests where the holder’s mount generation doesn’t match.

- **Heartbeat Sequence Numbers:** Monotonically increasing, so a node that reconnects can detect if its heartbeat was overwritten by a recovery process.

- **Quorum (optional):** For environments without reliable SCSI PR (e.g., some iSCSI targets), an optional quorum mode uses a majority vote among nodes before fencing.

## 6. I/O Path Architecture & Performance Optimization

### 6.1 VM Disk I/O Optimization

OCSFS is purpose-built for VM disk I/O patterns. A typical VM workload consists of large sequential writes (VM provisioning, cloning), large sequential reads (VM boot, bulk data), random 4K–64K I/O (guest OS operations), and metadata operations that are rare relative to data I/O. The filesystem optimizes for this profile.

#### 6.1.1 Extent Pre-Allocation

When a VM disk image file is created, the filesystem can pre-allocate extents in large chunks (default 256 MB). Pre-allocation uses UNWRITTEN extents—space is reserved on disk but reads return zeros. As the VM writes data, UNWRITTEN extents are converted to WRITTEN without any allocation overhead. This eliminates fragmentation and allocation latency during VM runtime.

#### 6.1.2 O_DIRECT and AIO Support

QEMU/KVM typically accesses VM disk images with O_DIRECT (bypassing the page cache) and Linux AIO or io_uring for asynchronous I/O. OCSFS fully supports both paths. For O_DIRECT, the filesystem ensures extent lookups are fast (the inline extent list or B+ tree is cached in-memory) and bypasses all buffering. The filesystem’s block layer interface uses bio-based I/O for zero-copy between QEMU’s buffers and the block device.

#### 6.1.3 Lock Locality Optimization

The key performance insight for VM workloads: once a VM is assigned to a node, that node holds an exclusive file-level lock on the VM’s disk image for the VM’s entire lifetime. During this time, no other node accesses that file, so per-block locking is unnecessary. OCSFS optimizes this: when a file has a single exclusive holder, all data I/O bypasses the Lock Table entirely. Locking is only needed for metadata changes (allocation, truncation) and is always per-AG, not per-block.

This is identical to VMFS’s approach and is why VMFS performs well despite on-disk locking: the common path (VM data I/O) doesn’t touch the Lock Table at all.

### 6.2 Thin Provisioning Engine

Thin provisioning in OCSFS works at the extent level. A thin-provisioned file has UNWRITTEN extents allocated lazily—when the file is created with a declared size, no physical space is allocated. The inode records the declared size but i_blocks is 0. As the VM writes to the file, OCSFS allocates extents on demand from the file’s home AG.

Space reclamation: when the guest OS issues TRIM/DISCARD/UNMAP, QEMU translates this to fallocate(FALLOC_FL_PUNCH_HOLE) on the disk image file. OCSFS handles this by converting WRITTEN extents back to free space and updating the allocation bitmap. The AG’s free space is immediately available to other files.

### 6.3 I/O Scheduling and Prioritization

OCSFS exposes per-file I/O priority hints to the block layer. Proxmox can tag VM disk I/O with the VM’s configured priority class (real-time, high, normal, low, idle). The filesystem passes these hints to the underlying request queue via bio flags, enabling the FC HBA’s hardware queue prioritization when supported.

### 6.4 Performance Targets

| **Metric**                                | **Target**                      | **Comparison with VMFS-6**          |
|-------------------------------------------|---------------------------------|-------------------------------------|
| Sequential write (single node)            | \>90% of raw LUN bandwidth      | Comparable (VMFS achieves ~95%)     |
| Random 4K IOPS (single file, single node) | \>95% of raw LUN IOPS           | Comparable                          |
| Metadata operations (create/delete)       | 1000+ ops/sec per node          | Similar (VMFS ~800-1200)            |
| Concurrent allocation (N nodes)           | Linear scaling up to AG count   | Similar (VMFS uses resource groups) |
| Lock acquisition latency                  | \<500µs average (SAN-dependent) | Comparable (\<200µs on fast FC)     |
| Crash recovery time                       | \<10s for journal replay        | Similar (VMFS \<15s typical)        |
| Mount time (cold)                         | \<5s                            | Similar                             |

## 7. Proxmox VE Integration

### 7.1 Storage Plugin Architecture

Proxmox VE uses a modular storage plugin system defined in Perl (PVE::Storage::Plugin). OCSFS integrates as a first-class storage type via a dedicated plugin: PVE::Storage::OCSFSPlugin. The plugin registers ‘ocsfs’ as a storage type in /etc/pve/storage.cfg.

```
# /etc/pve/storage.cfg
ocsfs: fc-shared
path /mnt/pve/fc-shared
device /dev/mapper/mpath-3600508b...
content images,iso,vztmpl,backup,rootdir,snippets
maxnodes 16
extentsize 4M
thin 1
shared 1
```

### 7.2 Content Type Support

Unlike LVM, which only supports ‘images’ content type on shared storage, OCSFS supports all Proxmox content types on a single shared datastore:

| **Content Type**  | **LVM (current)** | **OCSFS**              | **Notes**                                    |
|-------------------|-------------------|------------------------|----------------------------------------------|
| images (VM disks) | Yes (raw LV only) | Yes (raw, qcow2, vmdk) | qcow2 enables snapshots without LVM          |
| iso               | No                | Yes                    | ISO library shared across all nodes          |
| vztmpl            | No                | Yes                    | Container templates accessible from any node |
| backup            | No                | Yes                    | VZDump backups on shared storage             |
| rootdir           | No                | Yes                    | Container rootfs (bind mount)                |
| snippets          | No                | Yes                    | Cloud-init, Hookscripts, etc.                |

### 7.3 Live Migration Integration

With OCSFS, live migration becomes storage-agnostic: since all nodes mount the same filesystem, VM disk images don’t need to be migrated. The migration process is:

- **1. Source node:** Notifies OCSFS to prepare lock handoff for the VM’s disk files.

- **2. Memory migration:** QEMU migrates RAM state via the standard pre-copy algorithm.

- **3. Lock transfer:** Source releases EX lock on VM disk files; destination acquires EX lock. This is a single CAW on the Lock Table—typically \<1ms on FC.

- **4. VM resume:** Destination node resumes the VM with immediate full-speed disk access.

This is identical to how live migration works on VMFS and is a massive improvement over LVM, which requires storage migration (copying the entire LV) for non-shared migrations.

### 7.4 HA Manager Integration

OCSFS integrates with Proxmox’s HA manager (ha-manager) through a callback mechanism. When OCSFS’s recovery subsystem reclaims locks from a failed node, it emits a list of affected inode numbers. The Proxmox plugin maps these back to VM IDs and notifies ha-manager, which can then restart the affected VMs on surviving nodes. This tightens the HA recovery time compared to LVM, where the HA manager must wait for the entire LVM lock recovery process.

### 7.5 Management CLI

OCSFS provides a comprehensive CLI for administration:

```bash
mkfs.ocsfs [options] /dev/mapper/mpath-xxx # Format LUN
mount.ocsfs /dev/mapper/mpath-xxx /mnt/ocsfs # Mount (auto node-slot)
ocsfs-tool status /mnt/ocsfs # Cluster status
ocsfs-tool nodes /mnt/ocsfs # List active nodes
ocsfs-tool locks /mnt/ocsfs # Show lock table
ocsfs-tool df /mnt/ocsfs # Space usage + thin report
ocsfs-tool grow /mnt/ocsfs # Online expand after LUN grow
ocsfs-tool recover /mnt/ocsfs --node \<slot> # Manual recovery trigger
ocsfs-tool fence /mnt/ocsfs --node \<slot> # Emergency fencing
ocsfs-tool check /dev/mapper/mpath-xxx # Offline fsck
```

## 8. Advanced Features

### 8.1 Copy-on-Write Snapshots

OCSFS supports file-level CoW snapshots, providing a capability that VMFS entirely lacks. When a snapshot is created, the filesystem marks all extents of the file as shared (reference count \> 1). Subsequent writes to either the original or the snapshot trigger CoW: a new extent is allocated and the modified data is written there, while the old extent remains referenced by the other version.

Snapshots are implemented using an extent reference count table stored in a dedicated B+ tree per AG. This adds modest overhead (~2% space for the refcount tree) but enables instant snapshots regardless of file size. Combined with Proxmox’s snapshot UI, this provides a VMFS-superior experience where snapshots don’t require qcow2 chains.

### 8.2 Inline Compression

Optional per-file inline compression using LZ4 (default, optimized for speed) or ZSTD (higher ratio). Compression operates on extent-sized chunks. A compressed extent stores: original length, compressed length, compression algorithm ID, and the compressed data. The extent’s e_flags includes COMPRESSED, and the extent tree records both logical (uncompressed) and physical (compressed) sizes.

Compression is transparent to applications. Performance impact is minimal for VM workloads because: O_DIRECT bypasses compression entirely (the common VM data path), and compression only applies to buffered I/O (ISOs, templates, backups). This makes it ideal for the non-VM content types that OCSFS supports.

### 8.3 Online Defragmentation

A background defragmentation daemon (ocsfs-defrag) runs on one node at a time (elected via Lock Table). It scans files with high extent fragmentation and consolidates them by allocating contiguous extents, copying data, and atomically swapping the extent references. This is non-disruptive and rate-limited to avoid impacting foreground I/O.

### 8.4 Multi-LUN Spanning

Like VMFS extents, OCSFS supports spanning across multiple FC LUNs to create a single large volume. Each additional LUN is added as a new “extent segment” with its own set of Allocation Groups. The superblock maintains a segment table mapping AG ranges to LUN identifiers. This allows non-disruptive capacity expansion: add a LUN, run ocsfs-tool grow, and new AGs become available immediately.

### 8.5 Encryption at Rest

Optional per-file encryption using AES-256-XTS with per-file keys. The master key is stored encrypted (wrapped by a passphrase-derived key or TPM2 token) in the superblock. Per-file keys are stored in the inode, encrypted with the master key. This provides defense-in-depth for multi-tenant environments where FC LUNs may be accessible to multiple organizations’ HBAs.

## 9. Linux Kernel Implementation

### 9.1 Module Architecture

OCSFS is implemented as a Linux kernel module (GPLv2) targeting kernel 6.6+ LTS. The module is structured into several subsystems:

| **Subsystem** | **Source Files**        | **Responsibility**                             |
|---------------|-------------------------|------------------------------------------------|
| super         | super.c, mount.c        | Superblock management, mount/unmount lifecycle |
| inode         | inode.c, dir.c, file.c  | VFS inode/dentry/file operations               |
| extent        | extent.c, extent_tree.c | Extent allocation, B+ tree management          |
| alloc         | alloc.c, bitmap.c       | AG allocation, bitmap manipulation             |
| lock          | lock.c, scsi_pr.c       | On-disk locking, SCSI PR integration           |
| journal       | journal.c, recovery.c   | Write-ahead logging, crash recovery            |
| heartbeat     | heartbeat.c             | Heartbeat writer/reader threads                |
| thin          | thin.c                  | Thin provisioning, DISCARD handling            |
| snap          | snapshot.c, refcount.c  | CoW snapshots, extent refcounting              |
| compress      | compress.c              | Inline compression (LZ4/ZSTD)                  |
| ioctl         | ioctl.c                 | Admin commands, Proxmox plugin interface       |

### 9.2 VFS Integration

OCSFS implements the standard Linux VFS interfaces: file_system_type for registration and mount, super_operations for superblock lifecycle (statfs, sync_fs, put_super), inode_operations for metadata (lookup, create, unlink, rename, getattr, setattr), file_operations for data I/O (read_iter, write_iter, mmap, fsync, fallocate), and address_space_operations for page cache integration (readahead, writepages, direct_IO).

The filesystem registers as “ocsfs” and is mounted via the standard mount command or /etc/fstab. The mount helper (mount.ocsfs) handles node slot registration and heartbeat thread initialization before calling the kernel mount path.

### 9.3 SCSI Command Interface

The kernel module communicates with the FC LUN using the SCSI Generic (sg) interface and, where available, the block layer’s native SCSI passthrough. Key SCSI commands used:

- **COMPARE AND WRITE (0x89):** Atomic read-compare-write for lock table manipulation. Essential for lock acquisition/release.

- **PERSISTENT RESERVE IN/OUT (0x5E/0x5F):** Registration, reservation, preempt for fencing.

- **WRITE SAME (0x41/0x93):** Zero-fill for extent initialization (thin provisioning).

- **UNMAP (0x42):** Space reclamation to underlying storage (if LUN supports thin provisioning at array level).

Compatibility: OCSFS requires the SCSI target (FC array) to support CAW and PR. All enterprise FC arrays (Pure Storage, NetApp, Dell PowerStore/Unity, HPE Primera/Alletra, Hitachi VSP, IBM FlashSystem) support these. The mkfs tool validates SCSI capability at format time and refuses to format if required features are missing.

### 9.4 Memory Management

OCSFS caches metadata aggressively but conservatively: AG descriptors and allocation bitmaps are cached in the slab allocator with a shrinker callback so the kernel can reclaim memory under pressure. The extent B+ tree for active files is cached in a per-inode RCU-protected structure for lock-free read access. The Lock Table is partially cached (hot entries only) with a write-through policy: all lock modifications go to disk immediately via SCSI CAW, and the cache is invalidated on conflict.

### 9.5 DKMS and Distribution

The module is distributed via DKMS for easy installation on existing kernels. The DKMS package handles compilation against the running kernel headers. For Proxmox, a dedicated .deb package includes the kernel module, mount helper, CLI tools, and the PVE storage plugin. A long-term goal is mainline kernel inclusion (the Linux kernel has accepted GFS2, OCFS2, and most recently bcachefs, so precedent exists).

## 10. Development Roadmap

### 10.1 Phased Development Plan

| **Phase**               | **Timeline** | **Deliverables**                                                     | **Milestone**                              |
|-------------------------|--------------|----------------------------------------------------------------------|--------------------------------------------|
| Phase 0: Prototype      | Months 1–4   | FUSE prototype with on-disk layout, basic locking, single-node mount | Validate on-disk format design             |
| Phase 1: Kernel Module  | Months 5–10  | Kernel module (single-node), mkfs, mount, basic I/O, journaling      | Single-node read/write with crash recovery |
| Phase 2: Clustering     | Months 11–18 | Multi-node locking, heartbeat, SCSI PR fencing, recovery             | 2-node concurrent mount and I/O            |
| Phase 3: Proxmox Plugin | Months 19–22 | PVE storage plugin, live migration, HA integration                   | Proxmox cluster with shared FC datastore   |
| Phase 4: Advanced       | Months 23–30 | Thin provisioning, CoW snapshots, compression, defrag                | Feature parity with VMFS-6 (+ extras)      |
| Phase 5: Production     | Months 31–36 | Stress testing, certification, documentation, mainline submission    | Production-ready v1.0 release              |

### 10.2 Phase 0: FUSE Prototype (Months 1–4)

The FUSE prototype serves two critical purposes: validating the on-disk format and providing a testbed for the locking protocol. It is written in Rust for memory safety and uses the FUSE low-level API (libfuse3). The prototype implements: superblock read/write, AG management, extent allocation, inode operations (create, read, write, delete), directory operations, and a simplified version of the lock protocol using POSIX advisory locks as a stand-in for SCSI CAW.

The prototype is also the reference implementation for the mkfs and fsck tools, which can remain in userspace permanently (they don’t need kernel-speed execution).

### 10.3 Testing Strategy

- **Unit tests:** Per-subsystem tests for extent tree, bitmap, journal, lock protocol using the FUSE prototype and kernel module test harness.

- **Integration tests:** Multi-node scenarios on physical FC SAN hardware (Dell PowerStore and Pure Storage FlashArray in the test lab).

- **Stress testing:** fio, vdbench, and custom QEMU-based workload generators running concurrent VMs across 8+ nodes.

- **Fault injection:** dm-flakey for I/O errors, network partitioning (iptables), SCSI PR failure simulation, node kill -9 during I/O.

- **Formal verification:** TLA+ model of the locking protocol and recovery procedure to verify absence of deadlock and data loss.

### 10.4 Team and Resources

Estimated team for a 36-month v1.0 delivery:

| **Role**                      | **Count** | **Focus Area**                                            |
|-------------------------------|-----------|-----------------------------------------------------------|
| Lead Kernel Developer         | 1         | Module architecture, VFS integration, extent engine       |
| Kernel Developer (Locking)    | 1         | SCSI PR, lock protocol, fencing, recovery                 |
| Kernel Developer (Journal)    | 1         | Write-ahead log, crash recovery, ordered-data mode        |
| Systems Developer (Userspace) | 1         | mkfs, fsck, CLI tools, FUSE prototype, DKMS packaging     |
| Proxmox Integration Developer | 1         | PVE storage plugin, API integration, live migration hooks |
| QA/Test Engineer              | 1         | Test infrastructure, CI, stress testing, fault injection  |
| Technical Writer              | 0.5       | Documentation, man pages, architecture docs               |
| Project Lead                  | 0.5       | Coordination, community management, upstream relations    |

## 11. Comprehensive Comparison Matrix

| **Feature**                      | **VMFS-6**           | **OCSFS (Target)** | **GFS2**       | **OCFS2**         | **LVM+lvmlockd**  |
|----------------------------------|----------------------|--------------------|----------------|-------------------|-------------------|
| License                          | Proprietary          | GPLv2              | GPLv2          | GPLv2             | GPLv2             |
| Cluster awareness                | Yes (native)         | Yes (native)       | Yes (DLM)      | Yes (DLM)         | Yes (lvmlockd)    |
| Lock mechanism                   | On-disk SCSI         | On-disk SCSI       | Network DLM    | Network DLM       | Network DLM       |
| External dependencies            | vCenter              | None               | Corosync+DLM   | Cluster stack     | Corosync+lvmlockd |
| File-level access                | Yes                  | Yes                | Yes            | Yes               | No (block only)   |
| Max nodes                        | 64                   | 256                | 16 (practical) | 32 (practical)    | Unlimited         |
| Thin provisioning                | Yes                  | Yes                | No (native)    | No                | Yes (dm-thin)     |
| CoW snapshots                    | No (VMDK level)      | Yes (native)       | No             | No (reflink only) | Yes (LV snaps)    |
| ISO/template storage             | Yes                  | Yes                | Yes            | Yes               | No                |
| Proxmox plugin                   | N/A                  | Yes (native)       | No             | No                | Yes (built-in)    |
| Live migration (no storage copy) | Yes                  | Yes                | Possible       | Possible          | Yes (shared LV)   |
| Online grow                      | Yes                  | Yes                | Yes            | Yes               | Yes               |
| Compression                      | No                   | Yes (LZ4/ZSTD)     | No             | No                | No                |
| Encryption at rest               | No (datastore level) | Yes (per-file)     | No             | No                | Yes (LUKS LV)     |
| Network partition tolerant       | Yes (SAN-only)       | Yes (SAN-only)     | No (fences)    | No (fences)       | No (fences)       |
| Recovery time                    | \<15s                | \<10s target       | 30-60s         | 30-60s            | Variable          |
| Mainline kernel                  | N/A                  | Goal               | Yes            | Yes               | Yes               |
| Active development               | Yes                  | Proposed           | Minimal        | Abandoned         | Active            |

## 12. Community Strategy & Governance

### 12.1 Open-Source Governance

OCSFS will be released under GPLv2 (matching the Linux kernel’s license, mandatory for in-tree inclusion). Governance follows the Benevolent Dictator for Life (BDFL) model initially, transitioning to a Technical Steering Committee (TSC) once the project has 5+ active maintainers. All development happens in the open: public Git repository, mailing list, and IRC/Matrix channel.

### 12.2 Upstream Strategy

Mainline kernel inclusion is the long-term goal. The path: start as an out-of-tree DKMS module (Phase 1–4), submit to linux-next when the on-disk format is frozen (Phase 5), address review feedback from the linux-fsdevel mailing list, and target inclusion in a future kernel release. GFS2 (2006), OCFS2 (2006), and bcachefs (2024) all followed this path successfully.

### 12.3 Industry Engagement

Key partnerships to pursue: Proxmox Server Solutions GmbH (upstream integration in PVE), storage vendors (validation on their FC arrays—Pure, NetApp, Dell, HPE), hardware vendors (FC HBA testing with Broadcom Emulex, Marvell QLogic), and enterprise users (early adopter program for real-world validation).

### 12.4 Funding Model

Potential funding sources: Linux Foundation or similar foundation hosting, corporate sponsorship from organizations migrating off VMware, crowdfunding from the Proxmox community, European Union open-source funding programs (NGI, Horizon Europe), and commercial support contracts for enterprise users.

## 13. Appendix A: Quick Start Guide (Future)

This section describes the intended user experience once OCSFS reaches Phase 3:

```
# 1. Install on all Proxmox nodes
apt install ocsfs-tools ocsfs-dkms pve-storage-ocsfs
# 2. Identify FC LUN (multipath)
multipath -ll
# => mpath0 (3600508b...) dm-3 DGC,RAID 5
# 3. Format the LUN
mkfs.ocsfs -L shared-vm-store -N 16 -E 4M /dev/mapper/mpath0
# 4. Add to Proxmox storage configuration
pvesm add ocsfs fc-shared \
--path /mnt/pve/fc-shared \
--device /dev/mapper/mpath0 \
--content images,iso,vztmpl,backup \
--shared 1
# 5. Mount on all nodes (auto via PVE service)
systemctl enable --now ocsfs@fc-shared.service
# 6. Verify cluster status
ocsfs-tool status /mnt/pve/fc-shared
# Node 0 [pve1]: ACTIVE, heartbeat OK, 0 locks held
# Node 1 [pve2]: ACTIVE, heartbeat OK, 3 locks held
# Node 2 [pve3]: ACTIVE, heartbeat OK, 5 locks held
# Volume: 4.8 TiB total, 2.1 TiB used (1.4 TiB thin), 2.7 TiB free
```

## 14. Appendix B: Glossary

| **Term**          | **Definition**                                                                  |
|-------------------|---------------------------------------------------------------------------------|
| AG                | Allocation Group — independent unit of space management within the volume       |
| ATS               | Atomic Test and Set — hardware-level atomic compare-and-swap on SCSI devices    |
| CAW               | Compare And Write — SCSI command (0x89) for atomic read-modify-write            |
| CoW               | Copy on Write — technique for efficient snapshots                               |
| DLM               | Distributed Lock Manager — network-based lock coordination (used by GFS2/OCFS2) |
| Extent            | Contiguous range of blocks allocated to a file                                  |
| Fencing           | Mechanism to prevent a failed node from issuing I/O                             |
| FC                | Fibre Channel — high-speed storage networking protocol                          |
| HBA               | Host Bus Adapter — FC interface card in a server                                |
| LUN               | Logical Unit Number — a block device exported by a SAN array                    |
| PR                | Persistent Reservations — SCSI-3 mechanism for multi-initiator access control   |
| SCSI PR           | SCSI-3 Persistent Reservations — hardware fencing and access control            |
| Thin Provisioning | Allocating storage on demand rather than upfront                                |
| VMDK              | Virtual Machine Disk — VMware’s disk image format                               |
| VMFS              | Virtual Machine File System — VMware’s proprietary clustered filesystem         |
