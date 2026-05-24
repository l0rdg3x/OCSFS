// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — test_cas.c
 * KUnit tests per il CAS engine (M1).
 *
 * Run with: make -C /lib/modules/$(uname -r)/build \
 *             M=$(pwd) CONFIG_KUNIT=y modules
 */

#if IS_ENABLED(CONFIG_KUNIT)

#include <kunit/test.h>
#include "ocsfs.h"

/* Test 1: invariante di dimensione struct — 32 byte per entry */
static void test_cas_lease_size(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, (int)sizeof(struct ocsfs_disk_cas_lease), 32);
}

/* Test 2: lease area non sovrapposta alla lock table */
static void test_cas_lease_offset_no_overlap(struct kunit *test)
{
	u64 lock_end = (u64)OCSFS_LOCK_TABLE_OFF + OCSFS_LOCK_TABLE_SIZE;

	KUNIT_EXPECT_EQ(test, OCSFS_CAS_LEASE_OFF, lock_end);
}

/* Test 3: enum backend ordinato correttamente */
static void test_cas_backend_enum_order(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, (int)CAS_BACKEND_NONE,     0);
	KUNIT_EXPECT_EQ(test, (int)CAS_BACKEND_PR_LEASE, 1);
	KUNIT_EXPECT_EQ(test, (int)CAS_BACKEND_SCSI_CAW, 2);
}

/* Test 4: ns_version è nei primi 4 byte di ns_reserved2 (layout stabile) */
static void test_node_slot_ns_version_layout(struct kunit *test)
{
	struct ocsfs_disk_node_slot slot;

	/* ns_version + ns_reserved2[104] deve sommare a 108 (precedente ns_reserved2[108]) */
	KUNIT_EXPECT_EQ(test,
		(int)(sizeof(slot.ns_version) + sizeof(slot.ns_reserved2)),
		108);
}

/* Test 5: CAS_LEASE_ENTRIES * sizeof(lease) si adatta in blocchi da 4096 */
static void test_cas_lease_entries_fit_in_blocks(struct kunit *test)
{
	/* 256 entry * 32 byte = 8192 byte = 2 blocchi da 4096 */
	u32 total_bytes = OCSFS_CAS_LEASE_ENTRIES * sizeof(struct ocsfs_disk_cas_lease);

	KUNIT_EXPECT_EQ(test, total_bytes % 4096u, 0u);
}

static struct kunit_case cas_test_cases[] = {
	KUNIT_CASE(test_cas_lease_size),
	KUNIT_CASE(test_cas_lease_offset_no_overlap),
	KUNIT_CASE(test_cas_backend_enum_order),
	KUNIT_CASE(test_node_slot_ns_version_layout),
	KUNIT_CASE(test_cas_lease_entries_fit_in_blocks),
	{},
};

static struct kunit_suite ocsfs_cas_test_suite = {
	.name       = "ocsfs_cas",
	.test_cases = cas_test_cases,
};

kunit_test_suite(ocsfs_cas_test_suite);

#endif /* IS_ENABLED(CONFIG_KUNIT) */
