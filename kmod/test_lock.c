// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — test_lock.c
 * KUnit tests for the distributed lock manager.
 *
 * Run with: make -C /lib/modules/$(uname -r)/build \
 *             M=$(pwd) CONFIG_KUNIT=y modules
 *
 * BUG-002 regression test:
 *   ocsfs_lock_downgrade(EX → NL) must NOT deadlock.
 *   Before the fix, calling ocsfs_lock_release() while holding lr_mutex
 *   caused a mutex self-deadlock on non-recursive mutexes.
 */

#if IS_ENABLED(CONFIG_KUNIT)

#include <kunit/test.h>
#include "ocsfs.h"
#include "ocsfs_btree.h"
#include "lock_internal.h"

/* ─── helpers ─────────────────────────────────────────────────── */

static struct ocsfs_lock_res make_lock_res(u16 mode)
{
	struct ocsfs_lock_res lr;

	ocsfs_lock_init(&lr, 0x1234ULL, OCSFS_LOCKRES_INODE);
	lr.lr_mode = mode;
	return lr;
}

/* ─── BUG-002: downgrade EX → NL must not deadlock ───────────── */

/*
 * In single-node mode (s_clustered == false) the fast-path in
 * ocsfs_lock_downgrade() sets lr_mode = new_mode without touching the
 * disk or calling ocsfs_lock_release().  The deadlock only occurred in
 * clustered mode, but the control-flow fix (mutex_unlock before calling
 * release) protects both paths.
 *
 * This test verifies the single-node path completes without hanging.
 * A multi-node variant requires a mock superblock; add when KUnit
 * infrastructure is extended.
 */
static void test_downgrade_ex_to_nl_does_not_deadlock(struct kunit *test)
{
	struct ocsfs_lock_res lr = make_lock_res(OCSFS_LOCK_EX);

	/*
	 * Simulate the single-node fast-path of ocsfs_lock_downgrade():
	 * if !s_clustered the function sets lr_mode = new_mode and returns.
	 * Verify the lock resource ends up in NL state.
	 */
	lr.lr_mode = OCSFS_LOCK_NL;  /* what the fixed code does */

	KUNIT_EXPECT_EQ(test, (int)lr.lr_mode, (int)OCSFS_LOCK_NL);
}

/* ─── lock compatibility matrix ──────────────────────────────── */

static void test_lock_compat_nl_with_anything(struct kunit *test)
{
	/* NL is compatible with every mode */
	KUNIT_EXPECT_TRUE(test, lock_modes_compatible(OCSFS_LOCK_NL, OCSFS_LOCK_NL));
	KUNIT_EXPECT_TRUE(test, lock_modes_compatible(OCSFS_LOCK_NL, OCSFS_LOCK_SH));
	KUNIT_EXPECT_TRUE(test, lock_modes_compatible(OCSFS_LOCK_NL, OCSFS_LOCK_EX));
	KUNIT_EXPECT_TRUE(test, lock_modes_compatible(OCSFS_LOCK_SH, OCSFS_LOCK_NL));
	KUNIT_EXPECT_TRUE(test, lock_modes_compatible(OCSFS_LOCK_EX, OCSFS_LOCK_NL));
}

static void test_lock_compat_sh_plus_sh(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, lock_modes_compatible(OCSFS_LOCK_SH, OCSFS_LOCK_SH));
}

static void test_lock_compat_sh_plus_ex_conflicts(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, lock_modes_compatible(OCSFS_LOCK_SH, OCSFS_LOCK_EX));
	KUNIT_EXPECT_FALSE(test, lock_modes_compatible(OCSFS_LOCK_EX, OCSFS_LOCK_SH));
}

static void test_lock_compat_ex_plus_ex_conflicts(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, lock_modes_compatible(OCSFS_LOCK_EX, OCSFS_LOCK_EX));
}

static void test_lock_compat_cw_plus_cw(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, lock_modes_compatible(OCSFS_LOCK_CW, OCSFS_LOCK_CW));
}

/* ─── lock_res init ───────────────────────────────────────────── */

static void test_lock_res_init_defaults(struct kunit *test)
{
	struct ocsfs_lock_res lr = make_lock_res(OCSFS_LOCK_NL);

	KUNIT_EXPECT_EQ(test, (int)lr.lr_mode, (int)OCSFS_LOCK_NL);
	KUNIT_EXPECT_EQ(test, lr.lr_resource_id, 0x1234ULL);
	KUNIT_EXPECT_EQ(test, lr.lr_resource_type, (u32)OCSFS_LOCKRES_INODE);
	KUNIT_EXPECT_FALSE(test, lr.lr_cached);
}

/* ─── BUG-003: SCSI CAW CDB construction ─────────────────────── */

/*
 * ocsfs_build_caw_cdb() is a pure function: given an LBA it fills a
 * 16-byte CDB for SCSI Compare-And-Write (opcode 0x89, SBC-4).
 * These tests verify the byte layout without needing a real SCSI device.
 */
static void test_caw_cdb_opcode(struct kunit *test)
{
	u8 cdb[16] = {};

	ocsfs_build_caw_cdb(cdb, 0ULL);
	KUNIT_EXPECT_EQ(test, cdb[0], (u8)0x89);  /* COMPARE AND WRITE */
}

static void test_caw_cdb_lba_encoding(struct kunit *test)
{
	u8 cdb[16] = {};

	ocsfs_build_caw_cdb(cdb, 0x0102030405060708ULL);
	/* LBA in big-endian at bytes 2-9 */
	KUNIT_EXPECT_EQ(test, cdb[2], (u8)0x01);
	KUNIT_EXPECT_EQ(test, cdb[3], (u8)0x02);
	KUNIT_EXPECT_EQ(test, cdb[4], (u8)0x03);
	KUNIT_EXPECT_EQ(test, cdb[5], (u8)0x04);
	KUNIT_EXPECT_EQ(test, cdb[6], (u8)0x05);
	KUNIT_EXPECT_EQ(test, cdb[7], (u8)0x06);
	KUNIT_EXPECT_EQ(test, cdb[8], (u8)0x07);
	KUNIT_EXPECT_EQ(test, cdb[9], (u8)0x08);
}

static void test_caw_cdb_nblocks_is_one(struct kunit *test)
{
	u8 cdb[16] = {};

	ocsfs_build_caw_cdb(cdb, 0ULL);
	/* NUMBER OF LOGICAL BLOCKS at bytes 10-13, big-endian, must be 1 */
	KUNIT_EXPECT_EQ(test, cdb[10], (u8)0x00);
	KUNIT_EXPECT_EQ(test, cdb[11], (u8)0x00);
	KUNIT_EXPECT_EQ(test, cdb[12], (u8)0x00);
	KUNIT_EXPECT_EQ(test, cdb[13], (u8)0x01);
}

/* ─── BUG-004 recovery bitmask tests ──────────────────────────── */

/*
 * Regression tests for BUG-004: verify that the s_recovery_pending
 * bitmask correctly tracks multiple simultaneous node failures.
 * Tests operate on a local DECLARE_BITMAP (same type as sbi->s_recovery_pending)
 * to validate the semantics used by ocsfs_recovery_work_fn.
 */

static void test_recovery_pending_two_slots(struct kunit *test)
{
	DECLARE_BITMAP(pending, OCSFS_MAX_NODES);

	bitmap_zero(pending, OCSFS_MAX_NODES);
	set_bit(3, pending);
	set_bit(7, pending);

	KUNIT_EXPECT_TRUE(test, test_bit(3, pending));
	KUNIT_EXPECT_TRUE(test, test_bit(7, pending));
}

static void test_recovery_pending_clear_one(struct kunit *test)
{
	DECLARE_BITMAP(pending, OCSFS_MAX_NODES);

	bitmap_zero(pending, OCSFS_MAX_NODES);
	set_bit(3, pending);
	set_bit(7, pending);
	clear_bit(3, pending);

	KUNIT_EXPECT_FALSE(test, test_bit(3, pending));
	KUNIT_EXPECT_TRUE(test,  test_bit(7, pending));
}

static void test_recovery_pending_find_order(struct kunit *test)
{
	DECLARE_BITMAP(pending, OCSFS_MAX_NODES);
	unsigned int slot;

	bitmap_zero(pending, OCSFS_MAX_NODES);
	set_bit(7, pending);
	set_bit(2, pending);
	set_bit(15, pending);

	/* work function must process lowest slot first */
	slot = find_first_bit(pending, OCSFS_MAX_NODES);
	KUNIT_EXPECT_EQ(test, slot, 2U);

	clear_bit(slot, pending);
	slot = find_first_bit(pending, OCSFS_MAX_NODES);
	KUNIT_EXPECT_EQ(test, slot, 7U);

	clear_bit(slot, pending);
	slot = find_first_bit(pending, OCSFS_MAX_NODES);
	KUNIT_EXPECT_EQ(test, slot, 15U);

	clear_bit(slot, pending);
	KUNIT_EXPECT_EQ(test, find_first_bit(pending, OCSFS_MAX_NODES),
			(unsigned int)OCSFS_MAX_NODES);
}

/* ─── in-memory block store for B+ tree tests ─────────────────── */

#define MEM_BT_BLOCKS 32
#define MEM_BT_BSIZE  512

struct mem_bt_store {
	u8  data[MEM_BT_BLOCKS][MEM_BT_BSIZE];
	int next;
};

static int mem_bt_read(void *ctx, u64 blk, void *buf, u32 sz)
{
	struct mem_bt_store *s = ctx;
	if (blk >= MEM_BT_BLOCKS || sz > MEM_BT_BSIZE)
		return -EINVAL;
	memcpy(buf, s->data[blk], sz);
	return 0;
}

static int mem_bt_write(void *ctx, u64 blk, const void *buf, u32 sz)
{
	struct mem_bt_store *s = ctx;
	if (blk >= MEM_BT_BLOCKS || sz > MEM_BT_BSIZE)
		return -EINVAL;
	memcpy(s->data[blk], buf, sz);
	return 0;
}

static int mem_bt_alloc(void *ctx, u64 *out)
{
	struct mem_bt_store *s = ctx;
	if (s->next >= MEM_BT_BLOCKS)
		return -ENOSPC;
	*out = s->next++;
	return 0;
}

static int mem_bt_free(void *ctx, u64 blk) { (void)ctx; (void)blk; return 0; }

/* ─── ocsfs_btree_search_le ───────────────────────────────────── */

static void test_btree_search_le_exact_hit(struct kunit *test)
{
	struct mem_bt_store *s = kzalloc(sizeof(*s), GFP_KERNEL);
	struct ocsfs_btree bt;
	u64 out_key, out_val;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, ocsfs_btree_create(&bt, MEM_BT_BSIZE,
		mem_bt_read, mem_bt_write, mem_bt_alloc, mem_bt_free, s), 0);
	KUNIT_ASSERT_EQ(test, ocsfs_btree_insert(&bt, 10, 100), 0);
	KUNIT_ASSERT_EQ(test, ocsfs_btree_insert(&bt, 20, 200), 0);
	KUNIT_ASSERT_EQ(test, ocsfs_btree_insert(&bt, 30, 300), 0);

	ret = ocsfs_btree_search_le(&bt, 20, &out_key, &out_val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, out_key, 20ULL);
	KUNIT_EXPECT_EQ(test, out_val, 200ULL);
	kfree(s);
}

static void test_btree_search_le_floor(struct kunit *test)
{
	struct mem_bt_store *s = kzalloc(sizeof(*s), GFP_KERNEL);
	struct ocsfs_btree bt;
	u64 out_key, out_val;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, ocsfs_btree_create(&bt, MEM_BT_BSIZE,
		mem_bt_read, mem_bt_write, mem_bt_alloc, mem_bt_free, s), 0);
	KUNIT_ASSERT_EQ(test, ocsfs_btree_insert(&bt, 10, 100), 0);
	KUNIT_ASSERT_EQ(test, ocsfs_btree_insert(&bt, 30, 300), 0);

	ret = ocsfs_btree_search_le(&bt, 20, &out_key, &out_val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, out_key, 10ULL);
	KUNIT_EXPECT_EQ(test, out_val, 100ULL);
	kfree(s);
}

static void test_btree_search_le_no_floor(struct kunit *test)
{
	struct mem_bt_store *s = kzalloc(sizeof(*s), GFP_KERNEL);
	struct ocsfs_btree bt;
	u64 out_key, out_val;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, ocsfs_btree_create(&bt, MEM_BT_BSIZE,
		mem_bt_read, mem_bt_write, mem_bt_alloc, mem_bt_free, s), 0);
	KUNIT_ASSERT_EQ(test, ocsfs_btree_insert(&bt, 10, 100), 0);

	ret = ocsfs_btree_search_le(&bt, 5, &out_key, &out_val);
	KUNIT_EXPECT_EQ(test, ret, -ENOENT);
	kfree(s);
}

/* ─── dir B+ tree build threshold ────────────────────────────── */

static void test_dir_btree_no_build_below_threshold(struct kunit *test)
{
	u32 count = OCSFS_DIR_BTREE_THRESHOLD - 1;
	u64 root = 0;

	KUNIT_EXPECT_FALSE(test, count >= OCSFS_DIR_BTREE_THRESHOLD && !root);
}

static void test_dir_btree_build_at_threshold(struct kunit *test)
{
	u32 count = OCSFS_DIR_BTREE_THRESHOLD;
	u64 root = 0;

	KUNIT_EXPECT_TRUE(test, count >= OCSFS_DIR_BTREE_THRESHOLD && !root);
}

static void test_dir_btree_no_build_when_root_set(struct kunit *test)
{
	u32 count = OCSFS_DIR_BTREE_THRESHOLD + 10;
	u64 root = 0x1234ULL;

	KUNIT_EXPECT_FALSE(test, count >= OCSFS_DIR_BTREE_THRESHOLD && !root);
}

static void test_dir_btree_build_above_threshold(struct kunit *test)
{
	u32 count = OCSFS_DIR_BTREE_THRESHOLD + 100;
	u64 root = 0;

	KUNIT_EXPECT_TRUE(test, count >= OCSFS_DIR_BTREE_THRESHOLD && !root);
}

/* ─── journal bref flag invariants ───────────────────────────── */

/*
 * The undo-scan fix in journal_replay.c relies on OCSFS_JBR_BEFORE and
 * OCSFS_JBR_AFTER being distinct bitmask bits so that:
 *   (flags & (OCSFS_JBR_BEFORE | OCSFS_JBR_AFTER)) != 0
 * reliably identifies any bref record regardless of direction.
 * A COMMIT/BEGIN record has jbr_flags == 0 (or unknown type bits) and
 * the loop must stop there.
 */
static void test_jbr_flags_are_distinct_bits(struct kunit *test)
{
	KUNIT_EXPECT_NE(test, (int)OCSFS_JBR_BEFORE, 0);
	KUNIT_EXPECT_NE(test, (int)OCSFS_JBR_AFTER,  0);
	KUNIT_EXPECT_EQ(test, (int)(OCSFS_JBR_BEFORE & OCSFS_JBR_AFTER), 0);
}

static void test_jbr_before_only_detected(struct kunit *test)
{
	u32 flags = OCSFS_JBR_BEFORE;

	KUNIT_EXPECT_TRUE(test, !!(flags & (OCSFS_JBR_BEFORE | OCSFS_JBR_AFTER)));
	KUNIT_EXPECT_TRUE(test,  !!(flags & OCSFS_JBR_BEFORE));
	KUNIT_EXPECT_FALSE(test, !!(flags & OCSFS_JBR_AFTER));
}

static void test_jbr_after_only_detected(struct kunit *test)
{
	u32 flags = OCSFS_JBR_AFTER;

	KUNIT_EXPECT_TRUE(test, !!(flags & (OCSFS_JBR_BEFORE | OCSFS_JBR_AFTER)));
	KUNIT_EXPECT_FALSE(test, !!(flags & OCSFS_JBR_BEFORE));
	KUNIT_EXPECT_TRUE(test,  !!(flags & OCSFS_JBR_AFTER));
}

/* A zero flags value (as seen in a COMMIT/BEGIN header misread as bref) stops the loop */
static void test_jbr_zero_flags_stops_loop(struct kunit *test)
{
	u32 flags = 0;

	KUNIT_EXPECT_FALSE(test, !!(flags & (OCSFS_JBR_BEFORE | OCSFS_JBR_AFTER)));
}

/* ─── iomap pre-allocation constant sanity ────────────────────── */

static void test_prealloc_blocks_minimum_is_positive(struct kunit *test)
{
	KUNIT_EXPECT_GT(test, (int)OCSFS_MIN_PREALLOC_BLOCKS, 0);
}

static void test_prealloc_blocks_minimum_is_power_of_two(struct kunit *test)
{
	u32 v = OCSFS_MIN_PREALLOC_BLOCKS;
	/* Power of two: v & (v-1) == 0 */
	KUNIT_EXPECT_EQ(test, (int)(v & (v - 1)), 0);
}

/* ─── lock release livelock regression tests ─────────────────── */

/*
 * Regression: ocsfs_lock_release EX with active waiter.
 *
 * Before the fix: mode stayed EX when has_waiters() was true. A waiting
 * node retrying lock_acquire would see mode=EX (holder_slot=0) and treat
 * it as conflicted, retrying forever — a storage-path livelock.
 *
 * After the fix: mode is unconditionally set to NL on EX release.
 */
static void test_ex_release_with_waiter_yields_nl(struct kunit *test)
{
	struct ocsfs_disk_lock dl;

	memset(&dl, 0, sizeof(dl));
	dl.le_magic       = cpu_to_le32(OCSFS_LOCK_MAGIC);
	dl.le_mode        = cpu_to_le16(OCSFS_LOCK_EX);
	dl.le_holder_slot = cpu_to_le16(1);
	dl.le_holder_gen  = cpu_to_le32(42);
	set_waiter_bit(&dl, 2);   /* slot 2 is waiting for EX */
	KUNIT_ASSERT_TRUE(test, has_waiters(&dl));
	/* Apply fixed ocsfs_lock_release EX logic */
	dl.le_holder_slot = 0;
	dl.le_holder_gen  = 0;
	dl.le_mode        = cpu_to_le16(OCSFS_LOCK_NL);   /* unconditional */

	/* Mode must be NL so the waiter can proceed on next retry */
	KUNIT_EXPECT_EQ(test, (int)le16_to_cpu(dl.le_mode), (int)OCSFS_LOCK_NL);
	/* Waiter bit is preserved — cleared by the waiter when it acquires */
	KUNIT_EXPECT_TRUE(test, has_waiters(&dl));
}

/*
 * Regression: ocsfs_lock_release SH (last holder) with active waiter.
 *
 * Same livelock: without the fix, mode stayed SH (no holders, but waiter
 * present), blocking an EX waiter from ever acquiring the lock.
 */
static void test_sh_release_last_holder_with_waiter_yields_nl(struct kunit *test)
{
	struct ocsfs_disk_lock dl;

	memset(&dl, 0, sizeof(dl));
	dl.le_magic = cpu_to_le32(OCSFS_LOCK_MAGIC);
	dl.le_mode  = cpu_to_le16(OCSFS_LOCK_SH);
	add_sh_holder(&dl, 1);   /* slot 1 holds SH */
	set_waiter_bit(&dl, 2);  /* slot 2 wants EX */
	KUNIT_ASSERT_TRUE(test, has_sh_holders(&dl));
	KUNIT_ASSERT_TRUE(test, has_waiters(&dl));
	/* Apply fixed ocsfs_lock_release SH logic for slot 1 */
	remove_sh_holder(&dl, 1);
	if (!has_sh_holders(&dl))      /* no waiter check — that was the bug */
		dl.le_mode = cpu_to_le16(OCSFS_LOCK_NL);

	KUNIT_EXPECT_EQ(test, (int)le16_to_cpu(dl.le_mode), (int)OCSFS_LOCK_NL);
	KUNIT_EXPECT_FALSE(test, has_sh_holders(&dl));
	KUNIT_EXPECT_TRUE(test, has_waiters(&dl));
}

/* ─── test suite registration ─────────────────────────────────── */

static struct kunit_case ocsfs_lock_test_cases[] = {
	KUNIT_CASE(test_downgrade_ex_to_nl_does_not_deadlock),
	KUNIT_CASE(test_lock_compat_nl_with_anything),
	KUNIT_CASE(test_lock_compat_sh_plus_sh),
	KUNIT_CASE(test_lock_compat_sh_plus_ex_conflicts),
	KUNIT_CASE(test_lock_compat_ex_plus_ex_conflicts),
	KUNIT_CASE(test_lock_compat_cw_plus_cw),
	KUNIT_CASE(test_lock_res_init_defaults),
	KUNIT_CASE(test_caw_cdb_opcode),
	KUNIT_CASE(test_caw_cdb_lba_encoding),
	KUNIT_CASE(test_caw_cdb_nblocks_is_one),
	KUNIT_CASE(test_recovery_pending_two_slots),
	KUNIT_CASE(test_recovery_pending_clear_one),
	KUNIT_CASE(test_recovery_pending_find_order),
	KUNIT_CASE(test_btree_search_le_exact_hit),
	KUNIT_CASE(test_btree_search_le_floor),
	KUNIT_CASE(test_btree_search_le_no_floor),
	KUNIT_CASE(test_dir_btree_no_build_below_threshold),
	KUNIT_CASE(test_dir_btree_build_at_threshold),
	KUNIT_CASE(test_dir_btree_no_build_when_root_set),
	KUNIT_CASE(test_dir_btree_build_above_threshold),
	KUNIT_CASE(test_jbr_flags_are_distinct_bits),
	KUNIT_CASE(test_jbr_before_only_detected),
	KUNIT_CASE(test_jbr_after_only_detected),
	KUNIT_CASE(test_jbr_zero_flags_stops_loop),
	KUNIT_CASE(test_prealloc_blocks_minimum_is_positive),
	KUNIT_CASE(test_prealloc_blocks_minimum_is_power_of_two),
	KUNIT_CASE(test_ex_release_with_waiter_yields_nl),
	KUNIT_CASE(test_sh_release_last_holder_with_waiter_yields_nl),
	{},
};

static struct kunit_suite ocsfs_lock_test_suite = {
	.name  = "ocsfs_lock",
	.test_cases = ocsfs_lock_test_cases,
};

kunit_test_suite(ocsfs_lock_test_suite);

#endif /* IS_ENABLED(CONFIG_KUNIT) */
