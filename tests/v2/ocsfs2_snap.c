// SPDX-License-Identifier: GPL-2.0-only
/* ocsfs2_snap <srcfile> <snapname> — create a point-in-time reflink snapshot
 * of <srcfile> named <snapname> in the same directory, via the OCSFS2 ioctl. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define OCSFS2_MAX_NAME 255
#define OCSFS_IOC_SNAP_CREATE  _IOW('O', 0x01, char[OCSFS2_MAX_NAME + 1])

int main(int argc, char **argv)
{
	char name[OCSFS2_MAX_NAME + 1];
	int fd;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <srcfile> <snapname>\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) { perror("open src"); return 1; }

	memset(name, 0, sizeof(name));
	strncpy(name, argv[2], OCSFS2_MAX_NAME);
	if (ioctl(fd, OCSFS_IOC_SNAP_CREATE, name)) {
		perror("ioctl OCSFS_IOC_SNAP_CREATE");
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}
