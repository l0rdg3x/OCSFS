// SPDX-License-Identifier: GPL-2.0-only
/*
 * ocsfs2-scrub — online metadata scrub (and online fsck engine) for OCSFS v2.
 *
 *   ocsfs2-scrub [-q] <mountpoint-or-path>
 *     -q   print only the one-line summary
 *
 * Issues OCSFS_IOC_SCRUB, which verifies every metadata checksum (super, AG
 * headers, used inodes, extent + refcount B+tree nodes, xattr blocks) across the
 * live filesystem. Exit code 0 = clean, 1 = checksum findings, 2 = usage/error.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <getopt.h>
#include <sys/ioctl.h>

struct ocsfs2_scrub_result {
	uint64_t checked, errors, inodes;
	uint32_t ag_count, flags;
};
#define OCSFS_IOC_SCRUB _IOWR('O', 0x03, struct ocsfs2_scrub_result)

int main(int argc, char **argv)
{
	struct ocsfs2_scrub_result r;
	int fd, opt, quiet = 0;
	const char *path;

	while ((opt = getopt(argc, argv, "qh")) != -1) {
		if (opt == 'q') quiet = 1;
		else { fprintf(stderr, "usage: %s [-q] <mountpoint>\n", argv[0]); return 2; }
	}
	if (optind >= argc) {
		fprintf(stderr, "usage: %s [-q] <mountpoint>\n", argv[0]);
		return 2;
	}
	path = argv[optind];

	fd = open(path, O_RDONLY);
	if (fd < 0) { perror("open"); return 2; }
	memset(&r, 0, sizeof(r));
	if (ioctl(fd, OCSFS_IOC_SCRUB, &r)) { perror("OCSFS_IOC_SCRUB"); close(fd); return 2; }
	close(fd);

	if (!quiet)
		printf("ocsfs2-scrub: %s — checked %llu objects / %llu inodes / %u AGs\n",
		       path, (unsigned long long)r.checked,
		       (unsigned long long)r.inodes, r.ag_count);
	printf("ocsfs2-scrub: %s — %llu errors -> %s\n", path,
	       (unsigned long long)r.errors, r.errors ? "FINDINGS" : "CLEAN");
	return r.errors ? 1 : 0;
}
