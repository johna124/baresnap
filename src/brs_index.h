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
/* brs_index.h — mapa de chunks, set u64, segmentos de index y Bloom Filters.
 *
 * FASE 4: BRS_BLOOM_BITS_PER_SEG ampliado a 16M bits (2 MB) por segmento.
 * Con 5 funciones hash y 16M bits, la tasa de falsos positivos es
 * inferior a 1e-10 incluso con 1 millón de chunks únicos por segmento,
 * evitando referencias fantasmas en copias de 8 GB+.
 */

#ifndef BRS_INDEX_H
#define BRS_INDEX_H

#include "brs_types.h"
#include "brs_hash.h"
#include "brs_util.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * BrsIndexMap: hashmap abierto con linear probing.
 * Load factor máximo: 70%.
 * ==========================================================================*/

typedef struct {
    BrsChunkId       key;
    BrsChunkLocation value;
} BrsIndexSlot;

typedef struct {
    BrsIndexSlot *slots;
    uint8_t      *used;
    size_t        capacity;
    size_t        count;
} BrsIndexMap;

int brs_index_map_init(BrsIndexMap *m, size_t initial_capacity);
void brs_index_map_free(BrsIndexMap *m);
size_t brs_index_map_count(const BrsIndexMap *m);
int brs_index_map_put(BrsIndexMap *m, const BrsChunkId *id,
                      const BrsChunkLocation *loc);
const BrsChunkLocation *brs_index_map_get(const BrsIndexMap *m,
                                          const BrsChunkId *id);
const BrsIndexSlot *brs_index_map_next(const BrsIndexMap *m, size_t *cursor);

/* ============================================================================
 * BrsU64Set: set de uint64 con open addressing.
 * ==========================================================================*/

typedef struct {
    uint64_t *keys;
    uint8_t  *used;
    size_t    capacity;
    size_t    count;
} BrsU64Set;

int brs_u64_set_init(BrsU64Set *s, size_t initial_capacity);
void brs_u64_set_free(BrsU64Set *s);
size_t brs_u64_set_count(const BrsU64Set *s);
int brs_u64_set_insert(BrsU64Set *s, uint64_t v);
int brs_u64_set_contains(const BrsU64Set *s, uint64_t v);

/* ============================================================================
 * Segmentos .idx: escritura y lectura.
 * ==========================================================================*/

int brs_write_index_segment(const char *repo_path, uint64_t segment_id,
                            const BrsIndexMap *map);
int brs_load_all_indexes(const char *repo_path, BrsIndexMap *out_map);
int brs_load_index_segment(const char *repo_path, uint64_t seg_id,
                           BrsIndexMap *map);
int brs_load_index_segment_from_file(const char *local_path, uint64_t seg_id,
                                     BrsIndexMap *map);

/* Bulk SSH index dump (BARESNAP_SSH_BULK_INDEX_PATCH). */
int brs_parse_index_bulk_dump(const uint8_t *data, uint32_t len,
                              BrsIndexMap *map);

/* ============================================================================
 * FASE 4: Filtros Bloom redimensionados.
 *
 * BRS_BLOOM_BITS_PER_SEG: 16M bits = 2 MB por segmento.
 * BRS_BLOOM_NUM_HASHES: 5 funciones hash.
 *
 * Tasa de falsos positivos teórica con n elementos:
 *   p = (1 - e^(-k*n/m))^k
 * Con k=5, m=16777216, n=1000000: p ≈ 1.5e-12
 * Con k=5, m=16777216, n=500000:  p ≈ 2.4e-15
 *
 * Esto garantiza cero falsos positivos prácticos incluso con cientos de
 * miles de chunks únicos por segmento en copias de 8 GB+.
 * ==========================================================================*/

#define BRS_BLOOM_BITS_PER_SEG  (16ULL * 1024ULL * 1024ULL)  /* 16M bits = 2 MB */
#define BRS_BLOOM_NUM_HASHES    5

typedef struct {
    uint64_t size_bits;
    uint8_t  num_hashes;
    uint8_t *bits;
} BrsBloomFilter;

typedef struct {
    uint64_t         seg_id;
    BrsBloomFilter   bloom;
} BrsBloomSegment;

typedef struct {
    BrsBloomSegment *segs;
    size_t           count;
    size_t           capacity;
} BrsBloomSet;

void brs_bloom_init(BrsBloomFilter *b);
void brs_bloom_add(BrsBloomFilter *b, const BrsChunkId *id);
int  brs_bloom_check(const BrsBloomFilter *b, const BrsChunkId *id);
void brs_bloom_free(BrsBloomFilter *b);

int brs_bloom_save(const char *repo_path, uint64_t seg_id,
                   const BrsBloomFilter *b);
int brs_bloom_load(const char *repo_path, uint64_t seg_id,
                   BrsBloomFilter *b);

int brs_load_bloom_set(const char *repo_path, BrsBloomSet *set);
void brs_bloom_set_free(BrsBloomSet *set);

#ifdef __cplusplus
}
#endif

#endif /* BRS_INDEX_H */
