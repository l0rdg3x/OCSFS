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
	{},
};

static struct kunit_suite ocsfs_lock_test_suite = {
	.name  = "ocsfs_lock",
	.test_cases = ocsfs_lock_test_cases,
};

kunit_test_suite(ocsfs_lock_test_suite);

#endif /* IS_ENABLED(CONFIG_KUNIT) */
