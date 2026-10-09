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