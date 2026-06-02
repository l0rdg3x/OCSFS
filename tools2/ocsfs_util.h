/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OCSFS v2 — userspace utility helpers shared by mkfs / fsck.
 *
 * crc32c MUST match the Linux kernel's crc32c() exactly: CRC-32C (Castagnoli),
 * bit-reflected, seeded with the supplied crc, and NO final inversion — the
 * kernel returns the running state directly. Callers use crc32c(~0u, buf, len)
 * and store the result, matching ocsfs2_crc32c(~0U, ...) in the kernel.
 * (v1 bug CRC-1: userspace did a final inversion -> every volume rejected.)
 */
#ifndef OCSFS2_UTIL_H
#define OCSFS2_UTIL_H

#include <stdint.h>
#include <stddef.h>

static inline uint32_t ocsfs2_crc32c(uint32_t crc, const void *data, size_t len)
{
	static uint32_t tab[256];
	static int init;
	const uint8_t *p = (const uint8_t *)data;

	if (!init) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
			tab[i] = c;
		}
		init = 1;
	}
	while (len--)
		crc = (crc >> 8) ^ tab[(crc ^ *p++) & 0xff];
	return crc;
}

#endif /* OCSFS2_UTIL_H */
