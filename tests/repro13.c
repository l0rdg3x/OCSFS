// SPDX-License-Identifier: GPL-2.0-only
/*
 * repro13 — minimal reproducer for the #13 integrity bug.
 *
 * Stresses block reuse on a small fixed-size file: full-block writes,
 * punch-hole, partial-block writes, with periodic drop_caches and verify.
 * An in-memory mirror is the source of truth. Run on OCSFS and on ext4
 * (must pass on ext4). Usage: repro13 <file> [seed] [iters] [nblocks]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <linux/falloc.h>

#define BS 4096

static int fd;
static unsigned char *mirror;
static long nblocks = 8;
static long fsize;
static int verbose;

static void dropcaches(void)
{
	int f = open("/proc/sys/vm/drop_caches", O_WRONLY);
	if (f >= 0) { if (write(f, "1\n", 2)) {} close(f); }
}

static void verify_block(long blk, const char *what, long opn)
{
	unsigned char rb[BS];
	off_t off = blk * BS;
	ssize_t r = pread(fd, rb, BS, off);
	if (r != BS) { fprintf(stderr, "pread fail blk%ld\n", blk); exit(3); }
	for (int i = 0; i < BS; i++) {
		if (rb[i] != mirror[off + i]) {
			printf("*** MISMATCH op#%ld %s blk=%ld byte=%d (off=%ld) got=0x%02x want=0x%02x\n",
			       opn, what, blk, i, off + i, rb[i], mirror[off + i]);
			dropcaches();
			pread(fd, rb, BS, off);
			printf("    after drop_caches: got=0x%02x\n", rb[i]);
			exit(2);
		}
	}
}

int main(int argc, char **argv)
{
	const char *fn = argc > 1 ? argv[1] : "tf";
	unsigned seed = argc > 2 ? atoi(argv[2]) : 1;
	long iters = argc > 3 ? atol(argv[3]) : 200000;
	if (argc > 4) nblocks = atol(argv[4]);
	verbose = getenv("REPRO_V") != NULL;
	fsize = nblocks * BS;
	srandom(seed);

	mirror = calloc(1, fsize);
	fd = open(fn, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { perror("open"); return 1; }
	if (ftruncate(fd, fsize)) { perror("ftruncate"); return 1; }

	printf("repro13: file=%s seed=%u iters=%ld nblocks=%ld\n", fn, seed, iters, nblocks);
	for (long op = 1; op <= iters; op++) {
		long blk = random() % nblocks;
		off_t off = blk * BS;
		int kind = random() % 5;
		unsigned char pat = (unsigned char)(op * 13 + 1);

		if (kind == 0) {			/* full-block write */
			unsigned char b[BS];
			memset(b, pat, BS);
			if (pwrite(fd, b, BS, off) != BS) { perror("pwrite"); return 1; }
			memset(mirror + off, pat, BS);
			if (verbose) printf("op#%ld WRITE blk=%ld pat=0x%02x\n", op, blk, pat);
		} else if (kind == 1 && getenv("REPRO_NOPUNCH") == NULL) {	/* punch whole block */
			fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, BS);
			memset(mirror + off, 0, BS);
			if (verbose) printf("op#%ld PUNCH blk=%ld\n", op, blk);
		} else if (kind == 2) {			/* partial write (front) */
			long plen = 1 + random() % (BS - 1);
			unsigned char b[BS];
			memset(b, pat, plen);
			if (pwrite(fd, b, plen, off) != plen) { perror("pwrite p"); return 1; }
			memset(mirror + off, pat, plen);
			if (verbose) printf("op#%ld PWRITE blk=%ld [0,%ld) pat=0x%02x\n", op, blk, plen, pat);
		} else if (kind == 3) {			/* partial write (mid/tail) */
			long boff = random() % BS;
			long plen = 1 + random() % (BS - boff);
			unsigned char b[BS];
			memset(b, pat, plen);
			if (pwrite(fd, b, plen, off + boff) != plen) { perror("pwrite t"); return 1; }
			memset(mirror + off + boff, pat, plen);
			if (verbose) printf("op#%ld PWRITE blk=%ld [%ld,%ld) pat=0x%02x\n", op, blk, boff, boff + plen, pat);
		} else {				/* verify (sometimes cold) */
			int cold = (random() % 4 == 0);
			if (cold) { fsync(fd); dropcaches(); }
			if (verbose) printf("op#%ld VERIFY blk=%ld cold=%d\n", op, blk, cold);
			verify_block(blk, "read", op);
		}
		if ((op & 0x3ff) == 0) fsync(fd);
	}
	printf("repro13: ALL %ld ops OK\n", iters);
	return 0;
}
