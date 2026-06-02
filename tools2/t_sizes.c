/* SPDX-License-Identifier: GPL-2.0-only */
/* Compile-time + runtime check that the on-disk struct sizes are frozen. */
#include "ocsfs_ondisk.h"
#include <stdio.h>

int main(void)
{
	printf("super=%zu inode=%zu ag=%zu extent=%zu dirent=%zu node=%zu hb=%zu lease=%zu jhdr=%zu\n",
	       sizeof(struct ocsfs2_disk_super),
	       sizeof(struct ocsfs2_disk_inode),
	       sizeof(struct ocsfs2_disk_ag),
	       sizeof(struct ocsfs2_disk_extent),
	       sizeof(struct ocsfs2_disk_dirent),
	       sizeof(struct ocsfs2_disk_node_slot),
	       sizeof(struct ocsfs2_disk_heartbeat),
	       sizeof(struct ocsfs2_disk_lease),
	       sizeof(struct ocsfs2_disk_journal_hdr));
	return 0;
}
