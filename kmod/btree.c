// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — btree.c
 * B+ tree: create, open, search, range scan.
 * Modify operations (insert, delete) are in btree_mod.c.
 */

#include "ocsfs.h"
#include "ocsfs_btree.h"

/* ── node I/O wrappers ── */

/*
 * Validate magic, checksum, and count bounds of a node read from disk.
 * Rejects corrupted or maliciously crafted blocks before any array access.
 * VULN-001/VULN-002: without this, bn_count > leaf_order causes heap OOB.
 */
static int verify_node(const struct ocsfs_btree *bt, const void *buf)
{
	const struct ocsfs_btree_node_hdr *hdr = node_hdr((void *)buf);
	u32 expected_magic, stored_csum, computed_csum;
	u16 count, max_count;
	void *tmp;

	/* magic check */
	if (le16_to_cpu(hdr->bn_level) == 0)
		expected_magic = OCSFS_BTREE_LEAF_MAGIC;
	else
		expected_magic = OCSFS_BTREE_INTERNAL_MAGIC;

	if (le32_to_cpu(hdr->bn_magic) != expected_magic) {
		pr_err_ratelimited("ocsfs: btree: bad magic %08x (expected %08x) "
				   "at block %llu\n",
				   le32_to_cpu(hdr->bn_magic), expected_magic,
				   le64_to_cpu(hdr->bn_block_num));
		return -EIO;
	}

	/* checksum check — requires a writable copy to zero the field */
	tmp = kmemdup(buf, bt->block_size, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	stored_csum = le32_to_cpu(node_hdr(tmp)->bn_checksum);
	node_hdr(tmp)->bn_checksum = 0;
	computed_csum = ocsfs_crc32c(0, tmp, bt->block_size);
	kfree(tmp);

	if (stored_csum != computed_csum) {
		pr_err_ratelimited("ocsfs: btree: checksum mismatch at block %llu "
				   "(stored %08x computed %08x)\n",
				   le64_to_cpu(hdr->bn_block_num),
				   stored_csum, computed_csum);
		return -EIO;
	}

	/* count bounds — prevents OOB access in search/insert/delete */
	count = le16_to_cpu(hdr->bn_count);
	max_count = (le16_to_cpu(hdr->bn_level) == 0)
		    ? (u16)bt->leaf_order : (u16)bt->internal_order;

	if (count > max_count) {
		pr_err_ratelimited("ocsfs: btree: bn_count %u > max %u "
				   "at block %llu\n",
				   count, max_count,
				   le64_to_cpu(hdr->bn_block_num));
		return -EIO;
	}

	return 0;
}

static int read_node(struct ocsfs_btree *bt, u64 block, void *buf)
{
	int ret = bt->read_block(bt->io_ctx, block, buf, bt->block_size);

	if (ret < 0)
		return ret;
	return verify_node(bt, buf);
}

static int write_node(struct ocsfs_btree *bt, u64 block, const void *buf)
{
	return bt->write_block(bt->io_ctx, block, buf, bt->block_size);
}

static int alloc_node(struct ocsfs_btree *bt, u64 *out)
{
	return bt->alloc_block(bt->io_ctx, out);
}

/* ── node init and checksum (also called from btree_mod.c) ── */

void ocsfs_btree_node_update_csum(void *buf, u32 block_size)
{
	struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);

	hdr->bn_checksum = 0;
	hdr->bn_checksum = cpu_to_le32(ocsfs_crc32c(0, buf, block_size));
}

void ocsfs_btree_init_leaf(void *buf, u32 bsz, u64 block_num)
{
	struct ocsfs_btree_node_hdr *hdr;

	memset(buf, 0, bsz);
	hdr = node_hdr(buf);
	hdr->bn_magic      = cpu_to_le32(OCSFS_BTREE_LEAF_MAGIC);
	hdr->bn_flags      = cpu_to_le16(OCSFS_BTREE_NODE_LEAF);
	hdr->bn_level      = 0;
	hdr->bn_block_size = cpu_to_le32(bsz);
	hdr->bn_block_num  = cpu_to_le64(block_num);
}

void ocsfs_btree_init_internal(void *buf, u32 bsz, u64 block_num, u16 level)
{
	struct ocsfs_btree_node_hdr *hdr;

	memset(buf, 0, bsz);
	hdr = node_hdr(buf);
	hdr->bn_magic      = cpu_to_le32(OCSFS_BTREE_INTERNAL_MAGIC);
	hdr->bn_level      = cpu_to_le16(level);
	hdr->bn_block_size = cpu_to_le32(bsz);
	hdr->bn_block_num  = cpu_to_le64(block_num);
}

/* ── create / open ── */

/* VULN-004: block_size must be a power-of-2 in [512, 65536]; callers
 * derive it from the superblock which is validated at mount time, but
 * btree_create/open are also callable from tests — guard here too. */
static int btree_validate_block_size(u32 block_size)
{
	if (block_size < 512 || block_size > 65536 || !is_power_of_2(block_size)) {
		pr_err("ocsfs: btree: invalid block_size %u\n", block_size);
		return -EINVAL;
	}
	/* A leaf node must fit at least 2 entries, else the tree cannot split */
	if (ocsfs_btree_leaf_order(block_size) < 2) {
		pr_err("ocsfs: btree: block_size %u too small for B+ tree\n",
		       block_size);
		return -EINVAL;
	}
	return 0;
}

int ocsfs_btree_create(struct ocsfs_btree *bt, u32 block_size,
		       ocsfs_btree_read_fn read_fn,
		       ocsfs_btree_write_fn write_fn,
		       ocsfs_btree_alloc_fn alloc_fn,
		       ocsfs_btree_free_fn free_fn,
		       void *io_ctx)
{
	u64 root;
	void *buf;
	int ret;

	ret = btree_validate_block_size(block_size);
	if (ret)
		return ret;

	memset(bt, 0, sizeof(*bt));
	bt->block_size     = block_size;
	bt->leaf_order     = ocsfs_btree_leaf_order(block_size);
	bt->internal_order = ocsfs_btree_internal_order(block_size);
	bt->read_block     = read_fn;
	bt->write_block    = write_fn;
	bt->alloc_block    = alloc_fn;
	bt->free_block     = free_fn;
	bt->io_ctx         = io_ctx;

	ret = alloc_node(bt, &root);
	if (ret < 0)
		return ret;

	buf = kzalloc(block_size, GFP_KERNEL);
	if (!buf) {
		bt->free_block(io_ctx, root);
		return -ENOMEM;
	}

	ocsfs_btree_init_leaf(buf, block_size, root);
	node_hdr(buf)->bn_flags = cpu_to_le16(
		le16_to_cpu(node_hdr(buf)->bn_flags) | OCSFS_BTREE_NODE_ROOT);
	ocsfs_btree_node_update_csum(buf, block_size);

	ret = write_node(bt, root, buf);
	kfree(buf);
	if (ret < 0)
		return ret;

	bt->root_block  = root;
	bt->height      = 1;
	bt->entry_count = 0;
	return 0;
}

int ocsfs_btree_open(struct ocsfs_btree *bt, u64 root_block, u32 block_size,
		     ocsfs_btree_read_fn read_fn,
		     ocsfs_btree_write_fn write_fn,
		     ocsfs_btree_alloc_fn alloc_fn,
		     ocsfs_btree_free_fn free_fn,
		     void *io_ctx)
{
	void *buf, *cur;
	int ret;

	ret = btree_validate_block_size(block_size);
	if (ret)
		return ret;

	memset(bt, 0, sizeof(*bt));
	bt->root_block     = root_block;
	bt->block_size     = block_size;
	bt->leaf_order     = ocsfs_btree_leaf_order(block_size);
	bt->internal_order = ocsfs_btree_internal_order(block_size);
	bt->read_block     = read_fn;
	bt->write_block    = write_fn;
	bt->alloc_block    = alloc_fn;
	bt->free_block     = free_fn;
	bt->io_ctx         = io_ctx;

	buf = kzalloc(block_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = read_node(bt, root_block, buf);
	if (ret < 0) {
		kfree(buf);
		return ret;
	}

	bt->height = le16_to_cpu(node_hdr(buf)->bn_level) + 1;

	cur = kzalloc(block_size, GFP_KERNEL);
	if (!cur) {
		kfree(buf);
		return -ENOMEM;
	}
	memcpy(cur, buf, block_size);

	/* walk to leftmost leaf */
	while (!node_is_leaf(cur)) {
		u64 child = le64_to_cpu(*internal_first_child(cur));

		ret = read_node(bt, child, cur);
		if (ret < 0)
			goto out;
	}

	/* count all entries via leaf chain */
	bt->entry_count = 0;
	while (1) {
		u64 right;

		bt->entry_count += le16_to_cpu(node_hdr(cur)->bn_count);
		right = le64_to_cpu(node_hdr(cur)->bn_right_sibling);
		if (!right)
			break;
		if (read_node(bt, right, cur) < 0)
			break;
	}

out:
	kfree(buf);
	kfree(cur);
	return ret;
}

/* ── find_leaf — traverse root→leaf tracking path ── */

int ocsfs_btree_find_leaf(struct ocsfs_btree *bt, u64 key, void *buf,
			  u64 *path, int *path_len, int max_path)
{
	int ret = read_node(bt, bt->root_block, buf);

	if (ret < 0)
		return ret;

	if (path) {
		path[0] = bt->root_block;
		*path_len = 1;
	}

	while (!node_is_leaf(buf)) {
		struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);
		struct ocsfs_btree_ptr *ptrs = internal_ptrs(buf);
		u64 next = le64_to_cpu(*internal_first_child(buf));
		int i, n = le16_to_cpu(hdr->bn_count);

		for (i = 0; i < n; i++) {
			if (key >= le64_to_cpu(ptrs[i].key))
				next = le64_to_cpu(ptrs[i].child);
			else
				break;
		}

		ret = read_node(bt, next, buf);
		if (ret < 0)
			return ret;

		if (path && *path_len < max_path)
			path[(*path_len)++] = next;
	}
	return 0;
}

/* ── search ── */

int ocsfs_btree_search(struct ocsfs_btree *bt, u64 key, u64 *out_value)
{
	struct ocsfs_btree_node_hdr *hdr;
	struct ocsfs_btree_entry *entries;
	void *buf;
	int lo, hi, ret;

	buf = kzalloc(bt->block_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = ocsfs_btree_find_leaf(bt, key, buf, NULL, NULL, 0);
	if (ret < 0)
		goto out;

	hdr     = node_hdr(buf);
	entries = leaf_entries(buf);
	lo = 0;
	hi = le16_to_cpu(hdr->bn_count);

	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;

		if (le64_to_cpu(entries[mid].key) < key)
			lo = mid + 1;
		else
			hi = mid;
	}

	if (lo < le16_to_cpu(hdr->bn_count) &&
	    le64_to_cpu(entries[lo].key) == key) {
		if (out_value)
			*out_value = le64_to_cpu(entries[lo].value);
		ret = 0;
	} else {
		ret = -ENOENT;
	}

out:
	kfree(buf);
	return ret;
}

/* ── search_le — floor search: largest key ≤ target ── */

int ocsfs_btree_search_le(struct ocsfs_btree *bt, u64 key,
			  u64 *out_key, u64 *out_value)
{
	struct ocsfs_btree_node_hdr *hdr;
	struct ocsfs_btree_entry *entries;
	void *buf;
	int lo, hi, best, n, ret;

	buf = kzalloc(bt->block_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = ocsfs_btree_find_leaf(bt, key, buf, NULL, NULL, 0);
	if (ret < 0)
		goto out;

	hdr     = node_hdr(buf);
	entries = leaf_entries(buf);
	n       = le16_to_cpu(hdr->bn_count);

	lo = 0; hi = n - 1; best = -1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;

		if (le64_to_cpu(entries[mid].key) <= key) {
			best = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}

	if (best >= 0) {
		if (out_key)
			*out_key = le64_to_cpu(entries[best].key);
		if (out_value)
			*out_value = le64_to_cpu(entries[best].value);
		ret = 0;
	} else {
		ret = -ENOENT;
	}

out:
	kfree(buf);
	return ret;
}

/* ── range scan ── */

int ocsfs_btree_range_scan(struct ocsfs_btree *bt, u64 start_key, u64 end_key,
			   ocsfs_btree_scan_fn callback, void *ctx)
{
	struct ocsfs_btree_node_hdr *hdr;
	struct ocsfs_btree_entry *entries;
	void *buf;
	int scanned = 0;
	int i, n, ret;

	buf = kzalloc(bt->block_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = ocsfs_btree_find_leaf(bt, start_key, buf, NULL, NULL, 0);
	if (ret < 0) {
		kfree(buf);
		return ret;
	}

	while (1) {
		u64 right;

		hdr     = node_hdr(buf);
		entries = leaf_entries(buf);
		n       = le16_to_cpu(hdr->bn_count);

		for (i = 0; i < n; i++) {
			u64 k = le64_to_cpu(entries[i].key);

			if (k > end_key)
				goto done;
			if (k >= start_key) {
				ret = callback(k, le64_to_cpu(entries[i].value),
					       ctx);
				scanned++;
				if (ret)
					goto done;
			}
		}

		right = le64_to_cpu(hdr->bn_right_sibling);
		if (!right)
			break;
		if (read_node(bt, right, buf) < 0)
			break;
	}

done:
	kfree(buf);
	return scanned;
}
