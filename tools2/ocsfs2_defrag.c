// SPDX-License-Identifier: GPL-2.0-only
/*
 * ocsfs2-defrag — online defragmentation for OCSFS v2.
 *
 *   ocsfs2-defrag [-r] [-n] [-t MIN] <path>
 *     -r        recurse into a directory and defrag every regular file
 *     -n        dry-run: only report each file's extent count
 *     -t MIN    only defrag files with more than MIN extents (default 8)
 *
 * Counts extents with FIEMAP; if fragmented past the threshold, issues
 * OCSFS_IOC_DEFRAG, which relocates the file's private data into contiguous
 * runs (online, journaled; shared/reflinked extents are left intact).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <getopt.h>
#include <ftw.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

struct ocsfs2_defrag_result {
	uint64_t extents_before, extents_after, blocks_relocated, runs_relocated;
};
#define OCSFS_IOC_DEFRAG _IOWR('O', 0x04, struct ocsfs2_defrag_result)

static int g_min = 8;
static int g_dry = 0;
static int g_rc = 0;          /* worst exit code */

/* total mapped extents of an open fd, or -1 on error */
static long count_extents(int fd)
{
	struct fiemap fm;

	memset(&fm, 0, sizeof(fm));
	fm.fm_start = 0;
	fm.fm_length = ~0ULL;
	fm.fm_extent_count = 0;          /* count-only query */
	if (ioctl(fd, FS_IOC_FIEMAP, &fm))
		return -1;
	return (long)fm.fm_mapped_extents;
}

static void defrag_one(const char *path)
{
	struct ocsfs2_defrag_result r;
	long before;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		fprintf(stderr, "ocsfs2-defrag: %s: %s\n", path, strerror(errno));
		g_rc = 1;
		return;
	}
	before = count_extents(fd);
	if (before < 0) {
		fprintf(stderr, "ocsfs2-defrag: %s: FIEMAP: %s\n", path,
			strerror(errno));
		close(fd);
		g_rc = 1;
		return;
	}
	if (before <= g_min) {           /* not fragmented enough */
		if (g_dry)
			printf("%s: %ld extents (skip, <= %d)\n", path, before, g_min);
		close(fd);
		return;
	}
	if (g_dry) {
		printf("%s: %ld extents (would defrag)\n", path, before);
		close(fd);
		return;
	}
	memset(&r, 0, sizeof(r));
	if (ioctl(fd, OCSFS_IOC_DEFRAG, &r)) {
		fprintf(stderr, "ocsfs2-defrag: %s: %s\n", path, strerror(errno));
		g_rc = 1;
		close(fd);
		return;
	}
	printf("%s: %llu -> %llu extents (%llu runs, %llu blocks moved)\n",
	       path, (unsigned long long)r.extents_before,
	       (unsigned long long)r.extents_after,
	       (unsigned long long)r.runs_relocated,
	       (unsigned long long)r.blocks_relocated);
	close(fd);
}

static int walk_cb(const char *path, const struct stat *st, int type,
		   struct FTW *ftw)
{
	(void)ftw;
	if (type == FTW_F && S_ISREG(st->st_mode))
		defrag_one(path);
	return 0;
}

int main(int argc, char **argv)
{
	int recursive = 0, opt;

	while ((opt = getopt(argc, argv, "rnt:h")) != -1) {
		switch (opt) {
		case 'r': recursive = 1; break;
		case 'n': g_dry = 1; break;
		case 't': g_min = atoi(optarg); break;
		default:
			fprintf(stderr,
				"usage: %s [-r] [-n] [-t MIN] <path>\n", argv[0]);
			return 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "usage: %s [-r] [-n] [-t MIN] <path>\n", argv[0]);
		return 2;
	}

	for (; optind < argc; optind++) {
		struct stat st;

		if (stat(argv[optind], &st)) {
			fprintf(stderr, "ocsfs2-defrag: %s: %s\n", argv[optind],
				strerror(errno));
			g_rc = 1;
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (!recursive) {
				fprintf(stderr,
					"ocsfs2-defrag: %s is a directory (use -r)\n",
					argv[optind]);
				g_rc = 1;
				continue;
			}
			nftw(argv[optind], walk_cb, 32, FTW_PHYS | FTW_MOUNT);
		} else if (S_ISREG(st.st_mode)) {
			defrag_one(argv[optind]);
		} else {
			fprintf(stderr, "ocsfs2-defrag: %s: not a regular file\n",
				argv[optind]);
			g_rc = 1;
		}
	}
	return g_rc;
}
