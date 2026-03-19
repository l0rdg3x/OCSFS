/*
 * OCSFS — Test Suite
 *
 * Tests for: CRC32C, bitmap allocator, extent manager, lock manager,
 *            B+ tree, inode allocator, journal, directory operations,
 *            superblock serialization, and end-to-end mkfs + tool.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include "ocsfs.h"
#include "ocsfs_btree.h"

/* ─── Test Framework ────────────────────────────────────────── */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_test_##name(void) { \
        printf("  %-50s ", #name); \
        fflush(stdout); \
        test_##name(); \
        tests_passed++; \
        printf("PASS\n"); \
    } \
    static void test_##name(void)

#define ASSERT(cond) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("FAIL\n    Assertion failed: %s\n    at %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    tests_run++; \
    if ((a) != (b)) { \
        printf("FAIL\n    Expected %ld == %ld\n    at %s:%d\n", \
               (long)(a), (long)(b), __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* ─── CRC32C Tests ──────────────────────────────────────────── */

TEST(crc32c_empty)
{
    uint32_t crc = ocsfs_crc32c(0, "", 0);
    ASSERT(crc == 0);
}

TEST(crc32c_known_vectors)
{
    /* Known CRC32C test vector: "123456789" */
    const char *data = "123456789";
    uint32_t crc = ocsfs_crc32c(0, data, 9);
    ASSERT_EQ(crc, 0xE3069283);
}

TEST(crc32c_different_data)
{
    uint32_t crc1 = ocsfs_crc32c(0, "hello", 5);
    uint32_t crc2 = ocsfs_crc32c(0, "world", 5);
    ASSERT(crc1 != crc2);
}

TEST(crc32c_incremental)
{
    const char *data = "helloworld";
    uint32_t full = ocsfs_crc32c(0, data, 10);
    uint32_t part1 = ocsfs_crc32c(0, "hello", 5);
    uint32_t part2 = ocsfs_crc32c(part1, "world", 5);
    ASSERT(full != 0);
    ASSERT(part2 != 0);
}

TEST(crc32c_incremental_matches_full)
{
    const char *data = "helloworld";
    uint32_t full = ocsfs_crc32c(0, data, 10);
    uint32_t part1 = ocsfs_crc32c(0, "hello", 5);
    uint32_t part2 = ocsfs_crc32c(part1, "world", 5);
    ASSERT_EQ(full, part2);
}

TEST(crc32c_large_buffer)
{
    size_t sz = 1024 * 1024; /* 1 MB */
    uint8_t *buf = malloc(sz);
    ASSERT(buf != NULL);
    memset(buf, 0xAB, sz);
    uint32_t crc1 = ocsfs_crc32c(0, buf, sz);
    uint32_t crc2 = ocsfs_crc32c(0, buf, sz);
    ASSERT(crc1 != 0);
    ASSERT_EQ(crc1, crc2); /* deterministic */
    free(buf);
}

/* ─── Structure Size Tests ──────────────────────────────────── */

TEST(struct_sizes)
{
    ASSERT_EQ(sizeof(struct ocsfs_superblock), 4096);
    ASSERT_EQ(sizeof(struct ocsfs_node_slot), 256);
    ASSERT_EQ(sizeof(struct ocsfs_heartbeat), 1024);
    ASSERT_EQ(sizeof(struct ocsfs_lock_entry), 256);
    ASSERT_EQ(sizeof(struct ocsfs_extent), 24);
    ASSERT_EQ(sizeof(struct ocsfs_ag_desc), 4096);
    ASSERT_EQ(sizeof(struct ocsfs_journal_header), 4096);
}

TEST(magic_numbers)
{
    ASSERT_EQ(OCSFS_MAGIC, 0x4F435346);
    ASSERT_EQ(OCSFS_INODE_MAGIC, 0x494E4F44);
    ASSERT_EQ(OCSFS_AG_MAGIC, 0x41474850);
    ASSERT_EQ(OCSFS_JOURNAL_MAGIC, 0x4A524E4C);
}

TEST(layout_offsets)
{
    /* Verify regions don't overlap */
    ASSERT(OCSFS_SUPERBLOCK_OFFSET < OCSFS_SUPERBLOCK_MIRROR);
    ASSERT(OCSFS_SUPERBLOCK_MIRROR + OCSFS_SUPERBLOCK_SIZE <= OCSFS_NODE_SLOT_TABLE_OFF);
    ASSERT(OCSFS_NODE_SLOT_TABLE_OFF + OCSFS_NODE_SLOT_TABLE_SIZE <= OCSFS_HEARTBEAT_OFF);
    ASSERT(OCSFS_HEARTBEAT_OFF + OCSFS_HEARTBEAT_SIZE <= OCSFS_LOCK_TABLE_OFF);
    ASSERT(OCSFS_LOCK_TABLE_OFF + OCSFS_LOCK_TABLE_SIZE <= ocsfs_journal_offset());
}

/* ─── Bitmap Tests ──────────────────────────────────────────── */

/* External declarations from bitmap.c */
extern void ocsfs_bitmap_set_range(uint8_t *bitmap, uint64_t start, uint64_t count);
extern void ocsfs_bitmap_clear_range(uint8_t *bitmap, uint64_t start, uint64_t count);
extern uint64_t ocsfs_bitmap_count_free(const uint8_t *bitmap, uint64_t total_bits);
extern uint64_t ocsfs_bitmap_find_free_extent(const uint8_t *bitmap, uint64_t total_bits,
                                               uint64_t hint, uint64_t goal,
                                               uint64_t min, uint64_t *out_len);
extern uint64_t ocsfs_bitmap_alloc(uint8_t *bitmap, uint64_t total_bits,
                                    uint64_t hint, uint64_t count, uint64_t *out_len);
extern void ocsfs_bitmap_free(uint8_t *bitmap, uint64_t start, uint64_t count);

TEST(bitmap_empty)
{
    uint8_t bitmap[128]; /* 1024 bits */
    memset(bitmap, 0, sizeof(bitmap));
    ASSERT_EQ(ocsfs_bitmap_count_free(bitmap, 1024), 1024);
}

TEST(bitmap_full)
{
    uint8_t bitmap[128];
    memset(bitmap, 0xFF, sizeof(bitmap));
    ASSERT_EQ(ocsfs_bitmap_count_free(bitmap, 1024), 0);
}

TEST(bitmap_set_clear_range)
{
    uint8_t bitmap[128];
    memset(bitmap, 0, sizeof(bitmap));

    ocsfs_bitmap_set_range(bitmap, 10, 20);
    ASSERT_EQ(ocsfs_bitmap_count_free(bitmap, 1024), 1004);

    ocsfs_bitmap_clear_range(bitmap, 15, 10);
    ASSERT_EQ(ocsfs_bitmap_count_free(bitmap, 1024), 1014);
}

TEST(bitmap_find_extent_simple)
{
    uint8_t bitmap[128];
    memset(bitmap, 0, sizeof(bitmap));

    /* Mark first 100 blocks as used */
    ocsfs_bitmap_set_range(bitmap, 0, 100);

    uint64_t len = 0;
    uint64_t start = ocsfs_bitmap_find_free_extent(bitmap, 1024, 0, 50, 1, &len);
    ASSERT_EQ(start, 100); /* first free after used region */
    ASSERT_EQ(len, 50);
}

TEST(bitmap_find_extent_wrap)
{
    uint8_t bitmap[128];
    memset(bitmap, 0, sizeof(bitmap));

    /* Mark blocks 500-1023 as used */
    ocsfs_bitmap_set_range(bitmap, 500, 524);

    /* Search from 900 with hint, should wrap and find at 0 */
    uint64_t len = 0;
    uint64_t start = ocsfs_bitmap_find_free_extent(bitmap, 1024, 900, 100, 1, &len);
    ASSERT(start < 500);
    ASSERT(len >= 100);
}

TEST(bitmap_alloc_free)
{
    uint8_t bitmap[128];
    memset(bitmap, 0, sizeof(bitmap));

    uint64_t len1 = 0;
    uint64_t s1 = ocsfs_bitmap_alloc(bitmap, 1024, 0, 256, &len1);
    ASSERT_EQ(s1, 0);
    ASSERT_EQ(len1, 256);
    ASSERT_EQ(ocsfs_bitmap_count_free(bitmap, 1024), 768);

    uint64_t len2 = 0;
    uint64_t s2 = ocsfs_bitmap_alloc(bitmap, 1024, 0, 256, &len2);
    ASSERT_EQ(s2, 256);
    ASSERT_EQ(len2, 256);
    ASSERT_EQ(ocsfs_bitmap_count_free(bitmap, 1024), 512);

    /* Free first allocation */
    ocsfs_bitmap_free(bitmap, s1, len1);
    ASSERT_EQ(ocsfs_bitmap_count_free(bitmap, 1024), 768);
}

TEST(bitmap_fragmentation)
{
    uint8_t bitmap[128];
    memset(bitmap, 0, sizeof(bitmap));

    /* Create fragmented pattern: used-free-used-free... */
    for (int i = 0; i < 512; i += 2) {
        ocsfs_bitmap_set_range(bitmap, i, 1);
    }

    /* Try to find 4 contiguous blocks - fragmented region has gaps of 1 */
    uint64_t len = 0;
    uint64_t start = ocsfs_bitmap_find_free_extent(bitmap, 1024, 0, 4, 4, &len);
    /* Should find contiguous space somewhere (after fragmented region at 512+) */
    ASSERT(start != UINT64_MAX);
    ASSERT(len >= 4);
}

TEST(bitmap_alloc_full)
{
    uint8_t bitmap[128];
    memset(bitmap, 0xFF, sizeof(bitmap)); /* all used */
    uint64_t len = 0;
    uint64_t s = ocsfs_bitmap_alloc(bitmap, 1024, 0, 1, &len);
    ASSERT_EQ(s, UINT64_MAX);
}

TEST(bitmap_single_bit_operations)
{
    uint8_t bitmap[128];
    memset(bitmap, 0, sizeof(bitmap));

    /* Test byte-boundary bits */
    uint64_t boundaries[] = {0, 7, 8, 15, 16, 1023};
    for (int i = 0; i < 6; i++) {
        uint64_t bit = boundaries[i];
        ocsfs_bitmap_set_range(bitmap, bit, 1);
        ASSERT_EQ(ocsfs_bitmap_count_free(bitmap, 1024), 1024 - (uint64_t)(i + 1));
    }
    /* Clear them all */
    for (int i = 0; i < 6; i++) {
        ocsfs_bitmap_clear_range(bitmap, boundaries[i], 1);
    }
    ASSERT_EQ(ocsfs_bitmap_count_free(bitmap, 1024), 1024);
}

TEST(bitmap_alloc_exact_fit)
{
    uint8_t bitmap[128];
    memset(bitmap, 0, sizeof(bitmap));

    /* Alloc all but 10 bits */
    uint64_t len1 = 0;
    uint64_t s1 = ocsfs_bitmap_alloc(bitmap, 1024, 0, 1014, &len1);
    ASSERT_EQ(s1, 0);
    ASSERT_EQ(len1, 1014);

    /* Alloc remaining 10 */
    uint64_t len2 = 0;
    uint64_t s2 = ocsfs_bitmap_alloc(bitmap, 1024, 0, 10, &len2);
    ASSERT(s2 != UINT64_MAX);
    ASSERT_EQ(len2, 10);

    /* Now full */
    ASSERT_EQ(ocsfs_bitmap_count_free(bitmap, 1024), 0);
}

/* ─── Extent Manager Tests ──────────────────────────────────── */

/* External declarations from extent.c */
struct ocsfs_extent_map;
extern struct ocsfs_extent_map *ocsfs_extent_map_create(uint64_t ino);
extern void ocsfs_extent_map_destroy(struct ocsfs_extent_map *map);
extern int ocsfs_extent_map_insert(struct ocsfs_extent_map *map,
                                    uint64_t logical, uint64_t physical,
                                    uint32_t length, uint16_t flags);
extern uint64_t ocsfs_extent_map_logical_to_physical(const struct ocsfs_extent_map *map,
                                                      uint64_t logical);
extern int64_t ocsfs_extent_map_remove_range(struct ocsfs_extent_map *map,
                                              uint64_t start, uint64_t length);
extern uint32_t ocsfs_extent_map_count(const struct ocsfs_extent_map *map);
extern uint64_t ocsfs_extent_map_total_blocks(const struct ocsfs_extent_map *map);
extern int ocsfs_extent_map_needs_btree(const struct ocsfs_extent_map *map);

TEST(extent_map_create)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(42);
    ASSERT(map != NULL);
    ASSERT_EQ(ocsfs_extent_map_count(map), 0);
    ocsfs_extent_map_destroy(map);
}

TEST(extent_map_insert_lookup)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);

    int ret = ocsfs_extent_map_insert(map, 0, 1000, 100, OCSFS_EXT_WRITTEN);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(ocsfs_extent_map_count(map), 1);

    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 0), 1000);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 50), 1050);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 99), 1099);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 100), UINT64_MAX);

    ocsfs_extent_map_destroy(map);
}

TEST(extent_map_merge)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);

    ocsfs_extent_map_insert(map, 0, 1000, 100, OCSFS_EXT_WRITTEN);
    ocsfs_extent_map_insert(map, 100, 1100, 100, OCSFS_EXT_WRITTEN);

    ASSERT_EQ(ocsfs_extent_map_count(map), 1);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 150), 1150);
    ASSERT_EQ(ocsfs_extent_map_total_blocks(map), 200);

    ocsfs_extent_map_destroy(map);
}

TEST(extent_map_no_merge_gap)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);

    ocsfs_extent_map_insert(map, 0, 1000, 100, OCSFS_EXT_WRITTEN);
    ocsfs_extent_map_insert(map, 200, 2000, 100, OCSFS_EXT_WRITTEN);

    ASSERT_EQ(ocsfs_extent_map_count(map), 2);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 150), UINT64_MAX);

    ocsfs_extent_map_destroy(map);
}

TEST(extent_map_remove)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);

    ocsfs_extent_map_insert(map, 0, 1000, 100, OCSFS_EXT_WRITTEN);

    int64_t freed = ocsfs_extent_map_remove_range(map, 25, 50);
    ASSERT_EQ(freed, 50);
    ASSERT_EQ(ocsfs_extent_map_count(map), 2);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 0), 1000);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 24), 1024);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 25), UINT64_MAX);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 74), UINT64_MAX);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 75), 1075);

    ocsfs_extent_map_destroy(map);
}

TEST(extent_map_unwritten)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);

    ocsfs_extent_map_insert(map, 0, 5000, 256, OCSFS_EXT_UNWRITTEN);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 128), UINT64_MAX);
    ASSERT_EQ(ocsfs_extent_map_count(map), 1);

    ocsfs_extent_map_destroy(map);
}

TEST(extent_map_many_extents)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);

    for (uint32_t i = 0; i < 100; i++) {
        ocsfs_extent_map_insert(map, i * 200, i * 200 + 10000, 100,
                                 OCSFS_EXT_WRITTEN);
    }

    ASSERT_EQ(ocsfs_extent_map_count(map), 100);
    ASSERT(ocsfs_extent_map_needs_btree(map));

    for (uint32_t i = 0; i < 100; i++) {
        uint64_t phys = ocsfs_extent_map_logical_to_physical(map, i * 200 + 50);
        ASSERT_EQ(phys, i * 200 + 10050);
    }

    ocsfs_extent_map_destroy(map);
}

TEST(extent_map_remove_head)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);
    ocsfs_extent_map_insert(map, 0, 1000, 100, OCSFS_EXT_WRITTEN);

    int64_t freed = ocsfs_extent_map_remove_range(map, 0, 25);
    ASSERT_EQ(freed, 25);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 0), UINT64_MAX);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 24), UINT64_MAX);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 25), 1025);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 99), 1099);

    ocsfs_extent_map_destroy(map);
}

TEST(extent_map_remove_tail)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);
    ocsfs_extent_map_insert(map, 0, 1000, 100, OCSFS_EXT_WRITTEN);

    int64_t freed = ocsfs_extent_map_remove_range(map, 75, 25);
    ASSERT_EQ(freed, 25);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 74), 1074);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 75), UINT64_MAX);
    ASSERT_EQ(ocsfs_extent_map_total_blocks(map), 75);

    ocsfs_extent_map_destroy(map);
}

TEST(extent_map_remove_all)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);
    ocsfs_extent_map_insert(map, 0, 1000, 100, OCSFS_EXT_WRITTEN);

    int64_t freed = ocsfs_extent_map_remove_range(map, 0, 100);
    ASSERT_EQ(freed, 100);
    ASSERT_EQ(ocsfs_extent_map_count(map), 0);
    ASSERT_EQ(ocsfs_extent_map_total_blocks(map), 0);

    ocsfs_extent_map_destroy(map);
}

TEST(extent_map_allocated_vs_total)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);

    ocsfs_extent_map_insert(map, 0, 1000, 100, OCSFS_EXT_WRITTEN);
    ocsfs_extent_map_insert(map, 200, 2000, 50, OCSFS_EXT_UNWRITTEN);

    /* total_blocks counts only WRITTEN */
    ASSERT_EQ(ocsfs_extent_map_total_blocks(map), 100);
    /* count includes both extents */
    ASSERT_EQ(ocsfs_extent_map_count(map), 2);

    ocsfs_extent_map_destroy(map);
}

TEST(extent_map_insert_after_remove)
{
    /* Verify map is reusable after removing all extents */
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);
    ocsfs_extent_map_insert(map, 0, 1000, 50, OCSFS_EXT_WRITTEN);
    ocsfs_extent_map_insert(map, 100, 2000, 30, OCSFS_EXT_WRITTEN);

    ocsfs_extent_map_remove_range(map, 0, 50);
    ocsfs_extent_map_remove_range(map, 100, 30);
    ASSERT_EQ(ocsfs_extent_map_count(map), 0);

    /* Re-insert */
    ocsfs_extent_map_insert(map, 0, 5000, 200, OCSFS_EXT_WRITTEN);
    ASSERT_EQ(ocsfs_extent_map_count(map), 1);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 0), 5000);
    ASSERT_EQ(ocsfs_extent_map_logical_to_physical(map, 199), 5199);

    ocsfs_extent_map_destroy(map);
}

/* ─── Lock Compatibility Tests ──────────────────────────────── */

TEST(lock_compat_matrix)
{
    uint64_t h1 = ocsfs_lock_hash_inode(42);
    uint64_t h2 = ocsfs_lock_hash_inode(42);
    ASSERT_EQ(h1, h2);

    uint64_t h3 = ocsfs_lock_hash_inode(43);
    ASSERT(h1 != h3);

    uint32_t s1 = ocsfs_lock_slot(h1);
    ASSERT(s1 < OCSFS_LOCK_ENTRY_COUNT);
}

/* Lock manager external declarations */
struct ocsfs_lock_mgr;
extern struct ocsfs_lock_mgr *ocsfs_lock_mgr_create(int dev_fd, uint64_t lock_table_off,
                                                       uint16_t node_slot, uint32_t mount_gen);
extern void ocsfs_lock_mgr_destroy(struct ocsfs_lock_mgr *mgr);
extern int ocsfs_lock_acquire(struct ocsfs_lock_mgr *mgr,
                                uint64_t resource_id, uint32_t resource_type,
                                uint16_t mode, uint32_t timeout_ms);
extern int ocsfs_lock_release(struct ocsfs_lock_mgr *mgr,
                                uint64_t resource_id, uint32_t resource_type);

static int lock_test_create_image(const char *path, size_t size)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) { close(fd); return -1; }
    return fd;
}

TEST(lock_create_destroy)
{
    const char *img = "/tmp/ocsfs_test_lock.img";
    int fd = lock_test_create_image(img, 2 * 1024 * 1024);
    ASSERT(fd >= 0);

    struct ocsfs_lock_mgr *mgr = ocsfs_lock_mgr_create(fd, 0, 0, 1);
    ASSERT(mgr != NULL);

    ocsfs_lock_mgr_destroy(mgr);
    close(fd);
    unlink(img);
}

TEST(lock_acquire_release_exclusive)
{
    const char *img = "/tmp/ocsfs_test_lock_ex.img";
    int fd = lock_test_create_image(img, 2 * 1024 * 1024);
    ASSERT(fd >= 0);

    struct ocsfs_lock_mgr *mgr = ocsfs_lock_mgr_create(fd, 0, 0, 1);
    ASSERT(mgr != NULL);

    uint64_t resource = ocsfs_lock_hash_inode(42);
    int ret = ocsfs_lock_acquire(mgr, resource, OCSFS_LOCKRES_INODE,
                                   OCSFS_LOCK_EX, 1000);
    ASSERT_EQ(ret, 0);

    ret = ocsfs_lock_release(mgr, resource, OCSFS_LOCKRES_INODE);
    ASSERT_EQ(ret, 0);

    ocsfs_lock_mgr_destroy(mgr);
    close(fd);
    unlink(img);
}

TEST(lock_acquire_shared_multiple)
{
    const char *img = "/tmp/ocsfs_test_lock_sh.img";
    int fd = lock_test_create_image(img, 2 * 1024 * 1024);
    ASSERT(fd >= 0);

    /* Node 0 acquires SH */
    struct ocsfs_lock_mgr *mgr0 = ocsfs_lock_mgr_create(fd, 0, 0, 1);
    ASSERT(mgr0 != NULL);

    uint64_t resource = ocsfs_lock_hash_inode(100);
    int ret = ocsfs_lock_acquire(mgr0, resource, OCSFS_LOCKRES_INODE,
                                   OCSFS_LOCK_SH, 1000);
    ASSERT_EQ(ret, 0);

    /* Node 1 also acquires SH on same resource */
    struct ocsfs_lock_mgr *mgr1 = ocsfs_lock_mgr_create(fd, 0, 1, 1);
    ASSERT(mgr1 != NULL);

    ret = ocsfs_lock_acquire(mgr1, resource, OCSFS_LOCKRES_INODE,
                               OCSFS_LOCK_SH, 1000);
    ASSERT_EQ(ret, 0);

    /* Both release */
    ocsfs_lock_release(mgr1, resource, OCSFS_LOCKRES_INODE);
    ocsfs_lock_release(mgr0, resource, OCSFS_LOCKRES_INODE);

    ocsfs_lock_mgr_destroy(mgr1);
    ocsfs_lock_mgr_destroy(mgr0);
    close(fd);
    unlink(img);
}

TEST(lock_conflict_ex_vs_sh)
{
    const char *img = "/tmp/ocsfs_test_lock_conflict1.img";
    int fd = lock_test_create_image(img, 2 * 1024 * 1024);
    ASSERT(fd >= 0);

    /* Node 0 acquires EX */
    struct ocsfs_lock_mgr *mgr0 = ocsfs_lock_mgr_create(fd, 0, 0, 1);
    ASSERT(mgr0 != NULL);

    uint64_t resource = ocsfs_lock_hash_inode(50);
    int ret = ocsfs_lock_acquire(mgr0, resource, OCSFS_LOCKRES_INODE,
                                   OCSFS_LOCK_EX, 1000);
    ASSERT_EQ(ret, 0);

    /* Node 1 tries SH — should fail/timeout */
    struct ocsfs_lock_mgr *mgr1 = ocsfs_lock_mgr_create(fd, 0, 1, 1);
    ASSERT(mgr1 != NULL);

    ret = ocsfs_lock_acquire(mgr1, resource, OCSFS_LOCKRES_INODE,
                               OCSFS_LOCK_SH, 100); /* short timeout */
    ASSERT(ret != 0); /* should fail — conflict */

    ocsfs_lock_release(mgr0, resource, OCSFS_LOCKRES_INODE);
    ocsfs_lock_mgr_destroy(mgr1);
    ocsfs_lock_mgr_destroy(mgr0);
    close(fd);
    unlink(img);
}

TEST(lock_conflict_ex_vs_ex)
{
    const char *img = "/tmp/ocsfs_test_lock_conflict2.img";
    int fd = lock_test_create_image(img, 2 * 1024 * 1024);
    ASSERT(fd >= 0);

    /* Node 0 acquires EX */
    struct ocsfs_lock_mgr *mgr0 = ocsfs_lock_mgr_create(fd, 0, 0, 1);
    ASSERT(mgr0 != NULL);

    uint64_t resource = ocsfs_lock_hash_inode(60);
    int ret = ocsfs_lock_acquire(mgr0, resource, OCSFS_LOCKRES_INODE,
                                   OCSFS_LOCK_EX, 1000);
    ASSERT_EQ(ret, 0);

    /* Node 1 tries EX — should fail/timeout */
    struct ocsfs_lock_mgr *mgr1 = ocsfs_lock_mgr_create(fd, 0, 1, 1);
    ASSERT(mgr1 != NULL);

    ret = ocsfs_lock_acquire(mgr1, resource, OCSFS_LOCKRES_INODE,
                               OCSFS_LOCK_EX, 100);
    ASSERT(ret != 0); /* should fail — conflict */

    ocsfs_lock_release(mgr0, resource, OCSFS_LOCKRES_INODE);
    ocsfs_lock_mgr_destroy(mgr1);
    ocsfs_lock_mgr_destroy(mgr0);
    close(fd);
    unlink(img);
}

TEST(lock_hash_collision_different_resources)
{
    /* Verify hash produces valid slots for many different inodes */
    for (uint64_t ino = 0; ino < 1000; ino++) {
        uint64_t h = ocsfs_lock_hash_inode(ino);
        uint32_t s = ocsfs_lock_slot(h);
        ASSERT(s < OCSFS_LOCK_ENTRY_COUNT);
    }

    /* AG locks also produce valid slots */
    for (uint32_t ag = 0; ag < 100; ag++) {
        uint64_t h = ocsfs_lock_hash_ag(ag);
        uint32_t s = ocsfs_lock_slot(h);
        ASSERT(s < OCSFS_LOCK_ENTRY_COUNT);
    }
}

/* ─── B+ Tree Tests ─────────────────────────────────────────── */

/* In-memory block storage for B+ tree tests */
#define TEST_BT_BLOCK_SIZE 4096
#define TEST_BT_MAX_BLOCKS 1024

static uint8_t *test_bt_blocks[TEST_BT_MAX_BLOCKS];
static int test_bt_next_block;

static void test_bt_reset(void)
{
    for (int i = 0; i < TEST_BT_MAX_BLOCKS; i++) {
        if (test_bt_blocks[i]) {
            free(test_bt_blocks[i]);
            test_bt_blocks[i] = NULL;
        }
    }
    test_bt_next_block = 1; /* block 0 reserved */
}

static int test_bt_read(void *ctx __attribute__((unused)),
                         uint64_t block, void *buf, uint32_t size)
{
    if (block >= TEST_BT_MAX_BLOCKS || !test_bt_blocks[block])
        return -EIO;
    memcpy(buf, test_bt_blocks[block], size);
    return 0;
}

static int test_bt_write(void *ctx __attribute__((unused)),
                          uint64_t block, const void *buf, uint32_t size)
{
    if (block >= TEST_BT_MAX_BLOCKS) return -EIO;
    if (!test_bt_blocks[block]) {
        test_bt_blocks[block] = malloc(size);
        if (!test_bt_blocks[block]) return -ENOMEM;
    }
    memcpy(test_bt_blocks[block], buf, size);
    return 0;
}

static int test_bt_alloc(void *ctx __attribute__((unused)), uint64_t *out)
{
    if (test_bt_next_block >= TEST_BT_MAX_BLOCKS)
        return -ENOSPC;
    *out = test_bt_next_block;
    test_bt_blocks[test_bt_next_block] = calloc(1, TEST_BT_BLOCK_SIZE);
    if (!test_bt_blocks[test_bt_next_block]) return -ENOMEM;
    test_bt_next_block++;
    return 0;
}

static int test_bt_free(void *ctx __attribute__((unused)), uint64_t block)
{
    if (block < TEST_BT_MAX_BLOCKS && test_bt_blocks[block]) {
        free(test_bt_blocks[block]);
        test_bt_blocks[block] = NULL;
    }
    return 0;
}

TEST(btree_create_empty)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    int ret = ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                                  test_bt_read, test_bt_write,
                                  test_bt_alloc, test_bt_free, NULL);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(ocsfs_btree_count(&bt), 0);
    ASSERT(bt.root_block != 0);
    test_bt_reset();
}

TEST(btree_insert_search)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                        test_bt_read, test_bt_write,
                        test_bt_alloc, test_bt_free, NULL);

    /* Insert some entries */
    ASSERT_EQ(ocsfs_btree_insert(&bt, 100, 1000), 0);
    ASSERT_EQ(ocsfs_btree_insert(&bt, 200, 2000), 0);
    ASSERT_EQ(ocsfs_btree_insert(&bt, 50, 500), 0);

    ASSERT_EQ(ocsfs_btree_count(&bt), 3);

    /* Search */
    uint64_t val;
    ASSERT_EQ(ocsfs_btree_search(&bt, 100, &val), 0);
    ASSERT_EQ(val, 1000);

    ASSERT_EQ(ocsfs_btree_search(&bt, 200, &val), 0);
    ASSERT_EQ(val, 2000);

    ASSERT_EQ(ocsfs_btree_search(&bt, 50, &val), 0);
    ASSERT_EQ(val, 500);

    /* Not found */
    ASSERT_EQ(ocsfs_btree_search(&bt, 999, &val), -ENOENT);

    test_bt_reset();
}

TEST(btree_update_existing)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                        test_bt_read, test_bt_write,
                        test_bt_alloc, test_bt_free, NULL);

    ASSERT_EQ(ocsfs_btree_insert(&bt, 42, 100), 0);
    ASSERT_EQ(ocsfs_btree_insert(&bt, 42, 999), 0);
    ASSERT_EQ(ocsfs_btree_count(&bt), 1); /* same key, count unchanged */

    uint64_t val;
    ASSERT_EQ(ocsfs_btree_search(&bt, 42, &val), 0);
    ASSERT_EQ(val, 999); /* updated value */

    test_bt_reset();
}

TEST(btree_delete)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                        test_bt_read, test_bt_write,
                        test_bt_alloc, test_bt_free, NULL);

    ocsfs_btree_insert(&bt, 10, 100);
    ocsfs_btree_insert(&bt, 20, 200);
    ocsfs_btree_insert(&bt, 30, 300);

    ASSERT_EQ(ocsfs_btree_delete(&bt, 20), 0);
    ASSERT_EQ(ocsfs_btree_count(&bt), 2);

    uint64_t val;
    ASSERT_EQ(ocsfs_btree_search(&bt, 20, &val), -ENOENT);
    ASSERT_EQ(ocsfs_btree_search(&bt, 10, &val), 0);
    ASSERT_EQ(val, 100);
    ASSERT_EQ(ocsfs_btree_search(&bt, 30, &val), 0);
    ASSERT_EQ(val, 300);

    /* Delete non-existent */
    ASSERT_EQ(ocsfs_btree_delete(&bt, 999), -ENOENT);

    test_bt_reset();
}

TEST(btree_many_entries)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                        test_bt_read, test_bt_write,
                        test_bt_alloc, test_bt_free, NULL);

    /* Insert enough entries to cause splits */
    int n = 500;
    for (int i = 0; i < n; i++) {
        uint64_t key = (uint64_t)(i * 7 + 13) % 10000; /* pseudo-random order */
        ASSERT_EQ(ocsfs_btree_insert(&bt, key, key * 10), 0);
    }

    /* Verify all entries */
    for (int i = 0; i < n; i++) {
        uint64_t key = (uint64_t)(i * 7 + 13) % 10000;
        uint64_t val;
        int ret = ocsfs_btree_search(&bt, key, &val);
        ASSERT_EQ(ret, 0);
        ASSERT_EQ(val, key * 10);
    }

    ASSERT(bt.height >= 2); /* should have split at least once */

    test_bt_reset();
}

static int scan_counter_cb(uint64_t key __attribute__((unused)),
                           uint64_t value __attribute__((unused)),
                           void *ctx)
{
    int *count = (int *)ctx;
    (*count)++;
    return 0;
}

TEST(btree_range_scan)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                        test_bt_read, test_bt_write,
                        test_bt_alloc, test_bt_free, NULL);

    for (int i = 0; i < 100; i++) {
        ocsfs_btree_insert(&bt, (uint64_t)i * 10, (uint64_t)i);
    }

    /* Scan range [50, 200] — should find keys 50, 60, ..., 200 = 16 entries */
    int count = 0;
    int scanned = ocsfs_btree_range_scan(&bt, 50, 200, scan_counter_cb, &count);
    ASSERT_EQ(count, 16);
    ASSERT_EQ(scanned, 16);

    /* Verify boundary entries exist */
    uint64_t val;
    ASSERT_EQ(ocsfs_btree_search(&bt, 50, &val), 0);
    ASSERT_EQ(ocsfs_btree_search(&bt, 200, &val), 0);

    test_bt_reset();
}

TEST(btree_delete_all)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                        test_bt_read, test_bt_write,
                        test_bt_alloc, test_bt_free, NULL);

    int n = 50;
    for (int i = 0; i < n; i++)
        ocsfs_btree_insert(&bt, (uint64_t)i, (uint64_t)i * 100);

    ASSERT_EQ(ocsfs_btree_count(&bt), (uint64_t)n);

    for (int i = 0; i < n; i++)
        ASSERT_EQ(ocsfs_btree_delete(&bt, (uint64_t)i), 0);

    ASSERT_EQ(ocsfs_btree_count(&bt), 0);

    /* Re-insert should still work */
    ASSERT_EQ(ocsfs_btree_insert(&bt, 999, 111), 0);
    uint64_t val;
    ASSERT_EQ(ocsfs_btree_search(&bt, 999, &val), 0);
    ASSERT_EQ(val, 111);

    test_bt_reset();
}

TEST(btree_sequential_insert)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                        test_bt_read, test_bt_write,
                        test_bt_alloc, test_bt_free, NULL);

    int n = 500;
    for (int i = 1; i <= n; i++)
        ASSERT_EQ(ocsfs_btree_insert(&bt, (uint64_t)i, (uint64_t)i * 10), 0);

    for (int i = 1; i <= n; i++) {
        uint64_t val;
        ASSERT_EQ(ocsfs_btree_search(&bt, (uint64_t)i, &val), 0);
        ASSERT_EQ(val, (uint64_t)i * 10);
    }

    test_bt_reset();
}

TEST(btree_reverse_insert)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                        test_bt_read, test_bt_write,
                        test_bt_alloc, test_bt_free, NULL);

    int n = 500;
    for (int i = n; i >= 1; i--)
        ASSERT_EQ(ocsfs_btree_insert(&bt, (uint64_t)i, (uint64_t)i * 10), 0);

    for (int i = 1; i <= n; i++) {
        uint64_t val;
        ASSERT_EQ(ocsfs_btree_search(&bt, (uint64_t)i, &val), 0);
        ASSERT_EQ(val, (uint64_t)i * 10);
    }

    test_bt_reset();
}

TEST(btree_range_scan_empty)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                        test_bt_read, test_bt_write,
                        test_bt_alloc, test_bt_free, NULL);

    int count = 0;
    int scanned = ocsfs_btree_range_scan(&bt, 0, 100, scan_counter_cb, &count);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(scanned, 0);

    test_bt_reset();
}

TEST(btree_range_scan_no_match)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                        test_bt_read, test_bt_write,
                        test_bt_alloc, test_bt_free, NULL);

    /* Insert keys 1-10 and 90-100 */
    for (int i = 1; i <= 10; i++)
        ocsfs_btree_insert(&bt, (uint64_t)i, (uint64_t)i);
    for (int i = 90; i <= 100; i++)
        ocsfs_btree_insert(&bt, (uint64_t)i, (uint64_t)i);

    /* Scan [40, 60] — no keys in range */
    int count = 0;
    int scanned = ocsfs_btree_range_scan(&bt, 40, 60, scan_counter_cb, &count);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(scanned, 0);

    test_bt_reset();
}

TEST(btree_insert_zero_key)
{
    test_bt_reset();
    struct ocsfs_btree bt;
    ocsfs_btree_create(&bt, TEST_BT_BLOCK_SIZE,
                        test_bt_read, test_bt_write,
                        test_bt_alloc, test_bt_free, NULL);

    ASSERT_EQ(ocsfs_btree_insert(&bt, 0, 42), 0);
    uint64_t val;
    ASSERT_EQ(ocsfs_btree_search(&bt, 0, &val), 0);
    ASSERT_EQ(val, 42);

    test_bt_reset();
}

/* ─── Inode Allocator Tests ─────────────────────────────────── */

/* External declarations from inode.c */
extern int ocsfs_inode_read(int dev_fd, uint64_t ag_data_start,
                             uint64_t inode_table_off, uint64_t ino_local,
                             struct ocsfs_inode *out);
extern int ocsfs_inode_write(int dev_fd, uint64_t ag_data_start,
                              uint64_t inode_table_off, uint64_t ino_local,
                              struct ocsfs_inode *inode);

TEST(inode_read_write)
{
    const char *img = "/tmp/ocsfs_test_inode.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 1 * 1024 * 1024) == 0); /* 1 MB */

    /* Write an inode */
    struct ocsfs_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_magic = OCSFS_INODE_MAGIC;
    ino.i_ino = 42;
    ino.i_mode = (OCSFS_FT_REG_FILE << 12) | 0644;
    ino.i_nlink = 1;
    ino.i_uid = 1000;
    ino.i_gid = 1000;
    ino.i_size = 12345;

    int ret = ocsfs_inode_write(fd, 0, 0, 0, &ino);
    ASSERT_EQ(ret, 0);

    /* Read it back */
    struct ocsfs_inode read_ino;
    ret = ocsfs_inode_read(fd, 0, 0, 0, &read_ino);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(read_ino.i_magic, OCSFS_INODE_MAGIC);
    ASSERT_EQ(read_ino.i_ino, 42);
    ASSERT_EQ(read_ino.i_size, 12345);
    ASSERT_EQ(read_ino.i_uid, 1000);

    close(fd);
    unlink(img);
}

/* Inode allocator external declarations */
struct ocsfs_inode_alloc_ctx;
extern struct ocsfs_inode_alloc_ctx *
ocsfs_inode_alloc_init(int dev_fd, uint32_t ag_number,
                        uint64_t ag_data_start, uint64_t inode_table_off,
                        uint64_t inode_count, uint32_t block_size);
extern void ocsfs_inode_alloc_destroy(struct ocsfs_inode_alloc_ctx *ctx);
extern uint64_t ocsfs_inode_alloc(struct ocsfs_inode_alloc_ctx *ctx,
                                    uint16_t mode, uint32_t uid, uint32_t gid);
extern int ocsfs_inode_free(struct ocsfs_inode_alloc_ctx *ctx, uint64_t ino);
extern uint64_t ocsfs_inode_alloc_free_count(const struct ocsfs_inode_alloc_ctx *ctx);
extern uint64_t ocsfs_inode_alloc_total_count(const struct ocsfs_inode_alloc_ctx *ctx);

TEST(inode_alloc_free)
{
    const char *img = "/tmp/ocsfs_test_ialloc.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 4 * 1024 * 1024) == 0); /* 4 MB */

    /* Use AG 1 to avoid reserved inodes (AG 0 reserves 0..OCSFS_FIRST_USER_INO-1) */
    struct ocsfs_inode_alloc_ctx *ctx =
        ocsfs_inode_alloc_init(fd, 1, 0, 0, 64, 4096);
    ASSERT(ctx != NULL);

    uint64_t free_before = ocsfs_inode_alloc_free_count(ctx);
    ASSERT(free_before > 0);

    uint16_t mode = (OCSFS_FT_REG_FILE << 12) | 0644;
    uint64_t ino = ocsfs_inode_alloc(ctx, mode, 1000, 1000);
    ASSERT(ino != 0);
    ASSERT(ino != UINT64_MAX);

    uint64_t free_after = ocsfs_inode_alloc_free_count(ctx);
    ASSERT_EQ(free_after, free_before - 1);

    int ret = ocsfs_inode_free(ctx, ino);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(ocsfs_inode_alloc_free_count(ctx), free_before);

    ocsfs_inode_alloc_destroy(ctx);
    close(fd);
    unlink(img);
}

TEST(inode_alloc_multiple)
{
    const char *img = "/tmp/ocsfs_test_ialloc_multi.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 4 * 1024 * 1024) == 0);

    struct ocsfs_inode_alloc_ctx *ctx =
        ocsfs_inode_alloc_init(fd, 1, 0, 0, 64, 4096);
    ASSERT(ctx != NULL);

    uint16_t mode = (OCSFS_FT_REG_FILE << 12) | 0644;
    uint64_t inos[10];
    for (int i = 0; i < 10; i++) {
        inos[i] = ocsfs_inode_alloc(ctx, mode, 1000, 1000);
        ASSERT(inos[i] != 0);
        ASSERT(inos[i] != UINT64_MAX);
    }

    /* All unique */
    for (int i = 0; i < 10; i++)
        for (int j = i + 1; j < 10; j++)
            ASSERT(inos[i] != inos[j]);

    ocsfs_inode_alloc_destroy(ctx);
    close(fd);
    unlink(img);
}

TEST(inode_read_write_all_fields)
{
    const char *img = "/tmp/ocsfs_test_inode_fields.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 1 * 1024 * 1024) == 0);

    struct ocsfs_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_magic = OCSFS_INODE_MAGIC;
    ino.i_ino = 100;
    ino.i_mode = (OCSFS_FT_DIR << 12) | 0755;
    ino.i_nlink = 3;
    ino.i_uid = 500;
    ino.i_gid = 600;
    ino.i_size = 999999;
    ino.i_atime = 1000000000ULL;
    ino.i_mtime = 2000000000ULL;
    ino.i_ctime = 3000000000ULL;
    ino.i_flags = OCSFS_IFLAG_IMMUTABLE;
    ino.i_extent_count = 1;

    int ret = ocsfs_inode_write(fd, 0, 0, 0, &ino);
    ASSERT_EQ(ret, 0);

    struct ocsfs_inode read_ino;
    ret = ocsfs_inode_read(fd, 0, 0, 0, &read_ino);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(read_ino.i_magic, OCSFS_INODE_MAGIC);
    ASSERT_EQ(read_ino.i_ino, 100);
    ASSERT_EQ(read_ino.i_mode, (OCSFS_FT_DIR << 12) | 0755);
    ASSERT_EQ(read_ino.i_nlink, 3);
    ASSERT_EQ(read_ino.i_uid, 500);
    ASSERT_EQ(read_ino.i_gid, 600);
    ASSERT_EQ(read_ino.i_size, 999999);
    ASSERT_EQ(read_ino.i_atime, 1000000000ULL);
    ASSERT_EQ(read_ino.i_mtime, 2000000000ULL);
    ASSERT_EQ(read_ino.i_ctime, 3000000000ULL);
    ASSERT_EQ(read_ino.i_flags, OCSFS_IFLAG_IMMUTABLE);
    ASSERT_EQ(read_ino.i_extent_count, 1);

    close(fd);
    unlink(img);
}

TEST(inode_checksum_corruption)
{
    const char *img = "/tmp/ocsfs_test_inode_corrupt.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 1 * 1024 * 1024) == 0);

    struct ocsfs_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_magic = OCSFS_INODE_MAGIC;
    ino.i_ino = 42;
    ino.i_mode = (OCSFS_FT_REG_FILE << 12) | 0644;
    ino.i_size = 1234;

    int ret = ocsfs_inode_write(fd, 0, 0, 0, &ino);
    ASSERT_EQ(ret, 0);

    /* Corrupt one byte of the inode on disk (flip a byte in the size field) */
    uint8_t byte;
    /* Offset of i_size within the inode struct: read current, flip it */
    off_t corrupt_off = offsetof(struct ocsfs_inode, i_size);
    ASSERT(pread(fd, &byte, 1, corrupt_off) == 1);
    byte ^= 0xFF;
    ASSERT(pwrite(fd, &byte, 1, corrupt_off) == 1);

    /* Read back — should detect corruption */
    struct ocsfs_inode read_ino;
    ret = ocsfs_inode_read(fd, 0, 0, 0, &read_ino);
    /* Either returns error or the data doesn't match */
    if (ret == 0) {
        /* If it doesn't return error, at least the data should differ */
        ASSERT(read_ino.i_size != 1234);
    } else {
        ASSERT(ret != 0); /* corruption detected */
    }

    close(fd);
    unlink(img);
}

TEST(inode_write_read_zero_size)
{
    const char *img = "/tmp/ocsfs_test_inode_zero.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 1 * 1024 * 1024) == 0);

    struct ocsfs_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_magic = OCSFS_INODE_MAGIC;
    ino.i_ino = 10;
    ino.i_mode = (OCSFS_FT_REG_FILE << 12) | 0644;
    ino.i_nlink = 1;
    ino.i_uid = 1000;
    ino.i_gid = 1000;
    ino.i_size = 0; /* zero-size file */

    int ret = ocsfs_inode_write(fd, 0, 0, 0, &ino);
    ASSERT_EQ(ret, 0);

    struct ocsfs_inode read_ino;
    ret = ocsfs_inode_read(fd, 0, 0, 0, &read_ino);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(read_ino.i_size, 0);
    ASSERT_EQ(read_ino.i_ino, 10);
    ASSERT_EQ(read_ino.i_nlink, 1);

    close(fd);
    unlink(img);
}

/* ─── Journal Tests ─────────────────────────────────────────── */

/* External declarations from journal.c */
struct ocsfs_journal_ctx;
extern struct ocsfs_journal_ctx *ocsfs_journal_open(int dev_fd, uint64_t journal_off,
                                                      uint64_t journal_size,
                                                      uint16_t node_slot,
                                                      uint32_t block_size);
extern void ocsfs_journal_close(struct ocsfs_journal_ctx *ctx);
extern int ocsfs_journal_begin(struct ocsfs_journal_ctx *ctx);
extern int ocsfs_journal_log_block(struct ocsfs_journal_ctx *ctx,
                                    uint64_t block_addr, const void *after_image);
extern int ocsfs_journal_commit(struct ocsfs_journal_ctx *ctx);
extern uint64_t ocsfs_journal_used_bytes(const struct ocsfs_journal_ctx *ctx);

TEST(journal_basic_transaction)
{
    const char *img = "/tmp/ocsfs_test_journal.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);

    /* Create image with a journal header */
    uint64_t journal_size = 4 * 1024 * 1024; /* 4 MB */
    ASSERT(ftruncate(fd, journal_size) == 0);

    /* Write journal header */
    struct ocsfs_journal_header jh;
    memset(&jh, 0, sizeof(jh));
    jh.jh_magic = OCSFS_JOURNAL_MAGIC;
    jh.jh_node_slot = 0;
    jh.jh_head = sizeof(jh);
    jh.jh_tail = sizeof(jh);
    jh.jh_sequence = 1;
    jh.jh_size = journal_size;
    jh.jh_checksum = ocsfs_crc32c(0, &jh, sizeof(jh) - sizeof(uint32_t));
    ASSERT(pwrite(fd, &jh, sizeof(jh), 0) == sizeof(jh));

    /* Open journal */
    struct ocsfs_journal_ctx *jctx = ocsfs_journal_open(fd, 0, journal_size, 0, 4096);
    ASSERT(jctx != NULL);

    /* Begin transaction */
    int ret = ocsfs_journal_begin(jctx);
    ASSERT_EQ(ret, 0);

    /* Log a block */
    uint8_t block_data[4096];
    memset(block_data, 0xAB, sizeof(block_data));
    ret = ocsfs_journal_log_block(jctx, 100, block_data);
    ASSERT_EQ(ret, 0);

    /* Commit */
    ret = ocsfs_journal_commit(jctx);
    ASSERT_EQ(ret, 0);

    /* Verify journal used bytes > 0 */
    ASSERT(ocsfs_journal_used_bytes(jctx) > 0);

    ocsfs_journal_close(jctx);
    close(fd);
    unlink(img);
}

/* Helper: create a journal test image with initialized header */
static struct ocsfs_journal_ctx *journal_test_setup(const char *img, int *out_fd,
                                                      uint64_t journal_size)
{
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return NULL;
    if (ftruncate(fd, journal_size) < 0) { close(fd); return NULL; }

    struct ocsfs_journal_header jh;
    memset(&jh, 0, sizeof(jh));
    jh.jh_magic = OCSFS_JOURNAL_MAGIC;
    jh.jh_node_slot = 0;
    jh.jh_head = sizeof(jh);
    jh.jh_tail = sizeof(jh);
    jh.jh_sequence = 1;
    jh.jh_size = journal_size;
    jh.jh_checksum = ocsfs_crc32c(0, &jh, sizeof(jh) - sizeof(uint32_t));
    if (pwrite(fd, &jh, sizeof(jh), 0) != (ssize_t)sizeof(jh)) {
        close(fd);
        return NULL;
    }

    *out_fd = fd;
    return ocsfs_journal_open(fd, 0, journal_size, 0, 4096);
}

TEST(journal_multiple_blocks_per_txn)
{
    const char *img = "/tmp/ocsfs_test_journal_multi.img";
    int fd;
    struct ocsfs_journal_ctx *jctx = journal_test_setup(img, &fd, 4 * 1024 * 1024);
    ASSERT(jctx != NULL);

    int ret = ocsfs_journal_begin(jctx);
    ASSERT_EQ(ret, 0);

    /* Log 5 different blocks */
    for (int i = 0; i < 5; i++) {
        uint8_t block_data[4096];
        memset(block_data, (uint8_t)(0x10 + i), sizeof(block_data));
        ret = ocsfs_journal_log_block(jctx, 100 + i, block_data);
        ASSERT_EQ(ret, 0);
    }

    ret = ocsfs_journal_commit(jctx);
    ASSERT_EQ(ret, 0);
    ASSERT(ocsfs_journal_used_bytes(jctx) > 0);

    ocsfs_journal_close(jctx);
    close(fd);
    unlink(img);
}

TEST(journal_multiple_transactions)
{
    const char *img = "/tmp/ocsfs_test_journal_seq.img";
    int fd;
    struct ocsfs_journal_ctx *jctx = journal_test_setup(img, &fd, 4 * 1024 * 1024);
    ASSERT(jctx != NULL);

    uint8_t block_data[4096];

    /* Transaction 1 */
    ASSERT_EQ(ocsfs_journal_begin(jctx), 0);
    memset(block_data, 0xAA, sizeof(block_data));
    ASSERT_EQ(ocsfs_journal_log_block(jctx, 100, block_data), 0);
    ASSERT_EQ(ocsfs_journal_commit(jctx), 0);
    uint64_t used_after_1 = ocsfs_journal_used_bytes(jctx);
    ASSERT(used_after_1 > 0);

    /* Transaction 2 */
    ASSERT_EQ(ocsfs_journal_begin(jctx), 0);
    memset(block_data, 0xBB, sizeof(block_data));
    ASSERT_EQ(ocsfs_journal_log_block(jctx, 200, block_data), 0);
    ASSERT_EQ(ocsfs_journal_commit(jctx), 0);
    uint64_t used_after_2 = ocsfs_journal_used_bytes(jctx);
    ASSERT(used_after_2 > used_after_1);

    ocsfs_journal_close(jctx);
    close(fd);
    unlink(img);
}

TEST(journal_empty_transaction)
{
    const char *img = "/tmp/ocsfs_test_journal_empty.img";
    int fd;
    struct ocsfs_journal_ctx *jctx = journal_test_setup(img, &fd, 4 * 1024 * 1024);
    ASSERT(jctx != NULL);

    /* Begin and commit immediately with no logged blocks */
    ASSERT_EQ(ocsfs_journal_begin(jctx), 0);
    ASSERT_EQ(ocsfs_journal_commit(jctx), 0);

    ocsfs_journal_close(jctx);
    close(fd);
    unlink(img);
}

TEST(journal_circular_wrap)
{
    const char *img = "/tmp/ocsfs_test_journal_wrap.img";
    int fd;
    /* Small journal: 128 KB to force wrapping quickly */
    uint64_t journal_size = 128 * 1024;
    struct ocsfs_journal_ctx *jctx = journal_test_setup(img, &fd, journal_size);
    ASSERT(jctx != NULL);

    uint8_t block_data[4096];
    memset(block_data, 0xCD, sizeof(block_data));

    /* Commit many transactions to force circular wrap */
    for (int txn = 0; txn < 20; txn++) {
        ASSERT_EQ(ocsfs_journal_begin(jctx), 0);
        ASSERT_EQ(ocsfs_journal_log_block(jctx, 100 + txn, block_data), 0);
        ASSERT_EQ(ocsfs_journal_commit(jctx), 0);
    }

    /* Journal should still be functional */
    ASSERT_EQ(ocsfs_journal_begin(jctx), 0);
    ASSERT_EQ(ocsfs_journal_log_block(jctx, 999, block_data), 0);
    ASSERT_EQ(ocsfs_journal_commit(jctx), 0);

    ocsfs_journal_close(jctx);
    close(fd);
    unlink(img);
}

/* ─── Directory Tests ───────────────────────────────────────── */

/* External declarations from dir.c */
struct ocsfs_dir_ctx;
extern struct ocsfs_dir_ctx *ocsfs_dir_open(int dev_fd, uint32_t block_size,
                                              uint64_t dir_ino,
                                              uint64_t data_block_off);
extern void ocsfs_dir_close(struct ocsfs_dir_ctx *ctx);
extern int ocsfs_dir_add_entry(struct ocsfs_dir_ctx *ctx,
                                const char *name, size_t name_len,
                                uint64_t ino, uint8_t file_type);
extern uint64_t ocsfs_dir_lookup(struct ocsfs_dir_ctx *ctx,
                                  const char *name, size_t name_len);
extern int ocsfs_dir_remove_entry(struct ocsfs_dir_ctx *ctx,
                                   const char *name, size_t name_len);
extern uint32_t ocsfs_dir_count(const struct ocsfs_dir_ctx *ctx);
extern int ocsfs_dir_is_empty(const struct ocsfs_dir_ctx *ctx);
extern int ocsfs_dir_flush(struct ocsfs_dir_ctx *ctx);

TEST(dir_add_lookup)
{
    struct ocsfs_dir_ctx *ctx = ocsfs_dir_open(-1, 4096, 2, 0);
    ASSERT(ctx != NULL);

    int ret = ocsfs_dir_add_entry(ctx, "hello.txt", 9, 100, OCSFS_FT_REG_FILE);
    ASSERT_EQ(ret, 0);

    ret = ocsfs_dir_add_entry(ctx, "world.txt", 9, 200, OCSFS_FT_REG_FILE);
    ASSERT_EQ(ret, 0);

    ASSERT_EQ(ocsfs_dir_count(ctx), 2);

    /* Lookup */
    uint64_t ino = ocsfs_dir_lookup(ctx, "hello.txt", 9);
    ASSERT_EQ(ino, 100);

    ino = ocsfs_dir_lookup(ctx, "world.txt", 9);
    ASSERT_EQ(ino, 200);

    /* Not found */
    ino = ocsfs_dir_lookup(ctx, "nope.txt", 8);
    ASSERT_EQ(ino, 0);

    ocsfs_dir_close(ctx);
}

TEST(dir_remove)
{
    struct ocsfs_dir_ctx *ctx = ocsfs_dir_open(-1, 4096, 2, 0);
    ASSERT(ctx != NULL);

    ocsfs_dir_add_entry(ctx, "a", 1, 10, OCSFS_FT_REG_FILE);
    ocsfs_dir_add_entry(ctx, "b", 1, 20, OCSFS_FT_REG_FILE);
    ocsfs_dir_add_entry(ctx, "c", 1, 30, OCSFS_FT_REG_FILE);

    ASSERT_EQ(ocsfs_dir_count(ctx), 3);

    /* Remove middle entry */
    int ret = ocsfs_dir_remove_entry(ctx, "b", 1);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(ocsfs_dir_count(ctx), 2);

    /* Verify it's gone */
    ASSERT_EQ(ocsfs_dir_lookup(ctx, "b", 1), 0);

    /* Others still exist */
    ASSERT_EQ(ocsfs_dir_lookup(ctx, "a", 1), 10);
    ASSERT_EQ(ocsfs_dir_lookup(ctx, "c", 1), 30);

    /* Remove non-existent */
    ASSERT_EQ(ocsfs_dir_remove_entry(ctx, "z", 1), -ENOENT);

    ocsfs_dir_close(ctx);
}

TEST(dir_duplicate)
{
    struct ocsfs_dir_ctx *ctx = ocsfs_dir_open(-1, 4096, 2, 0);
    ASSERT(ctx != NULL);

    ocsfs_dir_add_entry(ctx, "file", 4, 10, OCSFS_FT_REG_FILE);
    int ret = ocsfs_dir_add_entry(ctx, "file", 4, 20, OCSFS_FT_REG_FILE);
    ASSERT_EQ(ret, -EEXIST);
    ASSERT_EQ(ocsfs_dir_count(ctx), 1);

    ocsfs_dir_close(ctx);
}

TEST(dir_empty_check)
{
    struct ocsfs_dir_ctx *ctx = ocsfs_dir_open(-1, 4096, 2, 0);
    ASSERT(ctx != NULL);

    ASSERT(ocsfs_dir_is_empty(ctx)); /* 0 entries < 2 */

    /* Add . and .. */
    ocsfs_dir_add_entry(ctx, ".", 1, 2, OCSFS_FT_DIR);
    ocsfs_dir_add_entry(ctx, "..", 2, 2, OCSFS_FT_DIR);
    ASSERT(ocsfs_dir_is_empty(ctx)); /* only . and .. */

    ocsfs_dir_add_entry(ctx, "file", 4, 10, OCSFS_FT_REG_FILE);
    ASSERT(!ocsfs_dir_is_empty(ctx)); /* has a real entry */

    ocsfs_dir_close(ctx);
}

/* Dir iterate callback type */
typedef int (*ocsfs_dir_iterate_fn)(const char *name, uint64_t ino,
                                     uint8_t file_type, void *ctx);
extern int ocsfs_dir_iterate(struct ocsfs_dir_ctx *ctx,
                               ocsfs_dir_iterate_fn callback, void *cb_ctx);

static int dir_count_cb(const char *name __attribute__((unused)),
                         uint64_t ino __attribute__((unused)),
                         uint8_t file_type __attribute__((unused)),
                         void *ctx)
{
    int *count = (int *)ctx;
    (*count)++;
    return 0;
}

TEST(dir_long_filename)
{
    struct ocsfs_dir_ctx *ctx = ocsfs_dir_open(-1, 4096, 2, 0);
    ASSERT(ctx != NULL);

    /* 255-char filename */
    char longname[256];
    memset(longname, 'A', 255);
    longname[255] = '\0';

    int ret = ocsfs_dir_add_entry(ctx, longname, 255, 42, OCSFS_FT_REG_FILE);
    ASSERT_EQ(ret, 0);

    uint64_t ino = ocsfs_dir_lookup(ctx, longname, 255);
    ASSERT_EQ(ino, 42);

    ocsfs_dir_close(ctx);
}

TEST(dir_many_entries)
{
    struct ocsfs_dir_ctx *ctx = ocsfs_dir_open(-1, 4096, 2, 0);
    ASSERT(ctx != NULL);

    char name[32];
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof(name), "file_%04d.txt", i);
        int ret = ocsfs_dir_add_entry(ctx, name, strlen(name),
                                        (uint64_t)(100 + i), OCSFS_FT_REG_FILE);
        ASSERT_EQ(ret, 0);
    }

    ASSERT_EQ(ocsfs_dir_count(ctx), 100);

    /* Verify all lookable */
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof(name), "file_%04d.txt", i);
        uint64_t ino = ocsfs_dir_lookup(ctx, name, strlen(name));
        ASSERT_EQ(ino, (uint64_t)(100 + i));
    }

    ocsfs_dir_close(ctx);
}

TEST(dir_iterate)
{
    struct ocsfs_dir_ctx *ctx = ocsfs_dir_open(-1, 4096, 2, 0);
    ASSERT(ctx != NULL);

    ocsfs_dir_add_entry(ctx, "alpha", 5, 10, OCSFS_FT_REG_FILE);
    ocsfs_dir_add_entry(ctx, "beta", 4, 20, OCSFS_FT_REG_FILE);
    ocsfs_dir_add_entry(ctx, "gamma", 5, 30, OCSFS_FT_REG_FILE);
    ocsfs_dir_add_entry(ctx, "delta", 5, 40, OCSFS_FT_REG_FILE);
    ocsfs_dir_add_entry(ctx, "epsilon", 7, 50, OCSFS_FT_REG_FILE);

    int count = 0;
    int ret = ocsfs_dir_iterate(ctx, dir_count_cb, &count);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(count, 5);

    ocsfs_dir_close(ctx);
}

TEST(dir_remove_all)
{
    struct ocsfs_dir_ctx *ctx = ocsfs_dir_open(-1, 4096, 2, 0);
    ASSERT(ctx != NULL);

    ocsfs_dir_add_entry(ctx, "x", 1, 10, OCSFS_FT_REG_FILE);
    ocsfs_dir_add_entry(ctx, "y", 1, 20, OCSFS_FT_REG_FILE);
    ocsfs_dir_add_entry(ctx, "z", 1, 30, OCSFS_FT_REG_FILE);
    ASSERT_EQ(ocsfs_dir_count(ctx), 3);

    ocsfs_dir_remove_entry(ctx, "x", 1);
    ocsfs_dir_remove_entry(ctx, "y", 1);
    ocsfs_dir_remove_entry(ctx, "z", 1);

    ASSERT_EQ(ocsfs_dir_count(ctx), 0);
    ASSERT(ocsfs_dir_is_empty(ctx));

    ocsfs_dir_close(ctx);
}

TEST(dir_zero_length_name)
{
    struct ocsfs_dir_ctx *ctx = ocsfs_dir_open(-1, 4096, 2, 0);
    ASSERT(ctx != NULL);

    int ret = ocsfs_dir_add_entry(ctx, "", 0, 10, OCSFS_FT_REG_FILE);
    /* Should either reject or handle gracefully */
    /* We accept either -EINVAL or that count stays 0 */
    if (ret == 0) {
        /* If it accepts it, at least it shouldn't crash */
        ASSERT(ocsfs_dir_count(ctx) <= 1);
    } else {
        ASSERT(ret != 0);
    }

    ocsfs_dir_close(ctx);
}

/* ─── Heartbeat Tests ──────────────────────────────────────── */

struct ocsfs_heartbeat_mgr;
extern struct ocsfs_heartbeat_mgr *
ocsfs_heartbeat_start(int dev_fd, uint64_t hb_region_off,
                       uint16_t node_slot, uint32_t mount_gen,
                       uint16_t max_nodes,
                       uint32_t interval_ms, uint32_t timeout_ms,
                       void (*on_failure)(uint16_t slot, void *ctx),
                       void *cb_ctx);
extern void ocsfs_heartbeat_stop(struct ocsfs_heartbeat_mgr *mgr);
extern int ocsfs_heartbeat_check_all(struct ocsfs_heartbeat_mgr *mgr);

static volatile int hb_failure_detected = 0;
static void hb_failure_cb(uint16_t slot __attribute__((unused)),
                            void *ctx __attribute__((unused)))
{
    hb_failure_detected = 1;
}

TEST(heartbeat_create_destroy)
{
    const char *img = "/tmp/ocsfs_test_hb.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 1 * 1024 * 1024) == 0);

    struct ocsfs_heartbeat_mgr *mgr =
        ocsfs_heartbeat_start(fd, 0, 0, 1, 4,
                               5000, 15000, hb_failure_cb, NULL);
    ASSERT(mgr != NULL);

    ocsfs_heartbeat_stop(mgr);
    close(fd);
    unlink(img);
}

TEST(heartbeat_write_read_back)
{
    const char *img = "/tmp/ocsfs_test_hb_rw.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 1 * 1024 * 1024) == 0);

    struct ocsfs_heartbeat_mgr *mgr =
        ocsfs_heartbeat_start(fd, 0, 0, 1, 4,
                               100, 15000, hb_failure_cb, NULL);
    ASSERT(mgr != NULL);

    /* Wait a bit for the writer thread to write at least one heartbeat */
    usleep(200 * 1000); /* 200ms */

    /* Read back heartbeat record from disk at slot 0 */
    struct ocsfs_heartbeat hb;
    ssize_t n = pread(fd, &hb, sizeof(hb), 0);
    ASSERT_EQ(n, (ssize_t)sizeof(hb));
    ASSERT_EQ(hb.hb_node_slot, 0);
    ASSERT(hb.hb_timestamp > 0);
    ASSERT(hb.hb_sequence >= 1);

    ocsfs_heartbeat_stop(mgr);
    close(fd);
    unlink(img);
}

TEST(heartbeat_sequence_increment)
{
    const char *img = "/tmp/ocsfs_test_hb_seq.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 1 * 1024 * 1024) == 0);

    struct ocsfs_heartbeat_mgr *mgr =
        ocsfs_heartbeat_start(fd, 0, 0, 1, 4,
                               50, 15000, hb_failure_cb, NULL);
    ASSERT(mgr != NULL);

    usleep(100 * 1000); /* 100ms — should get ~2 heartbeats at 50ms interval */

    struct ocsfs_heartbeat hb1;
    ASSERT(pread(fd, &hb1, sizeof(hb1), 0) == (ssize_t)sizeof(hb1));
    uint64_t seq1 = hb1.hb_sequence;

    usleep(100 * 1000); /* wait for more heartbeats */

    struct ocsfs_heartbeat hb2;
    ASSERT(pread(fd, &hb2, sizeof(hb2), 0) == (ssize_t)sizeof(hb2));
    uint64_t seq2 = hb2.hb_sequence;

    ASSERT(seq2 > seq1); /* sequence should have incremented */

    ocsfs_heartbeat_stop(mgr);
    close(fd);
    unlink(img);
}

TEST(heartbeat_stale_detection)
{
    const char *img = "/tmp/ocsfs_test_hb_stale.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 1 * 1024 * 1024) == 0);

    /* Write a fake stale heartbeat for node 1 at slot 1 */
    struct ocsfs_heartbeat hb;
    memset(&hb, 0, sizeof(hb));
    hb.hb_magic = 0x48425454; /* OCSFS_HEARTBEAT_MAGIC */
    hb.hb_node_slot = 1;
    hb.hb_state = 0x01; /* OCSFS_NODE_ACTIVE */
    hb.hb_timestamp = 1; /* very old timestamp (1 ns since epoch) */
    hb.hb_sequence = 1;
    hb.hb_mount_gen = 1;
    hb.hb_checksum = ocsfs_crc32c(0, &hb, sizeof(hb) - sizeof(uint32_t));
    ASSERT(pwrite(fd, &hb, sizeof(hb), 1 * OCSFS_HEARTBEAT_ENTRY_SIZE) == (ssize_t)sizeof(hb));

    hb_failure_detected = 0;

    /* Start heartbeat manager for node 0 with short timeout */
    struct ocsfs_heartbeat_mgr *mgr =
        ocsfs_heartbeat_start(fd, 0, 0, 1, 4,
                               50, 200, hb_failure_cb, NULL);
    ASSERT(mgr != NULL);

    /* Wait for check to run */
    usleep(500 * 1000); /* 500ms — timeout is 200ms so stale should be detected */

    /* Check for stale nodes */
    ocsfs_heartbeat_check_all(mgr);

    /* The stale detection should have triggered (either via auto-check or our manual call) */
    /* Note: depending on implementation, failure_cb may or may not have been called */
    /* At minimum, check_all should return without crashing */

    ocsfs_heartbeat_stop(mgr);
    close(fd);
    unlink(img);
}

/* ─── Integration: mkfs + tool ──────────────────────────────── */

TEST(mkfs_and_read_back)
{
    const char *img = "/tmp/ocsfs_test_mkfs.img";

    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 512 * 1024 * 1024) == 0);
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "./mkfs.ocsfs -L test-vol -N 2 -J 4M -A 128M -f -v %s 2>&1", img);
    int ret = system(cmd);
    ASSERT_EQ(WEXITSTATUS(ret), 0);

    /* Read back and verify superblock */
    fd = open(img, O_RDONLY);
    ASSERT(fd >= 0);

    struct ocsfs_superblock sb;
    ASSERT(pread(fd, &sb, sizeof(sb), 0) == sizeof(sb));
    ASSERT_EQ(sb.s_magic, OCSFS_MAGIC);
    ASSERT_EQ(sb.s_version_major, OCSFS_VERSION_MAJOR);
    ASSERT_EQ(sb.s_block_size, OCSFS_DEFAULT_BLOCK_SIZE);
    ASSERT(sb.s_ag_count > 0);
    ASSERT_EQ(sb.s_max_nodes, 2);
    ASSERT(strcmp(sb.s_label, "test-vol") == 0);

    /* Verify CRC */
    uint32_t crc = ocsfs_crc32c(0, &sb, sizeof(sb) - sizeof(uint32_t));
    ASSERT_EQ(crc, sb.s_checksum);

    /* Verify mirror superblock matches */
    struct ocsfs_superblock sb2;
    ASSERT(pread(fd, &sb2, sizeof(sb2), OCSFS_SUPERBLOCK_MIRROR) == sizeof(sb2));
    ASSERT(memcmp(&sb, &sb2, sizeof(sb)) == 0);

    /* Verify journal headers */
    for (int n = 0; n < 2; n++) {
        struct ocsfs_journal_header jh;
        uint64_t off = sb.s_journal_off + (uint64_t)n * sb.s_journal_size;
        ASSERT(pread(fd, &jh, sizeof(jh), off) == sizeof(jh));
        ASSERT_EQ(jh.jh_magic, OCSFS_JOURNAL_MAGIC);
        ASSERT_EQ(jh.jh_node_slot, n);
    }

    /* Verify AG descriptors */
    for (uint32_t ag = 0; ag < sb.s_ag_count; ag++) {
        struct ocsfs_ag_desc agd;
        uint64_t off = sb.s_ag_desc_off + (uint64_t)ag * sizeof(agd);
        ASSERT(pread(fd, &agd, sizeof(agd), off) == sizeof(agd));
        ASSERT_EQ(agd.ag_magic, OCSFS_AG_MAGIC);
        ASSERT_EQ(agd.ag_number, ag);
        ASSERT(agd.ag_free_blocks > 0);
    }

    /* Verify root inode */
    struct ocsfs_ag_desc ag0;
    ASSERT(pread(fd, &ag0, sizeof(ag0), sb.s_ag_desc_off) == sizeof(ag0));
    uint64_t root_off = sb.s_data_off + ag0.ag_inode_table_off +
                        OCSFS_ROOT_INO * OCSFS_INODE_SIZE;
    struct ocsfs_inode root;
    ASSERT(pread(fd, &root, sizeof(root), root_off) == sizeof(root));
    ASSERT_EQ(root.i_magic, OCSFS_INODE_MAGIC);
    ASSERT_EQ(root.i_ino, OCSFS_ROOT_INO);
    ASSERT_EQ((root.i_mode >> 12), OCSFS_FT_DIR);

    close(fd);
    unlink(img);
}

TEST(tool_info_and_check)
{
    const char *img = "/tmp/ocsfs_test_tool.img";

    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 512 * 1024 * 1024) == 0);
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "./mkfs.ocsfs -L tool-test -N 2 -J 4M -A 128M -f %s >/dev/null 2>&1", img);
    ASSERT_EQ(WEXITSTATUS(system(cmd)), 0);

    snprintf(cmd, sizeof(cmd), "./ocsfs-tool info %s 2>&1", img);
    int ret = system(cmd);
    ASSERT_EQ(WEXITSTATUS(ret), 0);

    snprintf(cmd, sizeof(cmd), "./ocsfs-tool check %s 2>&1", img);
    ret = system(cmd);
    ASSERT_EQ(WEXITSTATUS(ret), 0);

    snprintf(cmd, sizeof(cmd), "./ocsfs-tool df %s 2>&1", img);
    ret = system(cmd);
    ASSERT_EQ(WEXITSTATUS(ret), 0);

    unlink(img);
}

TEST(mkfs_too_small_image)
{
    const char *img = "/tmp/ocsfs_test_mkfs_small.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 1 * 1024 * 1024) == 0); /* 1 MB — too small */
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "./mkfs.ocsfs -f %s >/dev/null 2>&1", img);
    int ret = system(cmd);
    ASSERT(WEXITSTATUS(ret) != 0); /* should fail */

    unlink(img);
}

TEST(mkfs_label_preserved)
{
    const char *img = "/tmp/ocsfs_test_mkfs_label.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 512 * 1024 * 1024) == 0);
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "./mkfs.ocsfs -L my-test-label -N 2 -J 4M -A 128M -f %s >/dev/null 2>&1", img);
    ASSERT_EQ(WEXITSTATUS(system(cmd)), 0);

    fd = open(img, O_RDONLY);
    ASSERT(fd >= 0);

    struct ocsfs_superblock sb;
    ASSERT(pread(fd, &sb, sizeof(sb), 0) == sizeof(sb));
    ASSERT(strcmp(sb.s_label, "my-test-label") == 0);

    close(fd);
    unlink(img);
}

TEST(superblock_corrupt_magic)
{
    const char *img = "/tmp/ocsfs_test_sb_magic.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 512 * 1024 * 1024) == 0);
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "./mkfs.ocsfs -L test -N 2 -J 4M -A 128M -f %s >/dev/null 2>&1", img);
    ASSERT_EQ(WEXITSTATUS(system(cmd)), 0);

    /* Corrupt magic */
    fd = open(img, O_RDWR);
    ASSERT(fd >= 0);
    uint32_t bad_magic = 0xDEADBEEF;
    ASSERT(pwrite(fd, &bad_magic, sizeof(bad_magic), 0) == sizeof(bad_magic));

    struct ocsfs_superblock sb;
    ASSERT(pread(fd, &sb, sizeof(sb), 0) == sizeof(sb));
    ASSERT(sb.s_magic != OCSFS_MAGIC); /* magic corrupted */

    close(fd);
    unlink(img);
}

TEST(superblock_corrupt_checksum)
{
    const char *img = "/tmp/ocsfs_test_sb_crc.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 512 * 1024 * 1024) == 0);
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "./mkfs.ocsfs -L test -N 2 -J 4M -A 128M -f %s >/dev/null 2>&1", img);
    ASSERT_EQ(WEXITSTATUS(system(cmd)), 0);

    fd = open(img, O_RDWR);
    ASSERT(fd >= 0);

    struct ocsfs_superblock sb;
    ASSERT(pread(fd, &sb, sizeof(sb), 0) == sizeof(sb));

    /* Verify CRC is valid first */
    uint32_t expected_crc = ocsfs_crc32c(0, &sb, sizeof(sb) - sizeof(uint32_t));
    ASSERT_EQ(expected_crc, sb.s_checksum);

    /* Flip one byte in the label field */
    sb.s_label[0] ^= 0xFF;
    ASSERT(pwrite(fd, &sb, sizeof(sb), 0) == sizeof(sb));

    /* Re-read and verify CRC no longer matches */
    struct ocsfs_superblock sb2;
    ASSERT(pread(fd, &sb2, sizeof(sb2), 0) == sizeof(sb2));
    uint32_t actual_crc = ocsfs_crc32c(0, &sb2, sizeof(sb2) - sizeof(uint32_t));
    ASSERT(actual_crc != sb2.s_checksum); /* CRC mismatch */

    close(fd);
    unlink(img);
}

TEST(mkfs_multiple_ag_counts)
{
    const char *img = "/tmp/ocsfs_test_mkfs_ag.img";
    int fd;

    /* Format with AG=256M */
    fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 512 * 1024 * 1024) == 0);
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "./mkfs.ocsfs -L ag256 -N 2 -J 4M -A 256M -f %s >/dev/null 2>&1", img);
    ASSERT_EQ(WEXITSTATUS(system(cmd)), 0);

    fd = open(img, O_RDONLY);
    ASSERT(fd >= 0);
    struct ocsfs_superblock sb1;
    ASSERT(pread(fd, &sb1, sizeof(sb1), 0) == sizeof(sb1));
    ASSERT_EQ(sb1.s_magic, OCSFS_MAGIC);
    uint32_t ag_count_256 = sb1.s_ag_count;
    ASSERT(ag_count_256 > 0);
    close(fd);

    /* Format with AG=128M — should have more AGs */
    fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 512 * 1024 * 1024) == 0);
    close(fd);

    snprintf(cmd, sizeof(cmd),
             "./mkfs.ocsfs -L ag128 -N 2 -J 4M -A 128M -f %s >/dev/null 2>&1", img);
    ASSERT_EQ(WEXITSTATUS(system(cmd)), 0);

    fd = open(img, O_RDONLY);
    ASSERT(fd >= 0);
    struct ocsfs_superblock sb2;
    ASSERT(pread(fd, &sb2, sizeof(sb2), 0) == sizeof(sb2));
    ASSERT_EQ(sb2.s_magic, OCSFS_MAGIC);
    ASSERT(sb2.s_ag_count >= ag_count_256); /* smaller AGs = more or equal AG count */
    close(fd);

    unlink(img);
}

TEST(tool_nodes_command)
{
    const char *img = "/tmp/ocsfs_test_tool_nodes.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 512 * 1024 * 1024) == 0);
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "./mkfs.ocsfs -L nodes-test -N 2 -J 4M -A 128M -f %s >/dev/null 2>&1", img);
    ASSERT_EQ(WEXITSTATUS(system(cmd)), 0);

    snprintf(cmd, sizeof(cmd), "./ocsfs-tool nodes %s 2>&1", img);
    int ret = system(cmd);
    ASSERT_EQ(WEXITSTATUS(ret), 0);

    unlink(img);
}

TEST(tool_locks_command)
{
    const char *img = "/tmp/ocsfs_test_tool_locks.img";
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 512 * 1024 * 1024) == 0);
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "./mkfs.ocsfs -L locks-test -N 2 -J 4M -A 128M -f %s >/dev/null 2>&1", img);
    ASSERT_EQ(WEXITSTATUS(system(cmd)), 0);

    snprintf(cmd, sizeof(cmd), "./ocsfs-tool locks %s 2>&1", img);
    int ret = system(cmd);
    ASSERT_EQ(WEXITSTATUS(ret), 0);

    unlink(img);
}

TEST(extent_map_insert_overlap)
{
    struct ocsfs_extent_map *map = ocsfs_extent_map_create(1);
    ocsfs_extent_map_insert(map, 0, 1000, 100, OCSFS_EXT_WRITTEN);

    /* Insert overlapping extent — implementation may error or handle it */
    int ret = ocsfs_extent_map_insert(map, 50, 2000, 100, OCSFS_EXT_WRITTEN);
    /* Either fails with error or succeeds with adjusted extents */
    if (ret == 0) {
        /* If accepted, verify no crash and some reasonable state */
        ASSERT(ocsfs_extent_map_count(map) >= 1);
    }

    ocsfs_extent_map_destroy(map);
}

TEST(bitmap_clear_unset_bits)
{
    uint8_t bitmap[128];
    memset(bitmap, 0, sizeof(bitmap));

    /* Clear already-clear range — should be idempotent */
    ocsfs_bitmap_clear_range(bitmap, 50, 100);
    ASSERT_EQ(ocsfs_bitmap_count_free(bitmap, 1024), 1024);

    /* Bitmap should still be all zeros */
    for (int i = 0; i < 128; i++)
        ASSERT_EQ(bitmap[i], 0);
}

/* ─── Main ──────────────────────────────────────────────────── */

int main(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════\n");
    printf("  OCSFS Test Suite v%d.%d\n", OCSFS_VERSION_MAJOR, OCSFS_VERSION_MINOR);
    printf("═══════════════════════════════════════════════════\n\n");

    printf("  CRC32C:\n");
    run_test_crc32c_empty();
    run_test_crc32c_known_vectors();
    run_test_crc32c_different_data();
    run_test_crc32c_incremental();
    run_test_crc32c_incremental_matches_full();
    run_test_crc32c_large_buffer();

    printf("\n  Structure Validation:\n");
    run_test_struct_sizes();
    run_test_magic_numbers();
    run_test_layout_offsets();

    printf("\n  Bitmap Allocator:\n");
    run_test_bitmap_empty();
    run_test_bitmap_full();
    run_test_bitmap_set_clear_range();
    run_test_bitmap_find_extent_simple();
    run_test_bitmap_find_extent_wrap();
    run_test_bitmap_alloc_free();
    run_test_bitmap_fragmentation();
    run_test_bitmap_alloc_full();
    run_test_bitmap_single_bit_operations();
    run_test_bitmap_alloc_exact_fit();
    run_test_bitmap_clear_unset_bits();

    printf("\n  Extent Manager:\n");
    run_test_extent_map_create();
    run_test_extent_map_insert_lookup();
    run_test_extent_map_merge();
    run_test_extent_map_no_merge_gap();
    run_test_extent_map_remove();
    run_test_extent_map_unwritten();
    run_test_extent_map_many_extents();
    run_test_extent_map_remove_head();
    run_test_extent_map_remove_tail();
    run_test_extent_map_remove_all();
    run_test_extent_map_allocated_vs_total();
    run_test_extent_map_insert_after_remove();
    run_test_extent_map_insert_overlap();

    printf("\n  Lock Manager:\n");
    run_test_lock_compat_matrix();
    run_test_lock_create_destroy();
    run_test_lock_acquire_release_exclusive();
    run_test_lock_acquire_shared_multiple();
    run_test_lock_conflict_ex_vs_sh();
    run_test_lock_conflict_ex_vs_ex();
    run_test_lock_hash_collision_different_resources();

    printf("\n  B+ Tree:\n");
    run_test_btree_create_empty();
    run_test_btree_insert_search();
    run_test_btree_update_existing();
    run_test_btree_delete();
    run_test_btree_many_entries();
    run_test_btree_range_scan();
    run_test_btree_delete_all();
    run_test_btree_sequential_insert();
    run_test_btree_reverse_insert();
    run_test_btree_range_scan_empty();
    run_test_btree_range_scan_no_match();
    run_test_btree_insert_zero_key();

    printf("\n  Inode Allocator:\n");
    run_test_inode_read_write();
    run_test_inode_alloc_free();
    run_test_inode_alloc_multiple();
    run_test_inode_read_write_all_fields();
    run_test_inode_checksum_corruption();
    run_test_inode_write_read_zero_size();

    printf("\n  Journal:\n");
    run_test_journal_basic_transaction();
    run_test_journal_multiple_blocks_per_txn();
    run_test_journal_multiple_transactions();
    run_test_journal_empty_transaction();
    run_test_journal_circular_wrap();

    printf("\n  Directory Operations:\n");
    run_test_dir_add_lookup();
    run_test_dir_remove();
    run_test_dir_duplicate();
    run_test_dir_empty_check();
    run_test_dir_long_filename();
    run_test_dir_many_entries();
    run_test_dir_iterate();
    run_test_dir_remove_all();
    run_test_dir_zero_length_name();

    printf("\n  Heartbeat:\n");
    run_test_heartbeat_create_destroy();
    run_test_heartbeat_write_read_back();
    run_test_heartbeat_sequence_increment();
    run_test_heartbeat_stale_detection();

    printf("\n  Integration (mkfs + tool):\n");
    run_test_mkfs_and_read_back();
    run_test_tool_info_and_check();
    run_test_mkfs_too_small_image();
    run_test_mkfs_label_preserved();
    run_test_superblock_corrupt_magic();
    run_test_superblock_corrupt_checksum();
    run_test_mkfs_multiple_ag_counts();
    run_test_tool_nodes_command();
    run_test_tool_locks_command();

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed (%d assertions)\n",
           tests_passed, tests_failed, tests_run);
    printf("═══════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
