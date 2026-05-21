# OCSFS — Gap Analysis e Piano di Test

**Data:** 2026-05-21  
**Versione analizzata:** 0.1 (branch `main`, commit `6f9214f`)

## Indice

1. [Bug critici (bloccanti per multi-nodo)](#1-bug-critici)
2. [Bug non critici](#2-bug-non-critici)
3. [Feature incomplete o mancanti](#3-feature-incomplete-o-mancanti)
4. [Discrepanze tra README e codice reale](#4-discrepanze-readme-vs-codice)
5. [Piano di test](#5-piano-di-test)
6. [Priorità di sviluppo](#6-priorità-di-sviluppo)

---

## 1. Bug critici

Questi bug devono essere risolti prima di qualsiasi test multi-nodo reale.

---

### BUG-001 — Mismatch dimensione struct inode kernel vs userspace ✅ RISOLTO (2026-05-21)

**File:** `kmod/ocsfs.h` e `include/ocsfs.h`  
**Gravità:** CRITICA — corruzione dati silente  

**Problema:**

`include/ocsfs.h` (userspace):
```c
uint8_t i_reserved[32];   // → sizeof(ocsfs_inode) = 512  ✅
uint32_t i_checksum;
```
Con `_Static_assert(sizeof == 512)` che garantisce la correttezza.

`kmod/ocsfs.h` (kernel):
```c
__u8  i_reserved[12];     // → sizeof(ocsfs_disk_inode) = 492  ❌
__le32 i_checksum;
```
Nessun `_Static_assert`. Il kernel legge 512 byte per inode (`OCSFS_INODE_SIZE`) ma interpreta la struttura con `i_checksum` a offset 492 anziché 508. Il CRC viene letto e scritto in posizione errata: gli inode scritti dal kernel hanno checksum corrotto da mkfs e viceversa.

**Fix:**
```c
// kmod/ocsfs.h — cambia:
__u8  i_reserved[12];
// in:
__u8  i_reserved[32];

// e aggiungi subito dopo la struct:
static_assert(sizeof(struct ocsfs_disk_inode) == OCSFS_INODE_SIZE,
              "ocsfs_disk_inode must be exactly 512 bytes");
```

---

### BUG-002 — Deadlock in `ocsfs_lock_downgrade()` con `new_mode == NL` ✅ RISOLTO (2026-05-21)

**File:** `kmod/lock.c:523-554`  
**Gravità:** CRITICA — kernel deadlock / hang  

**Problema:** La funzione acquisisce `lr_mutex` alla riga 539, poi alla riga 553 chiama `ocsfs_lock_release()` che tenta di acquisire lo stesso `lr_mutex` (non è ricorsivo).

```c
// lock.c:539
mutex_lock(&lr->lr_mutex);
// ...
} else if (new_mode == OCSFS_LOCK_NL) {
    return ocsfs_lock_release(sb, lr);  // ← mutex_lock() di nuovo → DEADLOCK
}
```

**Fix:**
```c
} else if (new_mode == OCSFS_LOCK_NL) {
    mutex_unlock(&lr->lr_mutex);       // ← rilascia prima
    return ocsfs_lock_release(sb, lr);
}
```

---

### BUG-003 — Race TOCTOU nel locking multi-nodo ⚠️ PARZIALMENTE RISOLTO (2026-05-21)

**File:** `kmod/lock.c:56-119`, `kmod/scsi_pr.c`  
**Gravità:** CRITICA — corruzione dati in ambiente multi-nodo  

**Stato:** L'infrastruttura CAW è stata implementata in `scsi_pr.c`. `ocsfs_build_caw_cdb()` costruisce il CDB corretto (testato da KUnit). `lock_write_entry()` usa il percorso CAW quando `sbi->s_caw_supported = true`. Tuttavia, `scsi_device_from_queue()` NON è esportato in questo kernel (non in Module.symvers) — `ocsfs_scsi_caw_probe()` ritorna sempre `false` come fallback sicuro, mantenendo il software version-check path. Per abilitare il CAW hardware: aggiungere `EXPORT_SYMBOL_GPL(scsi_device_from_queue)` in `drivers/scsi/scsi_lib.c` e riabilitare l'implementazione in `scsi_pr.c`.

**Problema originale:** Il commento nel codice documentava esplicitamente il problema:

> "NOTE: This narrows the TOCTOU window but does not eliminate it — true atomicity requires SCSI Compare-And-Write (CAW). For production multi-node deployments on SCSI SANs, add CAW support via the sg layer."

La sequenza attuale è:
1. Read entry → ottieni version N
2. Check version N == atteso
3. Write entry con version N+1

Tra i passi 1 e 3, un altro nodo può scrivere (N→N+1) e la nostra write porta N+1→N+2 con dati errati. Il check al passo 2 aiuta ma non è atomico con la write.

**Fix:** Implementare SCSI Compare-And-Write (CDB opcode 0x89) per la write della lock entry. Il blocco critico è `lock_write_entry()` che deve diventare:

```c
// Pseudocodice:
ret = scsi_caw(bdev, block_lba,
               expected_data,   // 512 byte con version attesa
               new_data,        // 512 byte con version+1 e nuovi dati
               512);
if (ret == MISCOMPARE) return -EAGAIN;
```

Richiede: implementazione di `ocsfs_scsi_caw()` in `scsi_pr.c` tramite `blk_execute_rq()` con CDB CAW.

---

## 2. Bug non critici

---

### BUG-004 — Recovery di un solo nodo fallito alla volta

**File:** `kmod/recovery.c` e `kmod/ocsfs.h`  
**Gravità:** MEDIA — perdita di dati in failure scenari multipli  

**Problema:** `sbi->s_recovery_target` è un singolo `u16`. Se due nodi falliscono quasi contemporaneamente, il secondo non viene recuperato.

```c
// ocsfs.h
struct ocsfs_sb_info {
    // ...
    u16  s_recovery_target;      // ← singolo slot
    bool s_recovery_in_progress;
};
```

**Fix:** Cambiare in una coda o bitmask:
```c
unsigned long s_recovery_pending;  // bitmask, un bit per slot
// + flush ordinato tramite work queue dedicata
```

---

### BUG-005 — Heartbeat write blocca su I/O lento (`sync_dirty_buffer`)

**File:** `kmod/heartbeat.c:51`  
**Gravità:** BASSA — falsi positivi in case di I/O lento  

**Problema:** `ocsfs_heartbeat_write()` chiama `sync_dirty_buffer(bh)` che può bloccare a lungo se il path di storage è congestionato. Questo ritarda il prossimo ciclo di write e può far scattare falsi allarmi di timeout su altri nodi.

**Fix:** Rendere la write asincrona con `mark_buffer_dirty()` e flush separato, oppure usare un bio diretto non-blocking.

---

## 3. Feature incomplete o mancanti

### Feature bloccanti per produzione

| ID | Feature | Stato | Impatto |
|----|---------|-------|---------|
| F-001 | SCSI CAW per lock atomici | Non implementato | Correttezza multi-nodo |
| F-002 | B+ tree per directory grandi | Solo `src/btree.c` (userspace) | Performance directory grandi |
| F-003 | Extent B+ tree nel kmod | Non implementato | File con >16 extent non supportati |
| F-004 | fsck offline completo | Solo `ocsfs-tool check` base | Recupero da corruzione |

### Feature di completamento

| ID | Feature | Stato | Nota |
|----|---------|-------|------|
| F-005 | Multi-LUN spanning | Non implementato | Arch doc §8.4 |
| F-006 | Online grow | Parziale in `ocsfs-tool` | Richiede multi-LUN |
| F-007 | Proxmox HA inode→VM mapping | Plugin presente, callback mancante | Recovery HA |
| F-008 | Cifratura AES-256-XTS per-file | Flag definito, nessun codice | `OCSFS_FEAT_ENCRYPTION` |
| F-009 | Deduplicazione | Flag definito, nessun codice | `OCSFS_FEAT_DEDUP` |
| F-010 | Quorum mode (senza SCSI PR) | Non implementato | iSCSI economico |

---

## 4. Discrepanze README vs codice

Il README dichiara "All Phases Complete". Questa è la realtà per fase:

| Fase | README | Realtà |
|------|--------|--------|
| Phase 0 — FUSE prototype | Complete ✅ | Completo ✅ |
| Phase 1 — Kernel module single-node | Complete ✅ | Completo con BUG-001 e BUG-002 ⚠️ |
| Phase 2 — Multi-node clustering | Complete ✅ | Compilato ma BUG-003 blocca test reali ⚠️ |
| Phase 3 — Performance (iomap, O_DIRECT) | Complete ✅ | Sostanzialmente completo ✅ |
| Phase 4 — Advanced features + Proxmox | Complete ✅ | Plugin presente; snapshot/compress compilati ma non testati ⚠️ |
| Phase 5 — Production readiness | Complete ✅ | Packaging presente; test suite solo userspace ❌ |

---

## 5. Piano di test

### Livello 0 — Userspace (già parzialmente presente)

```bash
# Test suite esistente (36 test, 1770 assertion)
make test
./test_ocsfs

# Verifica tutti i test passino: CRC32C, bitmap, extent, lock, btree,
# inode, journal, directory, superblock serialization
```

**Gap:** I test coprono solo il layer userspace (`src/`). Non testano `kmod/`.

---

### Livello 1 — Kernel module single-node

**Obiettivo:** Verificare mount/unmount, I/O base, crash recovery.

```bash
# Prerequisiti
dd if=/dev/zero of=/tmp/ocsfs-test.img bs=1M count=2048
./mkfs.ocsfs -L test -N 4 -f /tmp/ocsfs-test.img

# Test 1: Mount base
sudo losetup /dev/loop0 /tmp/ocsfs-test.img
sudo insmod kmod/ocsfs.ko
sudo mount -t ocsfs /dev/loop0 /mnt/ocsfs
dmesg | grep -i ocsfs

# Test 2: Operazioni filesystem base
echo "hello" > /mnt/ocsfs/test.txt
mkdir /mnt/ocsfs/subdir
cp /bin/ls /mnt/ocsfs/subdir/
ls -la /mnt/ocsfs/subdir/
diff /bin/ls /mnt/ocsfs/subdir/ls

# Test 3: I/O grande (O_DIRECT)
fio --name=ocsfs-direct \
    --filename=/mnt/ocsfs/fio-test \
    --direct=1 --rw=randrw --bs=4k \
    --size=256M --numjobs=1 --runtime=30

# Test 4: Crash recovery
sync
# Simula crash (non umount)
sudo echo b > /proc/sysrq-trigger   # ATTENZIONE: riboota il sistema
# Al reboot, rimonta e verifica consistenza:
sudo mount -t ocsfs /dev/loop0 /mnt/ocsfs   # deve fare journal replay
ocsfs-tool check /dev/loop0

# Cleanup
sudo umount /mnt/ocsfs && sudo rmmod ocsfs && sudo losetup -d /dev/loop0
```

---

### Livello 2 — xfstests (generic filesystem tests)

```bash
# Installa xfstests
git clone https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git
cd xfstests-dev && make

# Configura per OCSFS (usa xfstests-ocsfs.conf esistente)
cp /opt/ocsfs/tests/xfstests-ocsfs.conf local.config

# Esegui subset generico
./check -E /opt/ocsfs/tests/xfstests-ocsfs.conf generic/001
./check -g quick                    # test rapidi (~30 min)
./check -g auto                     # suite completa (~ore)

# Test attesi al fallimento in questa fase:
# - Test che richiedono extent overflow (> 16 extent)
# - Test su directory molto grandi (> 1000 entry)
# - Test che richiedono FICLONE/FICLONERANGE (reflink)
```

---

### Livello 3 — Multi-nodo su due VM

**Setup:** Due VM con un disco condiviso (simulato con NBD o iSCSI locale).

> **Prerequisito:** BUG-001, BUG-002, BUG-003 devono essere risolti prima.

```bash
# VM1 (nodo 0): formatta e monta
mkfs.ocsfs -L shared -N 2 -f /dev/nbd0
mount -t ocsfs /dev/nbd0 /mnt/shared

# VM2 (nodo 1): monta lo stesso device
mount -t ocsfs /dev/nbd0 /mnt/shared

# Test 1: Write da VM1, read da VM2
echo "from-vm1" > /mnt/shared/test.txt     # VM1
cat /mnt/shared/test.txt                    # VM2 (deve vedere "from-vm1")

# Test 2: Lock contention
# VM1 apre e scrive in loop su file1
# VM2 tenta write concorrente sullo stesso file → deve aspettare il lock

# Test 3: Heartbeat e recovery
# VM1: monta e fa I/O
# VM2: monta
# Spegni VM1 brutalmente (kill -9 QEMU)
# VM2 deve rilevare dopo 15-25s e fare recovery

# Verifica recovery:
dmesg | grep -E "ocsfs.*RECOVERY|ocsfs.*phase"
ocsfs-tool nodes /mnt/shared   # nodo 0 deve essere DEAD
ocsfs-tool locks /mnt/shared   # nessun lock orfano
ls /mnt/shared/                # filesystem accessibile
```

---

### Livello 4 — Hardware FC reale

**Obiettivo:** Validare su storage enterprise con SCSI PR reale.

```bash
# Verifica CAW support (dopo implementazione BUG-003 fix)
sg_vpd -p b0 /dev/sda   # deve mostrare "Compare and Write" supportato

# Test PR register/preempt
sg_persist --out --register --param-rk=0x1234 /dev/sda
sg_persist --in -k /dev/sda    # mostra chiave registrata
sg_persist --out --preempt --param-rk=0 --param-sark=0x1234 /dev/sda

# Test multi-nodo con Proxmox cluster reale (4+ nodi)
# + test live migration VM
# + test HA failover (spegni nodo con VM attiva)
```

---

### Livello 5 — Test di carico e fault injection

```bash
# fio stress test multi-job
fio --name=stress \
    --filename=/mnt/shared/fio-\$jobnum \
    --direct=1 --rw=randrw --bs=4k-1m --bsrange=4k-1m \
    --size=10G --numjobs=8 --runtime=3600 \
    --group_reporting

# Fault injection con dm-flakey (simula I/O errors)
# (richiede device-mapper-flakey)
dmsetup create flakey --table \
  "0 $(blockdev --getsz /dev/loop0) flakey /dev/loop0 0 30 10"
mount -t ocsfs /dev/mapper/flakey /mnt/ocsfs

# I/O durante il recovery
# (scrivi in loop da N processi mentre spegni/riaccendi nodi)
```

---

### Livello 6 — KUnit (da implementare)

Attualmente non esistono KUnit test. Da aggiungere in `kmod/test/`:

```c
// kmod/test/test_lock.c — esempio struttura
#include <kunit/test.h>
#include "ocsfs.h"

static void test_lock_compatibility(struct kunit *test)
{
    KUNIT_EXPECT_TRUE(test, lock_modes_compatible(OCSFS_LOCK_SH, OCSFS_LOCK_SH));
    KUNIT_EXPECT_FALSE(test, lock_modes_compatible(OCSFS_LOCK_EX, OCSFS_LOCK_SH));
    // ...
}

static struct kunit_case ocsfs_lock_test_cases[] = {
    KUNIT_CASE(test_lock_compatibility),
    {}
};
```

---

## 6. Priorità di sviluppo

### Sprint 1 — Stabilità single-node (2-3 settimane)

1. **FIX BUG-001** — Mismatch inode struct (1 giorno)
2. **FIX BUG-002** — Deadlock lock_downgrade (1 giorno)
3. **ADD** — _Static_assert per ocsfs_disk_inode nel kmod (1 ora)
4. **RUN** — xfstests generic/quick su single-node (continuo)

### Sprint 2 — Multi-nodo corretto (3-4 settimane)

5. **FIX BUG-003** — Implementare SCSI CAW in scsi_pr.c e usarlo in lock_write_entry()
6. **FIX BUG-004** — Recovery bitmask per nodi multipli
7. **RUN** — Test multi-nodo livello 3 su VM simulate

### Sprint 3 — Completamento directory e extent (2-3 settimane)

8. **PORT** — `src/btree.c` nel kernel module per extent overflow
9. **IMPL** — Directory indicizzata con B+ tree (basata su btree.c portato)
10. **RUN** — xfstests generic/auto

### Sprint 4 — Proxmox production (2-3 settimane)

11. **IMPL** — Callback Proxmox HA per inode→VM mapping
12. **TEST** — Live migration su cluster Proxmox reale
13. **TEST** — HA failover end-to-end

### Sprint 5 — Hardening (4-6 settimane)

14. **IMPL** — KUnit test suite (lock, heartbeat, journal, extent)
15. **IMPL** — SCSI PR fix per BUG-003 su hardware FC reale
16. **RUN** — Fault injection (dm-flakey, node kill durante I/O)
17. **RUN** — Carico sostenuto 72h con 4+ nodi
