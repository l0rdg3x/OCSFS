// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — dir_btree.c
 * B+ tree index for large directories.
 *
 * When a directory's entry count exceeds OCSFS_DIR_BTREE_THRESHOLD,
 * a B+ tree is built on-disk to accelerate lookup from O(n) to O(log n).
 * The flat dirent list in the inode's data blocks is preserved — the B+ tree
 * is an index: key = name hash, value = encoded block+offset of the dirent.
 *
 * Collision handling: two names with the same hash are extremely rare with
 * 64-bit keys. On a hash match, the caller always verifies the actual name.
 * If the name doesn't match, ocsfs_dir_btree_lookup returns 0 and the caller
 * falls back to the flat linear scan.
 *
 * Write operations (insert, delete, migrate) open a WAL transaction so that
 * btree node writes and block alloc/free are atomic with respect to crashes.
 */

#include "ocsfs.h"
#include "ocsfs_btree.h"


/* Encode block+offset into the 64-bit btree value */
static inline u64 dir_encode_val(u64 block, u32 offset)
{
	return (block << 32) | (u64)offset;
}

static inline void dir_decode_val(u64 val, u64 *block, u32 *offset)
{
	*block  = val >> 32;
	*offset = (u32)(val & 0xffffffffu);
}

/* 64-bit name hash: two independent CRC32C passes */
static u64 dir_name_hash(const char *name, unsigned int len)
{
	u32 hi = ocsfs_crc32c(len,  name, len);
	u32 lo = ocsfs_crc32c(~hi,  name, len);

	return ((u64)hi << 32) | lo;
}

/* ── B+ tree I/O callbacks ── */

struct dir_btree_ctx {
	struct super_block *sb;
	struct ocsfs_txn   *txn;  /* NULL for read-only operations */
};

static int dir_btree_read(void *ctx, u64 block, void *buf, u32 size)
{
	struct dir_btree_ctx *dc = ctx;
	struct buffer_head *bh   = sb_bread(dc->sb, block);

	if (!bh)
		return -EIO;
	memcpy(buf, bh->b_data, size);
	brelse(bh);
	return 0;
}

/*
 * Journal before-image then overwrite: makes btree node writes atomic.
 * sb_bread is used (not sb_getblk) to capture the real before-image.
 */
static int dir_btree_write(void *ctx, u64 block, const void *buf, u32 size)
{
	struct dir_btree_ctx *dc = ctx;
	struct buffer_head *bh;
	int ret;

	bh = sb_bread(dc->sb, block);
	if (!bh)
		return -EIO;

	if (dc->txn) {
		ret = ocsfs_txn_add_bh(dc->txn, bh);
		if (ret) {
			brelse(bh);
			return ret;
		}
	}

	lock_buffer(bh);
	memcpy(bh->b_data, buf, size);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

static int dir_btree_alloc(void *ctx, u64 *out_block)
{
	struct dir_btree_ctx *dc = ctx;
	u64 block;
	int ret;

	if (dc->txn)
		ret = ocsfs_alloc_blocks_txn(dc->txn, dc->sb, 0, 1, &block);
	else
		ret = ocsfs_alloc_blocks(dc->sb, 0, 1, &block);

	if (!ret)
		*out_block = block;
	return ret;
}

static int dir_btree_free(void *ctx, u64 block)
{
	struct dir_btree_ctx *dc = ctx;

	if (dc->txn)
		return ocsfs_free_blocks_txn(dc->txn, block, 1);
	ocsfs_free_blocks(dc->sb, block, 1);
	return 0;
}

/* ── open existing btree for a directory ── */

static int dir_btree_open(struct inode *dir, struct ocsfs_btree *bt,
			  struct dir_btree_ctx *dc, struct ocsfs_txn *txn)
{
	struct ocsfs_sb_info *sbi    = OCSFS_SB(dir->i_sb);
	struct ocsfs_inode_info *oi  = OCSFS_I(dir);

	if (!oi->i_dir_btree_root)
		return -ENOENT;

	dc->sb  = dir->i_sb;
	dc->txn = txn;
	return ocsfs_btree_open(bt, oi->i_dir_btree_root, sbi->s_block_size,
				dir_btree_read, dir_btree_write,
				dir_btree_alloc, dir_btree_free, dc);
}

/* ── public API ── */

/*
 * Lookup a name in the directory's B+ tree.
 * Returns the inode number, or 0 if not found / hash collision.
 * *ft_out is set to the dirent's file type when non-NULL and found.
 */
u64 ocsfs_dir_btree_lookup(struct inode *dir, const struct qstr *name,
			   u8 *ft_out)
{
	struct ocsfs_inode_info *oi = OCSFS_I(dir);
	struct ocsfs_sb_info *sbi   = OCSFS_SB(dir->i_sb);
	struct ocsfs_btree bt;
	struct dir_btree_ctx dc;
	struct buffer_head *bh;
	struct ocsfs_disk_dirent *de;
	u64 hash, encoded, block;
	u32 offset;
	int ret;

	if (!oi->i_dir_btree_root)
		return 0;

	ret = dir_btree_open(dir, &bt, &dc, NULL);
	if (ret)
		return 0;

	hash = dir_name_hash(name->name, name->len);
	ret  = ocsfs_btree_search(&bt, hash, &encoded);
	if (ret)
		return 0;

	dir_decode_val(encoded, &block, &offset);

	/* Verify name — guards against hash collisions */
	bh = sb_bread(dir->i_sb, block);
	if (!bh)
		return 0;

	if (offset + sizeof(*de) > sbi->s_block_size) {
		brelse(bh);
		return 0;
	}

	de = (struct ocsfs_disk_dirent *)(bh->b_data + offset);

	if (le32_to_cpu(de->de_magic) != OCSFS_DIRENT_MAGIC ||
	    de->de_name_len != name->len ||
	    memcmp(de->de_name, name->name, name->len) != 0) {
		brelse(bh);
		return 0;  /* collision — caller falls back to linear scan */
	}

	if (ft_out)
		*ft_out = de->de_file_type;
	block = le64_to_cpu(de->de_ino);
	brelse(bh);
	return block;
}

/*
 * Insert or update a name→dirent mapping in the B+ tree.
 * phys_block and offset identify the dirent's location on disk.
 */
int ocsfs_dir_btree_insert(struct inode *dir, const struct qstr *name,
			   u64 phys_block, u32 offset)
{
	struct ocsfs_inode_info *oi = OCSFS_I(dir);
	struct ocsfs_btree bt;
	struct dir_btree_ctx dc;
	struct ocsfs_txn *txn;
	u64 hash;
	int ret;

	if (!oi->i_dir_btree_root)
		return -ENOENT;

	txn = ocsfs_txn_begin(dir->i_sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);

	if (dir_btree_open(dir, &bt, &dc, txn)) {
		ocsfs_txn_abort(txn);
		return -EIO;
	}

	hash = dir_name_hash(name->name, name->len);
	ret = ocsfs_btree_insert(&bt, hash, dir_encode_val(phys_block, offset));
	if (!ret)
		oi->i_dir_btree_root = bt.root_block;

	if (ret)
		ocsfs_txn_abort(txn);
	else
		ret = ocsfs_txn_commit(txn);
	return ret;
}

/* Remove a name from the B+ tree index */
int ocsfs_dir_btree_delete(struct inode *dir, const struct qstr *name)
{
	struct ocsfs_inode_info *oi = OCSFS_I(dir);
	struct ocsfs_btree bt;
	struct dir_btree_ctx dc;
	struct ocsfs_txn *txn;
	u64 hash;
	int ret;

	if (!oi->i_dir_btree_root)
		return 0;

	txn = ocsfs_txn_begin(dir->i_sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);

	if (dir_btree_open(dir, &bt, &dc, txn)) {
		ocsfs_txn_abort(txn);
		return -EIO;
	}

	hash = dir_name_hash(name->name, name->len);
	ret = ocsfs_btree_delete(&bt, hash);
	if (!ret)
		oi->i_dir_btree_root = bt.root_block;

	if (ret)
		ocsfs_txn_abort(txn);
	else
		ret = ocsfs_txn_commit(txn);
	return ret;
}

/*
 * Maximum btree inserts per journal transaction during migrate.
 * Limits j_lock hold time: large dirs no longer stall all other writers.
 * Each partial commit persists the btree root in the inode, so a crash
 * mid-migrate leaves a valid partial index; missing entries fall back to
 * the linear scan until the directory is re-opened.
 */
#define OCSFS_MIGRATE_CHUNK  64

/*
 * Build the B+ tree index by scanning the flat dirent list.
 * Called when i_dirent_count first crosses OCSFS_DIR_BTREE_THRESHOLD.
 * Uses chunked transactions (OCSFS_MIGRATE_CHUNK inserts per txn) to
 * bound j_lock hold time without sacrificing crash recoverability.
 */
int ocsfs_dir_btree_migrate(struct inode *dir)
{
	struct ocsfs_sb_info *sbi    = OCSFS_SB(dir->i_sb);
	struct ocsfs_inode_info *oi  = OCSFS_I(dir);
	struct ocsfs_btree bt;
	struct dir_btree_ctx dc;
	struct ocsfs_txn *txn;
	u64 dir_blocks, b;
	int ret, chunk = 0;

	txn = ocsfs_txn_begin(dir->i_sb);
	if (IS_ERR(txn))
		return PTR_ERR(txn);

	dc.sb  = dir->i_sb;
	dc.txn = txn;

	ret = ocsfs_btree_create(&bt, sbi->s_block_size,
				 dir_btree_read, dir_btree_write,
				 dir_btree_alloc, dir_btree_free, &dc);
	if (ret)
		goto abort;

	dir_blocks = (dir->i_size + sbi->s_block_size - 1) / sbi->s_block_size;

	for (b = 0; b < dir_blocks; b++) {
		struct ocsfs_extent ext;
		struct buffer_head *bh;
		u64 phys;
		u32 off;

		if (ocsfs_extent_lookup(dir, b, &ext) || !ext.physical_block)
			continue;
		phys = ext.physical_block + (b - ext.logical_block);
		bh = sb_bread(dir->i_sb, phys);
		if (!bh)
			continue;

		for (off = 0; off + sizeof(struct ocsfs_disk_dirent)
		              <= sbi->s_block_size;
		     off += sizeof(struct ocsfs_disk_dirent)) {
			struct ocsfs_disk_dirent *de =
				(struct ocsfs_disk_dirent *)(bh->b_data + off);
			u64 hash;

			if (le32_to_cpu(de->de_magic) != OCSFS_DIRENT_MAGIC ||
			    de->de_name_len == 0)
				continue;

			hash = dir_name_hash((char *)de->de_name,
					     de->de_name_len);
			ret = ocsfs_btree_insert(&bt, hash,
						 dir_encode_val(phys, off));
			if (ret) {
				brelse(bh);
				pr_err("ocsfs: dir btree migrate failed: %d\n",
				       ret);
				goto abort;
			}

			if (++chunk >= OCSFS_MIGRATE_CHUNK) {
				/* Commit partial batch, persist root */
				ret = ocsfs_txn_commit(txn);
				if (ret) { brelse(bh); return ret; }
				oi->i_dir_btree_root = bt.root_block;
				mark_inode_dirty(dir);

				txn = ocsfs_txn_begin(dir->i_sb);
				if (IS_ERR(txn)) {
					brelse(bh);
					return PTR_ERR(txn);
				}
				dc.txn = txn;
				chunk  = 0;
			}
		}
		brelse(bh);
	}

	ret = ocsfs_txn_commit(txn);
	if (ret)
		return ret;
	oi->i_dir_btree_root = bt.root_block;
	mark_inode_dirty(dir);
	return 0;

abort:
	ocsfs_txn_abort(txn);
	return ret;
}

bool ocsfs_dir_btree_should_build(struct inode *dir)
{
	return OCSFS_I(dir)->i_dirent_count >= OCSFS_DIR_BTREE_THRESHOLD &&
	       OCSFS_I(dir)->i_dir_btree_root == 0;
}
