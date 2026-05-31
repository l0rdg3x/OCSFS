// SPDX-License-Identifier: GPL-2.0-only
/*
 * ocsfs_fsx — compact fsx-style data-integrity fuzzer.
 *
 * Maintains an in-memory mirror ("good") of the file and performs random
 * pwrite / pread / ftruncate / mmap-write / mmap-read / fallocate operations,
 * verifying every read against the mirror.  Any mismatch = a filesystem bug.
 *
 * Usage: ocsfs_fsx [-N ops] [-l maxfilesize] [-o maxoplen] [-S seed] [-d] <file>
 *   -d : also exercise O_DIRECT aligned read/write (separate aligned ops)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/fiemap.h>
#include <time.h>

static unsigned char *good;          /* in-memory mirror */
static unsigned long  fsxsize = 256 * 1024;   /* max file size */
static unsigned long  maxoplen = 64 * 1024;
static unsigned long  file_size = 0;
static int fd;
static const char *fname;
static unsigned long opnum = 0;
static int do_direct = 0;
static unsigned blksz = 4096;
static int verbose = 0;
static int no_mmap = 0;   /* -M: run mmap ops as buffered pwrite/pread (same off/len) */
static unsigned long watch = ~0UL;   /* watched file offset, logged when touched */

static void logop(const char *t, unsigned long off, unsigned long len)
{
	int hit = (watch != ~0UL && off <= watch && watch < off + len);
	if (verbose || hit)
		printf("op#%-6lu %-9s off=%-7lu len=%-7lu fsize=%-7lu%s\n",
		       opnum, t, off, len, file_size, hit ? "  <== WATCH" : "");
}

/* On a data mismatch, dump the extent that maps the failing offset so we can
 * tell a HOLE (should read zero) from a stale MAPPED block. */
static void dump_fiemap(unsigned long off)
{
	char fmbuf[sizeof(struct fiemap) + 4 * sizeof(struct fiemap_extent)];
	struct fiemap *fm = (void *)fmbuf;
	memset(fmbuf, 0, sizeof(fmbuf));
	fm->fm_start = (off & ~4095UL);
	fm->fm_length = 8192;
	fm->fm_extent_count = 4;
	if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) { fprintf(stderr, "  fiemap ioctl failed\n"); return; }
	fprintf(stderr, "  fiemap around off=%lu: %u extent(s)\n", off, fm->fm_mapped_extents);
	for (unsigned e = 0; e < fm->fm_mapped_extents; e++) {
		struct fiemap_extent *fe = &fm->fm_extents[e];
		fprintf(stderr, "    logical=%llu phys=%llu len=%llu flags=0x%x\n",
			(unsigned long long)fe->fe_logical, (unsigned long long)fe->fe_physical,
			(unsigned long long)fe->fe_length, fe->fe_flags);
	}
	if (fm->fm_mapped_extents == 0)
		fprintf(stderr, "    (HOLE at off=%lu — should read zero)\n", off);
}

static void fail(const char *what, unsigned long off, unsigned long len)
{
	fprintf(stderr, "\n*** FSX FAIL op#%lu %s off=%lu len=%lu file_size=%lu: %s\n",
		opnum, what, off, len, file_size, strerror(errno));
	exit(1);
}

/* Verify buf[0..len) == good[off..off+len). */
static void check(const unsigned char *buf, unsigned long off, unsigned long len,
		  const char *what)
{
	unsigned long i;
	for (i = 0; i < len; i++) {
		if (buf[i] != good[off + i]) {
			fprintf(stderr, "\n*** DATA MISMATCH op#%lu %s off=%lu len=%lu: "
				"byte %lu (file off %lu) got=0x%02x want=0x%02x\n",
				opnum, what, off, len, i, off + i, buf[i], good[off + i]);
			dump_fiemap(off + i);
			{
				unsigned char b2[8];
				if (pread(fd, b2, 1, off + i) == 1)
					fprintf(stderr, "  re-pread (same fd): 0x%02x\n", b2[0]);
				if (system("sync; echo 1 > /proc/sys/vm/drop_caches 2>/dev/null")) {}
				if (pread(fd, b2, 1, off + i) == 1)
					fprintf(stderr, "  re-pread after drop_caches: 0x%02x %s\n",
						b2[0], b2[0] ? "(STILL STALE = on-disk/extent bug)"
							     : "(now ZERO = page-cache invalidation bug)");
			}
			exit(2);
		}
	}
}

static unsigned long rnd(unsigned long n) { return n ? (unsigned long)random() % n : 0; }

static void op_write(void)
{
	unsigned long off = rnd(fsxsize);
	unsigned long len = 1 + rnd(maxoplen);
	unsigned char pat = (unsigned char)(opnum * 7 + 1);
	unsigned char *buf;
	if (off + len > fsxsize) len = fsxsize - off;
	if (!len) return;
	logop("write", off, len);
	buf = malloc(len);
	memset(buf, pat, len);
	if (pwrite(fd, buf, len, off) != (ssize_t)len) { free(buf); fail("pwrite", off, len); }
	/* Writing past EOF zero-fills the [file_size, off) gap on disk. */
	if (off > file_size) memset(good + file_size, 0, off - file_size);
	memset(good + off, pat, len);
	if (off + len > file_size) file_size = off + len;
	free(buf);
}

static void op_read(void)
{
	if (!file_size) return;
	unsigned long off = rnd(file_size);
	unsigned long len = 1 + rnd(maxoplen);
	unsigned char *buf;
	ssize_t r;
	if (off + len > file_size) len = file_size - off;
	if (!len) return;
	logop("read", off, len);
	buf = malloc(len);
	r = pread(fd, buf, len, off);
	if (r != (ssize_t)len) { free(buf); fail("pread", off, len); }
	check(buf, off, len, "read");
	free(buf);
}

static void op_trunc(void)
{
	unsigned long nsz = rnd(fsxsize);
	logop("trunc", nsz, 0);
	if (ftruncate(fd, nsz) < 0) fail("ftruncate", nsz, 0);
	if (nsz > file_size) memset(good + file_size, 0, nsz - file_size);
	file_size = nsz;
}

static void op_mapwrite(void)
{
	if (!file_size) return;
	unsigned long off = rnd(file_size);
	unsigned long len = 1 + rnd(maxoplen);
	unsigned char pat = (unsigned char)(opnum * 7 + 3);
	unsigned long pg = off & ~(4096UL - 1);
	unsigned long maplen;
	unsigned char *m;
	if (off + len > file_size) len = file_size - off;
	if (!len) return;
	maplen = (off - pg) + len;
	logop("mapwrite", off, len);
	if (no_mmap) {
		unsigned char *buf = malloc(len);
		memset(buf, pat, len);
		if (pwrite(fd, buf, len, off) != (ssize_t)len) { free(buf); fail("pwrite(mw)", off, len); }
		free(buf);
		memset(good + off, pat, len);
		return;
	}
	m = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pg);
	if (m == MAP_FAILED) fail("mmap(w)", pg, maplen);
	memset(m + (off - pg), pat, len);
	if (msync(m, maplen, MS_SYNC) < 0) fail("msync", pg, maplen);
	munmap(m, maplen);
	memset(good + off, pat, len);
}

static void op_mapread(void)
{
	if (!file_size) return;
	unsigned long off = rnd(file_size);
	unsigned long len = 1 + rnd(maxoplen);
	unsigned long pg = off & ~(4096UL - 1);
	unsigned long maplen;
	unsigned char *m;
	if (off + len > file_size) len = file_size - off;
	if (!len) return;
	maplen = (off - pg) + len;
	logop("mapread", off, len);
	if (no_mmap) {
		unsigned char *buf = malloc(len);
		if (pread(fd, buf, len, off) != (ssize_t)len) { free(buf); fail("pread(mr)", off, len); }
		check(buf, off, len, "mapread");
		free(buf);
		return;
	}
	m = mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, pg);
	if (m == MAP_FAILED) fail("mmap(r)", pg, maplen);
	check(m + (off - pg), off, len, "mapread");
	munmap(m, maplen);
}

static void op_falloc(void)
{
	unsigned long off = rnd(fsxsize);
	unsigned long len = 1 + rnd(maxoplen);
	if (off + len > fsxsize) len = fsxsize - off;
	if (!len) return;
	/* keep-size fallocate: must not change data or file_size */
	logop("falloc", off, len);
	if (fallocate(fd, FALLOC_FL_KEEP_SIZE, off, len) < 0) {
		if (errno == EOPNOTSUPP) return;
		fail("fallocate", off, len);
	}
}

static void op_punch(void)
{
	if (!file_size) return;
	unsigned long off = rnd(file_size);
	unsigned long len = 1 + rnd(maxoplen);
	if (off + len > file_size) len = file_size - off;
	if (!len) return;
	logop("punch", off, len);
	if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, len) < 0) {
		if (errno == EOPNOTSUPP) return;
		fail("punch", off, len);
	}
	memset(good + off, 0, len);   /* punched range reads as zeroes */
}

/* O_DIRECT aligned write+read+verify on a separate fd. */
static void op_direct(void)
{
	unsigned long off = rnd(fsxsize / blksz) * blksz;
	unsigned long len = (1 + rnd(maxoplen / blksz)) * blksz;
	unsigned char pat = (unsigned char)(opnum * 7 + 5);
	void *buf;
	int dfd;
	if (off + len > fsxsize) len = (fsxsize - off) & ~(unsigned long)(blksz - 1);
	if (!len) return;
	dfd = open(fname, O_RDWR | O_DIRECT);
	if (dfd < 0) { if (errno == EINVAL) { do_direct = 0; return; } fail("open(direct)", off, len); }
	if (posix_memalign(&buf, blksz, len)) { close(dfd); fail("memalign", off, len); }
	memset(buf, pat, len);
	if (pwrite(dfd, buf, len, off) != (ssize_t)len) { free(buf); close(dfd); fail("dwrite", off, len); }
	if (off > file_size) memset(good + file_size, 0, off - file_size);
	memset(good + off, pat, len);
	if (off + len > file_size) file_size = off + len;
	memset(buf, 0, len);
	if (pread(dfd, buf, len, off) != (ssize_t)len) { free(buf); close(dfd); fail("dread", off, len); }
	check(buf, off, len, "directread");
	free(buf);
	close(dfd);
}

int main(int argc, char **argv)
{
	unsigned long nops = 50000;
	unsigned seed = (unsigned)time(NULL);
	int c;
	while ((c = getopt(argc, argv, "N:l:o:S:dvw:M")) != -1) {
		switch (c) {
		case 'N': nops = strtoul(optarg, NULL, 0); break;
		case 'l': fsxsize = strtoul(optarg, NULL, 0); break;
		case 'o': maxoplen = strtoul(optarg, NULL, 0); break;
		case 'S': seed = strtoul(optarg, NULL, 0); break;
		case 'd': do_direct = 1; break;
		case 'v': verbose = 1; break;
		case 'w': watch = strtoul(optarg, NULL, 0); break;
		case 'M': no_mmap = 1; break;
		default: fprintf(stderr, "usage: %s [-N ops][-l size][-o oplen][-S seed][-d] file\n", argv[0]); return 2;
		}
	}
	if (optind >= argc) { fprintf(stderr, "missing file\n"); return 2; }
	fname = argv[optind];
	if (maxoplen > fsxsize) maxoplen = fsxsize;
	srandom(seed);
	good = calloc(1, fsxsize);
	if (!good) { perror("calloc"); return 1; }
	fd = open(fname, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { perror("open"); return 1; }
	printf("ocsfs_fsx: %lu ops, maxsize=%lu, maxoplen=%lu, seed=%u, direct=%d, file=%s\n",
	       nops, fsxsize, maxoplen, seed, do_direct, fname);
	for (opnum = 1; opnum <= nops; opnum++) {
		int op = random() % (do_direct ? 8 : 7);
		switch (op) {
		case 0: op_write();    break;
		case 1: op_read();     break;
		case 2: op_trunc();    break;
		case 3: op_mapwrite(); break;
		case 4: op_mapread();  break;
		case 5: op_falloc();   break;
		case 6: op_punch();    break;
		case 7: op_direct();   break;
		}
		if ((opnum % 5000) == 0) { printf("  op#%lu file_size=%lu\n", opnum, file_size); fflush(stdout); }
	}
	/* final full verify */
	{
		unsigned char *buf = malloc(file_size ? file_size : 1);
		if (file_size) {
			if (pread(fd, buf, file_size, 0) != (ssize_t)file_size) fail("final pread", 0, file_size);
			check(buf, 0, file_size, "final");
		}
		free(buf);
	}
	close(fd);
	printf("ocsfs_fsx: ALL %lu ops verified OK (final file_size=%lu)\n", nops, file_size);
	return 0;
}
