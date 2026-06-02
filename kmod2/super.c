// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS v2 — super.c
 * Module entry/exit + (later) superblock / mount.
 *
 * v2 rearchitecture: single-writer ownership model. This file currently
 * holds the module skeleton and SCSI-transport pool lifecycle; mount support
 * lands in Task C1.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include "ocsfs.h"

static int __init ocsfs2_init(void)
{
	int ret;

	ret = ocsfs2_scsi_pool_init();
	if (ret) {
		pr_err("ocsfs2: SCSI pool init failed: %d\n", ret);
		return ret;
	}

	pr_info("ocsfs2: module loaded (v2 skeleton)\n");
	return 0;
}

static void __exit ocsfs2_exit(void)
{
	ocsfs2_scsi_pool_destroy();
	pr_info("ocsfs2: module unloaded\n");
}

module_init(ocsfs2_init);
module_exit(ocsfs2_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OCSFS v2 - clustered shared-disk filesystem (single-writer ownership)");
MODULE_AUTHOR("OCSFS Project Contributors");
