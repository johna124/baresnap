/* brs_cache.h — file cache persistente: path -> BrsFileCacheEntry */
#ifndef BRS_CACHE_H
#define BRS_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include "brs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    BrsFileCacheEntry *entries;
    uint8_t *state;     /* 0 vacio, 1 usado, 2 tombstone */
    size_t capacity;    /* potencia de 2 */
    size_t count;
    size_t tombstones;
} BrsCacheMap;

int  brs_cache_map_init(BrsCacheMap *m, size_t initial_capacity);
void brs_cache_map_free(BrsCacheMap *m);
size_t brs_cache_map_count(const BrsCacheMap *m);

/* Devuelve puntero a la entrada, creandola si no existe (NULL = OOM). */
BrsFileCacheEntry *brs_cache_map_touch(BrsCacheMap *m, const char *path);
const BrsFileCacheEntry *brs_cache_map_get(const BrsCacheMap *m,
                                           const char *path);
int brs_cache_map_remove(BrsCacheMap *m, const char *path);
const BrsFileCacheEntry *brs_cache_map_next(const BrsCacheMap *m,
                                            size_t *cursor);

/* Fichero "cache" del repo (formato identico al C++, con FNV1a al final). */
int brs_load_file_cache(const char *repo_path, BrsCacheMap *cache);
int brs_save_file_cache(const char *repo_path, const BrsCacheMap *cache);

#ifdef __cplusplus
}
#endif

#endif /* BRS_CACHE_H */