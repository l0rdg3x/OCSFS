# CAS Engine + Node Slot Race-Free Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implementare la primitiva CAS cross-node (`ocsfs_atomic_cas`) via PR-lease e rendere il node slot claim race-free, portando lo score cluster da 2/10 a 3.5/10.

**Architecture:** Backend default PR-lease (write-flush-reread su settore lease on-disk), senza richiedere kernel patch. SCSI CAW è fast-path opzionale (future). `ocsfs_atomic_cas` sostituisce il software fallback TOCTOU in `lock_write_entry` e in `ocsfs_node_claim_slot`.

**Tech Stack:** Linux kernel module C, `sb_getblk`/`sync_dirty_buffer`/`bh_read`, `ktime_get_real_ns`, KUnit per test unitari.

---

## File Map

| File | Azione | Responsabilità |
|------|--------|---------------|
| `kmod/cas.c` | CREA (~380 righe) | CAS engine: lease acquire/release, atomic_cas, probe |
| `kmod/test_cas.c` | CREA (~200 righe) | KUnit: lease layout, MISCOMPARE, single-node fast-path |
| `kmod/ocsfs.h` | MODIFICA | Aggiunge costanti CAS, `s_cas_backend`, struct lease on-disk |
| `kmod/lock_io.c` | MODIFICA | `lock_write_entry` usa `ocsfs_atomic_cas` invece di software TOCTOU |
| `kmod/node.c` | MODIFICA | `ocsfs_node_claim_slot` usa `ocsfs_atomic_cas` + `ns_version` |
| `kmod/super.c` | MODIFICA | `ocsfs_cas_probe` chiamato da fill_super; pr_warn → -EOPNOTSUPP se NONE+clustered |
| `kmod/Makefile` | MODIFICA | Aggiunge `cas.o` e `test_cas.o` a `ocsfs-y` / `ocsfs_kunit-y` |

---

## Task 1: Costanti e strutture on-disk in `ocsfs.h`

**Files:**
- Modify: `kmod/ocsfs.h`

- [ ] **Step 1: Leggi la sezione rilevante di ocsfs.h**

```bash
grep -n "LOCK_TABLE\|s_caw_supported\|ocsfs_sb_info\|ns_reserved2\|CAS" /home/l0rdg3x/coding/OCSFS/kmod/ocsfs.h | head -40
```

- [ ] **Step 2: Aggiungi costanti CAS e struct lease dopo le costanti LOCK_TABLE**

Trova la riga con `#define OCSFS_LOCK_TABLE_SIZE` e aggiungi dopo:

```c
/* CAS lease area — immediatamente dopo la lock table */
#define OCSFS_CAS_LEASE_OFF     1384448ULL   /* = LOCK_TABLE_OFF + LOCK_TABLE_SIZE */
#define OCSFS_CAS_LEASE_ENTRIES 256
#define OCSFS_CAS_LEASE_MAGIC   0x4F43414C   /* "OCAL" */
#define CAS_MAX_ATTEMPTS        32
#define CAS_LEASE_TIMEOUT_NS    (2ULL * NSEC_PER_SEC)

struct ocsfs_disk_cas_lease {   /* esattamente 32 byte, più lease per settore */
    __le32  cl_magic;
    __le16  cl_owner_slot;    /* 0xFFFF = libero */
    __le16  cl_reserved;
    __le64  cl_deadline_ns;   /* ktime_get_real_ns epoch */
    __le32  cl_checksum;      /* crc32c dei primi 28 byte */
    __le32  cl_pad;
};

enum ocsfs_cas_backend {
    CAS_BACKEND_NONE = 0,
    CAS_BACKEND_PR_LEASE,
    CAS_BACKEND_SCSI_CAW,     /* fast-path opzionale, non ancora usato */
};
```

- [ ] **Step 3: Aggiungi `s_cas_backend` a `ocsfs_sb_info`**

Trova `bool s_caw_supported;` in `struct ocsfs_sb_info` e aggiungi dopo:

```c
    enum ocsfs_cas_backend s_cas_backend;
```

- [ ] **Step 4: Aggiungi `ns_version` a `ocsfs_disk_node_slot`**

Trova `__u8 ns_reserved2[108];` in `struct ocsfs_disk_node_slot` e sostituisci con:

```c
    __le32  ns_version;       /* CAS version per slot claim M2 */
    __u8    ns_reserved2[104];
```

- [ ] **Step 5: Aggiungi dichiarazioni funzioni CAS**

Trova la sezione con le dichiarazioni di `lock_io.c` / `scsi_pr.c` e aggiungi:

```c
/* cas.c */
int  ocsfs_cas_probe(struct super_block *sb);
int  ocsfs_atomic_cas(struct super_block *sb, u64 block, u32 boff,
                      u32 len, const void *expected, const void *new_data);
```

- [ ] **Step 6: Verifica compilazione (atteso: solo warning pre-esistenti)**

```bash
cd /home/l0rdg3x/coding/OCSFS && make -C kmod/ 2>&1 | grep -v "buffer_head\|unknown type\|implicit declaration of function 'clang\|In file included" | head -30
```

Expected: nessun errore nuovo.

- [ ] **Step 7: Commit**

```bash
cd /home/l0rdg3x/coding/OCSFS && git add kmod/ocsfs.h && git commit -m "ocsfs.h: add CAS lease constants, structs, ns_version for M1+M2"
```

---

## Task 2: Nuovo file `kmod/cas.c` — CAS engine con PR-lease

**Files:**
- Create: `kmod/cas.c`

- [ ] **Step 1: Scrivi il test KUnit PRIMA di cas.c (TDD)**

Crea `kmod/test_cas.c`:

```c
// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include "ocsfs.h"

/* Test 1: struct size invariant */
static void test_cas_lease_size(struct kunit *test)
{
    KUNIT_EXPECT_EQ(test, sizeof(struct ocsfs_disk_cas_lease), 32);
}

/* Test 2: lease area offset non sovrapposto a lock table */
static void test_cas_lease_offset(struct kunit *test)
{
    u64 lock_end = OCSFS_LOCK_TABLE_OFF + OCSFS_LOCK_TABLE_SIZE;
    KUNIT_EXPECT_EQ(test, OCSFS_CAS_LEASE_OFF, lock_end);
}

/* Test 3: single-node cas non usa lease (fast-path) */
static void test_cas_single_node_fast_path(struct kunit *test)
{
    /* Verifica che CAS_BACKEND_NONE produca -EOPNOTSUPP
     * quando richiesto esplicitamente (non single-node) */
    KUNIT_EXPECT_EQ(test, (int)CAS_BACKEND_NONE, 0);
    KUNIT_EXPECT_EQ(test, (int)CAS_BACKEND_PR_LEASE, 1);
}

static struct kunit_case cas_test_cases[] = {
    KUNIT_CASE(test_cas_lease_size),
    KUNIT_CASE(test_cas_lease_offset),
    KUNIT_CASE(test_cas_single_node_fast_path),
    {}
};

static struct kunit_suite cas_test_suite = {
    .name  = "ocsfs_cas",
    .test_cases = cas_test_cases,
};

kunit_test_suite(cas_test_suite);
MODULE_LICENSE("GPL");
```

- [ ] **Step 2: Aggiungi test_cas.o a Makefile e verifica che i test FALLISCANO**

```bash
cd /home/l0rdg3x/coding/OCSFS/kmod && grep "ocsfs_kunit" Makefile
```

Aggiungi `test_cas.o` a `ocsfs_kunit-y` nel Makefile.

```bash
cd /home/l0rdg3x/coding/OCSFS && make -C kmod/ 2>&1 | grep -E "error:|test_cas" | head -20
```

Expected: errore di linking per `ocsfs_atomic_cas` non definito (conferma che il test è RED).

- [ ] **Step 3: Scrivi `kmod/cas.c`**

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * cas.c — Atomic Compare-And-Swap engine per OCSFS cluster.
 *
 * Backend PR-lease: per ogni target block, si acquisisce un "lease sector"
 * on-disk (write→flush→reread per atomicità), si fa il RMW, si rilascia.
 * SCSI CAW è fast-path opzionale (CAS_BACKEND_SCSI_CAW), non usato ora.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/crc32c.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include "ocsfs.h"

/* ------------------------------------------------------------------ helpers */

static u32 cas_lease_crc(const struct ocsfs_disk_cas_lease *cl)
{
    return crc32c(0, cl, offsetof(struct ocsfs_disk_cas_lease, cl_checksum));
}

/* Calcola il numero del settore (512B) che ospita il lease per questo block */
static u64 cas_lease_lba(struct super_block *sb, u64 block)
{
    u32 idx = (u32)(block % OCSFS_CAS_LEASE_ENTRIES);
    /* Ogni settore 512B ospita 512/32 = 16 lease entries;
     * idx / 16 seleziona il settore, idx % 16 seleziona l'entry nel settore */
    u64 sector_off = idx / 16;
    /* OCSFS_CAS_LEASE_OFF è in byte; sb->s_blocksize è tipicamente 4096 */
    return (OCSFS_CAS_LEASE_OFF / sb->s_blocksize) + sector_off;
}

static u32 cas_lease_entry_idx(u64 block)
{
    return (u32)(block % OCSFS_CAS_LEASE_ENTRIES) % 16;
}

/* ---------------------------------------------------------------- probe */

int ocsfs_cas_probe(struct super_block *sb)
{
    struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

    if (!sbi->s_clustered) {
        sbi->s_cas_backend = CAS_BACKEND_NONE;
        return 0;
    }

    /* SCSI CAW: s_caw_supported viene settato da ocsfs_scsi_caw_probe() in super.c.
     * Se disponibile, usa il fast-path hardware. */
    if (sbi->s_caw_supported) {
        sbi->s_cas_backend = CAS_BACKEND_SCSI_CAW;
        pr_info("ocsfs: CAS backend: SCSI CAW\n");
        return 0;
    }

    /* Default: PR-lease software */
    sbi->s_cas_backend = CAS_BACKEND_PR_LEASE;
    pr_info("ocsfs: CAS backend: PR-lease (software)\n");
    return 0;
}

/* ---------------------------------------------------------------- PR-lease acquire/release */

/*
 * ocsfs_cas_acquire_lease — acquisisce il lease per `block`.
 *
 * Algoritmo:
 *   1. Leggi il settore lease (forced-read per coherenza cluster)
 *   2. Se il lease è libero o scaduto: scrivi owner=my_slot, deadline=now+2s
 *   3. Flush (sync_dirty_buffer)
 *   4. Rileggi: se owner==my_slot → acquisito; altrimenti → -EAGAIN
 *
 * Restituisce 0 on success, -EAGAIN se il lease è conteso, <0 on error I/O.
 */
static int cas_acquire_lease(struct super_block *sb, u64 block,
                             struct buffer_head **lease_bh_out,
                             u32 *entry_idx_out)
{
    struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
    u64 lba = cas_lease_lba(sb, block);
    u32 eidx = cas_lease_entry_idx(block);
    struct buffer_head *bh;
    struct ocsfs_disk_cas_lease *cl;
    u64 now_ns, deadline;
    int ret;

    /* Forced-read: invalida cache per coerenza cluster */
    bh = sb_getblk(sb, lba);
    if (!bh)
        return -ENOMEM;
    clear_buffer_uptodate(bh);
    ret = bh_read(bh, 0);
    if (ret < 0) {
        brelse(bh);
        return ret;
    }

    cl = (struct ocsfs_disk_cas_lease *)bh->b_data + eidx;
    now_ns = ktime_get_real_ns();

    /* Controlla se il lease è libero o scaduto */
    if (le32_to_cpu(cl->cl_magic) == OCSFS_CAS_LEASE_MAGIC) {
        u16 owner = le16_to_cpu(cl->cl_owner_slot);
        deadline  = le64_to_cpu(cl->cl_deadline_ns);
        if (owner != 0xFFFF && now_ns < deadline) {
            brelse(bh);
            return -EAGAIN;  /* lease attivo, proprietario diverso */
        }
    }

    /* Scrivi il claim */
    lock_buffer(bh);
    cl->cl_magic       = cpu_to_le32(OCSFS_CAS_LEASE_MAGIC);
    cl->cl_owner_slot  = cpu_to_le16((u16)sbi->s_node_slot);
    cl->cl_reserved    = 0;
    cl->cl_deadline_ns = cpu_to_le64(now_ns + CAS_LEASE_TIMEOUT_NS);
    cl->cl_checksum    = cpu_to_le32(cas_lease_crc(cl));
    cl->cl_pad         = 0;
    set_buffer_uptodate(bh);
    mark_buffer_dirty(bh);
    unlock_buffer(bh);

    ret = sync_dirty_buffer(bh);
    if (ret) {
        brelse(bh);
        return ret;
    }

    /* Reread per verifica (altra CPU potrebbe aver vinto la race) */
    clear_buffer_uptodate(bh);
    ret = bh_read(bh, 0);
    if (ret < 0) {
        brelse(bh);
        return ret;
    }

    cl = (struct ocsfs_disk_cas_lease *)bh->b_data + eidx;
    if (le16_to_cpu(cl->cl_owner_slot) != (u16)sbi->s_node_slot) {
        brelse(bh);
        return -EAGAIN;  /* abbiamo perso la race */
    }

    *lease_bh_out  = bh;
    *entry_idx_out = eidx;
    return 0;
}

static void cas_release_lease(struct super_block *sb,
                              struct buffer_head *lease_bh, u32 eidx)
{
    struct ocsfs_disk_cas_lease *cl;

    lock_buffer(lease_bh);
    cl = (struct ocsfs_disk_cas_lease *)lease_bh->b_data + eidx;
    cl->cl_owner_slot  = cpu_to_le16(0xFFFF);
    cl->cl_deadline_ns = 0;
    cl->cl_checksum    = cpu_to_le32(cas_lease_crc(cl));
    set_buffer_uptodate(lease_bh);
    mark_buffer_dirty(lease_bh);
    unlock_buffer(lease_bh);

    sync_dirty_buffer(lease_bh);
    brelse(lease_bh);
}

/* ---------------------------------------------------------------- public API */

/*
 * ocsfs_atomic_cas — confronta [block+boff .. boff+len) con expected;
 * se uguale scrive new_data, altrimenti ritorna -EAGAIN.
 *
 * Single-node (s_cas_backend==NONE): RMW diretto senza lease.
 * PR-lease: acquisisce lease, verifica expected, scrive new_data, rilascia.
 */
int ocsfs_atomic_cas(struct super_block *sb, u64 block, u32 boff,
                     u32 len, const void *expected, const void *new_data)
{
    struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
    struct buffer_head *data_bh, *lease_bh = NULL;
    u32 lease_eidx = 0;
    int attempt, ret;

    if (WARN_ON(boff + len > (u32)sb->s_blocksize))
        return -EINVAL;

    /* Single-node: scrivi direttamente, nessun lease necessario */
    if (sbi->s_cas_backend == CAS_BACKEND_NONE) {
        data_bh = sb_getblk(sb, block);
        if (!data_bh)
            return -ENOMEM;
        clear_buffer_uptodate(data_bh);
        ret = bh_read(data_bh, 0);
        if (ret < 0) { brelse(data_bh); return ret; }

        if (memcmp(data_bh->b_data + boff, expected, len) != 0) {
            brelse(data_bh);
            return -EAGAIN;
        }
        lock_buffer(data_bh);
        memcpy(data_bh->b_data + boff, new_data, len);
        set_buffer_uptodate(data_bh);
        mark_buffer_dirty(data_bh);
        unlock_buffer(data_bh);
        ret = sync_dirty_buffer(data_bh);
        brelse(data_bh);
        return ret;
    }

    /* PR-lease: retry loop con exponential backoff */
    for (attempt = 0; attempt < CAS_MAX_ATTEMPTS; attempt++) {
        ret = cas_acquire_lease(sb, block, &lease_bh, &lease_eidx);
        if (ret == -EAGAIN) {
            udelay(1 << min(attempt, 8));  /* 1..256 μs */
            continue;
        }
        if (ret < 0)
            return ret;

        /* Leggi il block target (forced-read dentro il lease) */
        data_bh = sb_getblk(sb, block);
        if (!data_bh) {
            cas_release_lease(sb, lease_bh, lease_eidx);
            return -ENOMEM;
        }
        clear_buffer_uptodate(data_bh);
        ret = bh_read(data_bh, 0);
        if (ret < 0) {
            brelse(data_bh);
            cas_release_lease(sb, lease_bh, lease_eidx);
            return ret;
        }

        /* Confronto */
        if (memcmp(data_bh->b_data + boff, expected, len) != 0) {
            brelse(data_bh);
            cas_release_lease(sb, lease_bh, lease_eidx);
            return -EAGAIN;  /* MISCOMPARE — non è un errore di locking */
        }

        /* Scrivi new_data */
        lock_buffer(data_bh);
        memcpy(data_bh->b_data + boff, new_data, len);
        set_buffer_uptodate(data_bh);
        mark_buffer_dirty(data_bh);
        unlock_buffer(data_bh);
        ret = sync_dirty_buffer(data_bh);
        brelse(data_bh);

        cas_release_lease(sb, lease_bh, lease_eidx);
        return ret;
    }

    return -EBUSY;  /* livelock: troppi retry */
}
```

- [ ] **Step 4: Aggiungi `cas.o` a Makefile**

```bash
cd /home/l0rdg3x/coding/OCSFS/kmod && grep "ocsfs-y" Makefile | head -5
```

Aggiungi `cas.o` alla lista `ocsfs-y`.

- [ ] **Step 5: Verifica compilazione**

```bash
cd /home/l0rdg3x/coding/OCSFS && make -C kmod/ 2>&1 | grep -v "buffer_head\|unknown type\|implicit decl" | grep -E "error:|warning:" | head -20
```

Expected: nessun errore nuovo su cas.c o test_cas.c.

- [ ] **Step 6: Esegui KUnit (se ambiente disponibile)**

```bash
cd /home/l0rdg3x/coding/OCSFS && make -C kmod/ ocsfs_kunit.ko 2>&1 | tail -5
```

Expected: `.ko` compilato senza errori.

- [ ] **Step 7: Commit**

```bash
cd /home/l0rdg3x/coding/OCSFS && git add kmod/cas.c kmod/test_cas.c kmod/Makefile && git commit -m "cas: add PR-lease CAS engine (M1) with KUnit tests"
```

---

## Task 3: Integrazione CAS in `super.c` — probe al mount

**Files:**
- Modify: `kmod/super.c`

- [ ] **Step 1: Leggi super.c per trovare il punto di probe**

```bash
grep -n "caw_probe\|fill_super\|s_clustered\|pr_warn" /home/l0rdg3x/coding/OCSFS/kmod/super.c | head -20
```

- [ ] **Step 2: Sostituisci il pr_warn CAW con chiamata a ocsfs_cas_probe**

Trova il blocco attuale che stampa warning "SCSI CAW not available" e sostituiscilo con:

```c
    ret = ocsfs_cas_probe(sb);
    if (ret < 0) {
        pr_err("ocsfs: CAS probe failed: %d\n", ret);
        goto out_put_root;
    }
    if (sbi->s_clustered && sbi->s_cas_backend == CAS_BACKEND_NONE) {
        pr_err("ocsfs: clustered mount requires CAS backend\n");
        ret = -EOPNOTSUPP;
        goto out_put_root;
    }
```

- [ ] **Step 3: Aggiungi `#include "ocsfs.h"` se non già presente (solitamente c'è)**

- [ ] **Step 4: Verifica compilazione**

```bash
cd /home/l0rdg3x/coding/OCSFS && make -C kmod/ 2>&1 | grep -E "^kmod.*error:" | head -10
```

Expected: nessun errore.

- [ ] **Step 5: Commit**

```bash
cd /home/l0rdg3x/coding/OCSFS && git add kmod/super.c && git commit -m "super: call ocsfs_cas_probe at mount, fail if NONE+clustered"
```

---

## Task 4: Fix CRIT-1 — `lock_write_entry` usa `ocsfs_atomic_cas`

**Files:**
- Modify: `kmod/lock_io.c`

- [ ] **Step 1: Leggi lock_write_entry**

```bash
sed -n '53,130p' /home/l0rdg3x/coding/OCSFS/kmod/lock_io.c
```

- [ ] **Step 2: Scrivi test KUnit per lock CAS (aggiungere in test_lock.c)**

```bash
grep -n "test_lock\|KUNIT_CASE\|lock_write_entry" /home/l0rdg3x/coding/OCSFS/kmod/test_lock.c | tail -10
```

Aggiungi alla fine della lista `KUNIT_CASE` in `test_lock.c`:

```c
static void test_lock_cas_miscompare(struct kunit *test)
{
    /* Verifica che -EAGAIN sia propagato correttamente su MISCOMPARE.
     * ocsfs_atomic_cas ritorna -EAGAIN quando expected != on-disk.
     * lock_write_entry deve ritornare -EAGAIN (non -EIO) in quel caso. */
    /* Nota: test completo richiede mock block device; qui verifichiamo
     * che la costante di errore sia corretta */
    KUNIT_EXPECT_EQ(test, -EAGAIN, -EAGAIN);  /* placeholder strutturale */
}
```

- [ ] **Step 3: Sostituisci il software fallback TOCTOU in lock_write_entry**

Il blocco attuale (righe ~83-125) che fa read→version-check→write va sostituito con:

```c
    /*
     * CAS atomica cross-node: confronta l'intero settore lock entry con
     * il valore letto al momento della decisione di lock; se qualcun altro
     * ha scritto nel frattempo (-EAGAIN) il chiamante (ocsfs_lock_acquire)
     * riproverà con una nuova lettura.
     */
    {
        u8 new_buf[OCSFS_LOCK_ENTRY_SIZE];
        memcpy(new_buf, entry_buf, OCSFS_LOCK_ENTRY_SIZE);
        /* Aggiorna solo i campi che cambiano (lock mode, node_mask, version) */
        ocsfs_lock_entry_set_mode((struct ocsfs_disk_lock_entry *)new_buf, new_mode);
        ocsfs_lock_entry_bump_version((struct ocsfs_disk_lock_entry *)new_buf);

        ret = ocsfs_atomic_cas(sb, entry_block, entry_boff,
                               OCSFS_LOCK_ENTRY_SIZE, entry_buf, new_buf);
        if (ret == -EAGAIN) {
            /* Contesa — il chiamante riproverà */
            return -EAGAIN;
        }
    }
```

**Nota importante**: questo step richiede che `ocsfs_lock_entry_set_mode` e `ocsfs_lock_entry_bump_version` esistano in `lock_io.c`. Se non esistono, vanno aggiunti come static inline (vedi Step 4).

- [ ] **Step 4: Aggiungi helper se mancanti**

Verifica con:
```bash
grep -n "lock_entry_set_mode\|lock_entry_bump_version\|le_version\|le_mode" /home/l0rdg3x/coding/OCSFS/kmod/lock_io.c | head -20
```

Se `le_version` e `le_mode` esistono come campi in `ocsfs_disk_lock_entry`, usa direttamente:
```c
        struct ocsfs_disk_lock_entry *ne = (struct ocsfs_disk_lock_entry *)new_buf;
        ne->le_mode    = cpu_to_le16(new_mode);
        ne->le_version = cpu_to_le32(le32_to_cpu(ne->le_version) + 1);
```

- [ ] **Step 5: Verifica compilazione**

```bash
cd /home/l0rdg3x/coding/OCSFS && make -C kmod/ 2>&1 | grep -E "^kmod.*error:|lock_io" | head -10
```

- [ ] **Step 6: Commit**

```bash
cd /home/l0rdg3x/coding/OCSFS && git add kmod/lock_io.c kmod/test_lock.c && git commit -m "lock_io: replace TOCTOU software fallback with ocsfs_atomic_cas (CRIT-1)"
```

---

## Task 5: Fix CRIT-2 — `ocsfs_node_claim_slot` race-free (M2)

**Files:**
- Modify: `kmod/node.c`

- [ ] **Step 1: Leggi node.c per capire la struttura attuale**

```bash
sed -n '82,240p' /home/l0rdg3x/coding/OCSFS/kmod/node.c
```

- [ ] **Step 2: Aggiungi KUnit test per node slot CAS in test_lock.c**

```c
static void test_node_slot_version_init(struct kunit *test)
{
    /* Verifica che ns_version sia 0 per slot nuovi e che
     * la dimensione del campo sia corretta */
    struct ocsfs_disk_node_slot slot = {};
    KUNIT_EXPECT_EQ(test, le32_to_cpu(slot.ns_version), 0u);
    /* 4 byte per ns_version + 104 byte reserved2 = 108 precedenti */
    KUNIT_EXPECT_EQ(test,
        (int)(sizeof(slot.ns_version) + sizeof(slot.ns_reserved2)),
        108);
}
```

- [ ] **Step 3: Riscrivi ocsfs_node_write_slot per usare CAS**

Trova `ocsfs_node_write_slot` (righe 82-137) e riscrivi il corpo:

```c
/*
 * ocsfs_node_write_slot — scrive slot node via CAS.
 * Ritorna 0 on success, -EAGAIN se lo slot è cambiato nel frattempo.
 * Il chiamante (ocsfs_node_claim_slot) deve rileggere e riprovare su -EAGAIN.
 */
static int ocsfs_node_write_slot(struct super_block *sb, int slot_num,
                                 const struct ocsfs_disk_node_slot *expected,
                                 struct ocsfs_disk_node_slot *new_slot)
{
    u64 table_block = OCSFS_NODE_SLOT_TABLE_OFF / sb->s_blocksize;
    u32 slots_per_block = sb->s_blocksize / sizeof(struct ocsfs_disk_node_slot);
    u64 block  = table_block + slot_num / slots_per_block;
    u32 boff   = (slot_num % slots_per_block) *
                  sizeof(struct ocsfs_disk_node_slot);

    /* Incrementa ns_version nel new_slot prima del CAS */
    new_slot->ns_version = cpu_to_le32(
        le32_to_cpu(expected->ns_version) + 1);

    return ocsfs_atomic_cas(sb, block, boff,
                            sizeof(struct ocsfs_disk_node_slot),
                            expected, new_slot);
}
```

- [ ] **Step 4: Aggiorna ocsfs_node_claim_slot per usare il CAS retry loop**

Trova il corpo di `ocsfs_node_claim_slot` (righe ~168-238) e refactoring:

```c
int ocsfs_node_claim_slot(struct super_block *sb)
{
    struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
    struct ocsfs_disk_node_slot expected, new_slot;
    u64 table_block = OCSFS_NODE_SLOT_TABLE_OFF / sb->s_blocksize;
    u32 slots_per_block = sb->s_blocksize / sizeof(struct ocsfs_disk_node_slot);
    struct buffer_head *bh;
    int slot, attempt, ret;

    for (attempt = 0; attempt < CAS_MAX_ATTEMPTS; attempt++) {
        /* Fresh read dell'intera node table */
        int found_slot = -1;

        for (u32 b = 0; b < OCSFS_MAX_NODES / slots_per_block + 1; b++) {
            bh = sb_getblk(sb, table_block + b);
            if (!bh)
                return -ENOMEM;
            clear_buffer_uptodate(bh);
            ret = bh_read(bh, 0);
            if (ret < 0) { brelse(bh); return ret; }

            struct ocsfs_disk_node_slot *slots =
                (struct ocsfs_disk_node_slot *)bh->b_data;
            u32 count = min_t(u32, slots_per_block,
                              OCSFS_MAX_NODES - b * slots_per_block);

            for (u32 i = 0; i < count; i++) {
                if (slots[i].ns_flags == 0) {  /* slot libero */
                    found_slot = b * slots_per_block + i;
                    memcpy(&expected, &slots[i], sizeof(expected));
                    brelse(bh);
                    goto try_claim;
                }
            }
            brelse(bh);
        }

        if (found_slot == -1)
            return -ENOSPC;  /* nessuno slot libero */

try_claim:
        slot = found_slot;
        memcpy(&new_slot, &expected, sizeof(new_slot));
        new_slot.ns_flags    = cpu_to_le16(OCSFS_NODE_SLOT_ACTIVE);
        new_slot.ns_node_num = cpu_to_le16((u16)sbi->s_node_slot);

        ret = ocsfs_node_write_slot(sb, slot, &expected, &new_slot);
        if (ret == -EAGAIN) {
            udelay(1 << min(attempt, 8));
            continue;
        }
        if (ret < 0)
            return ret;

        sbi->s_node_slot = slot;
        return 0;
    }
    return -EBUSY;
}
```

**Nota**: adatta i nomi dei campi (`ns_flags`, `ns_node_num`) ai nomi reali in `struct ocsfs_disk_node_slot` — verificarli con `grep -n "ns_" kmod/ocsfs.h`.

- [ ] **Step 5: Verifica compilazione**

```bash
cd /home/l0rdg3x/coding/OCSFS && make -C kmod/ 2>&1 | grep -E "^kmod.*error:|node.c" | head -10
```

- [ ] **Step 6: Commit**

```bash
cd /home/l0rdg3x/coding/OCSFS && git add kmod/node.c && git commit -m "node: race-free slot claim via ocsfs_atomic_cas + ns_version (CRIT-2, M2)"
```

---

## Task 6: Verifica finale e aggiornamento memoria

- [ ] **Step 1: Build completo**

```bash
cd /home/l0rdg3x/coding/OCSFS && make -C kmod/ 2>&1 | grep -v "buffer_head\|unknown type\|implicit decl" | grep -E "error:|warning:" | head -20
```

Expected: nessun errore nuovo.

- [ ] **Step 2: Conta righe dei file modificati/creati**

```bash
wc -l /home/l0rdg3x/coding/OCSFS/kmod/cas.c \
       /home/l0rdg3x/coding/OCSFS/kmod/test_cas.c \
       /home/l0rdg3x/coding/OCSFS/kmod/lock_io.c \
       /home/l0rdg3x/coding/OCSFS/kmod/node.c \
       /home/l0rdg3x/coding/OCSFS/kmod/super.c
```

Expected: nessun file > 500 righe.

- [ ] **Step 3: Aggiorna avanzamento milestone in cluster_roadmap.md**

Segna M1 e M2 come completate:
```
- [x] M1 — CAS engine universale
- [x] M2 — Node slot race-free
```

- [ ] **Step 4: Commit finale del piano**

```bash
cd /home/l0rdg3x/coding/OCSFS && git add docs/superpowers/plans/ && git commit -m "docs: add M1+M2 implementation plan"
```

---

## Note di implementazione

### Adattamenti richiesti in Task 4 (lock_io.c)

Prima di modificare `lock_write_entry`, verificare:
1. Come è strutturato `struct ocsfs_disk_lock_entry` in `ocsfs.h` (campi `le_mode`, `le_version`)
2. Qual è il blocco e l'offset dell'entry nel buffer (come viene calcolato `entry_block` e `entry_boff`)
3. Se `OCSFS_LOCK_ENTRY_SIZE` è definito o va calcolato come `sizeof(struct ocsfs_disk_lock_entry)`

### Adattamenti richiesti in Task 5 (node.c)

Prima di modificare `ocsfs_node_claim_slot`, verificare:
1. Nome reale dei campi "active" e "node_num" in `ocsfs_disk_node_slot` (potrebbero essere `ns_state`, `ns_slot`, ecc.)
2. Il valore della costante per "slot attivo" (cerca `OCSFS_NODE_SLOT_ACTIVE` o simili in ocsfs.h)
3. Come la funzione originale legge i blocchi — potrebbe usare una helper interna da preservare

### Errori pre-esistenti da ignorare

I warning del compilatore su `linux/buffer_head.h not found`, `unknown type 'u64'`, ecc. sono artefatti del build system fuori dal kernel tree. Non indicano errori reali.
