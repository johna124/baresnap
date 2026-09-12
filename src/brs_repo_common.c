#include "brs_repo_internal.h"
#include "brs_vfs.h"
#include "brs_vfs_context.h"

/* ============================================================================
* Helpers
* ==========================================================================*/
void report_progress(BrsProgressCallback cb, void *user,
                     const char *phase, uint64_t cur, uint64_t total)
{
    if (cb) cb(user, phase, cur, total);
}

int check_cancel(_Atomic int *flag)
{
    return flag && atomic_load_explicit(flag, memory_order_relaxed);
}

int derive_repo_key(const BrsRepoConfig *cfg, BrsSecureKey *key)
{
    char pass[BRS_PASSPHRASE_MAX + 1];
    if (brs_read_passphrase(pass, sizeof pass, "Passphrase: ") != 0) return -1;
    int rc = brs_derive_key(cfg, pass, key);
    brs_secure_wipe(pass, sizeof pass);
    if (rc != 0) fprintf(stderr, "error: incorrect passphrase\n");
    return rc;
}

int resolve_snap_path(const char *repo, const char *arg,
                      char *out, size_t out_size)
{
    if (brs_path_exists(arg)) {
        size_t n = strlen(arg);
        if (n >= out_size) return -1;
        memcpy(out, arg, n + 1);
        return 0;
    }
    char snaps[BRS_PATH_MAX];
    if (brs_path_join(snaps, sizeof snaps, repo, "snapshots") != 0) return -1;
    return brs_path_join(out, out_size, snaps, arg);
}

int copy_chunk_array(BrsChunkId **dst, uint32_t *dst_count,
                     uint32_t *dst_cap,
                     const BrsChunkId *src, uint32_t n)
{
    free(*dst);
    *dst = NULL;
    *dst_count = 0;
    *dst_cap = 0;
    if (n == 0) return 0;
    *dst = (BrsChunkId *)malloc((size_t)n * sizeof(BrsChunkId));
    if (!*dst) return -1;
    memcpy(*dst, src, (size_t)n * sizeof(BrsChunkId));
    *dst_count = n;
    *dst_cap = n;
    return 0;
}

/* ============================================================================
* Pack Cache: pack ENTERO en RAM (thread-local)
* ==========================================================================*/
#define PACK_CACHE_SIZE        16
#define PACK_CACHE_MAX_BYTES   (256ull * 1024ull * 1024ull)
#define PACK_READ_MAX_BYTES    (256ull * 1024ull * 1024ull)

typedef struct {
    uint64_t    pack_id;
    BrsVfs     *vfs;
    uint8_t    *data;
    size_t      data_size;
    uint64_t    last_access;
} PackCacheEntry;

static __thread PackCacheEntry tl_pack_cache[PACK_CACHE_SIZE];
static __thread int            tl_pack_cache_count = 0;
static __thread size_t         tl_pack_cache_bytes = 0;
static __thread uint64_t       tl_pack_cache_clock = 0;

static void pack_cache_free_entry(int idx)
{
    if (idx < 0 || idx >= tl_pack_cache_count) return;
    tl_pack_cache_bytes -= tl_pack_cache[idx].data_size;
    free(tl_pack_cache[idx].data);
    tl_pack_cache[idx].data = NULL;
    tl_pack_cache[idx].data_size = 0;
    int last = tl_pack_cache_count - 1;
    if (idx != last) {
        tl_pack_cache[idx] = tl_pack_cache[last];
    }
    memset(&tl_pack_cache[last], 0, sizeof(tl_pack_cache[last]));
    tl_pack_cache_count--;
}

static int pack_cache_pick_lru(void)
{
    int victim = 0;
    uint64_t min_access = tl_pack_cache[0].last_access;
    for (int i = 1; i < tl_pack_cache_count; ++i) {
        if (tl_pack_cache[i].last_access < min_access) {
            min_access = tl_pack_cache[i].last_access;
            victim = i;
        }
    }
    return victim;
}

static PackCacheEntry *pack_cache_get(uint64_t pack_id, BrsVfs *vfs)
{
    for (int i = 0; i < tl_pack_cache_count; ++i) {
        if (tl_pack_cache[i].pack_id == pack_id &&
            tl_pack_cache[i].vfs == vfs) {
            tl_pack_cache[i].last_access = ++tl_pack_cache_clock;
            return &tl_pack_cache[i];
        }
    }
    return NULL;
}

static PackCacheEntry *pack_cache_alloc(uint64_t pack_id, BrsVfs *vfs,
                                        uint8_t *data, size_t data_size)
{
    if (!data || data_size == 0 || data_size > PACK_CACHE_MAX_BYTES)
        return NULL;
    while (tl_pack_cache_bytes + data_size > PACK_CACHE_MAX_BYTES &&
           tl_pack_cache_count > 0) {
        pack_cache_free_entry(pack_cache_pick_lru());
    }
    if (tl_pack_cache_bytes + data_size > PACK_CACHE_MAX_BYTES)
        return NULL;
    if (tl_pack_cache_count >= PACK_CACHE_SIZE)
        pack_cache_free_entry(pack_cache_pick_lru());

    PackCacheEntry *e = &tl_pack_cache[tl_pack_cache_count++];
    e->pack_id     = pack_id;
    e->vfs         = vfs;
    e->data        = data;
    e->data_size   = data_size;
    e->last_access = ++tl_pack_cache_clock;
    tl_pack_cache_bytes += data_size;
    return e;
}

void brs_pack_cache_flush(void)
{
    while (tl_pack_cache_count > 0)
        pack_cache_free_entry(0);
    tl_pack_cache_bytes = 0;
    tl_pack_cache_clock = 0;
}

/* ============================================================================
* Helpers de lectura de chunks
* ==========================================================================*/
static int pack_range_valid(uint64_t offset, uint32_t len, size_t size)
{
    return offset <= size && len <= (size_t)(size - offset);
}

static int read_chunk_streaming(BrsVfs *vfs, const char *rel,
                                const BrsChunkLocation *loc,
                                BrsBuffer *out)
{
    BrsVfsFile *f = brs_vfs_fopen(vfs, rel, BRS_VFS_OPEN_READ);
    if (!f)
        return -1;
    if (brs_buffer_resize(out, loc->comp_size) != 0) {
        brs_vfs_fclose(f);
        return -1;
    }
    size_t off = 0;
    while (off < loc->comp_size) {
        ssize_t r = brs_vfs_fread(f,
                                  out->data + off,
                                  loc->comp_size - off,
                                  loc->offset + off);
        if (r < 0) {
            brs_vfs_fclose(f);
            return -1;
        }
        if (r == 0)
            break;
        off += (size_t)r;
    }
    brs_vfs_fclose(f);
    if (off != loc->comp_size)
        return -1;
    return 0;
}

/* ============================================================================
* read_chunk_from_pack
* ==========================================================================*/
int read_chunk_from_pack(const char *repo, const BrsChunkLocation *loc,
                         BrsBuffer *out)
{
    BrsVfs *ctx_vfs = brs_vfs_context_get();
    BrsVfs *vfs     = ctx_vfs;
    int owns_vfs    = 0;

    if (!vfs) {
        vfs = brs_vfs_open(repo, 0);
        if (!vfs) return -1;
        owns_vfs = 1;
    }

    char name[40], rel[BRS_PATH_MAX];
    snprintf(name, sizeof name, "%llu.pack",
             (unsigned long long)loc->pack_id);
    snprintf(rel, sizeof rel, "packs/%s", name);

    /* Leer SOLO el chunk necesario, por offset, tanto en local como SSH */
    int rc = read_chunk_streaming(vfs, rel, loc, out);

    if (owns_vfs)
        brs_vfs_close(vfs);

    return rc;
}

/* ============================================================================
* Helpers para delta encoding
* ==========================================================================*/
uint8_t *reconstruct_from_chunks(const char *repo_path,
                                 const BrsIndexMap *index_map,
                                 const BrsSecureKey *key,
                                 BrsCipherAlgo cipher_algo,
                                 const BrsChunkId *chunks,
                                 uint32_t chunk_count,
                                 size_t *out_size)
{
    if (!out_size) return NULL;
    *out_size = 0;

    size_t total_size = 0;
    for (uint32_t c = 0; c < chunk_count; ++c) {
        const BrsChunkLocation *loc =
            brs_index_map_get(index_map, &chunks[c]);
        if (!loc) {
            fprintf(stderr, "  [RECON-FAIL] chunk[%u/%u] no en index: ",
                    c, chunk_count);
            for (int h = 0; h < 16; ++h)
                fprintf(stderr, "%02x", chunks[c].bytes[h]);
            fprintf(stderr, "\n");
            return NULL;
        }
        total_size += loc->uncomp_size;
    }

    if (total_size == 0) {
        *out_size = 0;
        return (uint8_t *)calloc(1, 1);
    }

    uint8_t *buffer = (uint8_t *)malloc(total_size);
    if (!buffer) return NULL;

    size_t offset = 0;
    BrsBuffer chunk_buf, dec_buf, uncomp_buf;
    brs_buffer_init(&chunk_buf);
    brs_buffer_init(&dec_buf);
    brs_buffer_init(&uncomp_buf);

    for (uint32_t c = 0; c < chunk_count; ++c) {
        const BrsChunkLocation *loc =
            brs_index_map_get(index_map, &chunks[c]);
        if (!loc) { free(buffer); goto fail; }

        if (read_chunk_from_pack(repo_path, loc, &chunk_buf) != 0) {
            free(buffer); goto fail;
        }

        const uint8_t *src = chunk_buf.data;
        size_t src_len = chunk_buf.size;

        if (loc->flags & BRS_CHUNK_FLAG_ENCRYPTED) {
            if (brs_decrypt_buffer(key, cipher_algo, src, src_len, &dec_buf) != 0) {
                free(buffer); goto fail;
            }
            src = dec_buf.data;
            src_len = dec_buf.size;
        }

        const uint8_t *to_write = src;
        size_t write_len = src_len;

        if (loc->flags & BRS_CHUNK_FLAG_COMPRESSED) {
            if (loc->uncomp_size > BRS_CHUNK_UNCOMP_MAX) {
                free(buffer); goto fail;
            }
            if (brs_buffer_resize(&uncomp_buf, loc->uncomp_size) != 0) {
                free(buffer); goto fail;
            }
            size_t dec_len;
            if (loc->flags & BRS_CHUNK_FLAG_ZSTD) {
                dec_len = brs_zstd_decompress(src, src_len,
                                              uncomp_buf.data,
                                              uncomp_buf.size);
            } else {
                dec_len = brs_lz4_decompress(src, src_len,
                                             uncomp_buf.data,
                                             uncomp_buf.size);
            }
            if (dec_len != (size_t)loc->uncomp_size) {
                free(buffer); goto fail;
            }
            to_write = uncomp_buf.data;
            write_len = dec_len;
        }

        if (offset + write_len > total_size) { free(buffer); goto fail; }
        memcpy(buffer + offset, to_write, write_len);
        offset += write_len;
    }

    brs_buffer_free(&chunk_buf);
    brs_buffer_free(&dec_buf);
    brs_buffer_wipe_free(&uncomp_buf);
    *out_size = total_size;
    return buffer;

fail:
    brs_buffer_free(&chunk_buf);
    brs_buffer_free(&dec_buf);
    brs_buffer_wipe_free(&uncomp_buf);
    return NULL;
}

int find_previous_version(const BrsCacheMap *cache,
                          const BrsIndexMap *existing_index,
                          const char *path,
                          BrsChunkId **out_chunks,
                          uint32_t *out_count)
{
    if (!out_chunks || !out_count) return -1;
    *out_chunks = NULL;
    *out_count  = 0;

    const BrsFileCacheEntry *ce = brs_cache_map_get(cache, path);
    if (!ce) {
        return -1;
    }

    for (uint32_t c = 0; c < ce->chunk_count; ++c) {
        if (!brs_index_map_get(existing_index, &ce->chunks[c])) {
            return -1;
        }
    }

    *out_chunks = (BrsChunkId *)malloc(
        (size_t)ce->chunk_count * sizeof(BrsChunkId));
    if (!*out_chunks) return -1;
    memcpy(*out_chunks, ce->chunks,
           (size_t)ce->chunk_count * sizeof(BrsChunkId));
    *out_count = ce->chunk_count;
    return 0;
}
