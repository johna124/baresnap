/*
 * BareSnap — bare-metal snapshot and backup system.
 * Copyright (C) 2026  John (johna124)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Source, issues, contact: https://github.com/johna124
 */
/* brs_hash.c — FNV1a-128 (unsigned __int128, extension GNU estable en
 * gcc/musl x86_64), BLAKE2b-128 (Monocypher) y CRC32C con tabla. */
#pragma GCC diagnostic ignored "-Wpedantic"

#include "brs_hash.h"

#include <pthread.h>
#include <string.h>

#include "monocypher.h"

typedef struct { uint64_t high; uint64_t low; } brs_u128;

#include <endian.h>

#include <endian.h>

void brs_hash_fnv1a_128(const uint8_t *data, size_t n, BrsChunkId *out) {

    if (!out || !data) return;

    uint64_t h_low  = 0x62b821756295c58dULL;

    uint64_t h_high = 0x6c62272e07bb0142ULL;

    uint64_t prime  = 0x13bULL;

    for (size_t i = 0; i < n; i++) {

        h_low ^= data[i];

        uint64_t al = h_low & 0xFFFFFFFFULL;

        uint64_t ah = h_low >> 32;

        uint64_t bl = prime & 0xFFFFFFFFULL;

        uint64_t bh = prime >> 32;

        uint64_t p0 = al * bl;

        uint64_t p1 = al * bh;

        uint64_t p2 = ah * bl;

        uint64_t p3 = ah * bh;

        uint64_t middle = p1 + p2 + (p0 >> 32);

        uint64_t new_low = (p0 & 0xFFFFFFFFULL) | (middle << 32);

        uint64_t carry = (middle >> 32) + p3;

        h_high = (h_high * prime) + carry;

        h_low = new_low;

    }

    for (int i = 0; i < 8; ++i) {

        out->bytes[i] = (uint8_t)((h_low >> (8 * i)) & 0xFF);

        out->bytes[i + 8] = (uint8_t)((h_high >> (8 * i)) & 0xFF);

    }
}

void brs_hash_blake2b_128(const uint8_t *data, size_t n, BrsChunkId *out)
{
    if (!out) return;
    crypto_blake2b(out->bytes, 16, data, n);
}

void brs_hash_chunk(BrsHashAlgo algo, const uint8_t *data, size_t n,
                    BrsChunkId *out)
{
    switch (algo) {
    case BRS_HASH_BLAKE2B_128:
        brs_hash_blake2b_128(data, n, out);
        break;
    case BRS_HASH_XXH3_128:   /* sin implementar en el original: FNV1a */
    case BRS_HASH_FNV1A_128:
    default:
        brs_hash_fnv1a_128(data, n, out);
        break;
    }
}

/* ---- CRC32C ---- */
static uint32_t g_crc_table[256];
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;

static void crc_table_build(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
            crc = (crc & 1) ? ((crc >> 1) ^ 0x82F63B78u) : (crc >> 1);
        g_crc_table[i] = crc;
    }
}

uint32_t brs_crc32c_update(uint32_t crc, const uint8_t *data, size_t n)
{
    pthread_once(&g_crc_once, crc_table_build);
    for (size_t i = 0; i < n; ++i)
        crc = g_crc_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

uint32_t brs_crc32c(const uint8_t *data, size_t n)
{
    return brs_crc32c_update(0xFFFFFFFFu, data, n) ^ 0xFFFFFFFFu;
}
