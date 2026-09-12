/* brs_hash.h — hashes de chunk (FNV1a-128, BLAKE2b-128) y CRC32C */
#ifndef BRS_HASH_H
#define BRS_HASH_H

#include <stddef.h>
#include <stdint.h>
#include "brs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void brs_hash_fnv1a_128(const uint8_t *data, size_t n, BrsChunkId *out);
void brs_hash_blake2b_128(const uint8_t *data, size_t n, BrsChunkId *out);

/* Dispatcher segun BrsHashAlgo. XXH3_128 cae en FNV1a, igual que el C++. */
void brs_hash_chunk(BrsHashAlgo algo, const uint8_t *data, size_t n,
                    BrsChunkId *out);

/* CRC32C (Castagnoli) por software: integridad fisica de packs. */
uint32_t brs_crc32c_update(uint32_t crc, const uint8_t *data, size_t n);
uint32_t brs_crc32c(const uint8_t *data, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* BRS_HASH_H */