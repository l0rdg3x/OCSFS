// SPDX-License-Identifier: GPL-2.0-only
/* Force an OCSFS2 online autogrow check. usage: growfs_tool <path-on-fs> */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define OCSFS_IOC_GROWFS  _IO('O', 0x02)

int main(int argc, char **argv)
{
	if (argc != 2) { fprintf(stderr, "usage: %s <path>\n", argv[0]); return 2; }
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }
	if (ioctl(fd, OCSFS_IOC_GROWFS)) { perror("OCSFS_IOC_GROWFS"); return 1; }
	printf("growfs ok\n");
	return 0;
}
