// SPDX-License-Identifier: GPL-2.0-only
/*
 * mkfs.ocsfs2 — authoritative formatter for the OCSFS v2 on-disk format.
 *
 * Layout: SB | SB-mirror | node-table | heartbeat | lease-table | recovery |
 *         journal[max_nodes] | AG[0..ag_count).
 * Each AG: header-block | block-bitmap | inode-table | data-blocks.
 *
 * The cluster regions (node/heartbeat/lease/recovery/journal) are written with
 * valid zeroed headers but are not used by the single-node Plan-1 kernel code.
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
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <endian.h>

#define BS                 OCSFS2_BLOCK_SIZE
#define AG_TARGET_BLOCKS   (262144ULL)        /* 1 GiB per AG */
#define INODE_PER_N_BLOCKS 64ULL              /* 1 inode per 64 blocks (256 KiB) */
#define MIN_INODES_PER_AG  256ULL

struct layout {
	uint64_t dev_blocks;
	uint64_t node_off,  node_blocks;
	uint64_t hb_off,    hb_blocks;
	uint64_t lease_off, lease_count, lease_blocks;
	uint64_t recovery_off;            /* 1 block */
	uint64_t journal_off, journal_size, journal_blocks_total;
	uint64_t ag_region_start;         /* first AG block index */
	uint64_t ag_blocks;               /* standard block span per AG */
	uint64_t inodes_per_ag;
	uint32_t ag_count;
	uint16_t max_nodes;
};

static uint64_t divup(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

/* Pure: compute the on-disk layout. Returns 0 on success, -1 if device too small. */
static int compute_layout(uint64_t dev_size, uint16_t max_nodes,
			  uint64_t journal_size, struct layout *L)
{
	uint64_t cur, remaining;

	memset(L, 0, sizeof(*L));
	L->dev_blocks = dev_size / BS;
	L->max_nodes  = max_nodes;
	if (journal_size % BS)
		journal_size = divup(journal_size, BS) * BS;
	L->journal_size = journal_size;

	cur = 2;  /* SB + mirror */

	L->node_blocks = divup((uint64_t)max_nodes * OCSFS2_NODE_SLOT_SIZE, BS);
	L->node_off    = cur * BS;  cur += L->node_blocks;

	L->hb_blocks   = divup((uint64_t)max_nodes * OCSFS2_HEARTBEAT_SIZE, BS);
	L->hb_off      = cur * BS;  cur += L->hb_blocks;

	L->lease_count = OCSFS2_DEFAULT_LEASE_COUNT;
	L->lease_blocks = divup(L->lease_count * OCSFS2_LEASE_ENTRY_SIZE, BS);
	L->lease_off   = cur * BS;  cur += L->lease_blocks;

	L->recovery_off = cur * BS; cur += 1;

	L->journal_blocks_total = (journal_size / BS) * (uint64_t)max_nodes;
	L->journal_off = cur * BS;  cur += L->journal_blocks_total;

	L->ag_region_start = cur;
	if (L->dev_blocks <= cur)
		return -1;
	remaining = L->dev_blocks - cur;

	L->ag_blocks = AG_TARGET_BLOCKS;
	if (remaining < L->ag_blocks)
		L->ag_blocks = remaining;          /* single (small) AG */
	L->ag_count = (uint32_t)(remaining / L->ag_blocks);
	if (L->ag_count == 0)
		return -1;

	L->inodes_per_ag = L->ag_blocks / INODE_PER_N_BLOCKS;
	L->inodes_per_ag &= ~7ULL;                 /* multiple of 8 (whole itable blocks) */
	if (L->inodes_per_ag < MIN_INODES_PER_AG)
		L->inodes_per_ag = MIN_INODES_PER_AG;

	return 0;
}

/* Per-AG geometry derived from the layout (all in blocks / absolute byte offsets). */
struct ag_geom {
	uint64_t start_block, block_count;
	uint64_t bitmap_blocks, itable_blocks, meta_blocks;
	uint64_t data_start_block, data_blocks;
	uint64_t bitmap_off, itable_off, data_off;
};

static void ag_geometry(const struct layout *L, uint32_t idx, struct ag_geom *g)
{
	memset(g, 0, sizeof(*g));
	g->start_block = L->ag_region_start + (uint64_t)idx * L->ag_blocks;
	/* Uniform AGs (every AG == ag_blocks): the descriptor formula
	 * ag_region_start + idx*ag_blocks stays valid for AGs appended by online
	 * grow, with no overlap. Any final < ag_blocks tail is left unformatted
	 * (reclaimed by a later grow once it reaches a full AG). */
	g->block_count = L->ag_blocks;

	g->bitmap_blocks = divup(divup(g->block_count, 8), BS);
	g->itable_blocks = (L->inodes_per_ag * OCSFS2_INODE_SIZE) / BS;
	g->meta_blocks   = 1 + g->bitmap_blocks + g->itable_blocks;
	g->data_start_block = g->start_block + g->meta_blocks;
	g->data_blocks   = g->block_count - g->meta_blocks;

	g->bitmap_off = (g->start_block + 1) * BS;
	g->itable_off = (g->start_block + 1 + g->bitmap_blocks) * BS;
	g->data_off   = g->data_start_block * BS;
}

static int pwrite_all(int fd, const void *buf, size_t len, uint64_t off)
{
	const uint8_t *p = buf;
	while (len) {
		ssize_t n = pwrite(fd, p, len, off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n; off += n; len -= n;
	}
	return 0;
}

static int write_zeros(int fd, uint64_t off, uint64_t len)
{
	static uint8_t zbuf[1 << 20];   /* 1 MiB */
	while (len) {
		size_t chunk = len < sizeof(zbuf) ? (size_t)len : sizeof(zbuf);
		if (pwrite_all(fd, zbuf, chunk, off))
			return -1;
		off += chunk; len -= chunk;
	}
	return 0;
}

static void set_bit(uint8_t *bm, uint64_t i) { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }

int main(int argc, char **argv)
{
	const char *label = "ocsfs2";
	uint16_t max_nodes = OCSFS2_DEFAULT_MAX_NODES;
	uint64_t journal_size = 16ULL << 20;
	int force = 0, opt, fd;
	const char *dev;
	uint64_t dev_size = 0, format_size = 0;
	struct stat st;
	struct layout L;
	uint64_t total_data_blocks = 0;

	while ((opt = getopt(argc, argv, "L:N:j:s:f")) != -1) {
		switch (opt) {
		case 'L': label = optarg; break;
		case 'N': max_nodes = (uint16_t)atoi(optarg); break;
		case 'j': journal_size = strtoull(optarg, NULL, 0); break;
		case 's': format_size = strtoull(optarg, NULL, 0); break;  /* format a prefix (test grow) */
		case 'f': force = 1; break;
		default:
			fprintf(stderr, "usage: %s [-L label] [-N max_nodes] [-j journal_bytes] [-s format_bytes] [-f] <device>\n", argv[0]);
			return 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "error: no device given\n");
		return 2;
	}
	dev = argv[optind];
	if (max_nodes < 1 || max_nodes > 256) {
		fprintf(stderr, "error: max_nodes must be 1..256\n");
		return 2;
	}

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 1; }
	if (fstat(fd, &st)) { perror("fstat"); close(fd); return 1; }
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &dev_size)) { perror("BLKGETSIZE64"); close(fd); return 1; }
	} else if (S_ISREG(st.st_mode)) {
		dev_size = (uint64_t)st.st_size;
	} else {
		fprintf(stderr, "error: %s is not a block device or regular file\n", dev);
		close(fd); return 1;
	}
	if (!force && S_ISBLK(st.st_mode)) {
		/* light guard: refuse if it already looks like an ocsfs2 volume */
		struct ocsfs2_disk_super probe;
		if (pread(fd, &probe, sizeof(probe), 0) == (ssize_t)sizeof(probe) &&
		    le32toh(probe.s_magic) == OCSFS2_MAGIC) {
			fprintf(stderr, "error: %s already has an ocsfs2 superblock (use -f)\n", dev);
			close(fd); return 1;
		}
	}

	if (format_size) {
		if (format_size > dev_size) {
			fprintf(stderr, "error: -s %llu exceeds device size %llu\n",
				(unsigned long long)format_size, (unsigned long long)dev_size);
			close(fd); return 1;
		}
		dev_size = format_size;   /* pretend the device is this small (grow testing) */
	}

	if (compute_layout(dev_size, max_nodes, journal_size, &L)) {
		fprintf(stderr, "error: device too small (%llu bytes)\n",
			(unsigned long long)dev_size);
		close(fd); return 1;
	}

	printf("mkfs.ocsfs2: device=%s size=%llu MiB blocks=%llu\n",
	       dev, (unsigned long long)(dev_size >> 20), (unsigned long long)L.dev_blocks);
	printf("  max_nodes=%u ags=%u ag_blocks=%llu inodes/ag=%llu journal=%llu MiB/node\n",
	       L.max_nodes, L.ag_count, (unsigned long long)L.ag_blocks,
	       (unsigned long long)L.inodes_per_ag, (unsigned long long)(L.journal_size >> 20));

	/* ── cluster regions: zero them (valid "empty" state) ── */
	if (write_zeros(fd, L.node_off, (L.node_blocks + L.hb_blocks + L.lease_blocks + 1) * BS) ||
	    write_zeros(fd, L.journal_off, L.journal_blocks_total * BS)) {
		perror("write cluster regions"); close(fd); return 1;
	}
	/* per-node journal headers */
	for (uint16_t n = 0; n < max_nodes; n++) {
		struct ocsfs2_disk_journal_hdr jh;
		uint64_t joff = L.journal_off + (uint64_t)n * L.journal_size;
		memset(&jh, 0, sizeof(jh));
		jh.jh_magic = htole32(OCSFS2_JOURNAL_MAGIC);
		jh.jh_node_slot = htole16(n);
		jh.jh_size = htole64(L.journal_size);
		jh.jh_checksum = htole32(ocsfs2_crc32c(~0u, &jh,
				 offsetof(struct ocsfs2_disk_journal_hdr, jh_checksum)));
		if (pwrite_all(fd, &jh, sizeof(jh), joff)) { perror("journal hdr"); close(fd); return 1; }
	}

	/* ── per-AG: zero inode table, write bitmap + header ── */
	for (uint32_t i = 0; i < L.ag_count; i++) {
		struct ag_geom g;
		struct ocsfs2_disk_ag ag;
		uint8_t *bm;
		uint64_t used;

		ag_geometry(&L, i, &g);
		if (g.data_blocks == 0 || (i == 0 && g.data_blocks < 1)) {
			fprintf(stderr, "error: AG %u has no data space\n", i);
			close(fd); return 1;
		}
		total_data_blocks += g.data_blocks;

		/* zero the inode table so every unused inode has magic==0 (free) */
		if (write_zeros(fd, g.itable_off, g.itable_blocks * BS)) {
			perror("zero itable"); close(fd); return 1;
		}

		/* bitmap: mark metadata blocks used; AG0 also marks the root dir block */
		bm = calloc(g.bitmap_blocks, BS);
		if (!bm) { perror("calloc bitmap"); close(fd); return 1; }
		for (uint64_t b = 0; b < g.meta_blocks; b++)
			set_bit(bm, b);
		used = g.meta_blocks;
		if (i == 0) {
			set_bit(bm, g.meta_blocks);   /* root dir data block (AG-relative) */
			used++;
		}
		if (pwrite_all(fd, bm, g.bitmap_blocks * BS, g.bitmap_off)) {
			perror("write bitmap"); free(bm); close(fd); return 1;
		}
		free(bm);

		/* AG header */
		memset(&ag, 0, sizeof(ag));
		ag.ag_magic = htole32(OCSFS2_AG_MAGIC);
		ag.ag_number = htole32(i);
		ag.ag_block_start = htole64(g.start_block);
		ag.ag_block_count = htole64(g.block_count);
		ag.ag_free_blocks = htole64(g.block_count - used);
		ag.ag_free_inodes = htole64(i == 0 ? L.inodes_per_ag - 1 : L.inodes_per_ag);
		ag.ag_bitmap_off = htole64(g.bitmap_off);
		ag.ag_bitmap_blocks = htole64(g.bitmap_blocks);
		ag.ag_inode_table_off = htole64(g.itable_off);
		ag.ag_inodes_per_ag = htole64(L.inodes_per_ag);
		ag.ag_data_off = htole64(g.data_off);
		ag.ag_data_blocks = htole64(g.data_blocks);
		ag.ag_rc_btree_root = htole64(0);
		ag.ag_checksum = htole32(ocsfs2_crc32c(~0u, &ag,
				 offsetof(struct ocsfs2_disk_ag, ag_checksum)));
		if (pwrite_all(fd, &ag, sizeof(ag), g.start_block * BS)) {
			perror("write ag header"); close(fd); return 1;
		}
	}

	/* ── root inode (ino=2) + root dir block in AG0 ── */
	{
		struct ag_geom g0;
		struct ocsfs2_disk_inode ri;
		struct ocsfs2_disk_extent ext;
		uint8_t dirblk[BS];
		struct ocsfs2_disk_dirent *de;
		uint64_t now = (uint64_t)time(NULL);

		ag_geometry(&L, 0, &g0);

		/* dir block: "." and ".." both point at ino 2 */
		memset(dirblk, 0, sizeof(dirblk));
		de = (struct ocsfs2_disk_dirent *)dirblk;
		de->de_magic = htole32(OCSFS2_DIRENT_MAGIC);
		de->de_ino = htole64(OCSFS2_ROOT_INO);
		de->de_file_type = OCSFS2_FT_DIR;
		de->de_name_len = 1;
		de->de_name[0] = '.';
		de->de_name_hash = htole64(0); /* hash unused for . / .. */
		de->de_checksum = htole32(ocsfs2_crc32c(~0u, de, OCSFS2_DIRENT_SIZE));
		de = (struct ocsfs2_disk_dirent *)(dirblk + OCSFS2_DIRENT_SIZE);
		de->de_magic = htole32(OCSFS2_DIRENT_MAGIC);
		de->de_ino = htole64(OCSFS2_ROOT_INO);
		de->de_file_type = OCSFS2_FT_DIR;
		de->de_name_len = 2;
		de->de_name[0] = '.'; de->de_name[1] = '.';
		de->de_checksum = htole32(ocsfs2_crc32c(~0u, de, OCSFS2_DIRENT_SIZE));
		if (pwrite_all(fd, dirblk, BS, g0.data_off)) {
			perror("write root dir block"); close(fd); return 1;
		}

		/* root inode */
		memset(&ri, 0, sizeof(ri));
		ri.i_magic = htole32(OCSFS2_INODE_MAGIC);
		ri.i_generation = htole32(1);
		ri.i_ino = htole64(OCSFS2_ROOT_INO);
		ri.i_mode = htole16(0040000 | 0755);   /* S_IFDIR | 0755 */
		ri.i_nlink = htole16(2);
		ri.i_size = htole64(BS);
		ri.i_blocks = htole64(BS / 512);
		ri.i_atime = ri.i_mtime = ri.i_ctime = htole64(now);
		ri.i_extent_count = htole16(1);
		ri.i_dirent_count = htole32(2);
		/* inline extent[0] = root dir block */
		memset(&ext, 0, sizeof(ext));
		ext.e_logical = htole64(0);
		ext.e_physical = htole64(g0.data_start_block);
		ext.e_length = htole32(1);
		ext.e_flags = htole16(OCSFS2_FT_UNKNOWN); /* OCSFS2_EXT_WRITTEN == 0 */
		memcpy(ri.i_inline_extents, &ext, sizeof(ext));
		ri.i_checksum = htole32(ocsfs2_crc32c(~0u, &ri,
				offsetof(struct ocsfs2_disk_inode, i_checksum)));
		if (pwrite_all(fd, &ri, sizeof(ri), g0.itable_off + OCSFS2_ROOT_INO * OCSFS2_INODE_SIZE)) {
			perror("write root inode"); close(fd); return 1;
		}
	}

	/* ── superblock (primary + mirror), written last ── */
	{
		struct ocsfs2_disk_super sb;
		struct ag_geom g0;
		ag_geometry(&L, 0, &g0);

		memset(&sb, 0, sizeof(sb));
		sb.s_magic = htole32(OCSFS2_MAGIC);
		sb.s_major = htole16(OCSFS2_VERSION_MAJOR);
		sb.s_minor = htole16(OCSFS2_VERSION_MINOR);
		/* simple deterministic UUID from time (not security-sensitive) */
		for (int k = 0; k < 16; k++) sb.s_uuid[k] = (uint8_t)((time(NULL) >> (k & 7)) + k * 7);
		strncpy((char *)sb.s_label, label, OCSFS2_MAX_LABEL - 1);
		sb.s_block_size = htole32(BS);
		sb.s_inode_size = htole32(OCSFS2_INODE_SIZE);
		/* span the FS manages: the uniform AG region (any sub-AG device tail is
		 * unformatted until a grow reaches a full AG). */
		sb.s_total_blocks = htole64(L.ag_region_start + (uint64_t)L.ag_count * L.ag_blocks);
		sb.s_free_blocks = htole64(total_data_blocks - 1);  /* minus root dir block */
		sb.s_feat_compat = htole64(OCSFS2_FEAT_COMPAT_AUTOGROW);
		sb.s_total_inodes = htole64((uint64_t)L.ag_count * L.inodes_per_ag);
		sb.s_free_inodes = htole64((uint64_t)L.ag_count * L.inodes_per_ag - 1);
		sb.s_ag_count = htole32(L.ag_count);
		sb.s_max_nodes = htole16(L.max_nodes);
		sb.s_ag_size = htole64(L.inodes_per_ag);   /* inode span per AG */
		sb.s_ag_blocks = htole64(L.ag_blocks);
		sb.s_node_table_off = htole64(L.node_off);
		sb.s_heartbeat_off = htole64(L.hb_off);
		sb.s_lease_table_off = htole64(L.lease_off);
		sb.s_lease_count = htole64(L.lease_count);
		sb.s_recovery_off = htole64(L.recovery_off);
		sb.s_journal_off = htole64(L.journal_off);
		sb.s_journal_size = htole64(L.journal_size);
		sb.s_ag_desc_off = htole64(L.ag_region_start * BS);
		sb.s_data_off = htole64(g0.data_off);
		sb.s_mkfs_time = htole64((uint64_t)time(NULL));
		sb.s_mount_count = htole64(0);
		sb.s_inodes_per_ag = htole64(L.inodes_per_ag);
		sb.s_checksum = htole32(ocsfs2_crc32c(~0u, &sb,
				offsetof(struct ocsfs2_disk_super, s_checksum)));
		if (pwrite_all(fd, &sb, sizeof(sb), 0) ||
		    pwrite_all(fd, &sb, sizeof(sb), BS)) {
			perror("write superblock"); close(fd); return 1;
		}
	}

	if (fsync(fd)) { perror("fsync"); close(fd); return 1; }
	close(fd);
	printf("mkfs.ocsfs2: done. free_blocks=%llu total_inodes=%llu\n",
	       (unsigned long long)(total_data_blocks - 1),
	       (unsigned long long)((uint64_t)L.ag_count * L.inodes_per_ag));
	return 0;
}
