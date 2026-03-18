/*
 * OCSFS — FUSE Userspace Filesystem Prototype
 *
 * Single-node FUSE implementation for testing and development.
 * Implements: getattr, readdir, lookup, mkdir, rmdir, create,
 *             unlink, read, write, truncate, rename, chmod, chown, utimens.
 *
 * Usage: ocsfs-fuse <device/image> <mountpoint> [FUSE options]
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include "ocsfs.h"

/* ─── Global filesystem state ──────────────────────────────── */

static struct ocsfs_fuse_state {
    int                     dev_fd;
    struct ocsfs_superblock sb;
    struct ocsfs_ag_desc    *ag_descs;   /* cached AG descriptors */
    uint64_t                inodes_per_ag;
    pthread_mutex_t         lock;        /* global metadata lock */
} fs;

/* ─── Time helper ──────────────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ─── Inode I/O helpers ─────────────────────────────────────── */

static uint64_t ag_data_start(uint32_t ag_num)
{
    return (uint64_t)fs.ag_descs[ag_num].ag_block_start * fs.sb.s_block_size;
}

static int read_inode(uint64_t ino, struct ocsfs_inode *out)
{
    uint32_t ag_num = (uint32_t)(ino / fs.inodes_per_ag);
    uint64_t local = ino % fs.inodes_per_ag;

    if (ag_num >= fs.sb.s_ag_count)
        return -EINVAL;

    uint64_t off = ag_data_start(ag_num) +
                   fs.ag_descs[ag_num].ag_inode_table_off +
                   local * OCSFS_INODE_SIZE;

    if (pread(fs.dev_fd, out, sizeof(*out), off) != sizeof(*out))
        return -EIO;

    if (out->i_magic != OCSFS_INODE_MAGIC)
        return -ENOENT;

    return 0;
}

static int write_inode(uint64_t ino, struct ocsfs_inode *inode)
{
    uint32_t ag_num = (uint32_t)(ino / fs.inodes_per_ag);
    uint64_t local = ino % fs.inodes_per_ag;

    if (ag_num >= fs.sb.s_ag_count)
        return -EINVAL;

    inode->i_checksum = ocsfs_crc32c(0, inode, sizeof(*inode) - sizeof(uint32_t));

    uint64_t off = ag_data_start(ag_num) +
                   fs.ag_descs[ag_num].ag_inode_table_off +
                   local * OCSFS_INODE_SIZE;

    if (pwrite(fs.dev_fd, inode, sizeof(*inode), off) != sizeof(*inode))
        return -EIO;

    return 0;
}

/* ─── Inode allocation ──────────────────────────────────────── */

static uint64_t alloc_inode(uint16_t mode, uint32_t uid, uint32_t gid)
{
    /* Scan AG 0 for a free inode slot */
    uint32_t ag_num = 0;
    uint64_t start = (ag_num == 0) ? OCSFS_FIRST_USER_INO : 0;
    uint64_t base = ag_data_start(ag_num) + fs.ag_descs[ag_num].ag_inode_table_off;

    for (uint64_t i = start; i < fs.inodes_per_ag; i++) {
        struct ocsfs_inode tmp;
        uint64_t off = base + i * OCSFS_INODE_SIZE;
        if (pread(fs.dev_fd, &tmp, sizeof(tmp), off) != sizeof(tmp))
            continue;
        if (tmp.i_magic != OCSFS_INODE_MAGIC || tmp.i_nlink == 0) {
            /* Free slot */
            uint64_t global_ino = (uint64_t)ag_num * fs.inodes_per_ag + i;
            memset(&tmp, 0, sizeof(tmp));
            tmp.i_magic = OCSFS_INODE_MAGIC;
            tmp.i_ino = global_ino;
            tmp.i_mode = mode;
            tmp.i_nlink = 1;
            tmp.i_uid = uid;
            tmp.i_gid = gid;
            tmp.i_atime = now_ns();
            tmp.i_mtime = now_ns();
            tmp.i_ctime = now_ns();
            tmp.i_extent_max = OCSFS_INLINE_EXTENTS;
            tmp.i_ag = ag_num;
            write_inode(global_ino, &tmp);
            return global_ino;
        }
    }
    return 0; /* ENOSPC */
}

static void free_inode(uint64_t ino)
{
    struct ocsfs_inode zero;
    memset(&zero, 0, sizeof(zero));

    uint32_t ag_num = (uint32_t)(ino / fs.inodes_per_ag);
    uint64_t local = ino % fs.inodes_per_ag;
    uint64_t off = ag_data_start(ag_num) +
                   fs.ag_descs[ag_num].ag_inode_table_off +
                   local * OCSFS_INODE_SIZE;
    pwrite(fs.dev_fd, &zero, sizeof(zero), off);
}

/* ─── Block allocation (simple bump allocator for prototype) ── */

/* We use a simple per-AG bitmap approach but for the prototype
 * we track a next-free hint in memory */
static uint64_t next_free_block = 0;

static uint64_t alloc_data_block(void)
{
    if (next_free_block == 0) {
        /* Start after AG0 metadata */
        uint64_t bitmap_blocks = (fs.ag_descs[0].ag_block_count +
                                   fs.sb.s_block_size * 8 - 1) /
                                  (fs.sb.s_block_size * 8);
        uint64_t inode_table_blocks = (fs.inodes_per_ag * OCSFS_INODE_SIZE +
                                        fs.sb.s_block_size - 1) / fs.sb.s_block_size;
        next_free_block = fs.ag_descs[0].ag_block_start + 1 +
                          bitmap_blocks + inode_table_blocks;
    }

    uint64_t block = next_free_block++;
    uint64_t ag_end = fs.ag_descs[0].ag_block_start + fs.ag_descs[0].ag_block_count;
    if (block >= ag_end)
        return 0; /* ENOSPC */

    return block;
}

/* ─── Directory entry storage ──────────────────────────────── */

/* Fixed-size directory entry matching dir.c */
#define OCSFS_DIRENT_FIXED_SIZE 288

struct ocsfs_dirent_fixed {
    uint32_t    de_magic;
    uint64_t    de_ino;
    uint64_t    de_name_hash;
    uint8_t     de_file_type;
    uint8_t     de_name_len;
    char        de_name[OCSFS_MAX_NAME_LEN + 1];
    uint16_t    de_checksum;
    uint8_t     de_padding[6];
} __attribute__((packed));

static uint64_t name_hash(const char *name, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)name[i];
        h *= 0x100000001b3ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

/*
 * Get the data block offset for a directory inode.
 * If the directory has no data block yet, allocate one.
 */
static uint64_t dir_data_offset(struct ocsfs_inode *dir_ino, int allocate)
{
    /* Check inline extents for an existing data block */
    struct ocsfs_extent *extents = (struct ocsfs_extent *)dir_ino->i_inline_extents;
    if (dir_ino->i_extent_count > 0 && extents[0].e_length > 0) {
        return extents[0].e_physical_block * fs.sb.s_block_size;
    }

    if (!allocate)
        return 0;

    /* Allocate a data block */
    uint64_t block = alloc_data_block();
    if (block == 0)
        return 0;

    /* Zero the new block */
    uint8_t *zbuf = calloc(1, fs.sb.s_block_size);
    if (zbuf) {
        pwrite(fs.dev_fd, zbuf, fs.sb.s_block_size, block * fs.sb.s_block_size);
        free(zbuf);
    }

    /* Record in inode */
    extents[0].e_logical_block = 0;
    extents[0].e_physical_block = block;
    extents[0].e_length = 1;
    extents[0].e_flags = OCSFS_EXT_WRITTEN;
    dir_ino->i_extent_count = 1;
    dir_ino->i_blocks = 1;
    dir_ino->i_size = fs.sb.s_block_size;

    return block * fs.sb.s_block_size;
}

/*
 * Read all directory entries from a dir inode into a buffer.
 * Returns allocated array and sets *count. Caller frees.
 */
static struct ocsfs_dirent_fixed *read_dir_entries(struct ocsfs_inode *dir_ino,
                                                     uint32_t *count)
{
    *count = 0;
    uint64_t off = dir_data_offset(dir_ino, 0);
    if (off == 0)
        return NULL;

    uint8_t *buf = malloc(fs.sb.s_block_size);
    if (!buf) return NULL;

    if (pread(fs.dev_fd, buf, fs.sb.s_block_size, off) != (ssize_t)fs.sb.s_block_size) {
        free(buf);
        return NULL;
    }

    size_t max_entries = fs.sb.s_block_size / OCSFS_DIRENT_FIXED_SIZE;
    struct ocsfs_dirent_fixed *entries = calloc(max_entries,
                                                 sizeof(struct ocsfs_dirent_fixed));
    if (!entries) { free(buf); return NULL; }

    struct ocsfs_dirent_fixed *src = (struct ocsfs_dirent_fixed *)buf;
    for (size_t i = 0; i < max_entries; i++) {
        if (src[i].de_magic != OCSFS_DIRENT_MAGIC)
            break;
        entries[*count] = src[i];
        (*count)++;
    }

    free(buf);
    return entries;
}

/*
 * Write directory entries back to the data block.
 */
static int write_dir_entries(struct ocsfs_inode *dir_ino,
                              struct ocsfs_dirent_fixed *entries, uint32_t count)
{
    uint64_t off = dir_data_offset(dir_ino, 1);
    if (off == 0) return -ENOSPC;

    uint8_t *buf = calloc(1, fs.sb.s_block_size);
    if (!buf) return -ENOMEM;

    size_t to_copy = count * OCSFS_DIRENT_FIXED_SIZE;
    if (to_copy > fs.sb.s_block_size)
        to_copy = fs.sb.s_block_size;
    memcpy(buf, entries, to_copy);

    int ret = 0;
    if (pwrite(fs.dev_fd, buf, fs.sb.s_block_size, off) != (ssize_t)fs.sb.s_block_size)
        ret = -EIO;

    free(buf);
    return ret;
}

/*
 * Lookup a name in a directory. Returns inode number or 0.
 */
static uint64_t dir_lookup(struct ocsfs_inode *dir_ino,
                            const char *name, size_t name_len)
{
    uint32_t count;
    struct ocsfs_dirent_fixed *entries = read_dir_entries(dir_ino, &count);
    if (!entries) return 0;

    uint64_t hash = name_hash(name, name_len);
    uint64_t result = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].de_name_hash == hash &&
            entries[i].de_name_len == name_len &&
            memcmp(entries[i].de_name, name, name_len) == 0) {
            result = entries[i].de_ino;
            break;
        }
    }

    free(entries);
    return result;
}

/*
 * Add an entry to a directory.
 */
static int dir_add_entry(struct ocsfs_inode *dir_ino, uint64_t dir_ino_num,
                          const char *name, size_t name_len,
                          uint64_t ino, uint8_t file_type)
{
    uint32_t count = 0;
    struct ocsfs_dirent_fixed *entries = read_dir_entries(dir_ino, &count);

    size_t max_entries = fs.sb.s_block_size / OCSFS_DIRENT_FIXED_SIZE;
    if (count >= max_entries) {
        free(entries);
        return -ENOSPC;
    }

    if (!entries) {
        entries = calloc(max_entries, sizeof(struct ocsfs_dirent_fixed));
        if (!entries) return -ENOMEM;
        count = 0;
    }

    struct ocsfs_dirent_fixed *de = &entries[count];
    memset(de, 0, sizeof(*de));
    de->de_magic = OCSFS_DIRENT_MAGIC;
    de->de_ino = ino;
    de->de_name_hash = name_hash(name, name_len);
    de->de_file_type = file_type;
    de->de_name_len = (uint8_t)name_len;
    memcpy(de->de_name, name, name_len);
    de->de_name[name_len] = '\0';
    count++;

    int ret = write_dir_entries(dir_ino, entries, count);
    if (ret == 0) {
        dir_ino->i_mtime = now_ns();
        dir_ino->i_ctime = now_ns();
        write_inode(dir_ino_num, dir_ino);
    }

    free(entries);
    return ret;
}

/*
 * Remove an entry from a directory.
 */
static int dir_remove_entry(struct ocsfs_inode *dir_ino, uint64_t dir_ino_num,
                             const char *name, size_t name_len)
{
    uint32_t count = 0;
    struct ocsfs_dirent_fixed *entries = read_dir_entries(dir_ino, &count);
    if (!entries) return -ENOENT;

    uint64_t hash = name_hash(name, name_len);
    int found = -1;
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].de_name_hash == hash &&
            entries[i].de_name_len == name_len &&
            memcmp(entries[i].de_name, name, name_len) == 0) {
            found = (int)i;
            break;
        }
    }

    if (found < 0) {
        free(entries);
        return -ENOENT;
    }

    /* Shift entries */
    if ((uint32_t)found < count - 1) {
        memmove(&entries[found], &entries[found + 1],
                (count - found - 1) * sizeof(struct ocsfs_dirent_fixed));
    }
    count--;

    int ret = write_dir_entries(dir_ino, entries, count);
    if (ret == 0) {
        dir_ino->i_mtime = now_ns();
        dir_ino->i_ctime = now_ns();
        write_inode(dir_ino_num, dir_ino);
    }

    free(entries);
    return ret;
}

/* ─── Path resolution ──────────────────────────────────────── */

/*
 * Resolve a path to an inode number.
 * Returns 0 on success, negative on error.
 */
static int resolve_path(const char *path, uint64_t *out_ino)
{
    if (path[0] != '/')
        return -EINVAL;

    if (strcmp(path, "/") == 0) {
        *out_ino = OCSFS_ROOT_INO;
        return 0;
    }

    uint64_t current_ino = OCSFS_ROOT_INO;
    char *pathcopy = strdup(path);
    if (!pathcopy) return -ENOMEM;

    char *saveptr;
    char *component = strtok_r(pathcopy, "/", &saveptr);

    while (component) {
        struct ocsfs_inode dir;
        int ret = read_inode(current_ino, &dir);
        if (ret < 0) { free(pathcopy); return ret; }

        if ((dir.i_mode >> 12) != OCSFS_FT_DIR) {
            free(pathcopy);
            return -ENOTDIR;
        }

        uint64_t child_ino = dir_lookup(&dir, component, strlen(component));
        if (child_ino == 0) {
            free(pathcopy);
            return -ENOENT;
        }

        current_ino = child_ino;
        component = strtok_r(NULL, "/", &saveptr);
    }

    free(pathcopy);
    *out_ino = current_ino;
    return 0;
}

/*
 * Resolve parent directory and get the last component name.
 */
static int resolve_parent(const char *path, uint64_t *parent_ino,
                           const char **basename_out)
{
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;

    *basename_out = last_slash + 1;
    if (**basename_out == '\0') return -EINVAL;

    if (last_slash == path) {
        /* Parent is root */
        *parent_ino = OCSFS_ROOT_INO;
        return 0;
    }

    /* Extract parent path */
    size_t parent_len = last_slash - path;
    char *parent_path = malloc(parent_len + 1);
    if (!parent_path) return -ENOMEM;
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';

    int ret = resolve_path(parent_path, parent_ino);
    free(parent_path);
    return ret;
}

/* ─── FUSE Operations ──────────────────────────────────────── */

static void inode_to_stat(const struct ocsfs_inode *ino, uint64_t ino_num,
                           struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = ino_num;
    st->st_nlink = ino->i_nlink;
    st->st_uid = ino->i_uid;
    st->st_gid = ino->i_gid;
    st->st_size = ino->i_size;
    st->st_blocks = ino->i_blocks * (fs.sb.s_block_size / 512);
    st->st_blksize = fs.sb.s_block_size;
    st->st_atim.tv_sec = ino->i_atime / 1000000000ULL;
    st->st_atim.tv_nsec = ino->i_atime % 1000000000ULL;
    st->st_mtim.tv_sec = ino->i_mtime / 1000000000ULL;
    st->st_mtim.tv_nsec = ino->i_mtime % 1000000000ULL;
    st->st_ctim.tv_sec = ino->i_ctime / 1000000000ULL;
    st->st_ctim.tv_nsec = ino->i_ctime % 1000000000ULL;

    uint8_t ft = ino->i_mode >> 12;
    uint16_t perm = ino->i_mode & 0xFFF;
    switch (ft) {
    case OCSFS_FT_DIR:      st->st_mode = S_IFDIR | perm; break;
    case OCSFS_FT_REG_FILE: st->st_mode = S_IFREG | perm; break;
    case OCSFS_FT_SYMLINK:  st->st_mode = S_IFLNK | perm; break;
    case OCSFS_FT_CHRDEV:   st->st_mode = S_IFCHR | perm; break;
    case OCSFS_FT_BLKDEV:   st->st_mode = S_IFBLK | perm; break;
    case OCSFS_FT_FIFO:     st->st_mode = S_IFIFO | perm; break;
    case OCSFS_FT_SOCK:     st->st_mode = S_IFSOCK | perm; break;
    default:                st->st_mode = S_IFREG | perm; break;
    }
}

static int ocsfs_getattr(const char *path, struct stat *st,
                          struct fuse_file_info *fi __attribute__((unused)))
{
    pthread_mutex_lock(&fs.lock);

    uint64_t ino;
    int ret = resolve_path(path, &ino);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct ocsfs_inode inode;
    ret = read_inode(ino, &inode);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    inode_to_stat(&inode, ino, st);

    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset __attribute__((unused)),
                          struct fuse_file_info *fi __attribute__((unused)),
                          enum fuse_readdir_flags flags __attribute__((unused)))
{
    pthread_mutex_lock(&fs.lock);

    uint64_t dir_ino_num;
    int ret = resolve_path(path, &dir_ino_num);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct ocsfs_inode dir;
    ret = read_inode(dir_ino_num, &dir);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    if ((dir.i_mode >> 12) != OCSFS_FT_DIR) {
        pthread_mutex_unlock(&fs.lock);
        return -ENOTDIR;
    }

    /* Always emit . and .. */
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    /* Read entries */
    uint32_t count;
    struct ocsfs_dirent_fixed *entries = read_dir_entries(&dir, &count);
    if (entries) {
        for (uint32_t i = 0; i < count; i++) {
            /* Skip . and .. if they are stored */
            if (strcmp(entries[i].de_name, ".") == 0 ||
                strcmp(entries[i].de_name, "..") == 0)
                continue;
            filler(buf, entries[i].de_name, NULL, 0, 0);
        }
        free(entries);
    }

    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_mkdir(const char *path, mode_t mode)
{
    pthread_mutex_lock(&fs.lock);

    uint64_t parent_ino;
    const char *name;
    int ret = resolve_parent(path, &parent_ino, &name);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct fuse_context *fctx = fuse_get_context();
    struct ocsfs_inode parent;
    ret = read_inode(parent_ino, &parent);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    /* Allocate inode */
    uint64_t new_ino = alloc_inode((OCSFS_FT_DIR << 12) | (mode & 0xFFF),
                                    fctx->uid, fctx->gid);
    if (new_ino == 0) { pthread_mutex_unlock(&fs.lock); return -ENOSPC; }

    /* Set nlink = 2 (. and ..) */
    struct ocsfs_inode new_dir;
    read_inode(new_ino, &new_dir);
    new_dir.i_nlink = 2;
    write_inode(new_ino, &new_dir);

    /* Add entry to parent */
    ret = dir_add_entry(&parent, parent_ino, name, strlen(name),
                         new_ino, OCSFS_FT_DIR);
    if (ret < 0) {
        free_inode(new_ino);
        pthread_mutex_unlock(&fs.lock);
        return ret;
    }

    /* Increment parent nlink for subdirectory */
    parent.i_nlink++;
    write_inode(parent_ino, &parent);

    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_rmdir(const char *path)
{
    pthread_mutex_lock(&fs.lock);

    uint64_t parent_ino;
    const char *name;
    int ret = resolve_parent(path, &parent_ino, &name);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct ocsfs_inode parent;
    ret = read_inode(parent_ino, &parent);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    uint64_t child_ino = dir_lookup(&parent, name, strlen(name));
    if (child_ino == 0) { pthread_mutex_unlock(&fs.lock); return -ENOENT; }

    struct ocsfs_inode child;
    ret = read_inode(child_ino, &child);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    if ((child.i_mode >> 12) != OCSFS_FT_DIR) {
        pthread_mutex_unlock(&fs.lock);
        return -ENOTDIR;
    }

    /* Check if empty */
    uint32_t count;
    struct ocsfs_dirent_fixed *entries = read_dir_entries(&child, &count);
    if (entries) {
        free(entries);
        if (count > 0) {
            pthread_mutex_unlock(&fs.lock);
            return -ENOTEMPTY;
        }
    }

    /* Remove entry from parent */
    ret = dir_remove_entry(&parent, parent_ino, name, strlen(name));
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    parent.i_nlink--;
    write_inode(parent_ino, &parent);

    free_inode(child_ino);

    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_create(const char *path, mode_t mode,
                         struct fuse_file_info *fi)
{
    pthread_mutex_lock(&fs.lock);

    uint64_t parent_ino;
    const char *name;
    int ret = resolve_parent(path, &parent_ino, &name);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct fuse_context *fctx = fuse_get_context();
    struct ocsfs_inode parent;
    ret = read_inode(parent_ino, &parent);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    /* Allocate inode */
    uint64_t new_ino = alloc_inode((OCSFS_FT_REG_FILE << 12) | (mode & 0xFFF),
                                    fctx->uid, fctx->gid);
    if (new_ino == 0) { pthread_mutex_unlock(&fs.lock); return -ENOSPC; }

    /* Add to parent directory */
    ret = dir_add_entry(&parent, parent_ino, name, strlen(name),
                         new_ino, OCSFS_FT_REG_FILE);
    if (ret < 0) {
        free_inode(new_ino);
        pthread_mutex_unlock(&fs.lock);
        return ret;
    }

    fi->fh = new_ino;
    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_unlink(const char *path)
{
    pthread_mutex_lock(&fs.lock);

    uint64_t parent_ino;
    const char *name;
    int ret = resolve_parent(path, &parent_ino, &name);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct ocsfs_inode parent;
    ret = read_inode(parent_ino, &parent);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    uint64_t child_ino = dir_lookup(&parent, name, strlen(name));
    if (child_ino == 0) { pthread_mutex_unlock(&fs.lock); return -ENOENT; }

    struct ocsfs_inode child;
    ret = read_inode(child_ino, &child);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    if ((child.i_mode >> 12) == OCSFS_FT_DIR) {
        pthread_mutex_unlock(&fs.lock);
        return -EISDIR;
    }

    /* Remove entry */
    ret = dir_remove_entry(&parent, parent_ino, name, strlen(name));
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    child.i_nlink--;
    if (child.i_nlink == 0) {
        free_inode(child_ino);
    } else {
        child.i_ctime = now_ns();
        write_inode(child_ino, &child);
    }

    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_open(const char *path, struct fuse_file_info *fi)
{
    pthread_mutex_lock(&fs.lock);

    uint64_t ino;
    int ret = resolve_path(path, &ino);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    fi->fh = ino;
    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_read(const char *path __attribute__((unused)),
                       char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi)
{
    pthread_mutex_lock(&fs.lock);

    uint64_t ino = fi->fh;
    struct ocsfs_inode inode;
    int ret = read_inode(ino, &inode);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    if ((uint64_t)offset >= inode.i_size) {
        pthread_mutex_unlock(&fs.lock);
        return 0;
    }

    if (offset + size > inode.i_size)
        size = inode.i_size - offset;

    /* Read from data blocks via extent map */
    struct ocsfs_extent *extents = (struct ocsfs_extent *)inode.i_inline_extents;
    size_t bytes_read = 0;
    uint32_t bs = fs.sb.s_block_size;

    while (bytes_read < size) {
        uint64_t file_block = (offset + bytes_read) / bs;
        uint64_t block_off = (offset + bytes_read) % bs;
        size_t to_read = bs - block_off;
        if (to_read > size - bytes_read)
            to_read = size - bytes_read;

        /* Find physical block */
        uint64_t phys_block = 0;
        int found = 0;
        for (uint16_t i = 0; i < inode.i_extent_count; i++) {
            uint64_t ext_start = extents[i].e_logical_block;
            uint64_t ext_len = extents[i].e_length;
            if (file_block >= ext_start && file_block < ext_start + ext_len) {
                phys_block = extents[i].e_physical_block +
                             (file_block - ext_start);
                found = 1;
                break;
            }
        }

        if (!found) {
            /* Hole — fill with zeros */
            memset(buf + bytes_read, 0, to_read);
        } else {
            uint64_t disk_off = phys_block * bs + block_off;
            ssize_t rd = pread(fs.dev_fd, buf + bytes_read, to_read, disk_off);
            if (rd < 0) {
                pthread_mutex_unlock(&fs.lock);
                return -EIO;
            }
            if ((size_t)rd < to_read)
                memset(buf + bytes_read + rd, 0, to_read - rd);
        }

        bytes_read += to_read;
    }

    /* Update atime */
    inode.i_atime = now_ns();
    write_inode(ino, &inode);

    pthread_mutex_unlock(&fs.lock);
    return (int)bytes_read;
}

static int ocsfs_write(const char *path __attribute__((unused)),
                        const char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi)
{
    pthread_mutex_lock(&fs.lock);

    uint64_t ino = fi->fh;
    struct ocsfs_inode inode;
    int ret = read_inode(ino, &inode);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct ocsfs_extent *extents = (struct ocsfs_extent *)inode.i_inline_extents;
    uint32_t bs = fs.sb.s_block_size;
    size_t bytes_written = 0;

    while (bytes_written < size) {
        uint64_t file_block = (offset + bytes_written) / bs;
        uint64_t block_off = (offset + bytes_written) % bs;
        size_t to_write = bs - block_off;
        if (to_write > size - bytes_written)
            to_write = size - bytes_written;

        /* Find or allocate physical block */
        uint64_t phys_block = 0;
        int found = 0;
        for (uint16_t i = 0; i < inode.i_extent_count; i++) {
            uint64_t ext_start = extents[i].e_logical_block;
            uint64_t ext_len = extents[i].e_length;
            if (file_block >= ext_start && file_block < ext_start + ext_len) {
                phys_block = extents[i].e_physical_block +
                             (file_block - ext_start);
                found = 1;
                break;
            }
        }

        if (!found) {
            /* Need to allocate a new block */
            uint64_t new_block = alloc_data_block();
            if (new_block == 0) {
                if (bytes_written > 0) break; /* partial write */
                pthread_mutex_unlock(&fs.lock);
                return -ENOSPC;
            }

            /* Zero new block first */
            uint8_t *zbuf = calloc(1, bs);
            if (zbuf) {
                pwrite(fs.dev_fd, zbuf, bs, new_block * bs);
                free(zbuf);
            }

            /* Add extent — try to extend existing or create new */
            int extended = 0;
            for (uint16_t i = 0; i < inode.i_extent_count; i++) {
                uint64_t ext_end = extents[i].e_logical_block + extents[i].e_length;
                uint64_t phys_end = extents[i].e_physical_block + extents[i].e_length;
                if (ext_end == file_block && phys_end == new_block) {
                    extents[i].e_length++;
                    extended = 1;
                    break;
                }
            }

            if (!extended) {
                if (inode.i_extent_count >= OCSFS_INLINE_EXTENTS) {
                    if (bytes_written > 0) break;
                    pthread_mutex_unlock(&fs.lock);
                    return -ENOSPC; /* out of inline extents */
                }
                uint16_t idx = inode.i_extent_count;
                extents[idx].e_logical_block = file_block;
                extents[idx].e_physical_block = new_block;
                extents[idx].e_length = 1;
                extents[idx].e_flags = OCSFS_EXT_WRITTEN;
                inode.i_extent_count++;
            }

            inode.i_blocks++;
            phys_block = new_block;
        }

        uint64_t disk_off = phys_block * bs + block_off;
        ssize_t wr = pwrite(fs.dev_fd, buf + bytes_written, to_write, disk_off);
        if (wr < 0) {
            if (bytes_written > 0) break;
            pthread_mutex_unlock(&fs.lock);
            return -EIO;
        }
        bytes_written += to_write;
    }

    /* Update size and times */
    if ((uint64_t)(offset + bytes_written) > inode.i_size)
        inode.i_size = offset + bytes_written;
    inode.i_mtime = now_ns();
    inode.i_ctime = now_ns();
    write_inode(ino, &inode);

    pthread_mutex_unlock(&fs.lock);
    return (int)bytes_written;
}

static int ocsfs_truncate(const char *path, off_t size,
                           struct fuse_file_info *fi __attribute__((unused)))
{
    pthread_mutex_lock(&fs.lock);

    uint64_t ino;
    int ret = resolve_path(path, &ino);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct ocsfs_inode inode;
    ret = read_inode(ino, &inode);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    inode.i_size = (uint64_t)size;
    inode.i_mtime = now_ns();
    inode.i_ctime = now_ns();
    write_inode(ino, &inode);

    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_chmod(const char *path, mode_t mode,
                        struct fuse_file_info *fi __attribute__((unused)))
{
    pthread_mutex_lock(&fs.lock);

    uint64_t ino;
    int ret = resolve_path(path, &ino);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct ocsfs_inode inode;
    ret = read_inode(ino, &inode);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    /* Preserve file type, update permissions */
    inode.i_mode = (inode.i_mode & 0xF000) | (mode & 0xFFF);
    inode.i_ctime = now_ns();
    write_inode(ino, &inode);

    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_chown(const char *path, uid_t uid, gid_t gid,
                        struct fuse_file_info *fi __attribute__((unused)))
{
    pthread_mutex_lock(&fs.lock);

    uint64_t ino;
    int ret = resolve_path(path, &ino);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct ocsfs_inode inode;
    ret = read_inode(ino, &inode);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    if (uid != (uid_t)-1) inode.i_uid = uid;
    if (gid != (gid_t)-1) inode.i_gid = gid;
    inode.i_ctime = now_ns();
    write_inode(ino, &inode);

    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_utimens(const char *path, const struct timespec tv[2],
                          struct fuse_file_info *fi __attribute__((unused)))
{
    pthread_mutex_lock(&fs.lock);

    uint64_t ino;
    int ret = resolve_path(path, &ino);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct ocsfs_inode inode;
    ret = read_inode(ino, &inode);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    if (tv[0].tv_nsec != UTIME_OMIT)
        inode.i_atime = (uint64_t)tv[0].tv_sec * 1000000000ULL + tv[0].tv_nsec;
    if (tv[1].tv_nsec != UTIME_OMIT)
        inode.i_mtime = (uint64_t)tv[1].tv_sec * 1000000000ULL + tv[1].tv_nsec;
    inode.i_ctime = now_ns();
    write_inode(ino, &inode);

    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_rename(const char *from, const char *to,
                         unsigned int flags __attribute__((unused)))
{
    pthread_mutex_lock(&fs.lock);

    /* Resolve source */
    uint64_t src_parent_ino;
    const char *src_name;
    int ret = resolve_parent(from, &src_parent_ino, &src_name);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct ocsfs_inode src_parent;
    ret = read_inode(src_parent_ino, &src_parent);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    uint64_t src_ino = dir_lookup(&src_parent, src_name, strlen(src_name));
    if (src_ino == 0) { pthread_mutex_unlock(&fs.lock); return -ENOENT; }

    struct ocsfs_inode src_inode;
    ret = read_inode(src_ino, &src_inode);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    /* Resolve destination parent */
    uint64_t dst_parent_ino;
    const char *dst_name;
    ret = resolve_parent(to, &dst_parent_ino, &dst_name);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    struct ocsfs_inode dst_parent;
    ret = read_inode(dst_parent_ino, &dst_parent);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    /* Check if destination exists — remove it */
    uint64_t existing = dir_lookup(&dst_parent, dst_name, strlen(dst_name));
    if (existing != 0) {
        dir_remove_entry(&dst_parent, dst_parent_ino, dst_name, strlen(dst_name));
        /* Re-read parent after modification */
        read_inode(dst_parent_ino, &dst_parent);
    }

    /* Add to destination */
    uint8_t ft = src_inode.i_mode >> 12;
    ret = dir_add_entry(&dst_parent, dst_parent_ino, dst_name, strlen(dst_name),
                         src_ino, ft);
    if (ret < 0) { pthread_mutex_unlock(&fs.lock); return ret; }

    /* Remove from source — re-read source parent in case src == dst parent */
    read_inode(src_parent_ino, &src_parent);
    dir_remove_entry(&src_parent, src_parent_ino, src_name, strlen(src_name));

    /* Update nlink for directory renames */
    if (ft == OCSFS_FT_DIR && src_parent_ino != dst_parent_ino) {
        read_inode(src_parent_ino, &src_parent);
        src_parent.i_nlink--;
        write_inode(src_parent_ino, &src_parent);

        read_inode(dst_parent_ino, &dst_parent);
        dst_parent.i_nlink++;
        write_inode(dst_parent_ino, &dst_parent);
    }

    src_inode.i_ctime = now_ns();
    write_inode(src_ino, &src_inode);

    pthread_mutex_unlock(&fs.lock);
    return 0;
}

static int ocsfs_statfs(const char *path __attribute__((unused)),
                         struct statvfs *st)
{
    memset(st, 0, sizeof(*st));
    st->f_bsize = fs.sb.s_block_size;
    st->f_frsize = fs.sb.s_block_size;
    st->f_blocks = fs.sb.s_total_blocks;
    st->f_bfree = fs.sb.s_free_blocks;
    st->f_bavail = fs.sb.s_free_blocks;
    st->f_files = fs.inodes_per_ag * fs.sb.s_ag_count;
    st->f_ffree = st->f_files / 2; /* estimate */
    st->f_namemax = OCSFS_MAX_NAME_LEN;
    return 0;
}

/* ─── FUSE operation table ─────────────────────────────────── */

static const struct fuse_operations ocsfs_ops = {
    .getattr    = ocsfs_getattr,
    .readdir    = ocsfs_readdir,
    .mkdir      = ocsfs_mkdir,
    .rmdir      = ocsfs_rmdir,
    .create     = ocsfs_create,
    .open       = ocsfs_open,
    .read       = ocsfs_read,
    .write      = ocsfs_write,
    .unlink     = ocsfs_unlink,
    .truncate   = ocsfs_truncate,
    .chmod      = ocsfs_chmod,
    .chown      = ocsfs_chown,
    .utimens    = ocsfs_utimens,
    .rename     = ocsfs_rename,
    .statfs     = ocsfs_statfs,
};

/* ─── Main ──────────────────────────────────────────────────── */

static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s <device/image> <mountpoint> [FUSE options]\n", progname);
    exit(1);
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        usage(argv[0]);
    }

    const char *device = argv[1];

    /* Open device */
    fs.dev_fd = open(device, O_RDWR);
    if (fs.dev_fd < 0) {
        fprintf(stderr, "ocsfs-fuse: cannot open %s: %s\n", device, strerror(errno));
        return 1;
    }

    /* Read superblock */
    if (pread(fs.dev_fd, &fs.sb, sizeof(fs.sb), 0) != sizeof(fs.sb)) {
        fprintf(stderr, "ocsfs-fuse: cannot read superblock\n");
        close(fs.dev_fd);
        return 1;
    }

    if (fs.sb.s_magic != OCSFS_MAGIC) {
        fprintf(stderr, "ocsfs-fuse: bad magic (0x%08X, expected 0x%08X)\n",
                fs.sb.s_magic, OCSFS_MAGIC);
        close(fs.dev_fd);
        return 1;
    }

    /* Read AG descriptors */
    fs.ag_descs = calloc(fs.sb.s_ag_count, sizeof(struct ocsfs_ag_desc));
    if (!fs.ag_descs) {
        fprintf(stderr, "ocsfs-fuse: out of memory\n");
        close(fs.dev_fd);
        return 1;
    }

    for (uint32_t i = 0; i < fs.sb.s_ag_count; i++) {
        uint64_t off = fs.sb.s_ag_desc_off + (uint64_t)i * sizeof(struct ocsfs_ag_desc);
        if (pread(fs.dev_fd, &fs.ag_descs[i], sizeof(struct ocsfs_ag_desc), off) !=
            sizeof(struct ocsfs_ag_desc)) {
            fprintf(stderr, "ocsfs-fuse: cannot read AG descriptor %u\n", i);
            free(fs.ag_descs);
            close(fs.dev_fd);
            return 1;
        }
    }

    fs.inodes_per_ag = fs.ag_descs[0].ag_inode_count;
    pthread_mutex_init(&fs.lock, NULL);

    printf("ocsfs-fuse: mounting %s\n", device);
    printf("  Block size:  %u\n", fs.sb.s_block_size);
    printf("  AG count:    %u\n", fs.sb.s_ag_count);
    printf("  Inodes/AG:   %lu\n", (unsigned long)fs.inodes_per_ag);

    /* Shift argv: remove device argument, keep mountpoint + FUSE options */
    argv[1] = argv[2];
    argc--;
    for (int i = 2; i < argc; i++)
        argv[i] = argv[i + 1];

    int ret = fuse_main(argc, argv, &ocsfs_ops, NULL);

    /* Cleanup */
    pthread_mutex_destroy(&fs.lock);
    free(fs.ag_descs);
    close(fs.dev_fd);

    return ret;
}
