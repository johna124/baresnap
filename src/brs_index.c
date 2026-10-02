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
/* brs_index.c — mapa de chunks, set u64, segmentos de index y Bloom Filters */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "brs_index.h"
#include "brs_buffer.h"
#include "brs_dir.h"
#include "brs_fsutil.h"
#include "brs_util.h"
#include "brs_vfs_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <fcntl.h>
#include <unistd.h>
#include "brs_vfs.h"

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* ============================================================================
 * BrsIndexMap: open addressing, linear probing, load factor 70%
 * ==========================================================================*/

static uint64_t chunk_id_hash(const BrsChunkId id)
{
    /* Mismo esquema que ChunkIdHash del C++: XOR de los 8 primeros bytes. */
    uint64_t h = 0;

    for (int i = 0; i < 8; ++i)
        h ^= (uint64_t)id.bytes[i] << (i * 8);

    return h;
}

int brs_index_map_init(BrsIndexMap *m, size_t initial_capacity)
{
    if (!m)
        return -1;

    memset(m, 0, sizeof *m);

    size_t cap = 16;
    while (cap < initial_capacity)
        cap *= 2;

    m->slots = (BrsIndexSlot *)calloc(cap, sizeof *m->slots);
    m->used = (uint8_t *)calloc(cap, 1);

    if (!m->slots || !m->used) {
        brs_index_map_free(m);
        return -1;
    }

    m->capacity = cap;
    return 0;
}

void brs_index_map_free(BrsIndexMap *m)
{
    if (!m)
        return;

    free(m->slots);
    free(m->used);
    memset(m, 0, sizeof *m);
}

size_t brs_index_map_count(const BrsIndexMap *m)
{
    return m ? m->count : 0;
}

static size_t index_slot_find(const BrsIndexMap *m, const BrsChunkId *id,
                              int *found)
{
    size_t mask = m->capacity - 1;
    size_t idx = (size_t)(chunk_id_hash(*id) & mask);

    for (;;) {
        if (!m->used[idx]) {
            *found = 0;
            return idx;
        }

        if (brs_chunk_id_equal(&m->slots[idx].key, id)) {
            *found = 1;
            return idx;
        }

        idx = (idx + 1) & mask;
    }
}


/* ============================================================================
 * 1. src/brs_index.c — index_map_grow
 * FIX: Re-inserción directa (inline) para evitar llamar a brs_index_map_put.
 * Esto elimina el chequeo de factor de carga interno y previene OOM recursivos.
 * ==========================================================================*/
static int index_map_grow(BrsIndexMap *m)
{
    size_t newcap = m->capacity * 2;
    BrsIndexSlot *nslots = (BrsIndexSlot *)calloc(newcap, sizeof *nslots);
    uint8_t *nused = (uint8_t *)calloc(newcap, 1);
    if (!nslots || !nused) {
        free(nslots);
        free(nused);
        return -1;
    }
    
    BrsIndexMap old = *m;
    m->slots = nslots;
    m->used = nused;
    m->capacity = newcap;
    m->count = 0;
    
    /* Re-inserción directa sin pasar por brs_index_map_put 
     * para evitar el chequeo de factor de carga y el OOM recursivo. */
    size_t mask = m->capacity - 1;
    for (size_t i = 0; i < old.capacity; ++i) {
        if (old.used[i]) {
            size_t idx = (size_t)(chunk_id_hash(old.slots[i].key) & mask);
            for (;;) {
                if (!m->used[idx]) {
                    m->used[idx] = 1;
                    m->slots[idx].key = old.slots[i].key;
                    m->slots[idx].value = old.slots[i].value;
                    m->count++;
                    break;
                }
                idx = (idx + 1) & mask;
            }
        }
    }
    
    free(old.slots);
    free(old.used);
    return 0;
}

int brs_index_map_put(BrsIndexMap *m, const BrsChunkId *id,
                      const BrsChunkLocation *loc)
{
    if (!m || !id || !loc)
        return -1;

    if (m->capacity == 0)
        return -1;

    if ((m->count + 1) * 10 >= m->capacity * 7) {
        if (index_map_grow(m) != 0)
            return -1;
    }

    int found;
    size_t idx = index_slot_find(m, id, &found);

    if (!found) {
        m->used[idx] = 1;
        m->count++;
    }

    m->slots[idx].key = *id;
    m->slots[idx].value = *loc;

    return 0;
}

const BrsChunkLocation *brs_index_map_get(const BrsIndexMap *m,
                                          const BrsChunkId *id)
{
    if (!m || !id || m->capacity == 0)
        return NULL;

    int found;
    size_t idx = index_slot_find(m, id, &found);

    return found ? &m->slots[idx].value : NULL;
}

const BrsIndexSlot *brs_index_map_next(const BrsIndexMap *m, size_t *cursor)
{
    if (!m || !cursor)
        return NULL;

    while (*cursor < m->capacity) {
        size_t i = *cursor;
        (*cursor)++;

        if (m->used[i])
            return &m->slots[i];
    }

    return NULL;
}

/* ============================================================================
 * BrsU64Set
 * ==========================================================================*/

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return x;
}

int brs_u64_set_init(BrsU64Set *s, size_t initial_capacity)
{
    if (!s)
        return -1;

    memset(s, 0, sizeof *s);

    size_t cap = 16;
    while (cap < initial_capacity)
        cap *= 2;

    s->keys = (uint64_t *)calloc(cap, sizeof *s->keys);
    s->used = (uint8_t *)calloc(cap, 1);

    if (!s->keys || !s->used) {
        brs_u64_set_free(s);
        return -1;
    }

    s->capacity = cap;
    return 0;
}

void brs_u64_set_free(BrsU64Set *s)
{
    if (!s)
        return;

    free(s->keys);
    free(s->used);
    memset(s, 0, sizeof *s);
}

size_t brs_u64_set_count(const BrsU64Set *s)
{
    return s ? s->count : 0;
}

static size_t u64_slot_find(const BrsU64Set *s, uint64_t v, int *found)
{
    size_t mask = s->capacity - 1;
    size_t idx = (size_t)(mix64(v) & mask);

    for (;;) {
        if (!s->used[idx]) {
            *found = 0;
            return idx;
        }

        if (s->keys[idx] == v) {
            *found = 1;
            return idx;
        }

        idx = (idx + 1) & mask;
    }
}

static int u64_set_grow(BrsU64Set *s)
{
    size_t newcap = s->capacity * 2;
    if (newcap < s->capacity) return -1;

    uint64_t *nkeys = (uint64_t *)calloc(newcap, sizeof *nkeys);
    uint8_t *nused = (uint8_t *)calloc(newcap, 1);
    if (!nkeys || !nused) {
        free(nkeys);
        free(nused);
        return -1;
    }

    BrsU64Set old = *s;
    s->keys = nkeys;
    s->used = nused;
    s->capacity = newcap;
    s->count = 0;

    size_t mask = s->capacity - 1;

    for (size_t i = 0; i < old.capacity; ++i) {
        if (!old.used[i]) continue;

        size_t idx = (size_t)(mix64(old.keys[i]) & mask);
        for (;;) {
            if (!s->used[idx]) {
                s->used[idx] = 1;
                s->keys[idx] = old.keys[i];
                s->count++;
                break;
            }
            idx = (idx + 1) & mask;
        }
    }

    free(old.keys);
    free(old.used);
    return 0;
}

int brs_u64_set_insert(BrsU64Set *s, uint64_t v)
{
    if (!s)
        return -1;

    if (s->capacity == 0)
        return -1;

    if ((s->count + 1) * 10 >= s->capacity * 7) {
        if (u64_set_grow(s) != 0)
            return -1;
    }

    int found;
    size_t idx = u64_slot_find(s, v, &found);

    if (!found) {
        s->used[idx] = 1;
        s->keys[idx] = v;
        s->count++;
    }

    return 0;
}

int brs_u64_set_contains(const BrsU64Set *s, uint64_t v)
{
    if (!s || s->capacity == 0)
        return 0;

    int found;
    (void)u64_slot_find(s, v, &found);

    return found;
}

/* ============================================================================
 * Segmentos .idx
 * ==========================================================================*/

int brs_write_index_segment(const char *repo_path, uint64_t segment_id,
                            const BrsIndexMap *map)
{
    if (!repo_path || !map)
        return -1;

    char idx_dir[BRS_PATH_MAX];
    char path[BRS_PATH_MAX];
    char name[40];

    if (brs_path_join(idx_dir, sizeof idx_dir, repo_path, "index") != 0)
        return -1;

    snprintf(name, sizeof name, "%llu.idx", (unsigned long long)segment_id);

    if (brs_path_join(path, sizeof path, idx_dir, name) != 0)
        return -1;

    static const uint8_t zeros16[16] = {0};

    BrsBuffer buf;
    brs_buffer_init(&buf);

    int ok =
        brs_buffer_append(&buf, BRS_MAGIC_INDEX, BRS_MAGIC_LEN) == 0 &&
        brs_buffer_append_u32_le(&buf, BRS_FORMAT_VERSION) == 0 &&
        brs_buffer_append_u64_le(&buf, segment_id) == 0 &&
        brs_buffer_append(&buf, zeros16, sizeof zeros16) == 0 &&
        brs_buffer_append_u64_le(&buf, (uint64_t)map->count) == 0;

    size_t cursor = 0;
    const BrsIndexSlot *s;

    while (ok && (s = brs_index_map_next(map, &cursor)) != NULL) {
        ok =
            brs_buffer_append(&buf, s->key.bytes, 16) == 0 &&
            brs_buffer_append_u64_le(&buf, s->value.pack_id) == 0 &&
            brs_buffer_append_u64_le(&buf, s->value.offset) == 0 &&
            brs_buffer_append_u32_le(&buf, s->value.comp_size) == 0 &&
            brs_buffer_append_u32_le(&buf, s->value.uncomp_size) == 0 &&
            brs_buffer_append_u8(&buf, s->value.flags) == 0;
    }

    /*
     * CHECKSUM FNV1A-64:
     * Se calcula sobre todo el contenido del buffer y se añade como los
     * últimos 8 bytes del segmento.
     */
    if (ok) {
        uint64_t csum = brs_fnv1a_64(buf.data, buf.size);
        ok = brs_buffer_append_u64_le(&buf, csum) == 0;
    }

    if (!ok) {
        brs_buffer_free(&buf);
        return -1;
    }

    BrsVfs *vfs = brs_vfs_context_get();

    if (vfs != NULL) {
        char tmp_path[BRS_PATH_MAX];
        snprintf(tmp_path, sizeof(tmp_path),
                 "/tmp/baresnap_pack_tmp/idx_%llu.tmp",
                 (unsigned long long)segment_id);

        int fd = open(tmp_path,
                      O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                      0600);
        if (fd < 0) {
            brs_buffer_free(&buf);
            return -1;
        }

        int wr_rc = brs_write_fd_all(fd, buf.data, buf.size);
        if (wr_rc == 0) wr_rc = fsync(fd);
        if (close(fd) != 0) wr_rc = -1;

        if (wr_rc != 0) {
            unlink(tmp_path);
            brs_buffer_free(&buf);
            return -1;
        }

        char rel[BRS_PATH_MAX];
        snprintf(rel, sizeof(rel), "index/%llu.idx",
                 (unsigned long long)segment_id);

        uint64_t uploaded = 0;
        int up_rc = brs_vfs_upload_pack(vfs, rel, tmp_path, &uploaded);

        unlink(tmp_path);
        brs_buffer_free(&buf);

        if (up_rc != 0)
            return -1;
    } else {
        int rc = brs_write_file_atomic(path, buf.data, buf.size);
        brs_buffer_free(&buf);

        if (rc != 0)
            return -1;
    }

    /* Generar y guardar Bloom Filter para este segmento. */
    {
        BrsBloomFilter bloom;
        brs_bloom_init(&bloom);

        if (bloom.bits) {
            size_t it = 0;
            const BrsIndexSlot *slot;

            while ((slot = brs_index_map_next(map, &it)) != NULL)
                brs_bloom_add(&bloom, &slot->key);

            brs_bloom_save(repo_path, segment_id, &bloom);
        }

        brs_bloom_free(&bloom);
    }

    return 0;
}

int brs_load_all_indexes(const char *repo_path, BrsIndexMap *out_map)
{
    if (!repo_path || !out_map)
        return -1;

    /* BARESNAP_SSH_BULK_INDEX_PATCH */
    BrsVfs *vfs_ctx = brs_vfs_context_get();
    if (vfs_ctx && brs_vfs_sync_index_bulk(vfs_ctx, repo_path, out_map) == 0) {
        return 1; /* Fast-path SSH OK */
    }


    char idx_dir[BRS_PATH_MAX];

    if (brs_path_join(idx_dir, sizeof idx_dir, repo_path, "index") != 0)
        return -1;

    BrsDirList list;

    if (brs_list_dir(idx_dir, &list) != 0)
        return 0; /* Sin index: repo vacío, no es error. */

    int loaded = 0;

    for (size_t i = 0; i < list.count; ++i) {
        const char *name = list.names[i];
        size_t len = strlen(name);

        if (len < 5 || strcmp(name + len - 4, ".idx") != 0)
            continue;

        unsigned long long seg_id = 0;

        if (sscanf(name, "%llu.idx", &seg_id) != 1)
            continue;

        if (brs_load_index_segment(repo_path, (uint64_t)seg_id, out_map) == 0)
            loaded++;
    }

    brs_dir_list_free(&list);

    return loaded;
}

int brs_load_index_segment(const char *repo_path, uint64_t seg_id,
                           BrsIndexMap *map)
{
    if (!repo_path || !map)
        return -1;

    char dir[BRS_PATH_MAX];
    char path[BRS_PATH_MAX];
    char name[40];

    if (brs_path_join(dir, sizeof dir, repo_path, "index") != 0)
        return -1;

    snprintf(name, sizeof name, "%llu.idx", (unsigned long long)seg_id);

    if (brs_path_join(path, sizeof path, dir, name) != 0)
        return -1;

    BrsBuffer data;
    brs_buffer_init(&data);

    if (brs_read_file(path, &data) != 0) {
        brs_buffer_free(&data);
        return -1;
    }

    /* Verificar checksum FNV1A-64 antes de parsear. */
    if (data.size >= 8) {
        BrsReader cr;
        brs_reader_init(&cr, data.data + data.size - 8, 8);

        uint64_t stored_csum = 0;

        if (brs_reader_u64_le(&cr, &stored_csum) == 0 && stored_csum != 0) {
            uint64_t computed = brs_fnv1a_64(data.data, data.size - 8);

            if (computed != stored_csum) {
                fprintf(stderr,
                        "warning: index segment %llu: checksum mismatch, skipping\n",
                        (unsigned long long)seg_id);
                brs_buffer_free(&data);
                return -1;
            }
        }
    }

    BrsReader r;
    brs_reader_init(&r, data.data, data.size);

    int ok = 0;

    do {
        const uint8_t *magic;
        uint32_t version;
        uint64_t file_seg_id;
        uint64_t count;

        if (brs_reader_bytes(&r, BRS_MAGIC_LEN, &magic) != 0)
            break;

        if (memcmp(magic, BRS_MAGIC_INDEX, BRS_MAGIC_LEN) != 0)
            break;

        if (brs_reader_u32_le(&r, &version) != 0)
            break;

        if (brs_reader_u64_le(&r, &file_seg_id) != 0)
            break;

        if (file_seg_id != seg_id)
            break;

        if (brs_reader_skip(&r, 16) != 0)
            break;

        if (brs_reader_u64_le(&r, &count) != 0)
            break;

        int parse_ok = 1;

        for (uint64_t e = 0; e < count; ++e) {
            const uint8_t *idb;
            BrsChunkId cid;
            BrsChunkLocation loc;
            uint8_t fl;

            if (brs_reader_bytes(&r, 16, &idb) != 0) {
                parse_ok = 0;
                break;
            }

            memcpy(cid.bytes, idb, 16);

            if (brs_reader_u64_le(&r, &loc.pack_id) != 0) {
                parse_ok = 0;
                break;
            }

            if (brs_reader_u64_le(&r, &loc.offset) != 0) {
                parse_ok = 0;
                break;
            }

            if (brs_reader_u32_le(&r, &loc.comp_size) != 0) {
                parse_ok = 0;
                break;
            }

            if (brs_reader_u32_le(&r, &loc.uncomp_size) != 0) {
                parse_ok = 0;
                break;
            }

            if (brs_reader_u8(&r, &fl) != 0) {
                parse_ok = 0;
                break;
            }

            loc.flags = fl;

            (void)brs_index_map_put(map, &cid, &loc);
        }

        if (parse_ok)
            ok = 1;

    } while (0);

    brs_buffer_free(&data);

    return ok ? 0 : -1;
}

/* ============================================================================
 * Bloom Filter
 * ==========================================================================*/

static inline void bloom_get_seeds(const BrsChunkId *id,
                                   uint64_t *h1,
                                   uint64_t *h2)
{
    const uint8_t *p = (const uint8_t *)id;

    memcpy(h1, p, 8);
    memcpy(h2, p + 8, 8);

    /* Asegurar que h2 sea impar para mejor distribución modulo 2^64. */
    *h2 |= 1;
}

void brs_bloom_init(BrsBloomFilter *b)
{
    if (!b)
        return;

    b->size_bits = BRS_BLOOM_BITS_PER_SEG;
    b->num_hashes = BRS_BLOOM_NUM_HASHES;
    b->bits = (uint8_t *)calloc((b->size_bits + 7) / 8, 1);
}

void brs_bloom_add(BrsBloomFilter *b, const BrsChunkId *id)
{
    if (!b || !b->bits || b->size_bits == 0 || !id)
        return;

    uint64_t h1, h2;

    bloom_get_seeds(id, &h1, &h2);

    for (uint8_t i = 0; i < b->num_hashes; ++i) {
        uint64_t idx = (h1 + (uint64_t)i * h2) % b->size_bits;
        b->bits[idx / 8] |= (uint8_t)(1 << (idx % 8));
    }
}

int brs_bloom_check(const BrsBloomFilter *b, const BrsChunkId *id)
{
    if (!b || !b->bits || b->size_bits == 0 || !id)
        return 0;

    uint64_t h1, h2;

    bloom_get_seeds(id, &h1, &h2);

    for (uint8_t i = 0; i < b->num_hashes; ++i) {
        uint64_t idx = (h1 + (uint64_t)i * h2) % b->size_bits;

        if (!(b->bits[idx / 8] & (1 << (idx % 8))))
            return 0;
    }

    return 1;
}

void brs_bloom_free(BrsBloomFilter *b)
{
    if (!b)
        return;

    if (b->bits) {
        free(b->bits);
        b->bits = NULL;
    }
}

int brs_bloom_save(const char *repo_path, uint64_t seg_id,
                   const BrsBloomFilter *b)
{
    if (!repo_path || !b || !b->bits || b->size_bits == 0)
        return -1;

    char dir[BRS_PATH_MAX];
    char path[BRS_PATH_MAX];
    char name[40];

    if (brs_path_join(dir, sizeof dir, repo_path, "index") != 0)
        return -1;

    snprintf(name, sizeof name, "%llu.blm", (unsigned long long)seg_id);

    if (brs_path_join(path, sizeof path, dir, name) != 0)
        return -1;

    uint64_t sz = b->size_bits;
    size_t bytes = (size_t)((sz + 7) / 8);
    size_t total = 8 + 8 + 1 + 7 + bytes;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf)
        return -1;

    size_t off = 0;

    memcpy(buf + off, "BRSBLM01", 8);
    off += 8;

    for (int i = 0; i < 8; ++i)
        buf[off++] = (uint8_t)((sz >> (i * 8)) & 0xFF);

    buf[off++] = b->num_hashes;

    memset(buf + off, 0, 7);
    off += 7;

    memcpy(buf + off, b->bits, bytes);
    off += bytes;

    BrsVfs *vfs = brs_vfs_context_get();

    if (vfs != NULL) {
        char tmp_path[BRS_PATH_MAX];
        snprintf(tmp_path, sizeof(tmp_path),
                 "/tmp/baresnap_pack_tmp/blm_%llu.tmp",
                 (unsigned long long)seg_id);

        int fd = open(tmp_path,
                      O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                      0600);
        if (fd < 0) {
            free(buf);
            return -1;
        }

        int wr_rc = brs_write_fd_all(fd, buf, total);
        if (wr_rc == 0) wr_rc = fsync(fd);
        if (close(fd) != 0) wr_rc = -1;

        if (wr_rc != 0) {
            unlink(tmp_path);
            free(buf);
            return -1;
        }

        char rel[BRS_PATH_MAX];
        snprintf(rel, sizeof(rel), "index/%llu.blm",
                 (unsigned long long)seg_id);

        uint64_t uploaded = 0;
        int up_rc = brs_vfs_upload_pack(vfs, rel, tmp_path, &uploaded);

        unlink(tmp_path);
        free(buf);

        return up_rc;
    }

    int rc = brs_write_file_atomic(path, buf, total);
    free(buf);
    return rc;
}

int brs_bloom_load(const char *repo_path, uint64_t seg_id,
                   BrsBloomFilter *b)
{
    if (!repo_path || !b)
        return -1;

    char dir[BRS_PATH_MAX];
    char path[BRS_PATH_MAX];
    char name[40];

    if (brs_path_join(dir, sizeof dir, repo_path, "index") != 0)
        return -1;

    snprintf(name, sizeof name, "%llu.blm", (unsigned long long)seg_id);

    if (brs_path_join(path, sizeof path, dir, name) != 0)
        return -1;

    BrsBuffer data;
    brs_buffer_init(&data);

    if (brs_read_file(path, &data) != 0) {
        brs_buffer_free(&data);
        return -1;
    }

    if (data.size < 24 || memcmp(data.data, "BRSBLM01", 8) != 0) {
        brs_buffer_free(&data);
        return -1;
    }

    uint64_t sz = 0;

    for (int i = 0; i < 8; ++i)
        sz |= ((uint64_t)data.data[8 + i]) << (i * 8);

    uint8_t nh = data.data[16];

    size_t bytes = (size_t)((sz + 7) / 8);

    if (data.size < 24 + bytes) {
        brs_buffer_free(&data);
        return -1;
    }

    b->size_bits = sz;
    b->num_hashes = nh;
    b->bits = (uint8_t *)malloc(bytes);

    if (!b->bits) {
        brs_buffer_free(&data);
        return -1;
    }

    memcpy(b->bits, data.data + 24, bytes);

    brs_buffer_free(&data);

    return 0;
}

int brs_load_bloom_set(const char *repo_path, BrsBloomSet *set)
{
    if (!repo_path || !set)
        return -1;

    char dir[BRS_PATH_MAX];

    if (brs_path_join(dir, sizeof dir, repo_path, "index") != 0)
        return -1;

    BrsDirList dl;

    if (brs_list_dir(dir, &dl) != 0)
        return -1;

            set->count = 0;
    set->capacity = dl.count ? (dl.count + 1) : 1;
    set->segs = (BrsBloomSegment *)calloc(set->capacity,
                                          sizeof(BrsBloomSegment));

    if (!set->segs) {
        brs_dir_list_free(&dl);
        return -1;
    }

    for (size_t i = 0; i < dl.count; ++i) {
        const char *name = dl.names[i];
        size_t ln = strlen(name);

        if (ln <= 4 || strcmp(name + ln - 4, ".blm") != 0)
            continue;

        unsigned long long sid = 0;

        if (sscanf(name, "%llu.blm", &sid) != 1)
            continue;

        BrsBloomFilter b;

        if (brs_bloom_load(repo_path, (uint64_t)sid, &b) != 0)
            continue;


                        if (set->count == set->capacity) {
                size_t ncap = set->capacity * 2;
                if (ncap < set->capacity) {
                    brs_bloom_free(&b);
                    break;
                }

                BrsBloomSegment *tmp =
                    (BrsBloomSegment *)realloc(set->segs,
                                               ncap *
                                               sizeof(BrsBloomSegment));
                if (!tmp) {
                    brs_bloom_free(&b);
                    break;
                }

                set->segs = tmp;
                set->capacity = ncap;
            }

        set->segs[set->count].seg_id = (uint64_t)sid;
        set->segs[set->count].bloom = b;
        set->count++;
    }

    brs_dir_list_free(&dl);

    return 0;
}

void brs_bloom_set_free(BrsBloomSet *set)
{
    if (!set)
        return;

    for (size_t i = 0; i < set->count; ++i)
        brs_bloom_free(&set->segs[i].bloom);

    free(set->segs);

    set->segs = NULL;
    set->count = 0;
    set->capacity = 0;
}

/* ============================================================================
 * Carga un segmento .idx desde un fichero LOCAL (cache SSH).
 * ==========================================================================*/
int brs_load_index_segment_from_file(const char *local_path, uint64_t seg_id,
                                     BrsIndexMap *map)
{
    if (!local_path || !map) return -1;
    BrsBuffer data;
    brs_buffer_init(&data);
    if (brs_read_file(local_path, &data) != 0) {
        brs_buffer_free(&data);
        return -1;
    }
    if (data.size >= 8) {
        BrsReader cr;
        brs_reader_init(&cr, data.data + data.size - 8, 8);
        uint64_t stored_csum = 0;
        if (brs_reader_u64_le(&cr, &stored_csum) == 0 && stored_csum != 0) {
            uint64_t computed = brs_fnv1a_64(data.data, data.size - 8);
            if (computed != stored_csum) {
                brs_buffer_free(&data);
                return -1;
            }
        }
    }
    BrsReader r;
    brs_reader_init(&r, data.data, data.size);
    int ok = 0;
    do {
        const uint8_t *magic;
        uint32_t version;
        uint64_t file_seg_id, count;
        if (brs_reader_bytes(&r, BRS_MAGIC_LEN, &magic) != 0) break;
        if (memcmp(magic, BRS_MAGIC_INDEX, BRS_MAGIC_LEN) != 0) break;
        if (brs_reader_u32_le(&r, &version) != 0) break;
        if (brs_reader_u64_le(&r, &file_seg_id) != 0) break;
        if (file_seg_id != seg_id) break;
        if (brs_reader_skip(&r, 16) != 0) break;
        if (brs_reader_u64_le(&r, &count) != 0) break;
        int parse_ok = 1;
        for (uint64_t e = 0; e < count; ++e) {
            const uint8_t *idb;
            BrsChunkId cid;
            BrsChunkLocation loc;
            uint8_t fl;
            if (brs_reader_bytes(&r, 16, &idb) != 0) { parse_ok = 0; break; }
            memcpy(cid.bytes, idb, 16);
            if (brs_reader_u64_le(&r, &loc.pack_id) != 0) { parse_ok = 0; break; }
            if (brs_reader_u64_le(&r, &loc.offset) != 0) { parse_ok = 0; break; }
            if (brs_reader_u32_le(&r, &loc.comp_size) != 0) { parse_ok = 0; break; }
            if (brs_reader_u32_le(&r, &loc.uncomp_size) != 0) { parse_ok = 0; break; }
            if (brs_reader_u8(&r, &fl) != 0) { parse_ok = 0; break; }
            loc.flags = fl;
            (void)brs_index_map_put(map, &cid, &loc);
        }
        if (parse_ok) ok = 1;
    } while (0);
    brs_buffer_free(&data);
    return ok ? 0 : -1;
}
