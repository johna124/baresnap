#include "brs_repo_internal.h"
#include "brs_uri.h"
#include "brs_vfs_context.h"
#include "brs_lock.h"
#include "brs_repo_health.h"
#include "brs_fsutil.h"
#include "brs_chunker.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

/* ============================================================================
 * Flag global para saber si realmente hicimos precarga completa del índice.
 * ==========================================================================*/
static _Atomic int g_brs_ssh_full_index = 0;

/* ============================================================================
 * Compresión dinámica por snapshot.
 * ==========================================================================*/
typedef struct {
    int use_compression;
    int use_zstd;
    int zstd_level;
} BrsSessionCompression;

static int brs_repo_create_impl(const char *repo_path, const char *source_spec,
                                const char *label, int delta_binary,
                                int compression_override, int zstd_level_override,
                                BrsProgressCallback cb, void *cb_user,
                                _Atomic int *cancel_flag);

static int brs_ascii_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 'a';
    return c;
}

static int brs_ci_equals(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (brs_ascii_tolower((unsigned char)*a) != brs_ascii_tolower((unsigned char)*b))
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static int brs_parse_compression_token(const char *token)
{
    if (!token || token[0] == '\0')
        return -1;

    if (brs_ci_equals(token, "auto") ||
        brs_ci_equals(token, "config") ||
        brs_ci_equals(token, "default")) {
        return -1;
    }

    if (brs_ci_equals(token, "none") ||
        brs_ci_equals(token, "off") ||
        brs_ci_equals(token, "store") ||
        brs_ci_equals(token, "uncompressed") ||
        brs_ci_equals(token, "0")) {
        return 0;
    }

    if (brs_ci_equals(token, "lz4") || brs_ci_equals(token, "1"))
        return 1;

    if (brs_ci_equals(token, "zstd") || brs_ci_equals(token, "2"))
        return 2;

    return -1;
}

static int brs_parse_zstd_level_token(const char *token, int fallback)
{
    if (!token || token[0] == '\0')
        return fallback;

    char *end = NULL;
    long v = strtol(token, &end, 10);
    if (end == token)
        return fallback;

    if (v < 1)
        return fallback;
    if (v > 22)
        v = 22;

    return (int)v;
}

/* ============================================================================
 * FIX O1: Guardián RAM POSIX contra OOM con ZSTD Ultra.
 * ==========================================================================*/
#define BRS_RAM_GUARD_THRESHOLD_BYTES (600ULL * 1024ULL * 1024ULL)
#define BRS_ZSTD_ULTRA_MIN_LEVEL      20
#define BRS_ZSTD_SAFE_LEVEL           19

static uint64_t brs_available_ram_bytes(void)
{
    {
        const char *fake = getenv("BRS_RAM_GUARD_FAKE_MB");
        if (fake && fake[0] != '\0') {
            long long mb = atoll(fake);
            if (mb >= 0)
                return (uint64_t)mb * 1024ULL * 1024ULL;
        }
    }

#if defined(__linux__)
    {
        FILE *f = fopen("/proc/meminfo", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                unsigned long long kb = 0;
                if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                    fclose(f);
                    return (uint64_t)kb * 1024ULL;
                }
            }
            fclose(f);
        }
    }
#endif

    {
        long pages = sysconf(_SC_AVPHYS_PAGES);
        long psize = sysconf(_SC_PAGESIZE);
        if (pages > 0 && psize > 0)
            return (uint64_t)pages * (uint64_t)psize;
    }

    return UINT64_MAX;
}

static BrsSessionCompression brs_resolve_session_compression(const BrsRepoConfig *cfg,
                                                               int compression_override,
                                                               int zstd_level_override)
{
    BrsSessionCompression s;
    s.use_compression = (cfg->compression != BRS_COMPRESSION_NONE);
    s.use_zstd = (cfg->compression == BRS_COMPRESSION_ZSTD);
    s.zstd_level = cfg->zstd_level;

    if (compression_override == 0) {
        s.use_compression = 0;
        s.use_zstd = 0;
    } else if (compression_override == 1) {
        s.use_compression = 1;
        s.use_zstd = 0;
    } else if (compression_override == 2) {
        s.use_compression = 1;
        s.use_zstd = 1;
    }

    if (zstd_level_override > 0) {
        s.zstd_level = zstd_level_override;
    }

    if (s.zstd_level < 1)  s.zstd_level = 1;
    if (s.zstd_level > 22) s.zstd_level = 22;

    if (s.use_zstd && s.zstd_level >= BRS_ZSTD_ULTRA_MIN_LEVEL) {
        uint64_t avail = brs_available_ram_bytes();
        if (avail < BRS_RAM_GUARD_THRESHOLD_BYTES) {
            fprintf(stderr,
                    "[RAM-GUARD] available RAM %llu MB < 600 MB: "
                    "ZSTD level %d -> %d (OOM mitigation)\n",
                    (unsigned long long)(avail / (1024ULL * 1024ULL)),
                    s.zstd_level, BRS_ZSTD_SAFE_LEVEL);
            s.zstd_level = BRS_ZSTD_SAFE_LEVEL;
        }
    }

    return s;
}

int brs_repo_create(const char *repo_path, const char *source_spec,
                    const char *label, int delta_binary,
                    BrsProgressCallback cb, void *cb_user,
                    _Atomic int *cancel_flag)
{
    int compression_override = -1;
    int zstd_level_override = 0;

    const char *env_comp = getenv("BRS_CREATE_COMPRESSION");
    if (env_comp) {
        int parsed = brs_parse_compression_token(env_comp);
        if (parsed >= 0)
            compression_override = parsed;
    }

    const char *env_level = getenv("BRS_CREATE_ZSTD_LEVEL");
    if (env_level)
        zstd_level_override = brs_parse_zstd_level_token(env_level, 0);

    return brs_repo_create_impl(repo_path, source_spec, label, delta_binary,
                                compression_override, zstd_level_override,
                                cb, cb_user, cancel_flag);
}

int brs_repo_create_ex(const char *repo_path, const char *source_spec,
                       const char *label, int delta_binary,
                       int compression_override, int zstd_level_override,
                       BrsProgressCallback cb, void *cb_user,
                       _Atomic int *cancel_flag)
{
    return brs_repo_create_impl(repo_path, source_spec, label, delta_binary,
                                compression_override, zstd_level_override,
                                cb, cb_user, cancel_flag);
}

/* ============================================================================
 * create: pipeline reader -> SPSC -> writer
 * ==========================================================================*/
typedef struct {
    const BrsRepoConfig *cfg;
    BrsManifestList *entries;
    char **entry_parents;
    pthread_mutex_t *index_mtx;
    BrsIndexMap *existing_index;
    BrsIndexMap *new_index;
    BrsBufferPool *pool;
    BrsSpscQueue *queue;
    BrsFastCDC *cdc;
    _Atomic int *cancel_flag;
    atomic_size_t *next_task;
    _Atomic uint64_t *files_done;
    BrsFileTask *tasks;
    size_t task_count;
    const char *repo_path;
    BrsCacheMap *cache;
    BrsSecureKey *key;
    BrsBloomSet *blooms;
    BrsU64Set *loaded_segs;
    int delta_binary;
    _Atomic uint64_t *total_input_bytes;
    _Atomic uint64_t *total_output_bytes;
    uint8_t *thread_read_buf;
} CreateWorkerCtx;

typedef struct {
    CreateWorkerCtx *w;
    uint32_t entry_index;
    int err;
    int delta_binary;
} CreateEmitCtx;

static const char *ssh_cache_dir(const char *repo_path)
{
    static char dir[BRS_PATH_MAX];
    const char *home = getenv("HOME");
    if (!home)
        home = "/tmp";

    uint64_t h = 0;
    for (const char *p = repo_path; *p; ++p)
        h = h * 31 + (uint64_t)(unsigned char)*p;

    snprintf(dir, sizeof dir, "%s/.cache/baresnap/%016llx/index",
             home, (unsigned long long)h);
    return dir;
}


static int brs_verify_index_packs_exist(const char *repo_path,
                                        const BrsIndexMap *index)
{
    BrsVfs *vfs = brs_vfs_context_get();
    BrsU64Set checked;
    brs_u64_set_init(&checked, 64);

    size_t cursor = 0;
    const BrsIndexSlot *slot;

    while ((slot = brs_index_map_next(index, &cursor)) != NULL) {
        uint64_t pack_id = slot->value.pack_id;

        if (brs_u64_set_contains(&checked, pack_id))
            continue;

        char rel_pack[BRS_PATH_MAX];
        snprintf(rel_pack, sizeof(rel_pack), "packs/%llu.pack",
                 (unsigned long long)pack_id);

        int missing = 0;

        if (vfs != NULL) {
            missing = !brs_vfs_exists(vfs, rel_pack);
        } else {
            char full_pack[BRS_PATH_MAX];
            if (brs_path_join(full_pack, sizeof(full_pack),
                              repo_path, rel_pack) == 0) {
                missing = !brs_path_exists(full_pack);
            } else {
                missing = 1;
            }
        }

        if (missing) {
            fprintf(stderr,
                    "error: pack file '%s' does not exist\n",
                    rel_pack);
            brs_u64_set_free(&checked);
            return -1;
        }

        brs_u64_set_insert(&checked, pack_id);
    }

    brs_u64_set_free(&checked);
    return 0;
}

static int ssh_load_all_indexes(const char *repo_path, BrsIndexMap *existing)
{
    char idx_dir[BRS_PATH_MAX];
    if (brs_path_join(idx_dir, sizeof idx_dir, repo_path, "index") != 0)
        return -1;

    BrsDirList list;
    memset(&list, 0, sizeof list);
    if (brs_list_dir(idx_dir, &list) != 0) {
        fprintf(stderr, "SSH: no se pudo listar index: %s\n", idx_dir);
        return 0;
    }

    const char *cache_dir = ssh_cache_dir(repo_path);
    char cache_dir_copy[BRS_PATH_MAX];
    snprintf(cache_dir_copy, sizeof cache_dir_copy, "%s", cache_dir);
    brs_mkdir_p(cache_dir_copy);

    int loaded = 0, cached = 0;

    for (size_t i = 0; i < list.count; ++i) {
        const char *name = list.names[i];
        size_t nlen = strlen(name);
        if (nlen < 5 || strcmp(name + nlen - 4, ".idx") != 0)
            continue;

        unsigned long long seg_id = 0;
        if (sscanf(name, "%llu.idx", &seg_id) != 1)
            continue;

        char local_path[BRS_PATH_MAX];
        int lp_sn = snprintf(local_path, sizeof local_path, "%s/%s", cache_dir, name);
        if (lp_sn < 0 || (size_t)lp_sn >= sizeof local_path)
            continue;

        if (brs_path_exists(local_path)) {
            if (brs_load_index_segment_from_file(local_path, (uint64_t)seg_id, existing) == 0) {
                cached++;
                loaded++;
                continue;
            }
            unlink(local_path);
        }

        char remote_path[BRS_PATH_MAX];
        if (brs_path_join(remote_path, sizeof remote_path, idx_dir, name) == 0) {
            BrsBuffer seg_data;
            brs_buffer_init(&seg_data);
            if (brs_read_file(remote_path, &seg_data) == 0 && seg_data.size > 0) {
                brs_write_file_atomic(local_path, seg_data.data, seg_data.size);
                brs_buffer_free(&seg_data);
                if (brs_load_index_segment_from_file(local_path, (uint64_t)seg_id, existing) == 0)
                    loaded++;
            } else {
                brs_buffer_free(&seg_data);
            }
        }
    }

    brs_dir_list_free(&list);
    fprintf(stderr, "SSH: %d segmentos (%d cache, %d descargados)\n",
            loaded, cached, loaded - cached);
    return loaded;
}

static int brs_verify_pack_existence(const char *repo_path,
                                     const BrsIndexMap *index)
{
    BrsVfs *vfs = brs_vfs_context_get();
    BrsU64Set checked;
    brs_u64_set_init(&checked, 64);

    size_t cursor = 0;
    const BrsIndexSlot *slot;

    while ((slot = brs_index_map_next(index, &cursor)) != NULL) {
        uint64_t pack_id = slot->value.pack_id;

        if (brs_u64_set_contains(&checked, pack_id))
            continue;

        char rel_pack[BRS_PATH_MAX];
        snprintf(rel_pack, sizeof(rel_pack), "packs/%llu.pack",
                 (unsigned long long)pack_id);

        if (vfs != NULL) {
            if (!brs_vfs_exists(vfs, rel_pack)) {
                fprintf(stderr,
                        "error: pack file '%s' does not exist on remote\n",
                        rel_pack);
                brs_u64_set_free(&checked);
                return -1;
            }
        } else {
            char full_pack[BRS_PATH_MAX];
            if (brs_path_join(full_pack, sizeof(full_pack),
                              repo_path, rel_pack) != 0) {
                brs_u64_set_free(&checked);
                return -1;
            }
            if (!brs_path_exists(full_pack)) {
                fprintf(stderr,
                        "error: pack file '%s' does not exist locally\n",
                        full_pack);
                brs_u64_set_free(&checked);
                return -1;
            }
        }

        brs_u64_set_insert(&checked, pack_id);
    }

    brs_u64_set_free(&checked);
    return 0;
}

/* ============================================================================
 * Lazy loading del índice mediante Bloom Filters
 * ==========================================================================*/
static void lazy_load_segment_locked(CreateWorkerCtx *w, uint64_t seg_id)
{
    if (brs_u64_set_contains(w->loaded_segs, seg_id))
        return;

    brs_load_index_segment(w->repo_path, seg_id, w->existing_index);
    brs_u64_set_insert(w->loaded_segs, seg_id);
}

static const BrsChunkLocation *lazy_get_locked(CreateWorkerCtx *w,
                                               const BrsChunkId *id)
{
    const BrsChunkLocation *loc = brs_index_map_get(w->existing_index, id);
    if (loc)
        return loc;

    if (atomic_load_explicit(&g_brs_ssh_full_index, memory_order_relaxed))
        return NULL;

    if (!w->blooms)
        return NULL;

    for (size_t b = 0; b < w->blooms->count; ++b) {
        if (brs_bloom_check(&w->blooms->segs[b].bloom, id)) {
            lazy_load_segment_locked(w, w->blooms->segs[b].seg_id);
            loc = brs_index_map_get(w->existing_index, id);
            if (loc)
                return loc;
        }
    }

    return NULL;
}

const BrsChunkLocation *lazy_get_main(const char *repo_path,
                                      BrsIndexMap *existing,
                                      BrsBloomSet *blooms,
                                      const BrsChunkId *id)
{
    const BrsChunkLocation *loc = brs_index_map_get(existing, id);
    if (loc)
        return loc;

    if (!blooms)
        return NULL;

    for (size_t b = 0; b < blooms->count; ++b) {
        if (brs_bloom_check(&blooms->segs[b].bloom, id)) {
            brs_load_index_segment(repo_path, blooms->segs[b].seg_id, existing);
            loc = brs_index_map_get(existing, id);
            if (loc)
                return loc;
        }
    }

    return NULL;
}

static uint8_t *reconstruct_from_chunks_worker(CreateWorkerCtx *w,
                                               const BrsChunkId *chunks,
                                               uint32_t chunk_count,
                                               size_t *out_size)
{
    if (!out_size)
        return NULL;

    *out_size = 0;

    BrsChunkLocation *locs =
        (BrsChunkLocation *)malloc((size_t)chunk_count * sizeof(BrsChunkLocation));
    if (!locs)
        return NULL;

    size_t total_size = 0;

    pthread_mutex_lock(w->index_mtx);
    for (uint32_t c = 0; c < chunk_count; ++c) {
        const BrsChunkLocation *loc = lazy_get_locked(w, &chunks[c]);
        if (!loc) {
            pthread_mutex_unlock(w->index_mtx);
            free(locs);
            return NULL;
        }
        locs[c] = *loc;
        total_size += loc->uncomp_size;
    }
    pthread_mutex_unlock(w->index_mtx);

    if (total_size == 0) {
        free(locs);
        *out_size = 0;
        return (uint8_t *)calloc(1, 1);
    }

    uint8_t *buffer = (uint8_t *)malloc(total_size);
    if (!buffer) {
        free(locs);
        return NULL;
    }

    size_t offset = 0;
    BrsBuffer chunk_buf, dec_buf, uncomp_buf;
    brs_buffer_init(&chunk_buf);
    brs_buffer_init(&dec_buf);
    brs_buffer_init(&uncomp_buf);

    for (uint32_t c = 0; c < chunk_count; ++c) {
        if (read_chunk_from_pack(w->repo_path, &locs[c], &chunk_buf) != 0) {
            free(buffer);
            goto fail;
        }

        const uint8_t *src = chunk_buf.data;
        size_t src_len = chunk_buf.size;

        if (locs[c].flags & BRS_CHUNK_FLAG_ENCRYPTED) {
            if (brs_decrypt_buffer(w->key, w->cfg->cipher_algo, src, src_len, &dec_buf) != 0) {
                free(buffer);
                goto fail;
            }
            src = dec_buf.data;
            src_len = dec_buf.size;
        }

        const uint8_t *to_write = src;
        size_t write_len = src_len;

        if (locs[c].flags & BRS_CHUNK_FLAG_COMPRESSED) {
            if (locs[c].uncomp_size > BRS_CHUNK_UNCOMP_MAX) {
                free(buffer);
                goto fail;
            }

            if (brs_buffer_resize(&uncomp_buf, locs[c].uncomp_size) != 0) {
                free(buffer);
                goto fail;
            }

            size_t dl;
            if (locs[c].flags & BRS_CHUNK_FLAG_ZSTD)
                dl = brs_zstd_decompress(src, src_len, uncomp_buf.data, uncomp_buf.size);
            else
                dl = brs_lz4_decompress(src, src_len, uncomp_buf.data, uncomp_buf.size);

            if (dl != (size_t)locs[c].uncomp_size) {
                free(buffer);
                goto fail;
            }

            to_write = uncomp_buf.data;
            write_len = dl;
        }

        if (offset + write_len > total_size) {
            free(buffer);
            goto fail;
        }

        memcpy(buffer + offset, to_write, write_len);
        offset += write_len;
    }

    brs_buffer_free(&chunk_buf);
    brs_buffer_free(&dec_buf);
    brs_buffer_wipe_free(&uncomp_buf);
    free(locs);
    *out_size = total_size;
    return buffer;

fail:
    brs_buffer_free(&chunk_buf);
    brs_buffer_free(&dec_buf);
    brs_buffer_wipe_free(&uncomp_buf);
    free(locs);
    return NULL;
}

static int worker_emit_chunk(const uint8_t *data, size_t size, void *user)
{
    CreateEmitCtx *ec = (CreateEmitCtx *)user;
    CreateWorkerCtx *w = ec->w;

    if (ec->err)
        return -1;

    if (size == 0)
        return 0;

    if (check_cancel(w->cancel_flag)) {
        ec->err = 1;
        return -1;
    }

    BrsChunkId id;
    brs_hash_chunk((BrsHashAlgo)w->cfg->hash_algo, data, size, &id);

    BrsManifestEntry *e = &w->entries->items[ec->entry_index];
    if (brs_manifest_entry_add_chunk(e, &id) != 0) {
        ec->err = 1;
        return -1;
    }

    pthread_mutex_lock(w->index_mtx);
    int known = (lazy_get_locked(w, &id) != NULL) ||
                (brs_index_map_get(w->new_index, &id) != NULL);

    if (!known) {
        BrsChunkLocation loc;
        memset(&loc, 0, sizeof loc);
        loc.comp_size = (uint32_t)size;
        loc.uncomp_size = (uint32_t)size;
        (void)brs_index_map_put(w->new_index, &id, &loc);
    }
    pthread_mutex_unlock(w->index_mtx);

    if (known)
        return 0;

    uint32_t slot = brs_buffer_pool_acquire(w->pool);
    if (check_cancel(w->cancel_flag)) {
        brs_buffer_pool_release(w->pool, slot);
        ec->err = 1;
        return -1;
    }

    memcpy(brs_buffer_pool_get(w->pool, slot), data, size);

    BrsChunkItem item;
    memset(&item, 0, sizeof item);
    item.buffer_slot = slot;
    item.size = (uint32_t)size;
    item.entry_index = ec->entry_index;
    item.id = id;

    brs_spsc_push(w->queue, &item);
    return 0;
}

static void *create_worker(void *arg)
{
    CreateWorkerCtx *w = (CreateWorkerCtx *)arg;
    BrsStreamingChunker sc;
    brs_streaming_chunker_init(&sc, w->cdc);

    int cancelled = 0;

    if (w->thread_read_buf) {
        for (;;) {
            if (check_cancel(w->cancel_flag)) {
                cancelled = 1;
                break;
            }

            size_t t = atomic_fetch_add_explicit(w->next_task, 1, memory_order_relaxed);
            if (t >= w->task_count)
                break;

            const BrsFileTask *task = &w->tasks[t];
            BrsManifestEntry *e = &w->entries->items[task->entry_index];

            /* === DELTA ENCODING === */
            int use_delta = 0;
            uint8_t *delta_payload = NULL;
            size_t delta_len = 0;
            size_t delta_max = w->delta_binary ? BRS_DELTA_MAX_SIZE_BINARY
                                               : BRS_DELTA_MAX_SIZE;

            if (brs_vfs_context_get() != NULL) {
                static _Atomic int ssh_delta_enabled = -1;
                int cur = atomic_load_explicit(&ssh_delta_enabled, memory_order_relaxed);
                if (cur < 0) {
                    const char *env = getenv("BRS_SSH_DELTA");
                    int val = (env && env[0] != '\0' && strcmp(env, "1") == 0) ? 1 : 0;
                    atomic_store_explicit(&ssh_delta_enabled, val, memory_order_relaxed);
                    cur = val;
                }
                if (!cur)
                    delta_max = 0;
            }

            if (e->size > 0 && e->size <= delta_max) {
                BrsChunkId *old_chunks = NULL;
                uint32_t old_count = 0;

                pthread_mutex_lock(w->index_mtx);
                int found_prev = find_previous_version(w->cache, w->existing_index,
                                                       e->path, &old_chunks, &old_count);
                pthread_mutex_unlock(w->index_mtx);

                if (found_prev == 0) {
                    size_t old_size = 0;
                    uint8_t *old_content = reconstruct_from_chunks_worker(
                        w, old_chunks, old_count, &old_size);

                    if (old_content) {
                        FILE *f = fopen(task->path, "rb");
                        if (f) {
                            uint8_t *new_content = (uint8_t *)malloc(e->size);
                            if (new_content && fread(new_content, 1, e->size, f) == e->size) {
                                if (brs_delta_encode(old_content, old_size,
                                                     new_content, e->size,
                                                     &delta_payload, &delta_len) == 0) {
                                    if (delta_len * BRS_DELTA_MIN_RATIO < e->size) {
                                        use_delta = 1;
                                        e->flags |= BRS_FLAG_DELTA;
                                        free(e->delta_source_chunks);
                                        e->delta_source_chunks = old_chunks;
                                        e->delta_source_count = old_count;
                                        e->delta_source_cap = old_count;
                                        old_chunks = NULL;
                                    } else {
                                        free(delta_payload);
                                        delta_payload = NULL;
                                        delta_len = 0;
                                    }
                                }
                            }
                            free(new_content);
                            fclose(f);
                        }
                        free(old_content);
                    }
                }

                free(old_chunks);
            }

            /* === PROCESAMIENTO NORMAL O DELTA === */
            FILE *f = fopen(task->path, "rb");
            if (!f) {
                free(delta_payload);
                continue;
            }

            brs_streaming_chunker_reset(&sc);

            CreateEmitCtx ec;
            ec.w = w;
            ec.entry_index = task->entry_index;
            ec.err = 0;

            if (use_delta && delta_payload && w->total_input_bytes) {
                atomic_fetch_add_explicit(w->total_input_bytes,
                                          (uint64_t)e->size,
                                          memory_order_relaxed);
            }

            if (use_delta && delta_payload) {
                size_t offset = 0;
                while (offset < delta_len) {
                    if (check_cancel(w->cancel_flag)) {
                        cancelled = 1;
                        break;
                    }

                    size_t chunk_size =
                        (delta_len - offset > w->cfg->chunk_max)
                            ? w->cfg->chunk_max
                            : (delta_len - offset);

                    if (brs_streaming_chunker_feed(&sc,
                                                   delta_payload + offset,
                                                   chunk_size,
                                                   worker_emit_chunk,
                                                   &ec) != 0) {
                        ec.err = 1;
                        break;
                    }

                    offset += chunk_size;
                }

                if (!ec.err && !cancelled) {
                    if (brs_streaming_chunker_finish(&sc, worker_emit_chunk, &ec) != 0)
                        ec.err = 1;
                }

                free(delta_payload);
            } else {
                for (;;) {
                    if (check_cancel(w->cancel_flag)) {
                        cancelled = 1;
                        break;
                    }

                    size_t n = fread(w->thread_read_buf, 1, brs_dyn_read_size, f);
                    if (n == 0)
                        break;

                    if (w->total_input_bytes) {
                        atomic_fetch_add_explicit(w->total_input_bytes,
                                                  (uint64_t)n,
                                                  memory_order_relaxed);
                    }

                    if (brs_streaming_chunker_feed(&sc, w->thread_read_buf, n,
                                                   worker_emit_chunk, &ec) != 0) {
                        ec.err = 1;
                        break;
                    }
                }

                if (!ec.err && !cancelled) {
                    if (brs_streaming_chunker_finish(&sc, worker_emit_chunk, &ec) != 0)
                        ec.err = 1;
                }
            }

            fclose(f);

            if (!ec.err && !cancelled)
                atomic_fetch_add_explicit(w->files_done, 1, memory_order_relaxed);

            if (cancelled)
                break;
        }
    }

    /* Sentinel */
    BrsChunkItem sentinel;
    memset(&sentinel, 0, sizeof sentinel);
    sentinel.buffer_slot = BRS_SENTINEL_SLOT;
    sentinel.entry_index = UINT32_MAX;
    brs_spsc_push(w->queue, &sentinel);

    brs_pack_cache_flush();
    brs_streaming_chunker_free(&sc);
    return NULL;
}

/* Agrupación de hardlinks por (dev,ino) con sort, sin hashmap. */
typedef struct {
    uint64_t dev, ino, idx;
} InodeRef;

static int inode_ref_cmp(const void *a, const void *b)
{
    const InodeRef *x = (const InodeRef *)a;
    const InodeRef *y = (const InodeRef *)b;

    if (x->dev != y->dev)
        return x->dev < y->dev ? -1 : 1;
    if (x->ino != y->ino)
        return x->ino < y->ino ? -1 : 1;
    return 0;
}

static void group_hardlinks(BrsManifestList *entries)
{
    for (uint64_t i = 0; i < entries->count; ++i) {
        BrsManifestEntry *e = &entries->items[i];
        if (e->type == BRS_FILETYPE_FILE) {
            e->flags = (uint16_t)(e->flags & ~BRS_FLAG_HARDLINK);
            free(e->hardlink_to);
            e->hardlink_to = NULL;
        }
    }

    InodeRef *refs = (InodeRef *)malloc(
        (entries->count ? (size_t)entries->count : 1) * sizeof(InodeRef));
    if (!refs)
        return;

    size_t nrefs = 0;
    for (uint64_t i = 0; i < entries->count; ++i) {
        const BrsManifestEntry *e = &entries->items[i];
        if (e->type == BRS_FILETYPE_FILE && e->ino != 0) {
            refs[nrefs].dev = e->dev;
            refs[nrefs].ino = e->ino;
            refs[nrefs].idx = i;
            nrefs++;
        }
    }

    if (nrefs > 1)
        qsort(refs, nrefs, sizeof *refs, inode_ref_cmp);

    size_t i = 0;
    while (i < nrefs) {
        size_t j = i + 1;
        while (j < nrefs &&
               refs[j].dev == refs[i].dev &&
               refs[j].ino == refs[i].ino) {
            j++;
        }

        if (j - i >= 2) {
            size_t canonical = i;
            for (size_t k = i + 1; k < j; ++k) {
                if (strcmp(entries->items[refs[k].idx].path,
                           entries->items[refs[canonical].idx].path) < 0) {
                    canonical = k;
                }
            }

            const char *canon_path = entries->items[refs[canonical].idx].path;

            for (size_t k = i; k < j; ++k) {
                if (k == canonical)
                    continue;

                BrsManifestEntry *e = &entries->items[refs[k].idx];
                e->flags |= BRS_FLAG_HARDLINK;
                free(e->hardlink_to);
                e->hardlink_to = strdup(canon_path);
                free(e->chunks);
                e->chunks = NULL;
                e->chunk_count = 0;
                e->chunk_cap = 0;
            }
        }

        i = j;
    }

    free(refs);
}

static int split_sources(const char *spec, char ***out, size_t *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (!spec || spec[0] == '\0')
        return -1;

    if (!strchr(spec, ',')) {
        char **arr = (char **)calloc(1, sizeof *arr);
        if (!arr)
            return -1;

        arr[0] = strdup(spec);
        if (!arr[0]) {
            free(arr);
            return -1;
        }

        *out = arr;
        *out_count = 1;
        return 0;
    }

    char **arr = NULL;
    size_t count = 0, cap = 0;
    const char *p = spec;

    for (;;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        while (len > 0 && (p[0] == ' ' || p[0] == '\t')) {
            p++;
            len--;
        }
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
            len--;

        if (len > 0) {
            if (count == cap) {
                size_t ncap = cap ? cap * 2 : 4;
                char **na = (char **)realloc(arr, ncap * sizeof *na);
                if (!na)
                    goto fail;
                arr = na;
                cap = ncap;
            }

            arr[count] = (char *)malloc(len + 1);
            if (!arr[count])
                goto fail;

            memcpy(arr[count], p, len);
            arr[count][len] = '\0';
            count++;
        }

        if (!comma)
            break;

        p = comma + 1;
    }

    if (count == 0)
        goto fail;

    *out = arr;
    *out_count = count;
    return 0;

fail:
    for (size_t i = 0; i < count; ++i)
        free(arr[i]);
    free(arr);
    return -1;
}

static int brs_repo_create_impl(const char *repo_path, const char *source_spec,
                                const char *label, int delta_binary,
                                int compression_override, int zstd_level_override,
                                BrsProgressCallback cb, void *cb_user,
                                _Atomic int *cancel_flag)
{
    if (!repo_path || !source_spec)
        return 1;

    BrsRepoLock repo_lock;
   brs_repo_lock_init(&repo_lock);
    int is_remote = (strncmp(repo_path, "ssh://", 6) == 0);
    
    if (!is_remote) {
        if (brs_repo_lock_acquire(&repo_lock, repo_path, "create") != 0) {
            return 1;
        }
    }
    struct timespec create_t0;
    clock_gettime(CLOCK_MONOTONIC, &create_t0);

    _Atomic uint64_t total_input_bytes = 0;
    _Atomic uint64_t total_output_bytes = 0;

    BrsVfs *vfs = brs_vfs_context_get();
    int owns_vfs = 0;

    if (vfs && !brs_uri_is_remote(repo_path)) {
        brs_vfs_context_clear();
        vfs = NULL;
    }

    if (!vfs && brs_uri_is_remote(repo_path)) {
        vfs = brs_vfs_open(repo_path, 0);
        if (!vfs) {
            fprintf(stderr, "cannot connect to remote repo: %s\n", repo_path);
            brs_repo_lock_release(&repo_lock);
            return 1;
        }
        brs_vfs_context_set(vfs, repo_path);
        owns_vfs = 1;
    }

    int rc = 1;
    char **sources = NULL;
    size_t num_sources = 0;
    char **canon = NULL;
    char **parents = NULL;
    BrsRepoConfig cfg;
    BrsSecureKey key;
    int key_valid = 0;
    memset(&cfg, 0, sizeof(cfg));
    memset(&key, 0, sizeof(key));
    BrsSessionCompression session_comp = {1, 0, 3};
    BrsManifestList entries;
    char **entry_parents = NULL;
    BrsIndexMap existing, new_index;
    int existing_init = 0, new_init = 0;
    BrsCacheMap cache;
    int cache_init = 0;
    BrsBloomSet blooms;
    int blooms_init = 0;
    BrsU64Set loaded_segs;
    int loaded_segs_init = 0;
    BrsFileTask *tasks = NULL;
    size_t task_count = 0;
    pthread_mutex_t index_mtx = PTHREAD_MUTEX_INITIALIZER;
    BrsBufferPool pool;
    int pool_init = 0;
    BrsSpscQueue *queues = NULL;
    pthread_t threads[BRS_NUM_WORKERS];
    int threads_started = 0;
    atomic_size_t next_task;
    _Atomic uint64_t files_done;
    BrsFastCDC cdc;
    BrsPackWriter writer;
    int writer_open = 0;
    uint64_t cache_hits = 0, cache_misses = 0;
    uint8_t *read_pool = NULL;

    memset(&entries, 0, sizeof entries);
    brs_repo_config_default(&cfg);

    if (split_sources(source_spec, &sources, &num_sources) != 0) {
        fprintf(stderr, "no source directories specified\n");
        goto cleanup;
    }

    canon = (char **)calloc(num_sources, sizeof(char *));
    parents = (char **)calloc(num_sources, sizeof(char *));
    if (!canon || !parents)
        goto cleanup;

    for (size_t s = 0; s < num_sources; ++s) {
        struct stat src_st;
        int stat_ok = (stat(sources[s], &src_st) == 0 && S_ISDIR(src_st.st_mode));
        char *rp = NULL;

        if (stat_ok) {
            rp = realpath(sources[s], NULL);
        }

        if (!rp) {
            rp = strdup(sources[s]);
            if (!rp) {
                fprintf(stderr, "cannot allocate source path: '%s'\n", sources[s]);
                goto cleanup;
            }
        }

        canon[s] = rp;

        const char *slash = strrchr(canon[s], '/');
        size_t plen = slash ? (size_t)(slash - canon[s]) : 0;

        if (plen == 0) {
            parents[s] = strdup("/");
        } else {
            parents[s] = (char *)malloc(plen + 1);
            if (parents[s]) {
                memcpy(parents[s], canon[s], plen);
                parents[s][plen] = '\0';
            }
        }

                if (!parents[s])
            goto cleanup;
 
    }

    
    vfs = brs_vfs_context_get(); 
    owns_vfs = 0;

    if (!vfs && strncmp(repo_path, "ssh://", 6) == 0) {
        vfs = brs_vfs_open(repo_path, 0);
        if (!vfs) {
            fprintf(stderr, "cannot connect to remote repo: %s\n", repo_path);
            goto cleanup;
        }
        brs_vfs_context_set(vfs, repo_path);
        owns_vfs = 1;
    }
    
      
            if (brs_load_config(repo_path, &cfg) != 0) {
        fprintf(stderr, "cannot load repo config\n");
        if (owns_vfs || strncmp(repo_path, "ssh://", 6) == 0) {
            brs_vfs_context_clear();
            if (vfs) {
                brs_vfs_close(vfs);
                vfs = NULL; // CORRECCIÓN: Anular puntero para evitar doble liberación
            }
                owns_vfs = 0; 
        }
        goto cleanup;
    }

    if (cfg.encrypted) {
        if (derive_repo_key(&cfg, &key) != 0) {
            if (owns_vfs || strncmp(repo_path, "ssh://", 6) == 0) {
                brs_vfs_context_clear();
                if (vfs) {
                    brs_vfs_close(vfs);
                    vfs = NULL; // CORRECCIÓN: Anular puntero para evitar doble liberación
                }
                    owns_vfs = 0; 
            }
            goto cleanup;
        }
        key_valid = 1;
    }

    session_comp = brs_resolve_session_compression(&cfg,
                                                   compression_override,
                                                   zstd_level_override);

    {
        const char *subdirs[] = {"snapshots", "packs", "index", "tmp"};
        for (size_t i = 0; i < 4; ++i) {
            char dir_path[BRS_PATH_MAX];
            if (brs_path_join(dir_path, sizeof dir_path, repo_path, subdirs[i]) == 0) {
                if (!brs_path_exists(dir_path)) {
                    if (brs_mkdir_p(dir_path) != 0) {
                        fprintf(stderr, "error: cannot create directory: %s\n", dir_path);
                        rc = 1;
                        goto cleanup;
                    }
                }
            }
        }
    }

    if (check_cancel(cancel_flag)) {
        printf("cancelled\n");
        rc = 2;
        goto cleanup;
    }


    {
        char tmp_dir[BRS_PATH_MAX];
        if (brs_path_join(tmp_dir, sizeof tmp_dir, repo_path, "tmp") == 0)
            brs_remove_files_with_suffix(tmp_dir, ".tmp");
    }

    /* ---- scan ---- */
    report_progress(cb, cb_user, "scan", 0, 0);
    memset(&entries, 0, sizeof entries);
    entry_parents = (char **)calloc(1, sizeof(char *));
    if (!entry_parents)
        goto cleanup;

    for (size_t s = 0; s < num_sources; ++s) {
        uint64_t before = entries.count;
        if (brs_scan_source(canon[s], &entries) != 0)
            goto cleanup;

        char **np = (char **)realloc(entry_parents, (size_t)entries.count * sizeof(char *));
        if (!np && entries.count > 0)
            goto cleanup;
        entry_parents = np;

        for (uint64_t i = before; i < entries.count; ++i)
            entry_parents[i] = parents[s];
    }

    group_hardlinks(&entries);

    /* ---- index existente + bloom ---- */
    if (brs_index_map_init(&existing, 1024) != 0)
        goto cleanup;
    existing_init = 1;

    g_brs_ssh_full_index = 0;

    if (brs_vfs_context_get() != NULL) {
        fprintf(stderr, "Optimización SSH: cargando índice remoto completo en RAM...\n");
                int loaded = ssh_load_all_indexes(repo_path, &existing);
        if (loaded > 0) {
            atomic_store_explicit(&g_brs_ssh_full_index, 1, memory_order_relaxed);

            if (brs_verify_index_packs_exist(repo_path, &existing) != 0) {
                fprintf(stderr, "error: remote index references missing packs\n");
                goto cleanup;
            }
        } else {
            fprintf(stderr,
                    "aviso: no se cargaron segmentos de índice remoto; usando blooms\n");
        }        
    }

    if (!g_brs_ssh_full_index) {
        if (brs_load_bloom_set(repo_path, &blooms) == 0) {
            blooms_init = 1;

            char idx_dir_b[BRS_PATH_MAX];
            if (brs_path_join(idx_dir_b, sizeof idx_dir_b, repo_path, "index") == 0) {
                size_t w = 0;
                for (size_t r = 0; r < blooms.count; ++r) {
                    char iname[40], ipath[BRS_PATH_MAX];
                    snprintf(iname, sizeof iname, "%llu.idx",
                             (unsigned long long)blooms.segs[r].seg_id);
                    if (brs_path_join(ipath, sizeof ipath, idx_dir_b, iname) == 0 &&
                        brs_path_exists(ipath)) {
                        if (w != r)
                            blooms.segs[w] = blooms.segs[r];
                        w++;
                    } else {
                        brs_bloom_free(&blooms.segs[r].bloom);
                    }
                }
                blooms.count = w;
            }
        }
    }

    if (brs_u64_set_init(&loaded_segs, 64) == 0)
        loaded_segs_init = 1;

    if (brs_index_map_init(&new_index, 1024) != 0)
        goto cleanup;
    new_init = 1;

    if (brs_cache_map_init(&cache, 1024) != 0)
        goto cleanup;
    cache_init = 1;

    (void)brs_load_file_cache(repo_path, &cache);

    /* ---- tareas ---- */
    tasks = (BrsFileTask *)calloc(entries.count ? (size_t)entries.count : 1,
                                  sizeof(BrsFileTask));
    if (!tasks)
        goto cleanup;

    for (uint64_t i = 0; i < entries.count; ++i) {
        BrsManifestEntry *e = &entries.items[i];
        if (e->type != BRS_FILETYPE_FILE || e->size == 0)
            continue;
        if (e->flags & BRS_FLAG_HARDLINK)
            continue;

        const BrsFileCacheEntry *ce = brs_cache_map_get(&cache, e->path);
        if (ce) {
            int meta_ok = (ce->dev == e->dev) &&
                          (ce->ino == e->ino) &&
                          (ce->size == e->size) &&
                          (ce->mtime_ns == e->mtime_ns) &&
                          (ce->ctime_ns == e->ctime_ns) &&
                          (ce->mode == e->mode);

            if (meta_ok) {
                int all_present = 1;
                for (uint32_t c = 0; c < ce->chunk_count; ++c) {
                    if (!lazy_get_main(repo_path, &existing,
                                       blooms_init ? &blooms : NULL,
                                       &ce->chunks[c])) {
                        all_present = 0;
                        break;
                    }
                }

                if (all_present) {
                    if (copy_chunk_array(&e->chunks, &e->chunk_count,
                                         &e->chunk_cap,
                                         ce->chunks, ce->chunk_count) == 0) {
                        cache_hits++;
                        continue;
                    }
                }
            }

            {
                size_t dmax = delta_binary ? BRS_DELTA_MAX_SIZE_BINARY : BRS_DELTA_MAX_SIZE;
                if (e->size > 0 && e->size <= dmax) {
                    for (uint32_t c = 0; c < ce->chunk_count; ++c) {
                        (void)lazy_get_main(repo_path, &existing,
                                            blooms_init ? &blooms : NULL,
                                            &ce->chunks[c]);
                    }
                }
            }
        }

        cache_misses++;

        char full[BRS_PATH_MAX];
        if (brs_path_join(full, sizeof full, entry_parents[i], e->path) != 0)
            continue;

        BrsFileTask *t = &tasks[task_count];
        t->entry_index = (uint32_t)i;
        t->path = strdup(full);
        t->size = e->size;
        if (!t->path)
            continue;
        task_count++;
    }

    if (task_count > 0) {
        BrsFastCDCConfig cdc_cfg;
        cdc_cfg.min_size = cfg.chunk_min;   // 16 KB
        cdc_cfg.avg_size = cfg.chunk_avg;   // 64 KB
        cdc_cfg.max_size = cfg.chunk_max;   // 256 KB
        brs_fastcdc_init(&cdc, cdc_cfg);

        if (brs_buffer_pool_init(&pool,
                                 BRS_SPSC_CAP * BRS_NUM_WORKERS + BRS_NUM_WORKERS + 4,
                                 cfg.chunk_max) != 0) {
            goto cleanup;
        }
        pool_init = 1;

        queues = (BrsSpscQueue *)calloc(BRS_NUM_WORKERS, sizeof(BrsSpscQueue));
        if (!queues)
            goto cleanup;

        for (size_t w = 0; w < BRS_NUM_WORKERS; ++w)
            brs_spsc_init(&queues[w]);

        atomic_init(&next_task, 0);
        atomic_init(&files_done, 0);

        /* ====================================================================
         * FASE 2 MUTACIÓN 2: Reserva ÚNICA del pool de buffers de lectura (Cold-Path).
         * Pool total = aligned_read_size * BRS_NUM_WORKERS
         * 
         * Usamos posix_memalign para garantizar alineación de 64 bytes (cacheline),
         * evitando SIGBUS en ARM y mejorando el rendimiento SIMD.
         * ================================================================== */
        size_t aligned_read_size = ((brs_dyn_read_size + 63) / 64) * 64;
        size_t read_pool_size = aligned_read_size * BRS_NUM_WORKERS;
        
        if (posix_memalign((void**)&read_pool, 64, read_pool_size) != 0) {
            fprintf(stderr, "error: cannot allocate aligned read pool (%zu bytes)\n",
                    read_pool_size);
            read_pool = NULL;
            goto cleanup;
        }

        CreateWorkerCtx wctx[BRS_NUM_WORKERS];
        for (size_t w = 0; w < BRS_NUM_WORKERS; ++w) {
            wctx[w].delta_binary = delta_binary;
            wctx[w].cfg = &cfg;
            wctx[w].entries = &entries;
            wctx[w].entry_parents = entry_parents;
            wctx[w].index_mtx = &index_mtx;
            wctx[w].existing_index = &existing;
            wctx[w].new_index = &new_index;
            wctx[w].pool = &pool;
            wctx[w].queue = &queues[w];
            wctx[w].cdc = &cdc;
            wctx[w].cancel_flag = cancel_flag;
            wctx[w].next_task = &next_task;
            wctx[w].files_done = &files_done;
            wctx[w].tasks = tasks;
            wctx[w].task_count = task_count;
            wctx[w].repo_path = repo_path;
            wctx[w].cache = &cache;
            wctx[w].key = cfg.encrypted ? &key : NULL;
            wctx[w].blooms = blooms_init ? &blooms : NULL;
            wctx[w].loaded_segs = loaded_segs_init ? &loaded_segs : NULL;
            wctx[w].total_input_bytes = &total_input_bytes;
            wctx[w].total_output_bytes = &total_output_bytes;

            /* ==============================================================
             * FASE 2 MUTACIÓN 3: Segmentación aritmética del pool continuo.
             * worker[w] obtiene el segmento w * aligned_read_size.
             * Cada segmento está alineado a 64 bytes para evitar desalineación.
             * ============================================================ */
            wctx[w].thread_read_buf = read_pool + (w * aligned_read_size);

            if (pthread_create(&threads[w], NULL, create_worker, &wctx[w]) != 0) {
                for (size_t k = w; k < BRS_NUM_WORKERS; ++k) {
                    BrsChunkItem s;
                    memset(&s, 0, sizeof s);
                    s.buffer_slot = BRS_SENTINEL_SLOT;
                    brs_spsc_push(&queues[k], &s);
                }
                threads_started = (int)w;
                goto join_and_fail;
            }
            threads_started++;
        }

        /* ---- escritor: round-robin sobre las colas ---- */
        BrsBuffer comp_buf;
        brs_buffer_init(&comp_buf);

        int *worker_done = (int *)calloc(BRS_NUM_WORKERS, sizeof(int));
        if (!worker_done) {
            goto join_and_fail;
        }

        size_t done_count = 0, next_q = 0;
        int cancelled = 0;
        uint64_t last_reported = 0;
        size_t new_chunks_count = 0;

        while (done_count < BRS_NUM_WORKERS) {
            if (!cancelled && check_cancel(cancel_flag))
                cancelled = 1;

            if (!cancelled && cb) {
                uint64_t done = atomic_load_explicit(&files_done, memory_order_relaxed);
                if (done != last_reported) {
                    last_reported = done;
                    report_progress(cb, cb_user, "dedup", done, task_count);
                }
            }

            int any_popped = 0;

            for (size_t i = 0; i < BRS_NUM_WORKERS; ++i) {
                size_t idx = (next_q + i) % BRS_NUM_WORKERS;
                if (worker_done[idx])
                    continue;

                BrsChunkItem item;
                if (brs_spsc_pop_notify(&queues[idx], &item) == 0) {
                    any_popped = 1;

                    if (item.buffer_slot == BRS_SENTINEL_SLOT) {
                        worker_done[idx] = 1;
                        done_count++;
                    } else if (cancelled) {
                        brs_buffer_pool_release(&pool, item.buffer_slot);
                    } else {
                        new_chunks_count++;

                        if (!writer_open) {
                            if (brs_pack_writer_init(&writer, repo_path, 0) != 0) {
                                fprintf(stderr, "error: cannot open pack\n");
                                brs_buffer_pool_release(&pool, item.buffer_slot);
                                free(worker_done);
                                goto join_and_fail;
                            }
                            writer_open = 1;
                            writer.index_map = &new_index;
                        }

                        const uint8_t *cdata = brs_buffer_pool_get(&pool, item.buffer_slot);
                        const uint8_t *to_write = cdata;
                        size_t write_len = item.size;
                        uint8_t flags = 0;

                        int try_compress = session_comp.use_compression &&
                                           item.size > 0 &&
                                           !brs_is_chunk_incompressible(cdata, item.size);

                        if (try_compress) {
                            if (session_comp.use_zstd) {
                                size_t bound = brs_zstd_compress_bound(item.size);
                                if (comp_buf.capacity < bound) {
                                    if (brs_buffer_resize(&comp_buf, bound) != 0) {
                                        fprintf(stderr, "error: cannot allocate compression buffer\n");
                                        brs_buffer_pool_release(&pool, item.buffer_slot);
                                        free(worker_done);
                                        goto join_and_fail;
                                    }
                                }

                                size_t comp_len = brs_zstd_compress(
                                    cdata, item.size,
                                    comp_buf.data, comp_buf.capacity,
                                    session_comp.zstd_level);

                                if (comp_len > 0 && comp_len < item.size) {
                                    to_write = comp_buf.data;
                                    write_len = comp_len;
                                    flags |= BRS_CHUNK_FLAG_COMPRESSED | BRS_CHUNK_FLAG_ZSTD;
                                }
                            } else {
                                size_t bound = brs_lz4_compress_bound(item.size);
                                if (comp_buf.capacity < bound) {
                                    if (brs_buffer_resize(&comp_buf, bound) != 0) {
                                        fprintf(stderr, "error: cannot allocate compression buffer\n");
                                        brs_buffer_pool_release(&pool, item.buffer_slot);
                                        free(worker_done);
                                        goto join_and_fail;
                                    }
                                }

                                size_t comp_len = brs_lz4_compress(
                                    cdata, item.size,
                                    comp_buf.data, comp_buf.capacity);

                                if (comp_len > 0 && comp_len < item.size) {
                                    to_write = comp_buf.data;
                                    write_len = comp_len;
                                    flags |= BRS_CHUNK_FLAG_COMPRESSED;
                                }
                            }
                        }

                        if (cfg.encrypted) {
                            BrsBuffer enc;
                            brs_buffer_init(&enc);

                            int enc_ok = brs_encrypt_buffer(&key, cfg.cipher_algo,
                                                            to_write, write_len, &enc);

                            if (enc_ok == 0) {
                                if (brs_pack_writer_add_chunk(&writer, &item.id,
                                                              enc.data, enc.size,
                                                              item.size,
                                                              flags | BRS_CHUNK_FLAG_ENCRYPTED) != 0) {
                                    fprintf(stderr, "error: pack write failed (encrypted chunk)\n");
                                    brs_buffer_wipe_free(&enc);
                                    brs_buffer_pool_release(&pool, item.buffer_slot);
                                    free(worker_done);
                                    goto join_and_fail;
                                }

                                atomic_fetch_add_explicit(&total_output_bytes,
                                                          (uint64_t)enc.size,
                                                          memory_order_relaxed);
                            } else {
                                fprintf(stderr, "error: chunk encryption failed\n");
                                brs_buffer_pool_release(&pool, item.buffer_slot);
                                free(worker_done);
                                goto join_and_fail;
                            }

                            brs_buffer_wipe_free(&enc);
                        } else {
                            if (brs_pack_writer_add_chunk(&writer, &item.id,
                                                          to_write, write_len,
                                                          item.size, flags) != 0) {
                                fprintf(stderr, "error: pack write failed (network I/O)\n");
                                brs_buffer_pool_release(&pool, item.buffer_slot);
                                free(worker_done);
                                goto join_and_fail;
                            }

                            atomic_fetch_add_explicit(&total_output_bytes,
                                                      (uint64_t)write_len,
                                                      memory_order_relaxed);
                        }

                        brs_buffer_pool_release(&pool, item.buffer_slot);
                    }

                    next_q = (idx + 1) % BRS_NUM_WORKERS;
                    break;
                }
            }

            if (!any_popped) {
                struct timespec ts = {0, 100000};
                nanosleep(&ts, NULL);
            }
        }

        free(worker_done);

        /* ====================================================================
         * FASE 2 MUTACIÓN 4: Sincronización de hilos y liberación ÚNICA del pool.
         * ================================================================== */
        for (int w = 0; w < threads_started; ++w)
            pthread_join(threads[w], NULL);
        threads_started = 0;

        if (read_pool) {
            free(read_pool);
            read_pool = NULL;
        }

        if (check_cancel(cancel_flag)) {
            printf("cancelled\n");
            rc = 2;
            brs_buffer_free(&comp_buf);
            goto cleanup;
        }

        if (writer_open) {
            if (new_chunks_count == 0) {
                brs_pack_writer_abort(&writer);
                writer_open = 0;
            } else {
                report_progress(cb, cb_user, "pack", 0, 0);

                BrsPackEntry *pe = NULL;
                uint32_t pc = 0;
                uint64_t pid = 0;

                if (brs_pack_writer_finalize(&writer, &pe, &pc, &pid) == 0) {
                    writer_open = 0;
                        pthread_mutex_lock(&index_mtx); 


                    for (uint32_t i = 0; i < pc; ++i) {
                        BrsChunkLocation loc;
                        loc.pack_id = pid;
                        loc.offset = pe[i].offset;
                        loc.comp_size = pe[i].comp_size;
                        loc.uncomp_size = pe[i].uncomp_size;
                        loc.flags = pe[i].flags;
                        brs_index_map_put(&new_index, &pe[i].chunk_id, &loc);
                    }
                            pthread_mutex_unlock(&index_mtx); 


                        if (brs_index_map_count(&new_index) > 0) {
    if (brs_write_index_segment(repo_path, pid, &new_index) != 0) {
        fprintf(stderr,
                "error: cannot write index segment for pack %llu\n",
                (unsigned long long)pid);
        free(pe);
        goto cleanup;
    }

    /* NUEVO: verificar que todos los packs referenciados existen */
    if (brs_verify_pack_existence(repo_path, &new_index) != 0) {
        fprintf(stderr, "error: missing pack files after backup\n");
        free(pe);
        goto cleanup;
    }
}

brs_write_open_pack_id(repo_path, pid);
free(pe);

                } else {
                    fprintf(stderr, "error: cannot finalize pack — aborting create\n");
                    goto cleanup;
                }
            }
        }

        brs_buffer_free(&comp_buf);
        goto after_pipeline;

join_and_fail:
        for (int w = 0; w < threads_started; ++w)
            pthread_join(threads[w], NULL);
        threads_started = 0;
        if (writer_open)
            brs_pack_writer_abort(&writer);
        brs_buffer_free(&comp_buf);
        if (read_pool) {
            free(read_pool);
            read_pool = NULL;
        }
        goto cleanup;

after_pipeline:
        ;
    }

    /* ---- actualizar cache ---- */
    for (uint64_t i = 0; i < entries.count; ++i) {
        const BrsManifestEntry *e = &entries.items[i];
        if (e->type != BRS_FILETYPE_FILE)
            continue;
        if (e->flags & BRS_FLAG_HARDLINK)
            continue;
        if (e->flags & BRS_FLAG_DELTA)
            continue;

        BrsFileCacheEntry *ce = brs_cache_map_touch(&cache, e->path);
        if (!ce)
            continue;

        ce->dev = e->dev;
        ce->ino = e->ino;
        ce->size = e->size;
        ce->mtime_ns = e->mtime_ns;
        ce->ctime_ns = e->ctime_ns;
        ce->mode = e->mode;

        copy_chunk_array(&ce->chunks, &ce->chunk_count, &ce->chunk_cap,
                         e->chunks, e->chunk_count);
    }

    brs_save_file_cache(repo_path, &cache);

    /* ---- manifiesto ---- */
    {
        char snap_path[BRS_PATH_MAX];
        if (brs_write_snapshot_manifest(repo_path, &cfg, canon[0],
                                        entries.items, entries.count,
                                        cfg.encrypted ? &key : NULL,
                                        label,
                                        snap_path, sizeof snap_path) != 0) {
            fprintf(stderr, "cannot write snapshot manifest\n");
            goto cleanup;
        }

        printf("created snapshot: %s\n", snap_path);
        printf("entries: %llu\n", (unsigned long long)entries.count);
        printf("cache: %llu hits, %llu misses\n",
               (unsigned long long)cache_hits,
               (unsigned long long)cache_misses);
    }

    /* === RECALCULAR STATS DEL SNAPSHOT === */
    {
        uint64_t snap_input_bytes = 0;
        for (uint64_t i = 0; i < entries.count; ++i) {
            const BrsManifestEntry *e = &entries.items[i];
            if (e->type == BRS_FILETYPE_FILE &&
                !(e->flags & BRS_FLAG_HARDLINK)) {
                snap_input_bytes += e->size;
            }
        }

        uint64_t snap_output_bytes = 0;
        BrsIndexMap stats_seen;

        if (brs_index_map_init(&stats_seen, 1024) == 0) {
            for (uint64_t i = 0; i < entries.count; ++i) {
                const BrsManifestEntry *e = &entries.items[i];
                if (e->type != BRS_FILETYPE_FILE)
                    continue;
                if (e->flags & BRS_FLAG_HARDLINK)
                    continue;

                for (uint32_t c = 0; c < e->chunk_count; ++c) {
                    const BrsChunkId *cid = &e->chunks[c];

                    if (brs_index_map_get(&stats_seen, cid) != NULL)
                        continue;

                    BrsChunkLocation dummy;
                    memset(&dummy, 0, sizeof dummy);
                    (void)brs_index_map_put(&stats_seen, cid, &dummy);

                    const BrsChunkLocation *loc = NULL;
                    loc = brs_index_map_get(&new_index, cid);
                    if (!loc)
                        loc = brs_index_map_get(&existing, cid);
                    if (!loc) {
                        loc = lazy_get_main(repo_path,
                                            &existing,
                                            blooms_init ? &blooms : NULL,
                                            cid);
                    }

                    if (loc)
                        snap_output_bytes += loc->comp_size;
                }
            }

            brs_index_map_free(&stats_seen);
        } else {
            snap_output_bytes = atomic_load_explicit(&total_output_bytes,
                                                     memory_order_relaxed);
        }

        atomic_store_explicit(&total_input_bytes,
                              snap_input_bytes,
                              memory_order_relaxed);
        atomic_store_explicit(&total_output_bytes,
                              snap_output_bytes,
                              memory_order_relaxed);
    }

    rc = 0;

cleanup:
    brs_pack_cache_flush();

    if (threads_started) {
        for (int w = 0; w < threads_started; ++w)
            pthread_join(threads[w], NULL);
    }

    if (writer_open)
        brs_pack_writer_abort(&writer);

    if (queues) {
        for (size_t w = 0; w < BRS_NUM_WORKERS; ++w)
            brs_spsc_free(&queues[w]);
        free(queues);
    }

    if (pool_init)
        brs_buffer_pool_free(&pool);

    if (read_pool) {
        free(read_pool);
        read_pool = NULL;
    }

    for (size_t i = 0; i < task_count; ++i)
        free(tasks[i].path);
    free(tasks);

    if (cache_init)
        brs_cache_map_free(&cache);
    if (new_init)
        brs_index_map_free(&new_index);
    if (existing_init)
        brs_index_map_free(&existing);
    if (blooms_init)
        brs_bloom_set_free(&blooms);
    if (loaded_segs_init)
        brs_u64_set_free(&loaded_segs);

    free(entry_parents);
    brs_manifest_list_free(&entries);

    if (canon) {
        for (size_t s = 0; s < num_sources; ++s)
            free(canon[s]);
        free(canon);
    }

    if (parents) {
        for (size_t s = 0; s < num_sources; ++s)
            free(parents[s]);
        free(parents);
    }

    if (sources) {
        for (size_t s = 0; s < num_sources; ++s)
            free(sources[s]);
        free(sources);
    }

    if (key_valid)
        brs_secure_key_wipe(&key);

    pthread_mutex_destroy(&index_mtx);

    /* === STATS === */
    {
        struct timespec create_t1;
        clock_gettime(CLOCK_MONOTONIC, &create_t1);

        double elapsed = (double)(create_t1.tv_sec - create_t0.tv_sec) +
                         (double)(create_t1.tv_nsec - create_t0.tv_nsec) / 1e9;

        if (elapsed < 0.0)
            elapsed = 0.0;

        uint64_t in_bytes = atomic_load_explicit(&total_input_bytes,
                                                 memory_order_relaxed);
        uint64_t out_bytes = atomic_load_explicit(&total_output_bytes,
                                                  memory_order_relaxed);

        double in_mb = (double)in_bytes / (1024.0 * 1024.0);
        double out_mb = (double)out_bytes / (1024.0 * 1024.0);

        double ratio = 0.0;
        if (in_bytes > 0) {
            ratio = (1.0 - ((double)out_bytes / (double)in_bytes)) * 100.0;
            if (ratio < 0.0)
                ratio = 0.0;
        }

        double throughput = 0.0;
        if (elapsed > 0.0)
            throughput = in_mb / elapsed;

        printf("[STATS] Raw Input Data: %.2f MB\n", in_mb);
        printf("[STATS] Packed Output Data: %.2f MB\n", out_mb);
        printf("[STATS] Compression Ratio: %.2f%%\n", ratio);
        printf("[STATS] Processing Time: %.3f s\n", elapsed);
        printf("[STATS] Real Throughput: %.2f MB/s\n", throughput);
        fflush(stdout);
    }

     if (strncmp(repo_path, "ssh://", 6) != 0) {
        brs_repo_lock_release(&repo_lock);
    }

//        if (owns_vfs && rc == 0) { 
//        brs_vfs_context_clear();
//        if (vfs != NULL) {
//            brs_vfs_close(vfs);
//            vfs = NULL;
//        }
//    }
    return rc;
}

