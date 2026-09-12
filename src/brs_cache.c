/* brs_cache.c — file cache: mapa de cadenas + serializacion */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "brs_cache.h"

#include "brs_buffer.h"
#include "brs_fsutil.h"
#include "brs_util.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Mapa path -> entrada (open addressing + tombstones)
 * ==========================================================================*/
int brs_cache_map_init(BrsCacheMap *m, size_t initial_capacity)
{
    if (!m) return -1;
    memset(m, 0, sizeof *m);
    size_t cap = 16;
    while (cap < initial_capacity) cap *= 2;
    m->entries = (BrsFileCacheEntry *)calloc(cap, sizeof *m->entries);
    m->state = (uint8_t *)calloc(cap, 1);
    if (!m->entries || !m->state) {
        brs_cache_map_free(m);
        return -1;
    }
    m->capacity = cap;
    return 0;
}

void brs_cache_map_free(BrsCacheMap *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->capacity; ++i) {
        if (m->state && m->state[i] == 1)
            brs_file_cache_entry_free(&m->entries[i]);
    }
    free(m->entries);
    free(m->state);
    memset(m, 0, sizeof *m);
}

size_t brs_cache_map_count(const BrsCacheMap *m)
{
    return m ? m->count : 0;
}

static size_t cache_find_slot(const BrsCacheMap *m, const char *path,
                              int *found)
{
    size_t mask = m->capacity - 1;
    uint64_t h = brs_fnv1a_64(path, strlen(path));
    size_t idx = (size_t)(h & mask);
    size_t first_tomb = SIZE_MAX;
    for (;;) {
        uint8_t st = m->state[idx];
        if (st == 0) {
            *found = 0;
            return (first_tomb != SIZE_MAX) ? first_tomb : idx;
        }
        if (st == 1 && strcmp(m->entries[idx].path, path) == 0) {
            *found = 1;
            return idx;
        }
        if (st == 2 && first_tomb == SIZE_MAX) first_tomb = idx;
        idx = (idx + 1) & mask;
    }
}

static int cache_map_grow(BrsCacheMap *m)
{
    BrsCacheMap fresh;
    if (brs_cache_map_init(&fresh, m->capacity * 2) != 0) return -1;
    for (size_t i = 0; i < m->capacity; ++i) {
        if (m->state[i] != 1) continue;
        int found;
        size_t idx = cache_find_slot(&fresh, m->entries[i].path, &found);
        fresh.state[idx] = 1;
        fresh.count++;
        fresh.entries[idx] = m->entries[i];   /* ownership transferida */
    }
    free(m->entries);
    free(m->state);
    *m = fresh;
    return 0;
}

BrsFileCacheEntry *brs_cache_map_touch(BrsCacheMap *m, const char *path)
{
    if (!m || !path || m->capacity == 0) return NULL;
    if ((m->count + m->tombstones + 1) * 10 >= m->capacity * 7) {
        if (cache_map_grow(m) != 0) return NULL;
    }
    int found;
    size_t idx = cache_find_slot(m, path, &found);
    if (found) return &m->entries[idx];

    uint8_t prev_state = m->state[idx];
    if (prev_state == 2) m->tombstones--;
    m->state[idx] = 1;
    m->count++;
    brs_file_cache_entry_init(&m->entries[idx]);
    m->entries[idx].path = strdup(path);
    if (!m->entries[idx].path) {
        m->state[idx] = prev_state;
        if (prev_state == 2) m->tombstones++;
        m->count--;
        return NULL;
    }
    return &m->entries[idx];
}

const BrsFileCacheEntry *brs_cache_map_get(const BrsCacheMap *m,
                                           const char *path)
{
    if (!m || !path || m->capacity == 0) return NULL;
    int found;
    size_t idx = cache_find_slot(m, path, &found);
    return found ? &m->entries[idx] : NULL;
}

int brs_cache_map_remove(BrsCacheMap *m, const char *path)
{
    if (!m || !path || m->capacity == 0) return -1;
    int found;
    size_t idx = cache_find_slot(m, path, &found);
    if (!found) return -1;
    brs_file_cache_entry_free(&m->entries[idx]);
    m->state[idx] = 2;
    m->count--;
    m->tombstones++;
    return 0;
}

const BrsFileCacheEntry *brs_cache_map_next(const BrsCacheMap *m,
                                            size_t *cursor)
{
    if (!m || !cursor) return NULL;
    while (*cursor < m->capacity) {
        size_t i = *cursor;
        (*cursor)++;
        if (m->state[i] == 1) return &m->entries[i];
    }
    return NULL;
}

/* ============================================================================
 * Serializacion (formato identico a cache.cpp del C++)
 * ==========================================================================*/
int brs_save_file_cache(const char *repo_path, const BrsCacheMap *cache)
{
    if (!repo_path || !cache) return -1;
    char path[BRS_PATH_MAX];
    if (brs_path_join(path, sizeof path, repo_path, "cache") != 0) return -1;

    BrsBuffer buf;
    brs_buffer_init(&buf);
    int ok =
        brs_buffer_append(&buf, BRS_MAGIC_CACHE, BRS_MAGIC_LEN) == 0 &&
        brs_buffer_append_u32_le(&buf, BRS_FORMAT_VERSION) == 0 &&
        brs_buffer_append_u64_le(&buf, (uint64_t)cache->count) == 0;

    size_t cursor = 0;
    const BrsFileCacheEntry *ce;
    while (ok && (ce = brs_cache_map_next(cache, &cursor)) != NULL) {
        size_t plen = strlen(ce->path);
        ok = brs_buffer_append_u32_le(&buf, (uint32_t)plen) == 0 &&
             brs_buffer_append(&buf, ce->path, plen) == 0 &&
             brs_buffer_append_u64_le(&buf, ce->dev) == 0 &&
             brs_buffer_append_u64_le(&buf, ce->ino) == 0 &&
             brs_buffer_append_u64_le(&buf, ce->size) == 0 &&
             brs_buffer_append_u64_le(&buf, ce->mtime_ns) == 0 &&
             brs_buffer_append_u64_le(&buf, ce->ctime_ns) == 0 &&
             brs_buffer_append_u32_le(&buf, ce->mode) == 0 &&
             brs_buffer_append_u32_le(&buf, ce->chunk_count) == 0;
        for (uint32_t c = 0; ok && c < ce->chunk_count; ++c)
            ok = brs_buffer_append(&buf, ce->chunks[c].bytes, 16) == 0;
    }
    if (ok) {
        uint64_t csum = brs_fnv1a_64(buf.data, buf.size);
        ok = brs_buffer_append_u64_le(&buf, csum) == 0;
    }

    int rc = ok ? brs_write_file_atomic(path, buf.data, buf.size) : -1;
    brs_buffer_free(&buf);
    return rc;
}

int brs_load_file_cache(const char *repo_path, BrsCacheMap *cache)
{
    if (!repo_path || !cache) return -1;
    char path[BRS_PATH_MAX];
    if (brs_path_join(path, sizeof path, repo_path, "cache") != 0) return -1;

    BrsBuffer data;
    brs_buffer_init(&data);
    if (brs_read_file(path, &data) != 0) {
        brs_buffer_free(&data);
        return -1;
    }

    int rc = -1;
    BrsReader r;
    brs_reader_init(&r, data.data, data.size);
    do {
        const uint8_t *magic;
        uint32_t version;
        uint64_t entry_count;
        if (brs_reader_bytes(&r, BRS_MAGIC_LEN, &magic) != 0) break;
        if (memcmp(magic, BRS_MAGIC_CACHE, BRS_MAGIC_LEN) != 0) break;
        if (brs_reader_u32_le(&r, &version) != 0) break;
        if (version != BRS_FORMAT_VERSION) break;
        if (brs_reader_u64_le(&r, &entry_count) != 0) break;

        int bad = 0;
        for (uint64_t i = 0; i < entry_count; ++i) {
            uint32_t plen;
            const uint8_t *p;
            if (brs_reader_u32_le(&r, &plen) != 0) { bad = 1; break; }
            if (plen >= BRS_PATH_MAX) { bad = 1; break; }
            if (brs_reader_bytes(&r, plen, &p) != 0) { bad = 1; break; }

            char tmp[BRS_PATH_MAX];
            memcpy(tmp, p, plen);
            tmp[plen] = '\0';

            BrsFileCacheEntry *ce = brs_cache_map_touch(cache, tmp);
            if (!ce) { bad = 1; break; }
            if (brs_reader_u64_le(&r, &ce->dev) != 0) { bad = 1; break; }
            if (brs_reader_u64_le(&r, &ce->ino) != 0) { bad = 1; break; }
            if (brs_reader_u64_le(&r, &ce->size) != 0) { bad = 1; break; }
            if (brs_reader_u64_le(&r, &ce->mtime_ns) != 0) { bad = 1; break; }
            if (brs_reader_u64_le(&r, &ce->ctime_ns) != 0) { bad = 1; break; }
            if (brs_reader_u32_le(&r, &ce->mode) != 0) { bad = 1; break; }

            uint32_t cc;
            if (brs_reader_u32_le(&r, &cc) != 0) { bad = 1; break; }
            ce->chunks = (BrsChunkId *)calloc(cc ? cc : 1, sizeof(BrsChunkId));
            if (!ce->chunks) { bad = 1; break; }
            ce->chunk_cap = cc;
            ce->chunk_count = cc;
            for (uint32_t c = 0; c < cc; ++c) {
                const uint8_t *cid;
                if (brs_reader_bytes(&r, 16, &cid) != 0) { bad = 1; break; }
                memcpy(ce->chunks[c].bytes, cid, 16);
            }
            if (bad) break;
        }
        if (bad) break;

        uint64_t stored;
        if (brs_reader_u64_le(&r, &stored) != 0) break;
        if (stored != 0) {
            if (data.size < 8) break;
            if (brs_fnv1a_64(data.data, data.size - 8) != stored) break;
        }
        rc = 0;
    } while (0);

    if (rc != 0) {
        /* Carga fallida: dejar el cache vacio y utilizable. */
        brs_cache_map_free(cache);
        (void)brs_cache_map_init(cache, 16);
    }
    brs_buffer_free(&data);
    return rc;
}
