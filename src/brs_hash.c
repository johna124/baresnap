/* brs_hash.c — FNV1a-128 (unsigned __int128, extension GNU estable en
 * gcc/musl x86_64), BLAKE2b-128 (Monocypher) y CRC32C con tabla. */
#pragma GCC diagnostic ignored "-Wpedantic"

#include "brs_hash.h"

#include <pthread.h>
#include <string.h>

#include "monocypher.h"

typedef unsigned __int128 brs_u128;

void brs_hash_fnv1a_128(const uint8_t *data, size_t n, BrsChunkId *out)
{
    if (!out) return;
    brs_u128 hash = ((brs_u128)0x6C62272E07BB0142ULL << 64) |
                    0x62B821756295C58DULL;
    brs_u128 prime = ((brs_u128)1 << 88) | ((brs_u128)1 << 8) | 0x3BULL;
    for (size_t i = 0; i < n; ++i) {
        hash ^= (brs_u128)data[i];
        hash *= prime;
    }
    for (int i = 0; i < 16; ++i)
        out->bytes[i] = (uint8_t)((hash >> (8 * i)) & 0xFF);
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
