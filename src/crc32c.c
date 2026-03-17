/*
 * OCSFS — CRC32C implementation
 *
 * Uses the Castagnoli polynomial (0x1EDC6F41), same as:
 *   - Linux kernel's crc32c
 *   - iSCSI, SCTP, ext4, btrfs
 *   - Intel CRC32 instruction (SSE 4.2)
 *
 * This is a software fallback. The kernel module will use
 * crypto_shash("crc32c") which auto-selects hardware acceleration.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdint.h>
#include <stddef.h>

static uint32_t crc32c_table[256];
static int crc32c_table_initialized = 0;

static void crc32c_init_table(void)
{
    uint32_t poly = 0x82F63B78; /* reversed Castagnoli */
    for (int i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ poly;
            else
                crc >>= 1;
        }
        crc32c_table[i] = crc;
    }
    crc32c_table_initialized = 1;
}

uint32_t ocsfs_crc32c(uint32_t crc, const void *data, size_t len)
{
    if (!crc32c_table_initialized)
        crc32c_init_table();

    const uint8_t *buf = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc = crc32c_table[(crc ^ *buf++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}
