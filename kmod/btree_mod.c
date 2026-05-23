// SPDX-License-Identifier: GPL-2.0-only
/*
 * OCSFS — btree_mod.c
 * B+ tree write operations: insert (with node splitting) and delete.
 */

#include "ocsfs.h"
#include "ocsfs_btree.h"

static int write_node(struct ocsfs_btree *bt, u64 block, const void *buf)
{
	return bt->write_block(bt->io_ctx, block, buf, bt->block_size);
}

static int read_node(struct ocsfs_btree *bt, u64 block, void *buf)
{
	return bt->read_block(bt->io_ctx, block, buf, bt->block_size);
}

static int alloc_node(struct ocsfs_btree *bt, u64 *out)
{
	return bt->alloc_block(bt->io_ctx, out);
}

static int free_node(struct ocsfs_btree *bt, u64 block)
{
	return bt->free_block(bt->io_ctx, block);
}

static void internal_insert_at(void *buf, int pos, u64 key, u64 child)
{
	struct ocsfs_btree_node_hdr *hdr = node_hdr(buf);
	struct ocsfs_btree_ptr *ptrs = internal_ptrs(buf);
	int n = le16_to_cpu(hdr->bn_count);

	if (pos < n)
		memmove(&ptrs[pos + 1], &ptrs[pos],
			(n - pos) * sizeof(*ptrs));
	ptrs[pos].key   = cpu_to_le64(key);
	ptrs[pos].child = cpu_to_le64(child);
	hdr->bn_count   = cpu_to_le16(n + 1);
}

int ocsfs_btree_insert(struct ocsfs_btree *bt, u64 key, u64 value)
{
	struct ocsfs_btree_node_hdr *hdr;
	struct ocsfs_btree_entry *entries;
	u64 path[64];
	int path_len = 0;
	int lo, hi, pos, ret, n;
	void *buf;

	buf = kzalloc(bt->block_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = ocsfs_btree_find_leaf(bt, key, buf, path, &path_len, 64);
	if (ret < 0)
		goto err_free_buf;

	hdr     = node_hdr(buf);
	entries = leaf_entries(buf);
	n       = le16_to_cpu(hdr->bn_count);

	/* binary search for insertion position */
	lo = 0; hi = n;
	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;

		if (le64_to_cpu(entries[mid].key) < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	pos = lo;

	if (pos < n && le64_to_cpu(entries[pos].key) == key) {
		/* update existing */
		entries[pos].value = cpu_to_le64(value);
		ocsfs_btree_node_update_csum(buf, bt->block_size);
		ret = write_node(bt, le64_to_cpu(hdr->bn_block_num), buf);
		kfree(buf);
		return ret;
	}

	if ((u32)n < bt->leaf_order) {
		/* room in leaf — insert in place */
		if (pos < n)
			memmove(&entries[pos + 1], &entries[pos],
				(n - pos) * sizeof(*entries));
		entries[pos].key   = cpu_to_le64(key);
		entries[pos].value = cpu_to_le64(value);
		hdr->bn_count = cpu_to_le16(n + 1);
		bt->entry_count++;
		ocsfs_btree_node_update_csum(buf, bt->block_size);
		ret = write_node(bt, le64_to_cpu(hdr->bn_block_num), buf);
		kfree(buf);
		return ret;
	}

	/* leaf full — split */
	{
		struct ocsfs_btree_entry *tmp;
		void *new_buf;
		struct ocsfs_btree_node_hdr *new_hdr;
		struct ocsfs_btree_entry *new_entries;
		u64 new_block, split_key, promote_key, promote_child;
		int total = n + 1, split, right_count, level;

		tmp = kcalloc(total, sizeof(*tmp), GFP_KERNEL);
		if (!tmp) { ret = -ENOMEM; goto err_free_buf; }

		memcpy(tmp, entries, pos * sizeof(*tmp));
		tmp[pos].key   = cpu_to_le64(key);
		tmp[pos].value = cpu_to_le64(value);
		memcpy(&tmp[pos + 1], &entries[pos], (n - pos) * sizeof(*tmp));

		split = total / 2;

		memcpy(entries, tmp, split * sizeof(*entries));
		hdr->bn_count = cpu_to_le16(split);

		new_buf = kzalloc(bt->block_size, GFP_KERNEL);
		if (!new_buf) { kfree(tmp); ret = -ENOMEM; goto err_free_buf; }

		ret = alloc_node(bt, &new_block);
		if (ret < 0) { kfree(new_buf); kfree(tmp); goto err_free_buf; }

		ocsfs_btree_init_leaf(new_buf, bt->block_size, new_block);
		new_hdr     = node_hdr(new_buf);
		new_entries = leaf_entries(new_buf);
		right_count = total - split;
		memcpy(new_entries, &tmp[split], right_count * sizeof(*new_entries));
		new_hdr->bn_count  = cpu_to_le16(right_count);
		new_hdr->bn_parent = hdr->bn_parent;
		split_key          = le64_to_cpu(new_entries[0].key);

		new_hdr->bn_right_sibling = hdr->bn_right_sibling;
		new_hdr->bn_left_sibling  = hdr->bn_block_num;
		hdr->bn_right_sibling     = cpu_to_le64(new_block);

		/* fix old right neighbour's back-link */
		if (le64_to_cpu(new_hdr->bn_right_sibling)) {
			void *rb = kzalloc(bt->block_size, GFP_KERNEL);

			if (rb && read_node(bt, le64_to_cpu(
					new_hdr->bn_right_sibling), rb) == 0) {
				node_hdr(rb)->bn_left_sibling = cpu_to_le64(new_block);
				ocsfs_btree_node_update_csum(rb, bt->block_size);
				write_node(bt, le64_to_cpu(
					new_hdr->bn_right_sibling), rb);
			}
			kfree(rb);
		}

		ocsfs_btree_node_update_csum(buf, bt->block_size);
		ret = write_node(bt, le64_to_cpu(hdr->bn_block_num), buf);
		if (!ret) {
			ocsfs_btree_node_update_csum(new_buf, bt->block_size);
			ret = write_node(bt, new_block, new_buf);
		}
		kfree(new_buf);
		kfree(tmp);
		if (ret) {
			free_node(bt, new_block);
			goto err_free_buf;
		}
		bt->entry_count++;

		promote_key   = split_key;
		promote_child = new_block;

		/* propagate split up the path */
		for (level = path_len - 2; level >= 0; level--) {
			struct ocsfs_btree_ptr *ptrs;
			int ipos, icount;

			ret = read_node(bt, path[level], buf);
			if (ret < 0) goto err_free_buf;

			hdr    = node_hdr(buf);
			ptrs   = internal_ptrs(buf);
			icount = le16_to_cpu(hdr->bn_count);

			for (ipos = 0; ipos < icount; ipos++)
				if (le64_to_cpu(ptrs[ipos].key) > promote_key)
					break;

			if ((u32)icount < bt->internal_order) {
				internal_insert_at(buf, ipos, promote_key,
						   promote_child);
				ocsfs_btree_node_update_csum(buf, bt->block_size);
				ret = write_node(bt, le64_to_cpu(hdr->bn_block_num),
						 buf);
				kfree(buf);
				return ret;
			}

			/* internal node full — split */
			{
				struct ocsfs_btree_ptr *tp;
				void *ni_buf;
				struct ocsfs_btree_node_hdr *ni_hdr;
				u64 new_int, new_promote;
				int new_total = icount + 1, isplit, ni_count;

				tp = kcalloc(new_total, sizeof(*tp), GFP_KERNEL);
				if (!tp) { ret = -ENOMEM; goto err_free_buf; }

				memcpy(tp, ptrs, ipos * sizeof(*tp));
				tp[ipos].key   = cpu_to_le64(promote_key);
				tp[ipos].child = cpu_to_le64(promote_child);
				memcpy(&tp[ipos + 1], &ptrs[ipos],
				       (icount - ipos) * sizeof(*tp));

				isplit      = new_total / 2;
				new_promote = le64_to_cpu(tp[isplit].key);

				memcpy(ptrs, tp, isplit * sizeof(*ptrs));
				hdr->bn_count = cpu_to_le16(isplit);

				ret = alloc_node(bt, &new_int);
				if (ret < 0) { kfree(tp); goto err_free_buf; }

				ni_buf = kzalloc(bt->block_size, GFP_KERNEL);
				if (!ni_buf) {
					free_node(bt, new_int);
					kfree(tp);
					ret = -ENOMEM;
					goto err_free_buf;
				}

				ocsfs_btree_init_internal(ni_buf, bt->block_size,
							  new_int,
							  le16_to_cpu(hdr->bn_level));
				ni_hdr  = node_hdr(ni_buf);
				*internal_first_child(ni_buf) =
					cpu_to_le64(le64_to_cpu(tp[isplit].child));
				ni_count = new_total - isplit - 1;
				if (ni_count > 0)
					memcpy(internal_ptrs(ni_buf),
					       &tp[isplit + 1],
					       ni_count * sizeof(*tp));
				ni_hdr->bn_count  = cpu_to_le16(ni_count);
				ni_hdr->bn_parent = hdr->bn_parent;

				ocsfs_btree_node_update_csum(buf, bt->block_size);
				ret = write_node(bt, le64_to_cpu(hdr->bn_block_num),
						 buf);
				if (!ret) {
					ocsfs_btree_node_update_csum(ni_buf,
								     bt->block_size);
					ret = write_node(bt, new_int, ni_buf);
				}
				/* Fix bn_parent for all children of the new right node */
				if (!ret) {
					void *cb = kzalloc(bt->block_size, GFP_KERNEL);

					if (cb) {
						int ci;
						u64 cblk = le64_to_cpu(
							*internal_first_child(ni_buf));

						if (read_node(bt, cblk, cb) == 0) {
							node_hdr(cb)->bn_parent =
								cpu_to_le64(new_int);
							ocsfs_btree_node_update_csum(
								cb, bt->block_size);
							write_node(bt, cblk, cb);
						}
						for (ci = 0; ci < ni_count; ci++) {
							cblk = le64_to_cpu(
								internal_ptrs(ni_buf)[ci].child);
							if (read_node(bt, cblk, cb) == 0) {
								node_hdr(cb)->bn_parent =
									cpu_to_le64(new_int);
								ocsfs_btree_node_update_csum(
									cb, bt->block_size);
								write_node(bt, cblk, cb);
							}
						}
						kfree(cb);
					}
				}
				kfree(ni_buf);
				kfree(tp);
				if (ret)
					goto err_free_buf;

				promote_key   = new_promote;
				promote_child = new_int;
			}
		}

		/* create new root */
		{
			u64 new_root;

			ret = alloc_node(bt, &new_root);
			if (ret < 0) goto err_free_buf;

			ocsfs_btree_init_internal(buf, bt->block_size, new_root,
						  (u16)bt->height);
			node_hdr(buf)->bn_flags = cpu_to_le16(OCSFS_BTREE_NODE_ROOT);
			*internal_first_child(buf) = cpu_to_le64(bt->root_block);
			internal_ptrs(buf)[0].key   = cpu_to_le64(promote_key);
			internal_ptrs(buf)[0].child = cpu_to_le64(promote_child);
			node_hdr(buf)->bn_count = cpu_to_le16(1);
			ocsfs_btree_node_update_csum(buf, bt->block_size);
			ret = write_node(bt, new_root, buf);
			if (ret) {
				free_node(bt, new_root);
				goto err_free_buf;
			}

			/* clear root flag on old root */
			if (read_node(bt, bt->root_block, buf) == 0) {
				node_hdr(buf)->bn_flags &=
					cpu_to_le16(~OCSFS_BTREE_NODE_ROOT);
				node_hdr(buf)->bn_parent = cpu_to_le64(new_root);
				ocsfs_btree_node_update_csum(buf, bt->block_size);
				write_node(bt, bt->root_block, buf);
			}
			/* update new right child's parent */
			if (read_node(bt, promote_child, buf) == 0) {
				node_hdr(buf)->bn_parent = cpu_to_le64(new_root);
				ocsfs_btree_node_update_csum(buf, bt->block_size);
				write_node(bt, promote_child, buf);
			}

			bt->root_block = new_root;
			bt->height++;
		}
	}

	kfree(buf);
	return 0;

err_free_buf:
	kfree(buf);
	return ret;
}

int ocsfs_btree_delete(struct ocsfs_btree *bt, u64 key)
{
	struct ocsfs_btree_node_hdr *hdr;
	struct ocsfs_btree_entry *entries;
	void *buf;
	int lo, hi, n, ret;

	buf = kzalloc(bt->block_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = ocsfs_btree_find_leaf(bt, key, buf, NULL, NULL, 0);
	if (ret < 0)
		goto out;

	hdr     = node_hdr(buf);
	entries = leaf_entries(buf);
	n       = le16_to_cpu(hdr->bn_count);

	lo = 0; hi = n;
	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;

		if (le64_to_cpu(entries[mid].key) < key)
			lo = mid + 1;
		else
			hi = mid;
	}

	if (lo >= n || le64_to_cpu(entries[lo].key) != key) {
		ret = -ENOENT;
		goto out;
	}

	if (lo < n - 1)
		memmove(&entries[lo], &entries[lo + 1],
			(n - lo - 1) * sizeof(*entries));
	n--;
	hdr->bn_count = cpu_to_le16(n);
	bt->entry_count--;

	if (n == 0 && !(le16_to_cpu(hdr->bn_flags) & OCSFS_BTREE_NODE_ROOT)) {
		u64 left  = le64_to_cpu(hdr->bn_left_sibling);
		u64 right = le64_to_cpu(hdr->bn_right_sibling);
		u64 self  = le64_to_cpu(hdr->bn_block_num);
		u64 parent_block = le64_to_cpu(hdr->bn_parent);
		void *nb;

		if (left) {
			nb = kzalloc(bt->block_size, GFP_KERNEL);
			if (nb && read_node(bt, left, nb) == 0) {
				node_hdr(nb)->bn_right_sibling = cpu_to_le64(right);
				ocsfs_btree_node_update_csum(nb, bt->block_size);
				write_node(bt, left, nb);
			}
			kfree(nb);
		}
		if (right) {
			nb = kzalloc(bt->block_size, GFP_KERNEL);
			if (nb && read_node(bt, right, nb) == 0) {
				node_hdr(nb)->bn_left_sibling = cpu_to_le64(left);
				ocsfs_btree_node_update_csum(nb, bt->block_size);
				write_node(bt, right, nb);
			}
			kfree(nb);
		}

		if (parent_block) {
			nb = kzalloc(bt->block_size, GFP_KERNEL);
			if (nb && read_node(bt, parent_block, nb) == 0) {
				struct ocsfs_btree_node_hdr *ph = node_hdr(nb);
				u64 *pf = internal_first_child(nb);
				struct ocsfs_btree_ptr *pp = internal_ptrs(nb);
				int pc = le16_to_cpu(ph->bn_count);

				if (le64_to_cpu(*pf) == self) {
					if (pc > 0) {
						*pf = pp[0].child;
						memmove(&pp[0], &pp[1],
							(pc-1) * sizeof(*pp));
						ph->bn_count = cpu_to_le16(pc-1);
					}
				} else {
					int i;

					for (i = 0; i < pc; i++) {
						if (le64_to_cpu(pp[i].child) == self) {
							memmove(&pp[i], &pp[i+1],
								(pc-i-1)*sizeof(*pp));
							ph->bn_count = cpu_to_le16(pc-1);
							break;
						}
					}
				}
				ocsfs_btree_node_update_csum(nb, bt->block_size);
				write_node(bt, parent_block, nb);
			}
			kfree(nb);
		}

		free_node(bt, self);
		kfree(buf);
		return 0;
	}

	ocsfs_btree_node_update_csum(buf, bt->block_size);
	ret = write_node(bt, le64_to_cpu(hdr->bn_block_num), buf);

out:
	kfree(buf);
	return ret;
}
