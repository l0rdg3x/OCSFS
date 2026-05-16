/*
 * mkfs.ocsfs — Format a block device with the OCSFS filesystem.
 *
 * Usage: mkfs.ocsfs [options] <device>
 *
 * Options:
 *   -L <label>      Volume label (max 64 chars)
 *   -N <max_nodes>  Maximum cluster nodes (default 64, max 256)
 *   -b <block_size> Block size in bytes (default 4096)
 *   -E <extent_sz>  Extent size (e.g., 1M, 4M, default 1M)
 *   -A <ag_size>    Allocation Group size (e.g., 1G, default 1G)
 *   -J <jnl_size>   Per-node journal size (e.g., 32M, default 32M)
 *   -f              Force (skip confirmation)
 *   -T              Enable thin provisioning
 *   -v              Verbose output
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <uuid/uuid.h>
#include <getopt.h>

#include "ocsfs.h"

/* ─── Globals ───────────────────────────────────────────────── */

static struct mkfs_config {
    const char *device_path;
    char        label[OCSFS_MAX_LABEL];
    uint16_t    max_nodes;
    uint32_t    block_size;
    uint32_t    extent_size;
    uint64_t    ag_size;
    uint32_t    journal_size;
    uint64_t    features;
    int         force;
    int         verbose;
} cfg = {
    .max_nodes    = OCSFS_DEFAULT_MAX_NODES,
    .block_size   = OCSFS_DEFAULT_BLOCK_SIZE,
    .extent_size  = OCSFS_DEFAULT_EXTENT_SIZE,
    .ag_size      = OCSFS_DEFAULT_AG_SIZE / OCSFS_DEFAULT_BLOCK_SIZE,
    .journal_size = OCSFS_DEFAULT_JOURNAL_SIZE,
    .features     = OCSFS_FEAT_THIN_PROV,
};

/* ─── Utility ───────────────────────────────────────────────── */

static uint64_t parse_size(const char *str)
{
    char *end;
    double val = strtod(str, &end);
    switch (*end) {
    case 'k': case 'K': val *= 1024; break;
    case 'm': case 'M': val *= 1024 * 1024; break;
    case 'g': case 'G': val *= 1024 * 1024 * 1024; break;
    case 't': case 'T': val *= 1024ULL * 1024 * 1024 * 1024; break;
    case '\0': break;
    default:
        fprintf(stderr, "mkfs.ocsfs: invalid size suffix '%c'\n", *end);
        exit(1);
    }
    return (uint64_t)val;
}

static uint64_t get_device_size(int fd)
{
    uint64_t size = 0;
    if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
        /* Fallback: try lseek */
        off_t end = lseek(fd, 0, SEEK_END);
        if (end < 0) {
            perror("mkfs.ocsfs: cannot determine device size");
            exit(1);
        }
        size = (uint64_t)end;
        lseek(fd, 0, SEEK_SET);
    }
    return size;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void write_at(int fd, uint64_t offset, const void *buf, size_t len)
{
    if (pwrite(fd, buf, len, offset) != (ssize_t)len) {
        fprintf(stderr, "mkfs.ocsfs: write error at offset %lu: %s\n",
                (unsigned long)offset, strerror(errno));
        exit(1);
    }
}

static void zero_region(int fd, uint64_t offset, uint64_t length)
{
    /* Use a 1 MB zero buffer for speed */
    size_t bufsz = (length < (1 << 20)) ? (size_t)length : (1 << 20);
    void *zbuf = calloc(1, bufsz);
    if (!zbuf) {
        perror("mkfs.ocsfs: calloc");
        exit(1);
    }
    uint64_t remaining = length;
    while (remaining > 0) {
        size_t chunk = (remaining < bufsz) ? (size_t)remaining : bufsz;
        write_at(fd, offset, zbuf, chunk);
        offset += chunk;
        remaining -= chunk;
    }
    free(zbuf);
}

/* ─── Validation ────────────────────────────────────────────── */

static void validate_config(uint64_t dev_size)
{
    if (cfg.block_size < 512 || cfg.block_size > 65536 ||
        (cfg.block_size & (cfg.block_size - 1)) != 0) {
        fprintf(stderr, "mkfs.ocsfs: block size must be a power of 2 between 512 and 65536\n");
        exit(1);
    }

    if (cfg.extent_size < OCSFS_MIN_EXTENT_SIZE || cfg.extent_size > OCSFS_MAX_EXTENT_SIZE ||
        cfg.extent_size % cfg.block_size != 0) {
        fprintf(stderr, "mkfs.ocsfs: extent size must be %u..%u and a multiple of block size\n",
                OCSFS_MIN_EXTENT_SIZE, OCSFS_MAX_EXTENT_SIZE);
        exit(1);
    }

    if (cfg.max_nodes < 1 || cfg.max_nodes > OCSFS_MAX_NODES) {
        fprintf(stderr, "mkfs.ocsfs: max nodes must be 1..%d\n", OCSFS_MAX_NODES);
        exit(1);
    }

    /* Calculate minimum volume size:
     * superblock(2×4K) + node_slots(64K) + heartbeat(256K) + lock_table(1M)
     * + journals(N×32M) + at least 1 AG
     */
    uint64_t overhead = OCSFS_SUPERBLOCK_SIZE * 2 +
                        OCSFS_NODE_SLOT_TABLE_SIZE +
                        OCSFS_HEARTBEAT_SIZE +
                        OCSFS_LOCK_TABLE_SIZE +
                        ocsfs_journal_region_size(cfg.max_nodes, cfg.journal_size) +
                        sizeof(struct ocsfs_ag_desc) +
                        cfg.ag_size * cfg.block_size; /* at least 1 AG of data */

    if (dev_size < overhead) {
        fprintf(stderr, "mkfs.ocsfs: device too small (%lu bytes). Minimum for this config: %lu bytes\n",
                (unsigned long)dev_size, (unsigned long)overhead);
        exit(1);
    }
}

/* ─── Core Formatting ───────────────────────────────────────── */

static void format_device(int fd, uint64_t dev_size)
{
    uint64_t total_blocks = dev_size / cfg.block_size;

    /* Calculate geometry */
    uint64_t data_start = ocsfs_data_offset(cfg.max_nodes, cfg.journal_size, 1);
    /* Round data_start up to AG boundary */
    uint64_t ag_size_bytes = cfg.ag_size * cfg.block_size;
    if (ag_size_bytes < OCSFS_MIN_AG_SIZE)
        ag_size_bytes = OCSFS_MIN_AG_SIZE;

    /* How many AGs fit in the remaining space? */
    uint64_t data_space = dev_size - data_start;
    /* AG size in blocks */
    uint64_t ag_blocks = ag_size_bytes / cfg.block_size;
    uint32_t ag_count = (uint32_t)(data_space / ag_size_bytes);

    if (ag_count < 1) {
        fprintf(stderr, "mkfs.ocsfs: not enough space for even 1 allocation group\n");
        exit(1);
    }

    uint64_t ag_desc_off = ocsfs_ag_desc_offset(cfg.max_nodes, cfg.journal_size);
    data_start = ag_desc_off + (uint64_t)ag_count * sizeof(struct ocsfs_ag_desc);
    /* Align to block boundary */
    data_start = (data_start + cfg.block_size - 1) & ~((uint64_t)cfg.block_size - 1);

    data_space = dev_size - data_start;
    ag_count = (uint32_t)(data_space / ag_size_bytes);
    if (ag_count < 1) {
        fprintf(stderr, "mkfs.ocsfs: geometry calculation error\n");
        exit(1);
    }

    uint64_t used_blocks = data_start / cfg.block_size;
    uint64_t free_blocks = (uint64_t)ag_count * ag_blocks;

    if (cfg.verbose) {
        printf("OCSFS volume geometry:\n");
        printf("  Device:           %s (%lu bytes, %.2f GiB)\n",
               cfg.device_path, (unsigned long)dev_size,
               (double)dev_size / (1024.0 * 1024 * 1024));
        printf("  Block size:       %u bytes\n", cfg.block_size);
        printf("  Extent size:      %u bytes (%u blocks)\n",
               cfg.extent_size, cfg.extent_size / cfg.block_size);
        printf("  Total blocks:     %lu\n", (unsigned long)total_blocks);
        printf("  Max nodes:        %u\n", cfg.max_nodes);
        printf("  Journal/node:     %u bytes (%.1f MiB)\n",
               cfg.journal_size, cfg.journal_size / (1024.0 * 1024));
        printf("  AG count:         %u\n", ag_count);
        printf("  AG size:          %lu blocks (%.2f GiB)\n",
               (unsigned long)ag_blocks,
               (double)ag_blocks * cfg.block_size / (1024.0 * 1024 * 1024));
        printf("  Metadata overhead:%lu blocks (%.2f MiB)\n",
               (unsigned long)used_blocks,
               (double)used_blocks * cfg.block_size / (1024.0 * 1024));
        printf("  Data start:       offset %lu\n", (unsigned long)data_start);
        printf("  Usable space:     %lu blocks (%.2f GiB)\n",
               (unsigned long)free_blocks,
               (double)free_blocks * cfg.block_size / (1024.0 * 1024 * 1024));
        printf("\n");
    }

    /* ── Step 1: Zero metadata regions ── */
    printf("  [1/4] Zeroing metadata regions...\n");
    zero_region(fd, 0, data_start);

    /* ── Step 2: Write superblock ── */
    printf("  [2/4] Writing superblock...\n");
    struct ocsfs_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_magic = OCSFS_MAGIC;
    sb.s_version_major = OCSFS_VERSION_MAJOR;
    sb.s_version_minor = OCSFS_VERSION_MINOR;
    uuid_generate(sb.s_uuid);
    snprintf(sb.s_label, OCSFS_MAX_LABEL, "%s", cfg.label);
    sb.s_block_size = cfg.block_size;
    sb.s_extent_size = cfg.extent_size;
    sb.s_total_blocks = total_blocks;
    sb.s_free_blocks = free_blocks;
    sb.s_ag_count = ag_count;
    sb.s_ag_size = ag_blocks;
    sb.s_max_nodes = cfg.max_nodes;
    sb.s_feature_flags = cfg.features;
    sb.s_heartbeat_interval = OCSFS_HEARTBEAT_INTERVAL_MS;
    sb.s_heartbeat_timeout = OCSFS_HEARTBEAT_TIMEOUT_MS;
    sb.s_journal_size = cfg.journal_size;
    sb.s_lock_table_off = OCSFS_LOCK_TABLE_OFF;
    sb.s_journal_off = ocsfs_journal_offset();
    sb.s_ag_desc_off = ag_desc_off;
    sb.s_data_off = data_start;
    sb.s_mkfs_time = now_ns();
    sb.s_mount_count = 0;
    sb.s_last_mount_time = 0;
    sb.s_checksum = ocsfs_crc32c(0, &sb, sizeof(sb) - sizeof(uint32_t));

    write_at(fd, OCSFS_SUPERBLOCK_OFFSET, &sb, sizeof(sb));
    write_at(fd, OCSFS_SUPERBLOCK_MIRROR, &sb, sizeof(sb));

    /* ── Step 3: Initialize journals ── */
    printf("  [3/4] Initializing per-node journals...\n");
    for (uint16_t n = 0; n < cfg.max_nodes; n++) {
        uint64_t joff = sb.s_journal_off + (uint64_t)n * cfg.journal_size;
        struct ocsfs_journal_header jh;
        memset(&jh, 0, sizeof(jh));
        jh.jh_magic = OCSFS_JOURNAL_MAGIC;
        jh.jh_node_slot = n;
        jh.jh_head = sizeof(struct ocsfs_journal_header); /* first write after header */
        jh.jh_tail = sizeof(struct ocsfs_journal_header);
        jh.jh_sequence = 1;
        jh.jh_size = cfg.journal_size;
        jh.jh_checksum = ocsfs_crc32c(0, &jh, sizeof(jh) - sizeof(uint32_t));
        write_at(fd, joff, &jh, sizeof(jh));
    }

    /* ── Step 4: Initialize Allocation Groups ── */
    printf("  [4/4] Initializing %u allocation groups...\n", ag_count);
    for (uint32_t ag = 0; ag < ag_count; ag++) {
        uint64_t ag_data_start = data_start + (uint64_t)ag * ag_size_bytes;
        uint64_t ag_data_blocks = ag_blocks;

        uint64_t bitmap_blocks = (ag_data_blocks + cfg.block_size * 8 - 1) /
                                 (cfg.block_size * 8);
        uint64_t inodes_per_ag = ag_data_blocks / 64; /* 1 inode per 64 blocks heuristic */
        if (inodes_per_ag < 64) inodes_per_ag = 64;
        uint64_t inode_table_blocks = (inodes_per_ag * OCSFS_INODE_SIZE + cfg.block_size - 1) /
                                      cfg.block_size;

        uint64_t metadata_blocks = 1 + bitmap_blocks + inode_table_blocks;
        uint64_t ag_free = ag_data_blocks - metadata_blocks;

        struct ocsfs_ag_desc agd;
        memset(&agd, 0, sizeof(agd));
        agd.ag_magic = OCSFS_AG_MAGIC;
        agd.ag_number = ag;
        agd.ag_block_start = ag_data_start / cfg.block_size;
        agd.ag_block_count = ag_data_blocks;
        agd.ag_free_blocks = ag_free;
        agd.ag_free_extents = 1; /* initially 1 big free extent */
        agd.ag_bitmap_off = cfg.block_size; /* block 1 within AG */
        agd.ag_bitmap_size = bitmap_blocks * cfg.block_size;
        agd.ag_inode_table_off = (1 + bitmap_blocks) * cfg.block_size;
        agd.ag_inode_count = inodes_per_ag;
        agd.ag_free_inodes = inodes_per_ag;
        agd.ag_extent_tree_off = 0; /* will be initialized on first use */
        agd.ag_inode_btree_off = 0; /* will be initialized on first use */
        agd.ag_owner_node = ag % cfg.max_nodes; /* round-robin affinity */
        agd.ag_checksum = ocsfs_crc32c(0, &agd, sizeof(agd) - sizeof(uint32_t));

        /* Write AG descriptor in the AG descriptor table area */
        write_at(fd, ag_desc_off + (uint64_t)ag * sizeof(agd), &agd, sizeof(agd));

        /* Also write AG descriptor at the start of the AG data region */
        write_at(fd, ag_data_start, &agd, sizeof(agd));

        /* Initialize bitmap: mark metadata blocks as used */
        uint8_t *bitmap = calloc(1, bitmap_blocks * cfg.block_size);
        if (!bitmap) {
            perror("mkfs.ocsfs: bitmap alloc");
            exit(1);
        }
        for (uint64_t b = 0; b < metadata_blocks; b++) {
            bitmap[b / 8] |= (1 << (b % 8));
        }
        write_at(fd, ag_data_start + cfg.block_size, bitmap,
                 bitmap_blocks * cfg.block_size);
        free(bitmap);

        /* Root directory inode: only in AG 0 */
        if (ag == 0) {
            struct ocsfs_inode root_ino;
            memset(&root_ino, 0, sizeof(root_ino));
            root_ino.i_magic = OCSFS_INODE_MAGIC;
            root_ino.i_ino = OCSFS_ROOT_INO;
            root_ino.i_mode = (OCSFS_FT_DIR << 12) | 0755;
            root_ino.i_nlink = 2; /* . and .. */
            root_ino.i_uid = 0;
            root_ino.i_gid = 0;
            root_ino.i_size = 0;
            root_ino.i_blocks = 0;
            root_ino.i_atime = now_ns();
            root_ino.i_mtime = now_ns();
            root_ino.i_ctime = now_ns();
            root_ino.i_extent_count = 0;
            root_ino.i_extent_max = OCSFS_INLINE_EXTENTS;
            root_ino.i_ag = 0;
            root_ino.i_checksum = ocsfs_crc32c(0, &root_ino,
                                                sizeof(root_ino) - sizeof(uint32_t));

            /* Write at inode table offset + (ROOT_INO * INODE_SIZE) */
            uint64_t ino_off = ag_data_start + agd.ag_inode_table_off +
                               OCSFS_ROOT_INO * OCSFS_INODE_SIZE;
            write_at(fd, ino_off, &root_ino, sizeof(root_ino));
        }

    }

    /* ── Done ── */
    fsync(fd);

    char uuid_str[37];
    uuid_unparse(sb.s_uuid, uuid_str);

    printf("\nOCSFS filesystem created successfully!\n");
    printf("  UUID:       %s\n", uuid_str);
    printf("  Label:      %s\n", cfg.label[0] ? cfg.label : "(none)");
    printf("  Block size: %u\n", cfg.block_size);
    printf("  Extent sz:  %u (%u KB)\n", cfg.extent_size, cfg.extent_size / 1024);
    printf("  AG count:   %u × %.2f GiB\n", ag_count,
           (double)ag_blocks * cfg.block_size / (1024.0 * 1024 * 1024));
    printf("  Max nodes:  %u\n", cfg.max_nodes);
    printf("  Capacity:   %.2f GiB usable (%.2f GiB total)\n",
           (double)free_blocks * cfg.block_size / (1024.0 * 1024 * 1024),
           (double)dev_size / (1024.0 * 1024 * 1024));
    printf("  Features:  ");
    if (cfg.features & OCSFS_FEAT_THIN_PROV) printf(" thin");
    if (cfg.features & OCSFS_FEAT_COMPRESSION) printf(" compress");
    if (cfg.features & OCSFS_FEAT_ENCRYPTION) printf(" encrypt");
    if (cfg.features & OCSFS_FEAT_SNAPSHOTS) printf(" snapshots");
    if (cfg.features & OCSFS_FEAT_AUTH) printf(" auth");
    printf("\n");
}

/* ─── Main ──────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr,
        "Usage: mkfs.ocsfs [options] <device>\n"
        "\n"
        "Options:\n"
        "  -L <label>      Volume label (max %d chars)\n"
        "  -N <max_nodes>  Maximum cluster nodes (default %d, max %d)\n"
        "  -b <block_size> Block size in bytes (default %d)\n"
        "  -E <extent_sz>  Extent size (default 1M, e.g., 1M, 4M)\n"
        "  -A <ag_size>    Allocation Group size (default 1G)\n"
        "  -J <jnl_size>   Per-node journal size (default 32M)\n"
        "  -K              Enable cluster auth (requires cluster_secret= at mount)\n"
        "  -T              Enable thin provisioning (default: on)\n"
        "  -f              Force (skip confirmation prompt)\n"
        "  -v              Verbose output\n"
        "  -h              Show this help\n",
        OCSFS_MAX_LABEL - 1, OCSFS_DEFAULT_MAX_NODES, OCSFS_MAX_NODES,
        OCSFS_DEFAULT_BLOCK_SIZE);
    exit(1);
}

int main(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "L:N:b:E:A:J:KTfvh")) != -1) {
        switch (opt) {
        case 'L':
            snprintf(cfg.label, OCSFS_MAX_LABEL, "%s", optarg);
            break;
        case 'N':
            cfg.max_nodes = (uint16_t)atoi(optarg);
            break;
        case 'b':
            cfg.block_size = (uint32_t)parse_size(optarg);
            break;
        case 'E':
            cfg.extent_size = (uint32_t)parse_size(optarg);
            break;
        case 'A':
            cfg.ag_size = parse_size(optarg) / cfg.block_size;
            break;
        case 'J':
            cfg.journal_size = (uint32_t)parse_size(optarg);
            break;
        case 'K':
            cfg.features |= OCSFS_FEAT_AUTH;
            break;
        case 'T':
            cfg.features |= OCSFS_FEAT_THIN_PROV;
            break;
        case 'f':
            cfg.force = 1;
            break;
        case 'v':
            cfg.verbose = 1;
            break;
        case 'h':
        default:
            usage();
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "mkfs.ocsfs: no device specified\n");
        usage();
    }
    cfg.device_path = argv[optind];

    printf("mkfs.ocsfs %d.%d — Open Cluster Shared FileSystem\n\n",
           OCSFS_VERSION_MAJOR, OCSFS_VERSION_MINOR);

    int fd = open(cfg.device_path, O_RDWR);
    if (fd < 0) {
        fd = open(cfg.device_path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            fprintf(stderr, "mkfs.ocsfs: cannot open %s: %s\n",
                    cfg.device_path, strerror(errno));
            exit(1);
        }
    }

    uint64_t dev_size = get_device_size(fd);
    validate_config(dev_size);

    /* Confirmation */
    if (!cfg.force) {
        printf("WARNING: This will destroy all data on %s (%.2f GiB).\n",
               cfg.device_path,
               (double)dev_size / (1024.0 * 1024 * 1024));
        printf("Continue? (y/N) ");
        fflush(stdout);
        char answer = 0;
        if (scanf(" %c", &answer) != 1 || (answer != 'y' && answer != 'Y')) {
            printf("Aborted.\n");
            close(fd);
            exit(0);
        }
    }

    printf("Formatting %s as OCSFS...\n", cfg.device_path);
    format_device(fd, dev_size);

    close(fd);
    return 0;
}
