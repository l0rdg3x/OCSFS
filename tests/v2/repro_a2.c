// SPDX-License-Identifier: GPL-2.0-only
/* A2 repro: build a multi-leaf extent tree (>169 single-block extents via
 * interleaving), punch the middle (empties a leaf -> stale internal routing),
 * rewrite the whole file, then verify every block. Without the delete-time
 * collapse fix the stale routing makes part of the rewrite read back wrong.
 *   repro_a2 write <F> <G>   ;  (drop caches)  ;  repro_a2 verify <F> */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/falloc.h>

#define BS 4096
#define N  400

int main(int argc, char **argv)
{
	char buf[BS];
	int i;

	if (argc >= 3 && !strcmp(argv[1], "write")) {
		int f = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
		int g = open(argv[3], O_RDWR | O_CREAT | O_TRUNC, 0644);

		if (f < 0 || g < 0) { perror("open"); return 1; }
		for (i = 0; i < N; i++) {            /* interleave -> fragment F */
			memset(buf, i & 0xff, BS); pwrite(f, buf, BS, (off_t)i * BS);
			memset(buf, 0x5a, BS);     pwrite(g, buf, BS, (off_t)i * BS);
		}
		fsync(f); fsync(g);
		/* punch a wide middle range -> empties whole leaf(s) */
		if (fallocate(f, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			      (off_t)100 * BS, (off_t)250 * BS))
			perror("punch");
		fsync(f);
		/* refill the hole in ONE wide write -> a single extent spanning the
		 * emptied leaves' key-range (the condition the stale routing breaks) */
		{
			char *big = malloc((size_t)250 * BS);

			for (i = 0; i < 250; i++)
				memset(big + (size_t)i * BS, ((100 + i) ^ 0xff) & 0xff, BS);
			pwrite(f, big, (size_t)250 * BS, (off_t)100 * BS);
			free(big);
		}
		fsync(f);
		close(f); close(g);
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "verify")) {
		int f = open(argv[2], O_RDONLY), bad = 0;

		if (f < 0) { perror("open"); return 1; }
		for (i = 0; i < N; i++) {
			/* [100,350) was rewritten as (i^0xff); the rest is (i&0xff) */
			char want = (i >= 100 && i < 350) ? ((i ^ 0xff) & 0xff)
						           : (i & 0xff);
			int j;

			if (pread(f, buf, BS, (off_t)i * BS) != BS) { perror("pread"); return 1; }
			for (j = 0; j < BS; j++)
				if (buf[j] != want) {
					if (bad < 8)
						printf("  block %d byte %d: got 0x%02x want 0x%02x\n",
						       i, j, (unsigned char)buf[j], (unsigned char)want);
					bad++; break;
				}
		}
		close(f);
		printf("repro_a2: %d/%d blocks bad -> %s\n", bad, N,
		       bad ? "CORRUPT" : "OK");
		return bad ? 1 : 0;
	}
	fprintf(stderr, "usage: %s write <F> <G> | verify <F>\n", argv[0]);
	return 2;
}
