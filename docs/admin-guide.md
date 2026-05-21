# OCSFS — Guida Amministratore

**Versione:** 0.1 — Maggio 2026  
**Piattaforma:** Proxmox VE 8.x, Linux kernel 6.6+ LTS  
**Stato:** Alpha — non per produzione

## Indice

1. [Prerequisiti](#1-prerequisiti)
2. [Installazione](#2-installazione)
3. [Formattazione del LUN](#3-formattazione-del-lun)
4. [Configurazione Proxmox VE](#4-configurazione-proxmox-ve)
5. [Mount e operazioni base](#5-mount-e-operazioni-base)
6. [Monitoraggio](#6-monitoraggio)
7. [Operazioni di manutenzione](#7-operazioni-di-manutenzione)
8. [Troubleshooting](#8-troubleshooting)
9. [Limitazioni note](#9-limitazioni-note)

---

## 1. Prerequisiti

### Infrastruttura

- **Storage:** FC SAN LUN (o iSCSI con supporto SCSI-3 PR) condiviso e accessibile da tutti i nodi Proxmox
- **Multipath:** `multipathd` configurato e i device mapper `/dev/mapper/mpath*` visibili su tutti i nodi
- **SCSI PR:** Il target SAN deve supportare Persistent Reservations (tutti gli array enterprise FC supportano PR)
- **Kernel:** 6.6 LTS o superiore
- **Proxmox VE:** 8.0 o superiore

### Verifica compatibilità SCSI PR

```bash
# Verifica che il LUN supporti SCSI PR
sg_persist -i -k /dev/mapper/mpathX
# Output atteso: "PR generation=0x0" (anche se vuoto, indica supporto)
```

### Pacchetti richiesti su ogni nodo

```bash
apt install build-essential dkms linux-headers-$(uname -r) \
            multipath-tools sg3-utils uuid-runtime
```

---

## 2. Installazione

### Opzione A — Pacchetti Debian (raccomandato)

```bash
# Clona il repository
git clone https://github.com/ocsfs/ocsfs /opt/ocsfs
cd /opt/ocsfs

# Costruisci i pacchetti
dpkg-buildpackage -us -uc -b

# Installa su tutti i nodi Proxmox
dpkg -i ../ocsfs-tools_0.1.0-1_amd64.deb \
        ../ocsfs-dkms_0.1.0-1_all.deb \
        ../ocsfs-proxmox_0.1.0-1_all.deb
```

### Opzione B — Installazione manuale

```bash
cd /opt/ocsfs

# Userspace tools
make all
make install   # installa in /usr/local/bin

# Kernel module via DKMS
sudo dkms add kmod/
sudo dkms build ocsfs/0.1.0
sudo dkms install ocsfs/0.1.0

# Plugin Proxmox
sudo proxmox/install.sh
```

### Verifica installazione

```bash
# Il modulo deve caricarsi senza errori
sudo modprobe ocsfs
dmesg | grep ocsfs
# Atteso: "ocsfs: Open Cluster Shared FileSystem v0.1 loaded"

# Tools disponibili
mkfs.ocsfs --version
ocsfs-tool --help
```

---

## 3. Formattazione del LUN

**ATTENZIONE:** `mkfs.ocsfs` distrugge tutti i dati sul device. Esegui solo su LUN dedicati.

```bash
# Identifica il LUN multipath
multipath -ll
# Esempio: mpath0 (3600508b1001c89480e7b8b1d7dc00f2) dm-3 DGC,RAID 5

# Formatta (esegui su UN SOLO nodo)
mkfs.ocsfs \
  -L nome-datastore \    # label del volume (max 64 char)
  -N 16 \                # max nodi concorrenti (default 64, max 256)
  -E 4M \                # extent size (default 1M, range 64K-64M)
  -f \                   # force (sovrascrive dati esistenti)
  -v \                   # verbose
  /dev/mapper/mpath0

# Verifica il risultato
ocsfs-tool info /dev/mapper/mpath0
```

### Parametri mkfs consigliati per Proxmox

| Scenario | max nodi (`-N`) | extent size (`-E`) |
|---|---|---|
| Cluster piccolo (2-4 nodi) | 8 | 1M |
| Cluster medio (5-16 nodi) | 32 | 4M |
| Cluster grande (17-64 nodi) | 64 | 8M |

> **Nota:** `max_nodes` è fisso al formato e non cambiabile dopo. Sovradimensiona leggermente.

---

## 4. Configurazione Proxmox VE

### Aggiunta storage via CLI (da un nodo, si replica automaticamente)

```bash
pvesm add ocsfs fc-shared \
  --path /mnt/pve/fc-shared \
  --device /dev/mapper/mpath0 \
  --content images,iso,vztmpl,backup,rootdir,snippets \
  --maxnodes 16 \
  --thin 1 \
  --shared 1
```

### Configurazione manuale in `/etc/pve/storage.cfg`

```
ocsfs: fc-shared
    path /mnt/pve/fc-shared
    device /dev/mapper/mpath0
    content images,iso,vztmpl,backup,rootdir,snippets
    maxnodes 16
    thin 1
    shared 1
```

### Opzioni configurazione storage

| Opzione | Descrizione | Default |
|---------|-------------|---------|
| `path` | Mount point locale | (richiesto) |
| `device` | Block device (preferire multipath) | (richiesto) |
| `content` | Tipi di contenuto supportati | images |
| `maxnodes` | Max nodi contemporanei | 64 |
| `thin` | Thin provisioning VM disks | 0 |
| `shared` | Storage condiviso tra nodi | 0 |

### Abilitazione su tutti i nodi

Il plugin Proxmox monta automaticamente lo storage su tutti i nodi tramite il servizio `ocsfs-mount@.service`:

```bash
# Verifica che il servizio sia attivo su ogni nodo
systemctl status ocsfs-mount@fc-shared.service

# Mount manuale se necessario
mount -t ocsfs /dev/mapper/mpath0 /mnt/pve/fc-shared
```

---

## 5. Mount e operazioni base

### Mount manuale

```bash
# Carica il modulo
sudo modprobe ocsfs

# Mount
sudo mount -t ocsfs /dev/mapper/mpath0 /mnt/ocsfs

# Con autenticazione cluster (se il volume ha FEAT_AUTH)
sudo mount -t ocsfs -o cluster_secret=<64-hex-chars> \
  /dev/mapper/mpath0 /mnt/ocsfs
```

### Unmount

```bash
# Verifica che nessun processo acceda al filesystem
lsof /mnt/ocsfs

# Unmount
sudo umount /mnt/ocsfs
```

### Mount via `/etc/fstab`

```
/dev/mapper/mpath0  /mnt/ocsfs  ocsfs  defaults,_netdev  0 0
```

---

## 6. Monitoraggio

### Stato cluster

```bash
# Panoramica completa
ocsfs-tool status /mnt/ocsfs

# Esempio output:
# Volume: shared-vm-store (UUID: a1b2c3...)
# Block size: 4096, Extent size: 4194304
# Total: 4.8 TiB, Free: 2.7 TiB, Used: 2.1 TiB
#
# Node 0 [pve1] ACTIVE  HB: 2s ago  Locks: 0
# Node 1 [pve2] ACTIVE  HB: 3s ago  Locks: 5
# Node 2 [pve3] ACTIVE  HB: 1s ago  Locks: 3
```

### Lista nodi attivi

```bash
ocsfs-tool nodes /mnt/ocsfs
```

### Lock table

```bash
# Mostra tutti i lock attivi
ocsfs-tool locks /mnt/ocsfs

# Utile per diagnosticare blocchi o lock orfani
```

### Utilizzo spazio

```bash
ocsfs-tool df /mnt/ocsfs

# Output include:
# - Spazio totale / usato / libero
# - Spazio thin-allocated vs effettivamente scritto
# - Inodes per AG
```

### Log kernel

```bash
# Messaggi OCSFS in tempo reale
dmesg -w | grep ocsfs

# Messaggi di heartbeat e recovery
journalctl -k | grep ocsfs
```

---

## 7. Operazioni di manutenzione

### Defragmentazione online

```bash
# Avvia defrag in background (limitato a 50 MB/s)
ocsfs-defrag /mnt/ocsfs -b 50

# Dry run (solo report, non modifica nulla)
ocsfs-defrag /mnt/ocsfs -n

# Verbose con soglia di frammentazione personalizzata
ocsfs-defrag /mnt/ocsfs -v -t 4 -b 100

# Pausa / Ripresa
kill -USR1 <pid-defrag>   # pausa
kill -USR2 <pid-defrag>   # ripresa
```

Il defrag daemon usa il lock table per garantire che giri su un solo nodo alla volta.

### Verifica integrità (offline)

```bash
# ATTENZIONE: il device NON deve essere montato
umount /mnt/ocsfs

ocsfs-tool check /dev/mapper/mpath0

# Controlla: magic, checksum CRC32C, AG consistency,
#            lock table entries, heartbeat staleness
```

### Recovery manuale di un nodo morto

```bash
# Se il recovery automatico non parte (es. timeout heartbeat troppo corto)
ocsfs-tool recover /mnt/ocsfs --node <slot-number>

# Fencing di emergenza
ocsfs-tool fence /mnt/ocsfs --node <slot-number>
```

### Thin provisioning — recupero spazio

Il recupero avviene automaticamente quando il guest OS esegue TRIM/DISCARD. Per forzarlo:

```bash
# Dal guest Linux
fstrim -v /

# Verifica spazio recuperato
ocsfs-tool df /mnt/ocsfs
```

---

## 8. Troubleshooting

### Il modulo non si carica

```bash
dmesg | grep ocsfs
modinfo ocsfs   # verifica che il modulo sia compilato per il kernel corrente
dkms status     # verifica stato DKMS
```

**Causa comune:** il kernel è stato aggiornato senza ricompilare il modulo. Soluzione:
```bash
dkms autoinstall
```

### Mount fallisce con "bad magic"

```bash
ocsfs-tool info /dev/mapper/mpath0
# Se ritorna errore → il device non è formattato come OCSFS
# Verifica device multipath corretto con: multipath -ll
```

### Mount fallisce con "superblock checksum mismatch"

Il superblock principale è corrotto. Prova il mirror:
```bash
# Internamente ocsfs tenta il mirror automaticamente
# Se fallisce entrambi, il volume richiede fsck offline:
ocsfs-tool check /dev/mapper/mpath0
```

### Nodo non si unisce al cluster (heartbeat timeout)

```bash
# Verifica che il device multipath sia accessibile in lettura/scrittura
dd if=/dev/mapper/mpath0 of=/dev/null bs=4096 count=100

# Verifica che il path di storage non sia saturato
iostat -x 1 5 dm-3

# Aumenta temporaneamente il timeout (richiede rimount)
# mount -t ocsfs -o heartbeat_timeout=30000 ...
```

### Lock timeout su operazione

```bash
# Identifica chi tiene il lock
ocsfs-tool locks /mnt/ocsfs

# Se il holder node è DEAD ma non recuperato:
ocsfs-tool recover /mnt/ocsfs --node <slot>
```

### SCSI PR non supportato (loop, iSCSI economico)

OCSFS rileva automaticamente se il device non supporta PR e opera in **single-node mode** (nessun locking on-disk attivo). I log mostrano:

```
ocsfs: PR not supported by device, skipping
```

In questo caso il mount su più nodi contemporaneamente è **non sicuro**.

---

## 9. Limitazioni note

| Limitazione | Impatto | Workaround |
|------------|---------|------------|
| Single-node mode su device senza PR | No protezione multi-nodo | Usa solo FC SAN con PR |
| Directory con >~1000 file lente | Performance degradata | Evita directory molto grandi in questa versione |
| File con >16 extent non supportati nel kmod | File molto frammentati possono non essere accessibili | Esegui defrag regolarmente |
| Recovery di un solo nodo fallito per volta | Se due nodi muoiono insieme, il secondo non viene recuperato | Contatta il team per recovery manuale |
| Nessun fsck offline completo | Errori di consistenza non riparabili automaticamente | `ocsfs-tool check` per diagnosi |

> **Stato Alpha:** OCSFS è in fase di sviluppo attivo. Non usare per dati di produzione critici senza un piano di backup e test approfonditi nel proprio ambiente.
