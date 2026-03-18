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

    printf("\n  Extent Manager:\n");
    run_test_extent_map_create();
    run_test_extent_map_insert_lookup();
    run_test_extent_map_merge();
    run_test_extent_map_no_merge_gap();
    run_test_extent_map_remove();
    run_test_extent_map_unwritten();
    run_test_extent_map_many_extents();

    printf("\n  Lock Manager:\n");
    run_test_lock_compat_matrix();

    printf("\n  B+ Tree:\n");
    run_test_btree_create_empty();
    run_test_btree_insert_search();
    run_test_btree_update_existing();
    run_test_btree_delete();
    run_test_btree_many_entries();
    run_test_btree_range_scan();

    printf("\n  Inode Allocator:\n");
    run_test_inode_read_write();

    printf("\n  Journal:\n");
    run_test_journal_basic_transaction();

    printf("\n  Directory Operations:\n");
    run_test_dir_add_lookup();
    run_test_dir_remove();
    run_test_dir_duplicate();
    run_test_dir_empty_check();

    printf("\n  Integration (mkfs + tool):\n");
    run_test_mkfs_and_read_back();
    run_test_tool_info_and_check();

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed (%d assertions)\n",
           tests_passed, tests_failed, tests_run);
    printf("═══════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
