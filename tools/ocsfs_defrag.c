// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — ocsfs_defrag.c
 * Online defragmentation daemon.
 *
 * Phase 4: Background defragmentation for OCSFS filesystems.
 *
 * This daemon scans files on a mounted OCSFS filesystem, identifies
 * fragmented files, and defragments them by copying data to new
 * contiguous locations.
 *
 * Operation:
 *   1. Scan directory tree for regular files
 *   2. For each file, count extents (fragmentation metric)
 *   3. Prioritize most-fragmented files
 *   4. Defragment: allocate contiguous space, copy data, swap extents
 *   5. Rate-limit I/O to avoid impacting foreground workloads
 *
 * Coordination:
 *   - Single-node operation: only one defrag daemon runs at a time
 *     (uses lock file on the filesystem)
 *   - Non-disruptive: foreground I/O continues during defrag
 *   - Can be paused/resumed via signals (SIGUSR1/SIGUSR2)
 *
 * Usage:
 *   ocsfs-defrag /mnt/ocsfs [options]
 *     -b <MB/s>   Bandwidth limit (default: 50 MB/s)
 *     -t <N>      Fragment threshold (defrag files with > N extents, default: 4)
 *     -n          Dry run (report fragmentation without defragmenting)
 *     -v          Verbose output
 *     -d          Run as daemon (background)
 *     -1          Single pass then exit (no continuous monitoring)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

/* ═══════════════════════════════════════════════════════════════
 * CONFIGURATION
 * ═══════════════════════════════════════════════════════════════ */

#define DEFRAG_VERSION          "0.1.0"
#define DEFRAG_LOCK_FILE        ".ocsfs-defrag.lock"
#define DEFRAG_MAX_EXTENTS      256
#define DEFRAG_COPY_BUF_SIZE    (4 * 1024 * 1024)  /* 4 MiB copy buffer */
#define DEFRAG_DEFAULT_BW_LIMIT 50  /* MB/s */
#define DEFRAG_DEFAULT_THRESHOLD 4  /* min extents to defrag */
#define DEFRAG_SCAN_INTERVAL    60  /* seconds between scans */

/* ═══════════════════════════════════════════════════════════════
 * GLOBAL STATE
 * ═══════════════════════════════════════════════════════════════ */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_paused = 0;

static struct {
	char        mount_path[4096];
	int         bw_limit;       /* MB/s */
	int         threshold;      /* min extents */
	int         dry_run;
	int         verbose;
	int         daemon_mode;
	int         single_pass;

	/* Statistics */
	uint64_t    files_scanned;
	uint64_t    files_fragmented;
	uint64_t    files_defragged;
	uint64_t    bytes_moved;
	uint64_t    extents_before;
	uint64_t    extents_after;
} g_config;

/* ═══════════════════════════════════════════════════════════════
 * SIGNAL HANDLERS
 * ═══════════════════════════════════════════════════════════════ */

static void sig_handler(int sig)
{
	switch (sig) {
	case SIGTERM:
	case SIGINT:
		g_running = 0;
		break;
	case SIGUSR1:
		g_paused = 1;
		break;
	case SIGUSR2:
		g_paused = 0;
		break;
	}
}

static void setup_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);

	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
}

/* ═══════════════════════════════════════════════════════════════
 * LOCK FILE — ensure single instance per mount
 * ═══════════════════════════════════════════════════════════════ */

static int acquire_lock(void)
{
	char lockpath[4200];
	int fd;
	struct flock fl;
	char pidbuf[32];

	snprintf(lockpath, sizeof(lockpath), "%s/%s",
		 g_config.mount_path, DEFRAG_LOCK_FILE);

	fd = open(lockpath, O_WRONLY | O_CREAT, 0600);
	if (fd < 0) {
		fprintf(stderr, "ocsfs-defrag: cannot create lock file: %s\n",
			strerror(errno));
		return -1;
	}

	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;

	if (fcntl(fd, F_SETLK, &fl) < 0) {
		fprintf(stderr, "ocsfs-defrag: another instance is running\n");
		close(fd);
		return -1;
	}

	/* Write our PID to the lock file */
	(void)!ftruncate(fd, 0);
	snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
	(void)!write(fd, pidbuf, strlen(pidbuf));

	/* Keep fd open — lock released when process exits */
	return fd;
}

/* ═══════════════════════════════════════════════════════════════
 * FRAGMENTATION ANALYSIS — using FIEMAP ioctl
 * ═══════════════════════════════════════════════════════════════ */

struct file_frag_info {
	char        path[4096];
	uint64_t    size;
	uint32_t    extent_count;
};

/*
 * Count the number of extents for a file using FIEMAP.
 */
static int count_extents(const char *path, uint32_t *extent_count)
{
	int fd;
	struct fiemap *fm;
	struct fiemap_extent *fe;
	uint32_t count = 0;
	uint64_t start = 0;
	int done = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	fm = calloc(1, sizeof(*fm) + sizeof(struct fiemap_extent) * 64);
	if (!fm) {
		close(fd);
		return -1;
	}

	while (!done) {
		fm->fm_start = start;
		fm->fm_length = ~0ULL;
		fm->fm_flags = 0;
		fm->fm_extent_count = 64;

		if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) {
			/* FIEMAP not supported — estimate from stat */
			struct stat st;

			if (fstat(fd, &st) == 0 && st.st_size > 0) {
				/* Rough estimate: 1 extent per 128 blocks */
				*extent_count = (st.st_blocks > 0) ?
					(uint32_t)((st.st_blocks + 255) / 256) : 1;
				if (*extent_count == 0)
					*extent_count = 1;
			} else {
				*extent_count = 1;
			}
			free(fm);
			close(fd);
			return 0;
		}

		if (fm->fm_mapped_extents == 0)
			break;

		count += fm->fm_mapped_extents;

		fe = &fm->fm_extents[fm->fm_mapped_extents - 1];
		if (fe->fe_flags & FIEMAP_EXTENT_LAST) {
			done = 1;
		} else {
			start = fe->fe_logical + fe->fe_length;
		}
	}

	free(fm);
	close(fd);

	*extent_count = count;
	return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * DEFRAGMENTATION — copy to temporary, rename
 *
 * The simplest approach that works on any filesystem:
 *   1. Open source file
 *   2. Create temp file with fallocate (contiguous)
 *   3. Copy data
 *   4. Copy attributes (mode, owner, timestamps)
 *   5. Rename temp → original (atomic)
 *
 * This works because OCSFS's allocator with preallocation hints
 * will allocate the new file contiguously.
 * ═══════════════════════════════════════════════════════════════ */

static int defrag_file(const char *path)
{
	int src_fd = -1, dst_fd = -1;
	char tmppath[4200];
	struct stat st;
	char *buf = NULL;
	ssize_t n;
	off_t offset = 0;
	int ret = -1;
	struct timespec times[2];

	if (stat(path, &st) < 0)
		return -1;

	/* Only defrag regular files */
	if (!S_ISREG(st.st_mode))
		return 0;

	/* Skip small files (< 64 KiB) */
	if (st.st_size < 65536)
		return 0;

	snprintf(tmppath, sizeof(tmppath), "%s.defrag.tmp", path);

	src_fd = open(path, O_RDONLY);
	if (src_fd < 0)
		goto out;

	dst_fd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
	if (dst_fd < 0)
		goto out;

	/* Preallocate contiguous space via fallocate */
	if (fallocate(dst_fd, 0, 0, st.st_size) < 0) {
		/* Fallback: just write without preallocation */
		if (g_config.verbose)
			printf("  fallocate failed, using direct copy\n");
	}

	/* Allocate copy buffer */
	buf = malloc(DEFRAG_COPY_BUF_SIZE);
	if (!buf)
		goto out;

	/* Copy data with bandwidth limiting */
	while ((n = read(src_fd, buf, DEFRAG_COPY_BUF_SIZE)) > 0) {
		ssize_t written = 0;

		while (written < n) {
			ssize_t w = write(dst_fd, buf + written, n - written);
			if (w < 0) {
				if (errno == EINTR)
					continue;
				goto out;
			}
			written += w;
		}

		offset += n;
		g_config.bytes_moved += n;

		/* Bandwidth limiting */
		if (g_config.bw_limit > 0) {
			/* Sleep to limit to bw_limit MB/s */
			uint64_t bytes_per_sec = (uint64_t)g_config.bw_limit *
						 1024 * 1024;
			if (bytes_per_sec > 0) {
				useconds_t sleep_us =
					(useconds_t)((uint64_t)n * 1000000 /
						     bytes_per_sec);
				if (sleep_us > 0)
					usleep(sleep_us);
			}
		}

		/* Check for pause/stop */
		while (g_paused && g_running)
			sleep(1);

		if (!g_running)
			goto out;
	}

	if (n < 0)
		goto out;

	/* Sync data to disk */
	fsync(dst_fd);

	/* Preserve ownership and permissions */
	(void)!fchown(dst_fd, st.st_uid, st.st_gid);
	fchmod(dst_fd, st.st_mode);

	/* Preserve timestamps */
	times[0] = st.st_atim;
	times[1] = st.st_mtim;
	futimens(dst_fd, times);

	close(src_fd);
	src_fd = -1;
	close(dst_fd);
	dst_fd = -1;

	/* Atomic rename */
	if (rename(tmppath, path) < 0) {
		fprintf(stderr, "ocsfs-defrag: rename failed for %s: %s\n",
			path, strerror(errno));
		unlink(tmppath);
		goto out;
	}

	ret = 0;

out:
	free(buf);
	if (src_fd >= 0) close(src_fd);
	if (dst_fd >= 0) {
		close(dst_fd);
		unlink(tmppath);
	}
	return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * DIRECTORY SCANNER
 * ═══════════════════════════════════════════════════════════════ */

static void scan_directory(const char *dirpath)
{
	DIR *d;
	struct dirent *ent;
	char path[4096];
	uint32_t extents;

	if (!g_running)
		return;

	d = opendir(dirpath);
	if (!d)
		return;

	while ((ent = readdir(d)) != NULL && g_running) {
		/* Skip . and .. */
		if (ent->d_name[0] == '.' &&
		    (ent->d_name[1] == '\0' ||
		     (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
			continue;

		/* Skip our lock and temp files */
		if (strstr(ent->d_name, ".defrag.") ||
		    strcmp(ent->d_name, DEFRAG_LOCK_FILE) == 0)
			continue;

		snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);

		/* Recurse into directories */
		if (ent->d_type == DT_DIR) {
			scan_directory(path);
			continue;
		}

		if (ent->d_type != DT_REG)
			continue;

		/* Wait while paused */
		while (g_paused && g_running)
			sleep(1);

		g_config.files_scanned++;

		/* Count extents */
		if (count_extents(path, &extents) < 0)
			continue;

		if (extents > (uint32_t)g_config.threshold) {
			g_config.files_fragmented++;
			g_config.extents_before += extents;

			if (g_config.verbose || g_config.dry_run) {
				printf("  %s: %u extents", path, extents);
				if (g_config.dry_run)
					printf(" (dry run, skipping)");
				printf("\n");
			}

			if (!g_config.dry_run) {
				if (defrag_file(path) == 0) {
					uint32_t new_extents;

					g_config.files_defragged++;

					if (count_extents(path, &new_extents) == 0)
						g_config.extents_after += new_extents;

					if (g_config.verbose) {
						printf("  -> defragmented: %u -> %u extents\n",
						       extents, new_extents);
					}
				}
			}
		}
	}

	closedir(d);
}

/* ═══════════════════════════════════════════════════════════════
 * REPORTING
 * ═══════════════════════════════════════════════════════════════ */

static void print_stats(void)
{
	printf("\nocsfs-defrag statistics:\n");
	printf("  Files scanned:      %lu\n",
	       (unsigned long)g_config.files_scanned);
	printf("  Files fragmented:   %lu (>%d extents)\n",
	       (unsigned long)g_config.files_fragmented,
	       g_config.threshold);
	printf("  Files defragmented: %lu\n",
	       (unsigned long)g_config.files_defragged);
	printf("  Data moved:         %.1f MiB\n",
	       (double)g_config.bytes_moved / (1024 * 1024));

	if (g_config.files_defragged > 0) {
		printf("  Extents before:     %lu\n",
		       (unsigned long)g_config.extents_before);
		printf("  Extents after:      %lu\n",
		       (unsigned long)g_config.extents_after);
		double ratio = (g_config.extents_before > 0) ?
			(double)g_config.extents_after / g_config.extents_before :
			1.0;
		printf("  Consolidation:      %.0f%% reduction\n",
		       (1.0 - ratio) * 100);
	}
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════ */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options] <mount-point>\n"
		"\n"
		"Online defragmentation for OCSFS filesystems.\n"
		"\n"
		"Options:\n"
		"  -b <MB/s>   Bandwidth limit (default: %d MB/s)\n"
		"  -t <N>      Fragment threshold (default: %d extents)\n"
		"  -n          Dry run (report only)\n"
		"  -v          Verbose output\n"
		"  -d          Run as daemon\n"
		"  -1          Single pass then exit\n"
		"  -h          Show this help\n"
		"\n"
		"Signals:\n"
		"  SIGUSR1     Pause defragmentation\n"
		"  SIGUSR2     Resume defragmentation\n"
		"  SIGTERM     Stop gracefully\n",
		prog, DEFRAG_DEFAULT_BW_LIMIT, DEFRAG_DEFAULT_THRESHOLD);
}

int main(int argc, char *argv[])
{
	int opt;
	int lock_fd;

	/* Defaults */
	g_config.bw_limit = DEFRAG_DEFAULT_BW_LIMIT;
	g_config.threshold = DEFRAG_DEFAULT_THRESHOLD;

	while ((opt = getopt(argc, argv, "b:t:nvd1h")) != -1) {
		switch (opt) {
		case 'b':
			g_config.bw_limit = atoi(optarg);
			break;
		case 't':
			g_config.threshold = atoi(optarg);
			break;
		case 'n':
			g_config.dry_run = 1;
			break;
		case 'v':
			g_config.verbose = 1;
			break;
		case 'd':
			g_config.daemon_mode = 1;
			break;
		case '1':
			g_config.single_pass = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "ocsfs-defrag: missing mount point\n");
		usage(argv[0]);
		return 1;
	}

	strncpy(g_config.mount_path, argv[optind],
		sizeof(g_config.mount_path) - 1);

	/* Verify mount point */
	struct statvfs svfs;
	if (statvfs(g_config.mount_path, &svfs) < 0) {
		fprintf(stderr, "ocsfs-defrag: cannot access '%s': %s\n",
			g_config.mount_path, strerror(errno));
		return 1;
	}

	printf("ocsfs-defrag v%s\n", DEFRAG_VERSION);
	printf("Mount point:       %s\n", g_config.mount_path);
	printf("Bandwidth limit:   %d MB/s\n", g_config.bw_limit);
	printf("Fragment threshold: %d extents\n", g_config.threshold);
	if (g_config.dry_run)
		printf("Mode:              DRY RUN\n");
	printf("\n");

	setup_signals();

	/* Daemonize if requested */
	if (g_config.daemon_mode) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid > 0) {
			printf("ocsfs-defrag daemon started (PID %d)\n",
			       (int)pid);
			return 0;
		}

		/* Child — become session leader */
		setsid();
		/* Redirect stdio to /dev/null */
		(void)!freopen("/dev/null", "r", stdin);
		(void)!freopen("/dev/null", "w", stdout);
		(void)!freopen("/dev/null", "w", stderr);
	}

	/* Acquire lock */
	lock_fd = acquire_lock();
	if (lock_fd < 0)
		return 1;

	/* Main loop */
	do {
		g_config.files_scanned = 0;
		g_config.files_fragmented = 0;
		g_config.files_defragged = 0;
		g_config.bytes_moved = 0;
		g_config.extents_before = 0;
		g_config.extents_after = 0;

		if (!g_config.daemon_mode)
			printf("Scanning %s...\n", g_config.mount_path);

		scan_directory(g_config.mount_path);

		if (!g_config.daemon_mode)
			print_stats();

		if (g_config.single_pass)
			break;

		/* Sleep between scans */
		for (int i = 0; i < DEFRAG_SCAN_INTERVAL && g_running; i++)
			sleep(1);

	} while (g_running);

	close(lock_fd);

	if (!g_config.daemon_mode)
		printf("\nocsfs-defrag finished.\n");

	return 0;
}
