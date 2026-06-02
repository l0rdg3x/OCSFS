// SPDX-License-Identifier: GPL-2.0-only
/*
 * ocsfs2-tool — OCSFS v2 admin multi-tool.
 *
 *   ocsfs2-tool snapshot <srcfile> <snapname>   point-in-time reflink copy
 *   ocsfs2-tool growfs   <path-on-fs>           force an online autogrow check
 *
 * (scrub and defrag have their own binaries: ocsfs2-scrub, ocsfs2-defrag.)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define OCSFS2_MAX_NAME 255
#define OCSFS_IOC_SNAP_CREATE  _IOW('O', 0x01, char[OCSFS2_MAX_NAME + 1])
#define OCSFS_IOC_GROWFS       _IO('O', 0x02)

static int do_snapshot(int argc, char **argv)
{
	char name[OCSFS2_MAX_NAME + 1];
	int fd;

	if (argc != 4) {
		fprintf(stderr, "usage: %s snapshot <srcfile> <snapname>\n", argv[0]);
		return 2;
	}
	fd = open(argv[2], O_RDONLY);
	if (fd < 0) { perror("open src"); return 1; }
	memset(name, 0, sizeof(name));
	strncpy(name, argv[3], OCSFS2_MAX_NAME);
	if (ioctl(fd, OCSFS_IOC_SNAP_CREATE, name)) {
		perror("OCSFS_IOC_SNAP_CREATE");
		close(fd);
		return 1;
	}
	close(fd);
	printf("snapshot: %s -> %s\n", argv[2], argv[3]);
	return 0;
}

static int do_growfs(int argc, char **argv)
{
	int fd;

	if (argc != 3) {
		fprintf(stderr, "usage: %s growfs <path-on-fs>\n", argv[0]);
		return 2;
	}
	fd = open(argv[2], O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }
	if (ioctl(fd, OCSFS_IOC_GROWFS)) {
		perror("OCSFS_IOC_GROWFS");
		close(fd);
		return 1;
	}
	close(fd);
	printf("growfs: ok\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <snapshot|growfs> ...\n"
			"  %s snapshot <srcfile> <snapname>\n"
			"  %s growfs   <path-on-fs>\n",
			argv[0], argv[0], argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "snapshot"))
		return do_snapshot(argc, argv);
	if (!strcmp(argv[1], "growfs"))
		return do_growfs(argc, argv);
	fprintf(stderr, "%s: unknown command '%s'\n", argv[0], argv[1]);
	return 2;
}
