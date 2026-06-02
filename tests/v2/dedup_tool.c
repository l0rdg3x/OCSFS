// SPDX-License-Identifier: GPL-2.0-only
/* Minimal FIDEDUPERANGE driver: dedup [0,len) of dst against src.
 * usage: dedup_tool <src> <dst> <len_bytes>
 * prints: "deduped=<bytes> status=<n>"  (status 0 = ok, -EBADE = data differs) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

int main(int argc, char **argv)
{
	if (argc != 4) { fprintf(stderr, "usage: %s <src> <dst> <len>\n", argv[0]); return 2; }
	unsigned long long len = strtoull(argv[3], NULL, 0);

	int sfd = open(argv[1], O_RDONLY);
	int dfd = open(argv[2], O_RDWR);
	if (sfd < 0 || dfd < 0) { perror("open"); return 1; }

	size_t sz = sizeof(struct file_dedupe_range) + sizeof(struct file_dedupe_range_info);
	struct file_dedupe_range *r = calloc(1, sz);
	r->src_offset = 0;
	r->src_length = len;
	r->dest_count = 1;
	r->info[0].dest_fd = dfd;
	r->info[0].dest_offset = 0;

	if (ioctl(sfd, FIDEDUPERANGE, r) < 0) { perror("FIDEDUPERANGE"); return 1; }
	printf("deduped=%llu status=%d\n",
	       (unsigned long long)r->info[0].bytes_deduped, r->info[0].status);
	return 0;
}
