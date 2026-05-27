// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — debugfs.c
 * Expose internal state at /sys/kernel/debug/ocsfs/<devname>/
 *
 *   lock_table    — active in-memory lock resources (mode, resource_id, slot)
 *   journal_stats — journal head/tail/sequence and checkpoint counters
 */

#include <linux/debugfs.h>
#include "ocsfs.h"

static struct dentry *ocsfs_debugfs_root;

/* ═══════════════════════════════════════════════════════════════
 * lock_table seq_file
 * ═══════════════════════════════════════════════════════════════ */

static const char * const lock_mode_name[] = {
	[OCSFS_LOCK_NL] = "NL",
	[OCSFS_LOCK_SH] = "SH",
	[OCSFS_LOCK_EX] = "EX",
	[OCSFS_LOCK_CW] = "CW",
};

static int lock_table_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	struct ocsfs_lock_res *lr;

	seq_printf(m, "%-18s %-4s %-8s %-12s %s\n",
		   "resource_id", "mode", "type", "slot", "overflow");

	spin_lock(&sbi->s_lock_list_lock);
	list_for_each_entry(lr, &sbi->s_lock_list, lr_list) {
		u16 m_idx = lr->lr_mode < ARRAY_SIZE(lock_mode_name)
			    ? lr->lr_mode : 0;
		seq_printf(m, "0x%016llx %-4s %-8u %-12u %llu\n",
			   lr->lr_resource_id,
			   lock_mode_name[m_idx],
			   lr->lr_resource_type,
			   lr->lr_slot,
			   lr->lr_overflow_addr);
	}
	spin_unlock(&sbi->s_lock_list_lock);
	return 0;
}

static int lock_table_open(struct inode *inode, struct file *file)
{
	return single_open(file, lock_table_show, inode->i_private);
}

static const struct file_operations lock_table_fops = {
	.open    = lock_table_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ═══════════════════════════════════════════════════════════════
 * journal_stats seq_file
 * ═══════════════════════════════════════════════════════════════ */

static int journal_stats_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct ocsfs_journal *j = &OCSFS_SB(sb)->s_journal;

	mutex_lock(&j->j_lock);
	seq_printf(m,
		   "head:           %llu\n"
		   "tail:           %llu\n"
		   "size:           %llu\n"
		   "used:           %llu\n"
		   "sequence:       %llu\n"
		   "ckpt_ticket:    %lld\n"
		   "ckpt_now:       %lld\n",
		   j->head, j->tail, j->size,
		   (j->head >= j->tail) ? (j->head - j->tail)
					: (j->size - j->tail + j->head),
		   j->sequence,
		   atomic64_read(&j->j_ckpt_ticket),
		   atomic64_read(&j->j_ckpt_now));
	mutex_unlock(&j->j_lock);
	return 0;
}

static int journal_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, journal_stats_show, inode->i_private);
}

static const struct file_operations journal_stats_fops = {
	.open    = journal_stats_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ═══════════════════════════════════════════════════════════════
 * Public API — called from super.c
 * ═══════════════════════════════════════════════════════════════ */

void ocsfs_debugfs_init(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);
	const char *name;
	struct dentry *dir;

	if (!ocsfs_debugfs_root)
		return;

	/* Use device name as subdirectory (e.g. "loop0", "sdb") */
	name = sb->s_id[0] ? sb->s_id : "unknown";
	dir = debugfs_create_dir(name, ocsfs_debugfs_root);
	if (IS_ERR_OR_NULL(dir)) {
		sbi->s_debugfs_dir = NULL;
		return;
	}
	sbi->s_debugfs_dir = dir;

	debugfs_create_file("lock_table",    0444, dir, sb, &lock_table_fops);
	debugfs_create_file("journal_stats", 0444, dir, sb, &journal_stats_fops);
}

void ocsfs_debugfs_exit(struct super_block *sb)
{
	struct ocsfs_sb_info *sbi = OCSFS_SB(sb);

	debugfs_remove_recursive(sbi->s_debugfs_dir);
	sbi->s_debugfs_dir = NULL;
}

/* Called once from module_init */
void ocsfs_debugfs_module_init(void)
{
	ocsfs_debugfs_root = debugfs_create_dir("ocsfs", NULL);
	if (IS_ERR(ocsfs_debugfs_root))
		ocsfs_debugfs_root = NULL;
}

/* Called once from module_exit */
void ocsfs_debugfs_module_exit(void)
{
	debugfs_remove_recursive(ocsfs_debugfs_root);
	ocsfs_debugfs_root = NULL;
}
