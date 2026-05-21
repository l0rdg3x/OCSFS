# OCSFS — Developer Guide

**Versione:** 0.1 — Maggio 2026  
**Stato:** Alpha — non per produzione

## Indice

1. [Panoramica architetturale](#1-panoramica-architetturale)
2. [Layout on-disk](#2-layout-on-disk)
3. [Strutture dati principali](#3-strutture-dati-principali)
4. [Sottosistemi del kernel module](#4-sottosistemi-del-kernel-module)
5. [Protocollo di locking distribuito](#5-protocollo-di-locking-distribuito)
6. [Heartbeat e rilevamento guasti](#6-heartbeat-e-rilevamento-guasti)
7. [Recovery a 5 fasi](#7-recovery-a-5-fasi)
8. [Journal e crash recovery](#8-journal-e-crash-recovery)
9. [Path I/O](#9-path-io)
10. [Build e sviluppo](#10-build-e-sviluppo)
11. [Bug noti e limitazioni](#11-bug-noti-e-limitazioni)

---

## 1. Panoramica architetturale

OCSFS è un filesystem cluster-aware per Linux, progettato per SAN Fibre Channel condivise in ambienti Proxmox VE. La differenza fondamentale rispetto a GFS2/OCFS2: **tutto il locking è on-disk** tramite SCSI Compare-And-Write, senza DLM esterno né dipendenza dalla rete di management.

```
┌────────────────────────────────────────────────────────────┐
│  Proxmox VE (PVE::Storage::OCSFSPlugin)                    │
├────────────────────────────────────────────────────────────┤
│  VFS Linux (inode_operations, file_operations, aops)       │
├───────────┬───────────┬──────────┬─────────────────────────┤
│ super.c   │ inode.c   │ dir.c    │ file.c + iomap.c        │
│ (mount)   │ (VFS ops) │ (dir ops)│ (I/O path)              │
├───────────┴───────────┴──────────┴─────────────────────────┤
│ extent.c   bitmap.c   alloc.c   thin.c   journal.c         │
│ (spazio)   (bitmap)   (allocat.) (thin)   (WAL)            │
├───────────────────────────────────────────────────────────-┤
│ lock.c    heartbeat.c  node.c   recovery.c  scsi_pr.c      │
│ (DLM)     (heartbeat)  (slot)   (recovery)  (SCSI PR)      │
├────────────────────────────────────────────────────────────┤
│  Block layer Linux — FC LUN / loopback / iSCSI             │
└────────────────────────────────────────────────────────────┘
```

### File sorgenti

| File | Responsabilità |
|------|---------------|
| `kmod/super.c` | Module init/exit, mount, fill_super, statfs, sync_fs |
| `kmod/inode.c` | VFS inode ops (iget, write_inode, evict, new_inode, setattr) |
| `kmod/dir.c` | Directory ops (lookup, create, mkdir, rmdir, rename, readdir) |
| `kmod/file.c` | File ops + get_block callback (buffer_head fallback) |
| `kmod/iomap.c` | iomap-based I/O — direct I/O (O_DIRECT), buffered, readahead |
| `kmod/extent.c` | Inline extent manager (lookup, insert, truncate, UNWRITTEN) |
| `kmod/alloc.c` | Smart allocator con prealloc e AG affinity |
| `kmod/bitmap.c` | Block bitmap e inode number allocator per-AG |
| `kmod/journal.c` | Write-ahead log, txn begin/commit/abort, replay |
| `kmod/lock.c` | DLM on-disk: acquire, release, downgrade, recover |
| `kmod/heartbeat.c` | kthread heartbeat writer + peer monitoring |
| `kmod/node.c` | Node slot table: claim, release, auth |
| `kmod/recovery.c` | 5-phase recovery orchestration |
| `kmod/scsi_pr.c` | SCSI-3 PR via block-layer `pr_ops` |
| `kmod/thin.c` | Thin provisioning: fallocate, punch_hole, DISCARD |
| `kmod/snapshot.c` | CoW file-level snapshots |
| `kmod/refcount.c` | Extent reference counting per-AG |
| `kmod/compress.c` | Inline compression LZ4/ZSTD |

---

## 2. Layout on-disk

Il volume occupa un intero block device. Le regioni sono sequenziali dall'offset 0.

```
Offset 0       : Superblock            (4 KB)
Offset 4 KB    : Superblock mirror     (4 KB)
Offset 8 KB    : Node Slot Table       (64 KB — 256 slot × 256 byte)
Offset 72 KB   : Heartbeat Region      (256 KB — 256 entry × 1 KB)
Offset 328 KB  : Lock Table            (1 MB — 4096 entry × 256 byte)
Offset ~1.3 MB : Journal Region        (N × 32 MB, N = max_nodes)
After journals : AG Descriptors        (ag_count × 4 KB)
After descs    : Data Region           (Allocation Groups)
```

Tutti i campi multi-byte sono **little-endian** su disco.

### Superblock (offset 0 e 4 KB)

```c
struct ocsfs_disk_super {
    __le32 s_magic;           // 0x4F435346 'OCSF'
    __le16 s_version_major;   // 0
    __le16 s_version_minor;   // 1
    __u8   s_uuid[16];
    __u8   s_label[64];
    __le32 s_block_size;      // 4096 (unico supportato)
    __le32 s_extent_size;     // default 1 MB
    __le64 s_total_blocks;
    __le64 s_free_blocks;     // approssimativo; per-AG è autoritativo
    __le32 s_ag_count;
    __le64 s_ag_size;         // blocchi per AG
    __le16 s_max_nodes;       // default 64, max 256
    __le64 s_feature_flags;   // OCSFS_FEAT_*
    __le32 s_journal_size;    // byte per journal per nodo
    // ... offsets e timestamps
    __le32 s_checksum;        // CRC32C dei byte 0..4091
};
```

Il CRC32C copre tutto il superblock tranne gli ultimi 4 byte (il campo checksum stesso).

### Inode (512 byte)

Ogni inode contiene fino a **16 extent inline** (16 × 24 = 384 byte). Quando servono più extent, `i_extent_tree_root` punta alla radice di un B+ tree — **non ancora implementato nel kernel module** (vedi §11).

### Lock Table Entry (256 byte)

```c
struct ocsfs_disk_lock {
    __le32 le_magic;          // OCSFS_LOCK_MAGIC
    __le64 le_resource_id;    // hash della risorsa
    __le32 le_resource_type;  // INODE, AG, JOURNAL, ...
    __le16 le_mode;           // NL/SH/EX/CW
    __le16 le_holder_slot;    // slot del nodo holder EX
    __le32 le_holder_gen;     // mount generation holder
    __le64 le_grant_time;
    __le32 le_sh_holders;     // bitmask nodi 0-31 con SH
    __u8   le_sh_holders_ext[32]; // nodi 32-255
    __u8   le_waiters[32];    // bitmask waiting nodes
    __u8   le_waiter_modes[64];
    __le32 le_version;        // versione CAS
    // ... reserved + checksum
};
```

---

## 3. Strutture dati principali

### `ocsfs_sb_info` — superblock in-memory (`sb->s_fs_info`)

Campi chiave:

| Campo | Tipo | Descrizione |
|-------|------|-------------|
| `s_ags` | `ocsfs_ag_info[]` | Array degli AG (kvmalloc al mount) |
| `s_journal` | `ocsfs_journal` | Journal del nodo corrente |
| `s_node_slot` | `u16` | Slot acquisito al mount |
| `s_mount_gen` | `u32` | Generazione del mount corrente |
| `s_clustered` | `bool` | True se multi-nodo attivo |
| `s_nodes[]` | `ocsfs_node_info[256]` | Stato in-memory dei peer |
| `s_hb` | `ocsfs_heartbeat_info` | Thread e stato heartbeat |
| `s_pr` | `ocsfs_pr_info` | Chiave SCSI PR registrata |
| `s_recovery_work` | `work_struct` | Recovery asincrono |

### `ocsfs_inode_info` — per-inode (`container_of(inode, ...)`)

```c
struct ocsfs_inode_info {
    u64                  i_disk_ino;
    u32                  i_ag;
    u32                  i_flags;          // OCSFS_IFLAG_*
    u16                  i_extent_count;
    struct ocsfs_extent  i_extents[16];    // extent inline
    u64                  i_extent_tree_root;
    struct mutex         i_extent_lock;
    struct ocsfs_lock_res i_lock_res;      // DLM cross-nodo
    struct inode         vfs_inode;        // DEVE essere ultimo
};
```

Accesso: `OCSFS_I(inode)` → `container_of(inode, struct ocsfs_inode_info, vfs_inode)`

### `ocsfs_lock_res` — risorsa di lock

```c
struct ocsfs_lock_res {
    u64          lr_resource_id;
    u32          lr_resource_type;
    u16          lr_mode;          // lock attualmente tenuto
    u16          lr_slot;          // slot nella lock table
    bool         lr_cached;        // cache 500ms attiva
    u64          lr_cache_expires;
    struct mutex lr_mutex;         // serializzazione locale
    struct list_head lr_list;
};
```

---

## 4. Sottosistemi del kernel module

### Sequenza di mount (`super.c`)

```
ocsfs_fill_super()
  ├── sb_read(block 0) → validate superblock (magic, version, CRC32C)
  ├── ocsfs_load_ags()     → legge tutti gli AG descriptor
  ├── ocsfs_cluster_init() → node.c: claim slot + scsi_pr: register
  │     ├── ocsfs_node_claim_slot()
  │     ├── ocsfs_pr_register()
  │     └── ocsfs_heartbeat_start()  → avvia kthread
  ├── ocsfs_journal_init()  → trova la regione journal del nodo
  ├── ocsfs_journal_replay() → crash recovery del nodo corrente
  └── ocsfs_iget(OCSFS_ROOT_INO) → monta root inode
```

### Sequenza di unmount (`super.c`)

```
ocsfs_put_super()
  ├── ocsfs_journal_exit()   → flush journal
  └── ocsfs_cluster_exit()
        ├── ocsfs_heartbeat_stop()
        ├── ocsfs_node_release_slot()
        └── ocsfs_pr_unregister()
```

---

## 5. Protocollo di locking distribuito

### Compatibilità lock

| Held \ Requested | NL | SH | EX | CW |
|---|---|---|---|---|
| NL | ✅ | ✅ | ✅ | ✅ |
| SH | ✅ | ✅ | ❌ | ❌ |
| EX | ✅ | ❌ | ❌ | ❌ |
| CW | ✅ | ❌ | ❌ | ✅ |

### Acquisizione lock (`lock.c:ocsfs_lock_acquire`)

```
1. Cache fast-path: se lr_cached && non scaduto → return 0
2. ocsfs_lock_probe_slot() → trova slot fisico (linear probing)
3. lock_read_entry() → legge entry dal disco
4. Compatibile? → aggiorna entry via lock_write_entry() con version check
5. Conflitto? → set_waiter_bit() + exponential backoff (1ms → 100ms, max 50 retry)
```

**Atomicità:** Attualmente basata su versioning software (read-version → check-version → write). La vera atomicità richiederebbe SCSI CAW — vedi §11.

### Hashing risorse

```c
// Inode: FNV-1a mixing sul numero inode
ocsfs_lock_hash_inode(ino)

// AG: mixing con prefisso 0xA6...
ocsfs_lock_hash_ag(ag_num)

// Slot nella lock table
slot = resource_id % OCSFS_LOCK_ENTRY_COUNT  // 4096 entry
```

Collisioni gestite via linear probing (max `OCSFS_LOCK_PROBE_MAX = 16` slot).

### Single-node mode

Se `sbi->s_clustered == false` (device senza supporto PR), tutte le funzioni di lock retornano immediatamente senza I/O su disco. Il filesystem funziona come single-node standard.

---

## 6. Heartbeat e rilevamento guasti

Il kthread `ocsfs-hb/<slot>` esegue due compiti su timer:

| Operazione | Intervallo | Azione |
|---|---|---|
| Write heartbeat | `HB_INTERVAL_MS` = 5s | Scrive timestamp + sequenza monotona nel proprio settore |
| Check peers | `HB_CHECK_MS` = 2s | Legge heartbeat di tutti i nodi ACTIVE |

### Rilevamento a 2 fasi (`heartbeat.c`)

```
Heartbeat stale (> 15s)  → ni_state = SUSPECTED, record ni_suspect_time
Ancora stale dopo 10s    → ocsfs_recovery_trigger(sb, slot)
Heartbeat fresco         → torna ACTIVE (falso allarme)
```

La doppia finestra riduce i falsi positivi da rallentamenti transitori del path di storage.

---

## 7. Recovery a 5 fasi

Orchestrato da `recovery.c`. Solo il nodo con lo slot più basso tra quelli ACTIVE diventa leader.

| Fase | Azione | File |
|---|---|---|
| 1 — Leader election | Lowest-slot surviving node wins | `recovery.c:ocsfs_is_recovery_leader()` |
| 2 — SCSI PR fencing | `PREEMPT_AND_ABORT` con la PR key del nodo morto | `scsi_pr.c:ocsfs_pr_preempt_abort()` |
| 3 — Journal replay | Replay del journal del nodo fallito | `journal.c:ocsfs_journal_replay_node()` |
| 4 — Lock recovery | Scan dell'intera lock table, rilascio lock del nodo morto | `lock.c:ocsfs_lock_recover_node()` |
| 5 — Slot cleanup | Marca lo slot come DEAD | `node.c:ocsfs_node_mark_dead()` |

Il recovery è asincrono (`schedule_work`) e serializzato da `s_recovery_lock`.

**Limitazione attuale:** un solo recovery alla volta (`s_recovery_target` è un singolo `u16`).

---

## 8. Journal e crash recovery

Journal circolare per-nodo a 32 MB (configurabile). Struttura:

```
[Journal Header 4KB][TXN Begin][Block Ref][Block Data]...[TXN Commit][...]
```

### Ciclo di vita transazione

```c
txn = ocsfs_txn_begin(sb);
ocsfs_txn_add_bh(txn, bh);  // aggiunge before-image
// ... modifica bh ...
ocsfs_txn_commit(txn);       // scrive after-image + commit record
```

Il replay al mount è **idempotente**: transazioni senza commit record vengono scartate.

---

## 9. Path I/O

### Dati (file regolari) — iomap path

```
write_iter → ocsfs_file_write_iter()
  ├── O_DIRECT: iomap_dio_rw() → ocsfs_iomap_ops → extent_lookup/alloc
  └── Buffered: iomap_file_buffered_write() → pagecache → writepages
```

### Metadata (directory, inode) — buffer_head path

```
dir lookup → ocsfs_dir_bread() → ocsfs_extent_lookup() → sb_bread()
```

### Extent lookup (inline)

```c
ocsfs_extent_lookup(inode, logical_block, &ext)
  → scansiona oi->i_extents[0..i_extent_count-1]
  → ritorna estensione che contiene logical_block
```

Complessità: O(16) per extent inline. B+ tree per overflow **non implementato nel kmod**.

---

## 10. Build e sviluppo

### Dipendenze

```bash
# Debian/Ubuntu/Proxmox
apt install build-essential uuid-dev linux-headers-$(uname -r)

# Per il prototipo FUSE
apt install libfuse3-dev
```

### Build

```bash
# Userspace tools + test suite
make all && make test

# Kernel module
cd kmod && make
# oppure via DKMS:
sudo dkms add kmod/
sudo dkms build ocsfs/0.1.0 && sudo dkms install ocsfs/0.1.0

# Test su immagine loopback
make demo
```

### Test rapido single-node

```bash
# Crea immagine 2 GiB
dd if=/dev/zero of=/tmp/test.img bs=1M count=2048

# Formatta
./mkfs.ocsfs -L test -N 4 -f /tmp/test.img

# Monta
sudo losetup /dev/loop0 /tmp/test.img
sudo insmod kmod/ocsfs.ko
sudo mount -t ocsfs /dev/loop0 /mnt/ocsfs

# Verifica
ls /mnt/ocsfs && df /mnt/ocsfs

# Smonta
sudo umount /mnt/ocsfs && sudo rmmod ocsfs && sudo losetup -d /dev/loop0
```

### Struttura del repository

```
include/     # Header on-disk condiviso userspace/kernel
src/         # Prototipo FUSE + librerie userspace
kmod/        # Kernel module
tools/       # mkfs.ocsfs, ocsfs-tool, ocsfs-defrag
tests/       # test_ocsfs.c (userspace), xfstests-ocsfs.conf
proxmox/     # OCSFSPlugin.pm, mount.ocsfs, install.sh
conf/        # Systemd units, udev rules
man/         # Man pages
debian/      # Packaging Debian
docs/        # Questa documentazione
```

---

## 11. Bug noti e limitazioni

### CRITICO — Da risolvere prima del testing multi-nodo

**BUG-001: Mismatch dimensione struct inode tra header**

- `include/ocsfs.h`: `i_reserved[32]` → sizeof = 512 (con _Static_assert ✅)
- `kmod/ocsfs.h`: `i_reserved[12]` → sizeof = 492 ❌ (senza _Static_assert)
- Il kernel legge 512 byte ma il campo `i_checksum` è a offset 492 anziché 508
- **Fix:** aggiungere `i_reserved[32]` in `kmod/ocsfs.h` + `_Static_assert`

**BUG-002: Deadlock in `ocsfs_lock_downgrade()` quando `new_mode == NL`**

- `lock.c:523-554`: la funzione tiene `lr_mutex`, poi chiama `ocsfs_lock_release()` che tenta di acquisire lo stesso `lr_mutex`
- **Fix:** rilasciare `lr_mutex` prima di chiamare `ocsfs_lock_release()`, o ristrutturare per un'implementazione inline del release

**BUG-003: TOCTOU race nel locking multi-nodo**

- `lock.c:63-119`: il commento stesso documenta il problema
- L'approccio attuale (read-version → check-version → write) riduce ma non elimina la race window
- **Fix:** implementare SCSI Compare-And-Write (CAW, opcode 0x89) tramite `sg` layer
- Priorità: bloccante per qualsiasi test multi-nodo reale

### MEDIA priorità

**BUG-004: Recovery limitato a singolo target**

- `recovery.c`: `s_recovery_target` è un `u16`; se due nodi falliscono contemporaneamente, il secondo non viene recuperato
- **Fix:** cambiare in bitmask + queue di recovery

**LIMIT-001: Directory come lista piatta O(n)**

- `dir.c:8`: "Future phases will add a B+ tree index"
- Impatto su directory con molti file (ISO library, template repository)
- **Fix:** implementare `src/btree.c` nel kmod

**LIMIT-002: Extent B+ tree non implementato nel kmod**

- File con più di 16 extent non supportati nel kernel module
- `i_extent_tree_root` nel disco è previsto ma non gestito nel kmod
- Impatto: file molto frammentati o molto grandi con extent piccoli

### BASSA priorità / Feature mancanti

| Feature | Stato | Note |
|---------|-------|------|
| SCSI CAW vero | Non implementato | Richiesto per multi-nodo production |
| B+ tree directory | Solo userspace | `src/btree.c` pronto |
| Multi-LUN spanning | Non implementato | Sezione 8.4 arch doc |
| Cifratura per-file AES-256-XTS | Non implementato | Flag `OCSFS_FEAT_ENCRYPTION` definito |
| Deduplicazione | Non implementato | Flag `OCSFS_FEAT_DEDUP` definito |
| Proxmox HA inode→VM mapping | Non collegato | `OCSFSPlugin.pm` esiste |
| fsck offline completo | Solo check base | `ocsfs-tool check` |
| Online grow multi-LUN | Non implementato | Menzionato arch doc §8.4 |
| KUnit kernel tests | Non esistono | Solo userspace test_ocsfs.c |
