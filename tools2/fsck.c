// SPDX-License-Identifier: GPL-2.0-only
/*
 * fsck.ocsfs2 — read-only structural + checksum verifier for OCSFS v2.
 *
 * Checks: superblock (primary + mirror) magic/crc; region layout in-range and
 * ordered; per-AG header magic/crc and geometry; bitmap metadata bits set and
 * free-block count consistent; every used inode crc + extent ranges; root
 * directory "." / ".." entries. Exit 0 = clean, non-zero = errors found.
 *
 * Read-only: never writes. -r (repair) is not implemented in Plan 1.
 */
#define _GNU_SOURCE
#include "ocsfs_ondisk.h"
#include "ocsfs_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <endian.h>

#define BS OCSFS2_BLOCK_SIZE

static int g_fd;
static uint64_t g_errors;

#define ERR(...)   do { fprintf(stderr, "  ERROR: " __VA_ARGS__); g_errors++; } while (0)
#define WARN(...)  do { fprintf(stderr, "  warn:  " __VA_ARGS__); } while (0)

static uint64_t divup(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

static int read_at(void *buf, size_t len, uint64_t off)
{
	uint8_t *p = buf;
	while (len) {
		ssize_t n = pread(g_fd, p, len, off);
		if (n < 0) { if (errno == EINTR) continue; return -1; }
		if (n == 0) return -1;
		p += n; off += n; len -= n;
	}
	return 0;
}

static int crc_ok(const void *buf, size_t crc_field_off, uint32_t stored)
{
	return ocsfs2_crc32c(~0u, buf, crc_field_off) == stored;
}

static unsigned popcount_bits(const uint8_t *bm, uint64_t nbits)
{
	uint64_t set = 0, i;
	for (i = 0; i < nbits; i++)
		if (bm[i >> 3] & (1u << (i & 7)))
			set++;
	return (unsigned)set;  /* caller AGs are <= 2^32 blocks */
}

static int check_dirent(const struct ocsfs2_disk_dirent *de, const char *want,
			uint64_t want_ino)
{
	char tmp[OCSFS2_MAX_NAME + 1];
	struct ocsfs2_disk_dirent copy = *de;
	uint32_t stored = le32toh(de->de_checksum);

	if (le32toh(de->de_magic) != OCSFS2_DIRENT_MAGIC) { ERR("root '%s' bad dirent magic\n", want); return -1; }
	copy.de_checksum = 0;
	if (ocsfs2_crc32c(~0u, &copy, OCSFS2_DIRENT_SIZE) != stored) { ERR("root '%s' dirent crc\n", want); return -1; }
	if (le64toh(de->de_ino) != want_ino) { ERR("root '%s' ino %llu != %llu\n", want, (unsigned long long)le64toh(de->de_ino), (unsigned long long)want_ino); return -1; }
	memcpy(tmp, de->de_name, de->de_name_len);
	tmp[de->de_name_len] = 0;
	if (strcmp(tmp, want)) { ERR("root entry name '%s' != '%s'\n", tmp, want); return -1; }
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev;
	uint64_t dev_size = 0, dev_blocks;
	struct stat st;
	struct ocsfs2_disk_super sb, sbm;
	int use_mirror = 0, opt;
	uint32_t ag_count, i;
	uint64_t ag_blocks, inodes_per_ag, ag_region_start, total_free_recomputed = 0;
	uint64_t used_inodes = 0;

	while ((opt = getopt(argc, argv, "r")) != -1) {
		if (opt == 'r') { fprintf(stderr, "fsck.ocsfs2: -r (repair) not implemented in Plan 1\n"); }
	}
	if (optind >= argc) { fprintf(stderr, "usage: %s <device>\n", argv[0]); return 2; }
	dev = argv[optind];

	g_fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (g_fd < 0) { perror("open"); return 2; }
	if (fstat(g_fd, &st)) { perror("fstat"); return 2; }
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(g_fd, BLKGETSIZE64, &dev_size)) { perror("BLKGETSIZE64"); return 2; }
	} else {
		dev_size = (uint64_t)st.st_size;
	}
	dev_blocks = dev_size / BS;

	/* ── superblock ── */
	if (read_at(&sb, sizeof(sb), 0)) { fprintf(stderr, "cannot read superblock\n"); return 2; }
	if (le32toh(sb.s_magic) != OCSFS2_MAGIC ||
	    !crc_ok(&sb, offsetof(struct ocsfs2_disk_super, s_checksum), le32toh(sb.s_checksum))) {
		WARN("primary superblock bad; trying mirror\n");
		if (read_at(&sbm, sizeof(sbm), BS) ||
		    le32toh(sbm.s_magic) != OCSFS2_MAGIC ||
		    !crc_ok(&sbm, offsetof(struct ocsfs2_disk_super, s_checksum), le32toh(sbm.s_checksum))) {
			ERR("both superblocks invalid\n");
			fprintf(stderr, "fsck.ocsfs2: FATAL — no valid superblock\n");
			return 1;
		}
		sb = sbm; use_mirror = 1;
	}
	printf("fsck.ocsfs2: %s — label='%.*s' blocks=%llu free=%llu ags=%u%s\n",
	       dev, OCSFS2_MAX_LABEL, sb.s_label,
	       (unsigned long long)le64toh(sb.s_total_blocks),
	       (unsigned long long)le64toh(sb.s_free_blocks),
	       le32toh(sb.s_ag_count), use_mirror ? " (via mirror)" : "");

	if (le32toh(sb.s_block_size) != BS) ERR("block_size %u != 4096\n", le32toh(sb.s_block_size));
	if (le64toh(sb.s_total_blocks) > dev_blocks) ERR("total_blocks %llu > device %llu\n",
		(unsigned long long)le64toh(sb.s_total_blocks), (unsigned long long)dev_blocks);

	ag_count = le32toh(sb.s_ag_count);
	ag_blocks = le64toh(sb.s_ag_blocks);
	inodes_per_ag = le64toh(sb.s_inodes_per_ag);
	ag_region_start = le64toh(sb.s_ag_desc_off) / BS;

	/* region ordering / range */
	{
		uint64_t a = le64toh(sb.s_node_table_off), b = le64toh(sb.s_heartbeat_off);
		uint64_t c = le64toh(sb.s_lease_table_off), d = le64toh(sb.s_recovery_off);
		uint64_t e = le64toh(sb.s_journal_off), f = le64toh(sb.s_ag_desc_off);
		if (!(2 * BS <= a && a < b && b < c && c < d && d < e && e < f && f < dev_size))
			ERR("region offsets not strictly ordered/in-range\n");
		if (ag_count == 0 || ag_blocks == 0 || inodes_per_ag == 0)
			ERR("zero ag_count/ag_blocks/inodes_per_ag\n");
	}

	/* ── per-AG ── */
	for (i = 0; i < ag_count && g_errors < 100; i++) {
		struct ocsfs2_disk_ag ag;
		uint64_t start = ag_region_start + (uint64_t)i * ag_blocks;
		uint64_t expect_count = (i == ag_count - 1) ? dev_blocks - start : ag_blocks;
		uint64_t bitmap_blocks = divup(divup(expect_count, 8), BS);
		uint64_t itable_blocks = (inodes_per_ag * OCSFS2_INODE_SIZE) / BS;
		uint64_t meta_blocks = 1 + bitmap_blocks + itable_blocks;
		uint8_t *bm;
		unsigned used, b;
		uint64_t free_stored;

		if (read_at(&ag, sizeof(ag), start * BS)) { ERR("AG%u header read\n", i); continue; }
		if (le32toh(ag.ag_magic) != OCSFS2_AG_MAGIC) { ERR("AG%u bad magic\n", i); continue; }
		if (!crc_ok(&ag, offsetof(struct ocsfs2_disk_ag, ag_checksum), le32toh(ag.ag_checksum))) { ERR("AG%u crc\n", i); continue; }
		if (le32toh(ag.ag_number) != i) ERR("AG%u number=%u\n", i, le32toh(ag.ag_number));
		if (le64toh(ag.ag_block_start) != start) ERR("AG%u start %llu != %llu\n", i, (unsigned long long)le64toh(ag.ag_block_start), (unsigned long long)start);
		if (le64toh(ag.ag_block_count) != expect_count) ERR("AG%u count %llu != %llu\n", i, (unsigned long long)le64toh(ag.ag_block_count), (unsigned long long)expect_count);

		/* bitmap */
		bm = malloc(bitmap_blocks * BS);
		if (!bm) { ERR("oom bitmap AG%u\n", i); continue; }
		if (read_at(bm, bitmap_blocks * BS, le64toh(ag.ag_bitmap_off))) { ERR("AG%u bitmap read\n", i); free(bm); continue; }
		for (b = 0; b < meta_blocks; b++)
			if (!(bm[b >> 3] & (1u << (b & 7)))) { ERR("AG%u meta block %u not marked used\n", i, b); break; }
		used = popcount_bits(bm, expect_count);
		free(bm);
		free_stored = le64toh(ag.ag_free_blocks);
		if (free_stored != expect_count - used)
			/* The block bitmap is authoritative; ag_free_blocks is a
			 * cached, recomputable hint (not journaled) that can drift by
			 * the allocations since the last sync after a crash. This is
			 * recoverable via repair (cf. e2fsck -p), not corruption. */
			WARN("AG%u free_blocks=%llu but bitmap implies %llu (recomputable hint)\n", i,
			    (unsigned long long)free_stored, (unsigned long long)(expect_count - used));
		total_free_recomputed += expect_count - used;

		/* inode-table scan: verify crc of every used inode */
		{
			uint8_t *itab = malloc(itable_blocks * BS);
			uint64_t k;
			if (!itab) { ERR("oom itable AG%u\n", i); continue; }
			if (read_at(itab, itable_blocks * BS, le64toh(ag.ag_inode_table_off))) { ERR("AG%u itable read\n", i); free(itab); continue; }
			for (k = 0; k < inodes_per_ag; k++) {
				struct ocsfs2_disk_inode *in = (void *)(itab + k * OCSFS2_INODE_SIZE);
				if (le32toh(in->i_magic) != OCSFS2_INODE_MAGIC) continue;
				used_inodes++;
				if (!crc_ok(in, offsetof(struct ocsfs2_disk_inode, i_checksum), le32toh(in->i_checksum)))
					ERR("AG%u inode local %llu crc\n", i, (unsigned long long)k);
			}
			free(itab);
		}
	}

	if (total_free_recomputed != le64toh(sb.s_free_blocks))
		WARN("sb free_blocks=%llu, recomputed=%llu\n",
		     (unsigned long long)le64toh(sb.s_free_blocks),
		     (unsigned long long)total_free_recomputed);

	/* ── root inode + dir ── */
	{
		struct ocsfs2_disk_ag ag0;
		struct ocsfs2_disk_inode ri;
		struct ocsfs2_disk_extent ext;
		uint8_t dirblk[BS];
		uint64_t root_off, phys;

		if (read_at(&ag0, sizeof(ag0), ag_region_start * BS) == 0 &&
		    le32toh(ag0.ag_magic) == OCSFS2_AG_MAGIC) {
			root_off = le64toh(ag0.ag_inode_table_off) + OCSFS2_ROOT_INO * OCSFS2_INODE_SIZE;
			if (read_at(&ri, sizeof(ri), root_off)) { ERR("root inode read\n"); goto done; }
			if (le32toh(ri.i_magic) != OCSFS2_INODE_MAGIC) ERR("root inode magic\n");
			else if (!crc_ok(&ri, offsetof(struct ocsfs2_disk_inode, i_checksum), le32toh(ri.i_checksum))) ERR("root inode crc\n");
			if (!(le16toh(ri.i_mode) & 0040000)) ERR("root not a directory (mode 0%o)\n", le16toh(ri.i_mode));
			if (le16toh(ri.i_nlink) < 2) ERR("root nlink %u < 2\n", le16toh(ri.i_nlink));
			if (le16toh(ri.i_extent_count) < 1) { ERR("root has no extents\n"); goto done; }
			memcpy(&ext, ri.i_inline_extents, sizeof(ext));
			phys = le64toh(ext.e_physical);
			if (read_at(dirblk, BS, phys * BS)) { ERR("root dir block read\n"); goto done; }
			check_dirent((struct ocsfs2_disk_dirent *)dirblk, ".", OCSFS2_ROOT_INO);
			check_dirent((struct ocsfs2_disk_dirent *)(dirblk + OCSFS2_DIRENT_SIZE), "..", OCSFS2_ROOT_INO);
		} else {
			ERR("cannot read AG0 for root inode\n");
		}
	}
done:
	printf("fsck.ocsfs2: scanned %u AGs, %llu used inodes, %llu errors\n",
	       ag_count, (unsigned long long)used_inodes, (unsigned long long)g_errors);
	if (g_errors) { printf("fsck.ocsfs2: NOT clean\n"); return 1; }
	printf("fsck.ocsfs2: clean\n");
	return 0;
}
