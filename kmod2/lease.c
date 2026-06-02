// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — lease.c
 * L4 ownership leases (single-writer ownership, spec §6.5) and L5 recovery.
 *
 * A lease entry in the on-disk lease table is claimed/released by SCSI CAW and
 * honoured only while its owner node is ALIVE with a matching generation (the
 * liveness-epoch rule — no per-lease renewal). On a single-node volume these
 * are no-ops. This file currently provides the skeleton; the lease-table CAS
 * and recovery (fence -> replay -> reclaim) are filled in by the L4/L5 steps.
 */
#include "ocsfs.h"

int ocsfs2_lease_acquire(struct super_block *sb, u64 resource, int mode)
{
	if (!OCSFS2_SB(sb)->s_cluster)
		return 0;          /* single-node: ownership is implicit */
	/* TODO L4: CAS the lease-table entry for @resource to {self, EX/SH}. */
	return 0;
}

void ocsfs2_lease_release(struct super_block *sb, u64 resource, int mode)
{
	if (!OCSFS2_SB(sb)->s_cluster)
		return;
	/* TODO L4: CAS the lease-table entry back to NONE. */
}

void ocsfs2_recover_node(struct super_block *sb, u16 slot, u32 gen)
{
	pr_warn("ocsfs2: recovery requested for dead node slot %u gen %u (L5 pending)\n",
		slot, gen);
	/* TODO L5: elect a recovery leader (CAS the recovery block), replay the
	 * dead node's journal, then reclaim every lease owned by {slot, gen}. */
}
