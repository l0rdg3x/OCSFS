// SPDX-License-Identifier: GPL-2.0-only
/* Run an OCSFS2 online scrub. usage: scrub_tool <path-on-fs>
 * prints: "checked=<n> errors=<n> inodes=<n> ags=<n>" */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>

struct ocsfs2_scrub_result {
	uint64_t checked, errors, inodes;
	uint32_t ag_count, flags;
};
#define OCSFS_IOC_SCRUB _IOWR('O', 0x03, struct ocsfs2_scrub_result)

int main(int argc, char **argv)
{
	struct ocsfs2_scrub_result r;
	int fd;

	if (argc != 2) { fprintf(stderr, "usage: %s <path>\n", argv[0]); return 2; }
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }
	memset(&r, 0, sizeof(r));
	if (ioctl(fd, OCSFS_IOC_SCRUB, &r)) { perror("OCSFS_IOC_SCRUB"); return 1; }
	printf("checked=%llu errors=%llu inodes=%llu ags=%u\n",
	       (unsigned long long)r.checked, (unsigned long long)r.errors,
	       (unsigned long long)r.inodes, r.ag_count);
	return 0;
}
