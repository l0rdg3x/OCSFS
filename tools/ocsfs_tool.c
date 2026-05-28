/*
 * ocsfs-tool — Administration CLI for OCSFS volumes.
 *
 * Usage: ocsfs-tool <command> <device|mountpoint> [options]
 *
 * Commands:
 *   info      Show volume information (superblock dump)
 *   nodes     List registered node slots and their status
 *   locks     Show active lock table entries
 *   df        Space usage report with thin provisioning details
 *   check     Verify filesystem integrity (offline)
 *   dump-sb   Raw superblock hex dump
 *   dump-ag   Dump AG descriptor(s)
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
#include <uuid/uuid.h>

#include "ocsfs.h"

/* ─── Read helpers ──────────────────────────────────────────── */

static void read_at(int fd, uint64_t offset, void *buf, size_t len)
{
    if (pread(fd, buf, len, offset) != (ssize_t)len) {
        fprintf(stderr, "ocsfs-tool: read error at offset %lu: %s\n",
                (unsigned long)offset, strerror(errno));
        exit(1);
    }
}

static void write_at(int fd, uint64_t offset, const void *buf, size_t len)
{
    if (pwrite(fd, buf, len, offset) != (ssize_t)len) {
        fprintf(stderr, "ocsfs-tool: write error at offset %lu: %s\n",
                (unsigned long)offset, strerror(errno));
        exit(1);
    }
}

static struct ocsfs_superblock read_superblock(int fd, int mirror)
{
    struct ocsfs_superblock sb;
    uint64_t off = mirror ? OCSFS_SUPERBLOCK_MIRROR : OCSFS_SUPERBLOCK_OFFSET;
    read_at(fd, off, &sb, sizeof(sb));

    if (sb.s_magic != OCSFS_MAGIC) {
        fprintf(stderr, "ocsfs-tool: invalid magic %08X at %s superblock (expected %08X)\n",
                sb.s_magic, mirror ? "mirror" : "primary", OCSFS_MAGIC);
        if (!mirror) {
            fprintf(stderr, "  Trying mirror superblock...\n");
            return read_superblock(fd, 1);
        }
        fprintf(stderr, "  Both superblocks invalid. Not an OCSFS volume.\n");
        exit(1);
    }

    /* Verify checksum */
    uint32_t computed = ocsfs_crc32c(0, &sb, sizeof(sb) - sizeof(uint32_t));
    if (computed != sb.s_checksum) {
        fprintf(stderr, "ocsfs-tool: WARNING: %s superblock checksum mismatch "
                "(stored=%08X computed=%08X)\n",
                mirror ? "mirror" : "primary", sb.s_checksum, computed);
    }

    return sb;
}

static const char *format_time(uint64_t ns)
{
    static char buf[64];
    if (ns == 0) {
        snprintf(buf, sizeof(buf), "(never)");
        return buf;
    }
    time_t secs = ns / 1000000000ULL;
    struct tm *tm = localtime(&secs);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    return buf;
}

static const char *format_size(uint64_t bytes)
{
    static char buf[64];
    if (bytes >= (1ULL << 40))
        snprintf(buf, sizeof(buf), "%.2f TiB", (double)bytes / (1ULL << 40));
    else if (bytes >= (1ULL << 30))
        snprintf(buf, sizeof(buf), "%.2f GiB", (double)bytes / (1ULL << 30));
    else if (bytes >= (1ULL << 20))
        snprintf(buf, sizeof(buf), "%.2f MiB", (double)bytes / (1ULL << 20));
    else if (bytes >= (1ULL << 10))
        snprintf(buf, sizeof(buf), "%.2f KiB", (double)bytes / (1ULL << 10));
    else
        snprintf(buf, sizeof(buf), "%lu B", (unsigned long)bytes);
    return buf;
}

static const char *node_state_str(uint8_t state)
{
    switch (state) {
    case OCSFS_NODE_FREE:     return "FREE";
    case OCSFS_NODE_ACTIVE:   return "ACTIVE";
    case OCSFS_NODE_EVICTING: return "EVICTING";
    case OCSFS_NODE_DEAD:     return "DEAD";
    default:                  return "UNKNOWN";
    }
}

static const char *lock_mode_str(uint16_t mode)
{
    switch (mode) {
    case OCSFS_LOCK_NL: return "NL";
    case OCSFS_LOCK_SH: return "SH";
    case OCSFS_LOCK_EX: return "EX";
    case OCSFS_LOCK_CW: return "CW";
    default:            return "??";
    }
}

static const char *lockres_type_str(uint32_t type)
{
    switch (type) {
    case OCSFS_LOCKRES_INODE:    return "INODE";
    case OCSFS_LOCKRES_AG:       return "AG";
    case OCSFS_LOCKRES_JOURNAL:  return "JOURNAL";
    case OCSFS_LOCKRES_RENAME:   return "RENAME";
    case OCSFS_LOCKRES_RECOVERY: return "RECOVERY";
    case OCSFS_LOCKRES_SUPER:    return "SUPER";
    default:                     return "UNKNOWN";
    }
}

/* ─── Commands ──────────────────────────────────────────────── */

static void cmd_info(int fd)
{
    struct ocsfs_superblock sb = read_superblock(fd, 0);
    char uuid_str[37];
    uuid_unparse(sb.s_uuid, uuid_str);

    printf("═══════════════════════════════════════════════════\n");
    printf("  OCSFS Volume Information\n");
    printf("═══════════════════════════════════════════════════\n\n");

    printf("  UUID:             %s\n", uuid_str);
    printf("  Label:            %s\n", sb.s_label[0] ? sb.s_label : "(none)");
    printf("  Version:          %u.%u\n", sb.s_version_major, sb.s_version_minor);
    printf("  Created:          %s\n", format_time(sb.s_mkfs_time));
    printf("  Last mount:       %s\n", format_time(sb.s_last_mount_time));
    printf("  Mount count:      %lu\n", (unsigned long)sb.s_mount_count);
    printf("\n");

    printf("  Block size:       %u bytes\n", sb.s_block_size);
    printf("  Extent size:      %s (%u blocks)\n",
           format_size(sb.s_extent_size), sb.s_extent_size / sb.s_block_size);
    printf("  Total blocks:     %lu\n", (unsigned long)sb.s_total_blocks);
    printf("  Free blocks:      %lu\n", (unsigned long)sb.s_free_blocks);
    printf("  Total capacity:   %s\n",
           format_size((uint64_t)sb.s_total_blocks * sb.s_block_size));
    printf("  Free space:       %s\n",
           format_size((uint64_t)sb.s_free_blocks * sb.s_block_size));
    printf("\n");

    printf("  Allocation Groups:%u × %s\n", sb.s_ag_count,
           format_size(sb.s_ag_size * sb.s_block_size));
    printf("  Max nodes:        %u\n", sb.s_max_nodes);
    printf("  Journal/node:     %s\n", format_size(sb.s_journal_size));
    printf("  Heartbeat int.:   %u ms\n", sb.s_heartbeat_interval);
    printf("  Heartbeat timeout:%u ms\n", sb.s_heartbeat_timeout);
    printf("\n");

    printf("  Features:        ");
    if (sb.s_feature_flags & OCSFS_FEAT_THIN_PROV)   printf(" thin-provisioning");
    if (sb.s_feature_flags & OCSFS_FEAT_COMPRESSION)  printf(" compression");
    if (sb.s_feature_flags & OCSFS_FEAT_ENCRYPTION)   printf(" encryption");
    if (sb.s_feature_flags & OCSFS_FEAT_SNAPSHOTS)    printf(" snapshots");
    if (sb.s_feature_flags & OCSFS_FEAT_DEDUP)        printf(" dedup");
    if (sb.s_feature_flags & OCSFS_FEAT_MULTI_LUN)    printf(" multi-lun");
    if (!sb.s_feature_flags) printf(" (none)");
    printf("\n\n");

    printf("  Layout offsets:\n");
    printf("    Superblock:     0x%08lX\n", (unsigned long)OCSFS_SUPERBLOCK_OFFSET);
    printf("    Node slots:     0x%08lX\n", (unsigned long)OCSFS_NODE_SLOT_TABLE_OFF);
    printf("    Heartbeat:      0x%08lX\n", (unsigned long)OCSFS_HEARTBEAT_OFF);
    printf("    Lock table:     0x%08lX (%u entries)\n",
           (unsigned long)sb.s_lock_table_off, OCSFS_LOCK_ENTRY_COUNT);
    printf("    Journals:       0x%08lX (%u × %s)\n",
           (unsigned long)sb.s_journal_off, sb.s_max_nodes,
           format_size(sb.s_journal_size));
    printf("    AG descriptors: 0x%08lX\n", (unsigned long)sb.s_ag_desc_off);
    printf("    Data region:    0x%08lX\n", (unsigned long)sb.s_data_off);
    printf("\n");
}

static void cmd_nodes(int fd)
{
    struct ocsfs_superblock sb = read_superblock(fd, 0);

    printf("═══════════════════════════════════════════════════\n");
    printf("  OCSFS Node Slot Table (%u max)\n", sb.s_max_nodes);
    printf("═══════════════════════════════════════════════════\n\n");

    printf("  %-4s  %-8s  %-20s  %-8s  %-10s  %s\n",
           "Slot", "State", "Hostname", "MountGen", "PR Key", "Last Heartbeat");
    printf("  %-4s  %-8s  %-20s  %-8s  %-10s  %s\n",
           "────", "────────", "────────────────────", "────────",
           "──────────", "──────────────────");

    int active_count = 0;
    for (uint16_t i = 0; i < sb.s_max_nodes; i++) {
        struct ocsfs_node_slot ns;
        uint64_t off = OCSFS_NODE_SLOT_TABLE_OFF + (uint64_t)i * sizeof(ns);
        read_at(fd, off, &ns, sizeof(ns));

        if (ns.ns_state == OCSFS_NODE_FREE)
            continue;

        char uuid_short[16];
        snprintf(uuid_short, sizeof(uuid_short), "%02x%02x%02x%02x",
                 ns.ns_uuid[0], ns.ns_uuid[1], ns.ns_uuid[2], ns.ns_uuid[3]);

        printf("  %-4u  %-8s  %-20s  %-8u  0x%08lX  %s\n",
               i, node_state_str(ns.ns_state),
               ns.ns_name[0] ? ns.ns_name : uuid_short,
               ns.ns_mount_gen,
               (unsigned long)ns.ns_pr_key,
               format_time(ns.ns_last_heartbeat));

        if (ns.ns_state == OCSFS_NODE_ACTIVE)
            active_count++;
    }

    printf("\n  Active nodes: %d / %u\n\n", active_count, sb.s_max_nodes);
}

static void cmd_locks(int fd)
{
    struct ocsfs_superblock sb = read_superblock(fd, 0);

    printf("═══════════════════════════════════════════════════\n");
    printf("  OCSFS Lock Table (%u entries)\n", OCSFS_LOCK_ENTRY_COUNT);
    printf("═══════════════════════════════════════════════════\n\n");

    printf("  %-6s  %-10s  %-4s  %-6s  %-8s  %-16s  %s\n",
           "Slot", "Type", "Mode", "Holder", "Gen", "Resource ID", "Granted");
    printf("  %-6s  %-10s  %-4s  %-6s  %-8s  %-16s  %s\n",
           "──────", "──────────", "────", "──────", "────────",
           "────────────────", "───────────────────");

    int active_locks = 0;
    for (uint32_t i = 0; i < OCSFS_LOCK_ENTRY_COUNT; i++) {
        struct ocsfs_lock_entry le;
        uint64_t off = sb.s_lock_table_off + (uint64_t)i * sizeof(le);
        read_at(fd, off, &le, sizeof(le));

        if (le.le_mode == OCSFS_LOCK_NL && le.le_resource_id == 0)
            continue;

        printf("  %-6u  %-10s  %-4s  %-6u  %-8u  0x%014lX  %s\n",
               i, lockres_type_str(le.le_resource_type),
               lock_mode_str(le.le_mode),
               le.le_holder_slot, le.le_holder_gen,
               (unsigned long)le.le_resource_id,
               format_time(le.le_grant_time));
        active_locks++;
    }

    printf("\n  Active locks: %d / %u\n\n", active_locks, OCSFS_LOCK_ENTRY_COUNT);
}

static void cmd_df(int fd)
{
    struct ocsfs_superblock sb = read_superblock(fd, 0);

    printf("═══════════════════════════════════════════════════\n");
    printf("  OCSFS Space Report\n");
    printf("═══════════════════════════════════════════════════\n\n");

    uint64_t total_bytes = (uint64_t)sb.s_total_blocks * sb.s_block_size;
    uint64_t free_bytes = (uint64_t)sb.s_free_blocks * sb.s_block_size;
    uint64_t used_bytes = total_bytes - free_bytes;
    double pct_used = (total_bytes > 0) ? 100.0 * used_bytes / total_bytes : 0;

    printf("  Total:    %s (%lu blocks)\n", format_size(total_bytes),
           (unsigned long)sb.s_total_blocks);
    printf("  Used:     %s (%lu blocks, %.1f%%)\n", format_size(used_bytes),
           (unsigned long)(sb.s_total_blocks - sb.s_free_blocks), pct_used);
    printf("  Free:     %s (%lu blocks, %.1f%%)\n", format_size(free_bytes),
           (unsigned long)sb.s_free_blocks, 100.0 - pct_used);
    printf("\n");

    /* Per-AG breakdown */
    printf("  %-4s  %-12s  %-12s  %-12s  %-8s  %s\n",
           "AG", "Total", "Free", "Used", "Use%", "Owner");
    printf("  %-4s  %-12s  %-12s  %-12s  %-8s  %s\n",
           "────", "────────────", "────────────", "────────────",
           "────────", "─────");

    for (uint32_t ag = 0; ag < sb.s_ag_count && ag < 64; ag++) {
        struct ocsfs_ag_desc agd;
        uint64_t off = sb.s_ag_desc_off + (uint64_t)ag * sizeof(agd);
        read_at(fd, off, &agd, sizeof(agd));

        if (agd.ag_magic != OCSFS_AG_MAGIC) {
            printf("  %-4u  (invalid AG descriptor)\n", ag);
            continue;
        }

        uint64_t ag_total = agd.ag_block_count * sb.s_block_size;
        uint64_t ag_free = agd.ag_free_blocks * sb.s_block_size;
        uint64_t ag_used = ag_total - ag_free;
        double ag_pct = (ag_total > 0) ? 100.0 * ag_used / ag_total : 0;

        printf("  %-4u  %-12s  %-12s  %-12s  %5.1f%%   node %u\n",
               ag, format_size(ag_total), format_size(ag_free),
               format_size(ag_used), ag_pct, agd.ag_owner_node);
    }

    if (sb.s_ag_count > 64) {
        printf("  ... (%u more AGs not shown)\n", sb.s_ag_count - 64);
    }
    printf("\n");
}

static void cmd_check(int fd)
{
    printf("═══════════════════════════════════════════════════\n");
    printf("  OCSFS Integrity Check\n");
    printf("═══════════════════════════════════════════════════\n\n");

    int errors = 0;
    int warnings = 0;

    /* Check primary superblock */
    printf("  [1/5] Checking primary superblock... ");
    struct ocsfs_superblock sb;
    read_at(fd, OCSFS_SUPERBLOCK_OFFSET, &sb, sizeof(sb));
    if (sb.s_magic != OCSFS_MAGIC) {
        printf("FAIL (bad magic: 0x%08X)\n", sb.s_magic);
        errors++;
    } else {
        uint32_t crc = ocsfs_crc32c(0, &sb, sizeof(sb) - sizeof(uint32_t));
        if (crc != sb.s_checksum) {
            printf("FAIL (CRC mismatch: stored=0x%08X computed=0x%08X)\n",
                   sb.s_checksum, crc);
            errors++;
        } else {
            printf("OK\n");
        }
    }

    /* Check mirror superblock */
    printf("  [2/5] Checking mirror superblock... ");
    struct ocsfs_superblock sb2;
    read_at(fd, OCSFS_SUPERBLOCK_MIRROR, &sb2, sizeof(sb2));
    if (memcmp(&sb, &sb2, sizeof(sb)) != 0) {
        printf("WARNING (mirror differs from primary)\n");
        warnings++;
    } else {
        printf("OK\n");
    }

    /* Check journal headers */
    printf("  [3/5] Checking journal headers... ");
    int jnl_ok = 0, jnl_fail = 0;
    for (uint16_t n = 0; n < sb.s_max_nodes; n++) {
        struct ocsfs_journal_header jh;
        uint64_t off = sb.s_journal_off + (uint64_t)n * sb.s_journal_size;
        read_at(fd, off, &jh, sizeof(jh));
        if (jh.jh_magic == OCSFS_JOURNAL_MAGIC) {
            jnl_ok++;
        } else {
            jnl_fail++;
        }
    }
    if (jnl_fail > 0) {
        printf("FAIL (%d/%u journals invalid)\n", jnl_fail, sb.s_max_nodes);
        errors += jnl_fail;
    } else {
        printf("OK (%d journals)\n", jnl_ok);
    }

    /* Check AG descriptors */
    printf("  [4/5] Checking AG descriptors... ");
    int ag_ok = 0, ag_fail = 0;
    for (uint32_t ag = 0; ag < sb.s_ag_count; ag++) {
        struct ocsfs_ag_desc agd;
        uint64_t off = sb.s_ag_desc_off + (uint64_t)ag * sizeof(agd);
        read_at(fd, off, &agd, sizeof(agd));
        if (agd.ag_magic == OCSFS_AG_MAGIC && agd.ag_number == ag) {
            ag_ok++;
        } else {
            ag_fail++;
        }
    }
    if (ag_fail > 0) {
        printf("FAIL (%d/%u AGs invalid)\n", ag_fail, sb.s_ag_count);
        errors += ag_fail;
    } else {
        printf("OK (%d AGs)\n", ag_ok);
    }

    /* Check root inode */
    printf("  [5/5] Checking root inode... ");
    if (sb.s_ag_count > 0) {
        struct ocsfs_ag_desc ag0;
        read_at(fd, sb.s_ag_desc_off, &ag0, sizeof(ag0));
        /* ag_inode_table_off is AG-relative; absolute = ag_block_start*block_size + rel_off */
        uint64_t root_off = (uint64_t)ag0.ag_block_start * sb.s_block_size +
                            ag0.ag_inode_table_off +
                            OCSFS_ROOT_INO * OCSFS_INODE_SIZE;
        struct ocsfs_inode root;
        read_at(fd, root_off, &root, sizeof(root));
        if (root.i_magic != OCSFS_INODE_MAGIC) {
            printf("FAIL (bad magic: 0x%08X)\n", root.i_magic);
            errors++;
        } else if (root.i_ino != OCSFS_ROOT_INO) {
            printf("FAIL (wrong inode number: %lu)\n", (unsigned long)root.i_ino);
            errors++;
        } else if ((root.i_mode >> 12) != OCSFS_FT_DIR) {
            printf("FAIL (not a directory)\n");
            errors++;
        } else {
            printf("OK (ino=%lu, mode=%o)\n",
                   (unsigned long)root.i_ino, root.i_mode & 0xFFF);
        }
    } else {
        printf("SKIP (no AGs)\n");
    }

    printf("\n  ─────────────────────────────────────────────\n");
    printf("  Result: %d errors, %d warnings\n", errors, warnings);
    if (errors == 0)
        printf("  Status: CLEAN\n");
    else
        printf("  Status: ERRORS FOUND — manual repair may be needed\n");
    printf("\n");
}

static int cmd_tune(int fd, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--upgrade") != 0) {
        fprintf(stderr, "Usage: ocsfs-tool tune <device> --upgrade\n");
        return 1;
    }

    struct ocsfs_superblock sb = read_superblock(fd, 0);

    if (sb.s_revision_level >= 1) {
        printf("Volume is already V2 (revision_level=%u), nothing to do.\n",
               sb.s_revision_level);
        return 0;
    }

    /* Apply V2 fields */
    sb.s_revision_level    = 1;
    sb.s_feature_incompat |= OCSFS_FEATURE_INCOMPAT_LOCK_TABLE_V2 |
                              OCSFS_FEATURE_INCOMPAT_RC_BTREE_PER_AG;
    sb.s_feature_ro_compat|= OCSFS_FEATURE_RO_COMPAT_DEDUP_SCRUB;
    sb.s_lock_primary_count = OCSFS_LOCK_ENTRY_COUNT;

    /* Recompute checksum */
    sb.s_checksum = ocsfs_crc32c(0, &sb, sizeof(sb) - sizeof(uint32_t));

    /* Write primary then mirror */
    write_at(fd, OCSFS_SUPERBLOCK_OFFSET, &sb, sizeof(sb));
    write_at(fd, OCSFS_SUPERBLOCK_MIRROR,  &sb, sizeof(sb));

    printf("Upgrade V1->V2 completato.\n");
    return 0;
}

/* ─── Main ──────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr,
        "Usage: ocsfs-tool <command> <device>\n"
        "\n"
        "Commands:\n"
        "  info      Volume information\n"
        "  nodes     Node slot table\n"
        "  locks     Lock table entries\n"
        "  df        Space usage report\n"
        "  check     Integrity check (offline)\n"
        "  tune      Offline volume tuning (--upgrade: migrate V1->V2)\n"
        "\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    if (argc < 3)
        usage();

    const char *cmd = argv[1];
    const char *dev = argv[2];

    int flags = strcmp(cmd, "tune") == 0 ? O_RDWR : O_RDONLY;
    int fd = open(dev, flags);
    if (fd < 0) {
        fprintf(stderr, "ocsfs-tool: cannot open %s: %s\n", dev, strerror(errno));
        exit(1);
    }

    printf("ocsfs-tool %d.%d — OCSFS Administration Tool\n\n",
           OCSFS_VERSION_MAJOR, OCSFS_VERSION_MINOR);

    if (strcmp(cmd, "info") == 0)        cmd_info(fd);
    else if (strcmp(cmd, "nodes") == 0)  cmd_nodes(fd);
    else if (strcmp(cmd, "locks") == 0)  cmd_locks(fd);
    else if (strcmp(cmd, "df") == 0)     cmd_df(fd);
    else if (strcmp(cmd, "check") == 0)  cmd_check(fd);
    else if (strcmp(cmd, "tune") == 0) {
        int rc = cmd_tune(fd, argc - 2, argv + 2);
        close(fd);
        return rc;
    } else {
        fprintf(stderr, "ocsfs-tool: unknown command '%s'\n", cmd);
        usage();
    }

    close(fd);
    return 0;
}
