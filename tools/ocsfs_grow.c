// SPDX-License-Identifier: GPL-2.0-only
/*
 * ocsfs_grow — grow an OCSFS volume into an expanded backing LUN (offline).
 *
 * The primary AG-descriptor array has no slack, so new AGs are described in an
 * extension region placed in the newly-added space (INCOMPAT_AG_GROW).  Each AG
 * descriptor carries absolute geometry, so no existing data is moved.
 *
 * The volume MUST be unmounted on every node.  v1 supports a single grow
 * (s_ag_desc_ext_off must be 0); growing an already-grown volume is rejected.
 *
 * Usage: ocsfs_grow [-n] <device>      (-n = dry run, report only)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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

#include "ocsfs.h"

/* Online-grow ioctl (mirrors kmod/ocsfs.h OCSFS_IOC_GROW). */
#define OCSFS_IOC_GROW   _IO('O', 40)

/* Online grow: trigger OCSFS_IOC_GROW on the mounted filesystem.  The ioctl is
 * served by regular-file fops, so create a throwaway file to issue it on. */
static int grow_online(const char *mnt)
{
	char trig[4096];
	int fd, r, e;

	snprintf(trig, sizeof(trig), "%s/.ocsfs-grow-trigger", mnt);
	fd = open(trig, O_RDWR | O_CREAT, 0600);
	if (fd < 0) {
		fprintf(stderr, "ocsfs_grow: cannot create trigger in %s: %s\n",
			mnt, strerror(errno));
		return 1;
	}
	r = ioctl(fd, OCSFS_IOC_GROW);
	e = errno;
	close(fd);
	unlink(trig);
	if (r < 0) {
		if (e == ENOSPC)
			fprintf(stderr, "ocsfs_grow: no new space — expand the LUN and "
					"rescan (iscsiadm -m node -R) on this node first\n");
		else
			fprintf(stderr, "ocsfs_grow: online grow failed: %s\n", strerror(e));
		return 1;
	}
	printf("ocsfs_grow: online grow done (see dmesg / df). Peers pick up the new "
	       "space on their next allocation.\n");
	return 0;
}

static uint64_t get_device_size(int fd)
{
	uint64_t size = 0;
	struct stat st;

	if (ioctl(fd, BLKGETSIZE64, &size) == 0)
		return size;
	if (fstat(fd, &st) == 0)
		return (uint64_t)st.st_size;
	return 0;
}

static void die(const char *msg)
{
	fprintf(stderr, "ocsfs_grow: %s: %s\n", msg, strerror(errno));
	exit(1);
}

static void rd(int fd, uint64_t off, void *buf, size_t len)
{
	if (pread(fd, buf, len, off) != (ssize_t)len)
		die("read");
}

static void wr(int fd, uint64_t off, const void *buf, size_t len)
{
	if (pwrite(fd, buf, len, off) != (ssize_t)len)
		die("write");
}

static void zero_range(int fd, uint64_t off, uint64_t len)
{
	static uint8_t z[1 << 20];
	while (len) {
		uint64_t c = len < sizeof(z) ? len : sizeof(z);
		wr(fd, off, z, c);
		off += c;
		len -= c;
	}
}

int main(int argc, char **argv)
{
	int dry = 0, argi = 1;

	if (argc > 1 && strcmp(argv[1], "-n") == 0) { dry = 1; argi = 2; }
	if (argi >= argc) {
		fprintf(stderr, "usage: ocsfs_grow [-n] <device>\n");
		return 2;
	}

	/* A directory argument is a mountpoint -> ONLINE grow via ioctl (volume
	 * stays mounted).  A block device -> offline grow (must be unmounted). */
	{
		struct stat pst;

		if (stat(argv[argi], &pst) == 0 && S_ISDIR(pst.st_mode)) {
			if (dry) {
				printf("ocsfs_grow: online mode does not support -n; "
				       "run without -n to grow the mounted volume\n");
				return 0;
			}
			return grow_online(argv[argi]);
		}
	}

	int fd = open(argv[argi], dry ? O_RDONLY : O_RDWR);
	if (fd < 0)
		die("open device");

	struct ocsfs_superblock sb;
	rd(fd, OCSFS_SUPERBLOCK_OFFSET, &sb, sizeof(sb));
	if (sb.s_magic != OCSFS_MAGIC) {
		fprintf(stderr, "ocsfs_grow: not an OCSFS volume (bad magic)\n");
		return 1;
	}
	{
		uint32_t crc = ocsfs_crc32c(~0U, &sb, OCSFS_SUPERBLOCK_SIZE - 4);
		/* mkfs seeds the SB checksum with 0 in some revisions; accept either. */
		uint32_t crc0 = ocsfs_crc32c(0, &sb, OCSFS_SUPERBLOCK_SIZE - 4);
		if (crc != sb.s_checksum && crc0 != sb.s_checksum)
			fprintf(stderr, "ocsfs_grow: warning: SB checksum mismatch, continuing\n");
	}

	if (sb.s_ag_desc_ext_off != 0) {
		fprintf(stderr, "ocsfs_grow: volume already grown (ext region present); "
				"multi-grow not yet supported\n");
		return 1;
	}

	uint32_t bs        = sb.s_block_size;
	uint64_t ag_blocks = sb.s_ag_size;
	uint64_t ag_bytes  = ag_blocks * bs;
	uint32_t old_ags   = sb.s_ag_count;
	uint64_t data_off  = sb.s_data_off;
	uint64_t old_end   = data_off + (uint64_t)old_ags * ag_bytes;
	uint64_t dev_size  = get_device_size(fd);

	if (dev_size <= old_end) {
		fprintf(stderr, "ocsfs_grow: no new space (device %llu, current end %llu) "
				"— expand the LUN first\n",
			(unsigned long long)dev_size, (unsigned long long)old_end);
		return 1;
	}

	uint64_t avail = dev_size - old_end;
	/* Each new AG costs its data (ag_bytes) plus one 4096-byte descriptor in the
	 * extension region. */
	uint64_t per_ag = ag_bytes + sizeof(struct ocsfs_ag_desc);
	uint32_t new_ags = (uint32_t)(avail / per_ag);
	if (new_ags == 0) {
		fprintf(stderr, "ocsfs_grow: new space (%.2f GiB) too small for even one "
				"%llu-block AG\n",
			avail / (double)(1ULL << 30), (unsigned long long)ag_blocks);
		return 1;
	}

	uint64_t ext_off       = old_end; /* extension descriptors here (block aligned) */
	uint64_t new_data_start = ext_off + (uint64_t)new_ags * sizeof(struct ocsfs_ag_desc);

	printf("OCSFS grow:\n");
	printf("  device size:      %.2f GiB\n", dev_size / (double)(1ULL << 30));
	printf("  current AGs:      %u  (data ends at %.2f GiB)\n",
	       old_ags, old_end / (double)(1ULL << 30));
	printf("  adding AGs:       %u  (+%.2f GiB usable)\n",
	       new_ags, (new_ags * ag_bytes) / (double)(1ULL << 30));
	printf("  ext desc region:  @%.2f GiB, %u descriptors\n",
	       ext_off / (double)(1ULL << 30), new_ags);
	if (dry) {
		printf("  (dry run — no changes written)\n");
		return 0;
	}

	uint64_t added_free = 0;
	for (uint32_t j = 0; j < new_ags; j++) {
		uint32_t agno = old_ags + j;
		uint64_t ag_data_start = new_data_start + (uint64_t)j * ag_bytes;

		uint64_t bitmap_blocks = (ag_blocks + (uint64_t)bs * 8 - 1) / ((uint64_t)bs * 8);
		uint64_t inodes_per_ag = ag_blocks / 64;
		if (inodes_per_ag < 64) inodes_per_ag = 64;
		uint64_t inode_table_blocks =
			(inodes_per_ag * OCSFS_INODE_SIZE + bs - 1) / bs;
		uint64_t metadata_blocks = 1 + bitmap_blocks + inode_table_blocks;
		uint64_t ag_free = ag_blocks - metadata_blocks;

		struct ocsfs_ag_desc agd;
		memset(&agd, 0, sizeof(agd));
		agd.ag_magic           = OCSFS_AG_MAGIC;
		agd.ag_number          = agno;
		agd.ag_block_start     = ag_data_start / bs;
		agd.ag_block_count     = ag_blocks;
		agd.ag_free_blocks     = ag_free;
		agd.ag_free_extents    = 1;
		agd.ag_bitmap_off      = bs;                      /* block 1 within AG */
		agd.ag_bitmap_size     = bitmap_blocks * bs;
		agd.ag_inode_table_off = (1 + bitmap_blocks) * bs;
		agd.ag_inode_count     = inodes_per_ag;
		agd.ag_free_inodes     = inodes_per_ag;
		agd.ag_owner_node      = agno % (sb.s_max_nodes ? sb.s_max_nodes : 1);
		agd.ag_checksum        = ocsfs_crc32c(0, &agd, sizeof(agd) - sizeof(uint32_t));

		/* Zero this AG's metadata region (the new LUN space may hold stale data
		 * that would otherwise be misread as inodes / bitmap). */
		zero_range(fd, ag_data_start, metadata_blocks * bs);

		/* Descriptor: in the extension region AND a copy at the AG data start. */
		wr(fd, ext_off + (uint64_t)j * sizeof(agd), &agd, sizeof(agd));
		wr(fd, ag_data_start, &agd, sizeof(agd));

		/* Bitmap: mark the metadata blocks used, the rest free. */
		uint8_t *bitmap = calloc(1, bitmap_blocks * bs);
		if (!bitmap) die("bitmap alloc");
		for (uint64_t b = 0; b < metadata_blocks; b++)
			bitmap[b / 8] |= (1u << (b % 8));
		wr(fd, ag_data_start + bs, bitmap, bitmap_blocks * bs);
		free(bitmap);

		added_free += ag_free;
	}

	/* Update the superblock. */
	sb.s_ag_count               = old_ags + new_ags;
	sb.s_total_blocks          += (uint64_t)new_ags * ag_blocks;
	sb.s_free_blocks           += added_free;
	sb.s_ag_desc_primary_count  = old_ags;   /* legacy was 0 -> pin the original AGs */
	sb.s_ag_desc_ext_off        = ext_off;
	sb.s_feature_incompat      |= OCSFS_FEATURE_INCOMPAT_AG_GROW;
	/* mkfs seeds the SB checksum with 0, and the kernel's ocsfs_crc32c(~0U,...)
	 * validation is equal to this userspace ocsfs_crc32c(0,...); use seed 0 to
	 * match (a ~0U seed here writes a checksum the kernel rejects). */
	sb.s_checksum = ocsfs_crc32c(0, &sb, OCSFS_SUPERBLOCK_SIZE - 4);

	wr(fd, OCSFS_SUPERBLOCK_OFFSET, &sb, sizeof(sb));
	wr(fd, OCSFS_SUPERBLOCK_MIRROR, &sb, sizeof(sb));

	if (fsync(fd) < 0) die("fsync");
	close(fd);

	printf("  done: %u -> %u AGs, +%.2f GiB usable. Run fsck, then mount.\n",
	       old_ags, old_ags + new_ags,
	       (added_free * (uint64_t)bs) / (double)(1ULL << 30));
	return 0;
}
