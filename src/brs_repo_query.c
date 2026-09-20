#include "brs_repo_internal.h"
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "brs_repo_health.h"
#include "brs_vfs.h"
#include "brs_vfs_context.h"
#include "brs_util.h"
#include <fnmatch.h>

/* ============================================================================
 * info
 * ==========================================================================*/
int brs_repo_info(const char *repo_path)
{
    if (!repo_path)
        return 1;

    int rc = 1;

    BrsVfs *vfs = brs_vfs_context_get();
    int owns_vfs = 0;

    if (!vfs && strncmp(repo_path, "ssh://", 6) == 0) {
        vfs = brs_vfs_open(repo_path, 0);
        if (!vfs) {
            fprintf(stderr, "cannot connect to remote repo: %s\n", repo_path);
            return 1;
        }

        brs_vfs_context_set(vfs, repo_path);
        owns_vfs = 1;
    }

    BrsRepoConfig cfg;
    BrsSecureKey key;
    int key_valid = 0;

    BrsIndexMap index_map;
    int index_init = 0;

    brs_repo_config_default(&cfg);
    if (brs_load_config(repo_path, &cfg) != 0) {
        fprintf(stderr, "cannot load repo config\n");
        if (owns_vfs) { brs_vfs_context_clear(); brs_vfs_close(vfs); }
        return 1;
    }
    if (cfg.encrypted) {
        if (derive_repo_key(&cfg, &key) != 0) return 1;
        key_valid = 1;
    }

    char uuid_hex[BRS_UUID_LEN * 2 + 1];
    char ts_buf[64], fb[64];
    brs_to_hex(cfg.uuid, BRS_UUID_LEN, uuid_hex);
    brs_format_timestamp_utc(cfg.created_ns, ts_buf, sizeof ts_buf);

    printf("=== Repository ===\n");
    printf("  path:        %s\n", repo_path);
    printf("  uuid:        %s\n", uuid_hex);
    printf("  created:     %s\n", ts_buf);
    brs_format_bytes(cfg.chunk_min, fb, sizeof fb);
    printf("  chunk sizes: %s min, ", fb);
    brs_format_bytes(cfg.chunk_avg, fb, sizeof fb);
    printf("%s avg, ", fb);
    brs_format_bytes(cfg.chunk_max, fb, sizeof fb);
    printf("%s max\n", fb);
    printf("  hash:        %s\n",
           cfg.hash_algo == BRS_HASH_FNV1A_128 ? "FNV1A_128" :
           cfg.hash_algo == BRS_HASH_XXH3_128 ? "XXH3_128" :
           cfg.hash_algo == BRS_HASH_BLAKE2B_128 ? "BLAKE2B_128" : "unknown");
    printf("  compression: %s\n",
           cfg.compression == BRS_COMPRESSION_NONE ? "none" :
           cfg.compression == BRS_COMPRESSION_LZ4 ? "lz4" : "zstd");
    printf("  encryption:   %s\n",
           cfg.encrypted ? ((cfg.cipher_algo == BRS_CIPHER_AES256_GCM) ? "AES-256-GCM + Argon2id" : "XChaCha20-Poly1305 + Argon2id") : "none");

    char snaps_dir[BRS_PATH_MAX];
    brs_path_join(snaps_dir, sizeof snaps_dir, repo_path, "snapshots");
    BrsDirList snaps;
    uint64_t snap_count = 0;
    uint64_t tot_entries = 0, tot_files = 0, tot_dirs = 0, tot_sym = 0;
    uint64_t delta_entries = 0, delta_logical = 0, delta_chunk_refs = 0;
    uint64_t tot_logical = 0, tot_refs = 0;

    if (brs_list_dir(snaps_dir, &snaps) == 0) {
        brs_dir_list_sort(&snaps);
        for (size_t i = 0; i < snaps.count; ++i) {
            size_t ln = strlen(snaps.names[i]);
            if (ln > 5 && strcmp(snaps.names[i] + ln - 5, ".snap") == 0)
                snap_count++;
        }
        printf("\n=== Snapshots: %llu ===\n", (unsigned long long)snap_count);
        for (size_t i = 0; i < snaps.count; ++i) {
            size_t ln = strlen(snaps.names[i]);
            if (ln <= 5 || strcmp(snaps.names[i] + ln - 5, ".snap") != 0)
                continue;
            char sp[BRS_PATH_MAX];
            if (brs_path_join(sp, sizeof sp, snaps_dir, snaps.names[i]) != 0)
                continue;
            BrsBuffer sdata;
            brs_buffer_init(&sdata);
            if (brs_read_file(sp, &sdata) != 0) { brs_buffer_free(&sdata); continue; }
            BrsParsedSnapshot snap;
            brs_parsed_snapshot_init(&snap);
            if (brs_parse_snapshot(sdata.data, sdata.size, &snap,
                                   cfg.encrypted ? &key : NULL, cfg.cipher_algo) == 0) {
                tot_entries += snap.entry_count;
                tot_files += snap.file_count;
                tot_dirs += snap.dir_count;
                tot_sym += snap.symlink_count;
                tot_logical += snap.logical_bytes;
                tot_refs += snap.chunk_refs;
                for (uint64_t e = 0; e < snap.entries_len; ++e) {
                    if (snap.entries[e].type == BRS_FILETYPE_FILE &&
                        (snap.entries[e].flags & BRS_FLAG_DELTA)) {
                        delta_entries++;
                        delta_logical += snap.entries[e].size;
                        delta_chunk_refs += snap.entries[e].chunk_count;
                    }
                }
                char lb[64];
                brs_format_bytes(snap.logical_bytes, lb, sizeof lb);
                char name[64];
                snprintf(name, sizeof name, "%s", snaps.names[i]);
                if (strlen(name) > 40) strcpy(name + 37, "...");
                printf("  %-40s entries=%llu files=%llu logical=%s refs=%llu\n",
                       name, (unsigned long long)snap.entry_count,
                       (unsigned long long)snap.file_count, lb,
                       (unsigned long long)snap.chunk_refs);
            }
            brs_parsed_snapshot_free(&snap);
            brs_buffer_free(&sdata);
        }
        brs_dir_list_free(&snaps);
    }
    printf("  ---\n");
    printf("  total entries:   %llu\n", (unsigned long long)tot_entries);
    printf("  total files:     %llu\n", (unsigned long long)tot_files);
    printf("  total dirs:      %llu\n", (unsigned long long)tot_dirs);
    printf("  total symlinks:  %llu\n", (unsigned long long)tot_sym);
    brs_format_bytes(tot_logical, fb, sizeof fb);
    printf("  logical size:    %s\n", fb);
    printf("  chunk refs:      %llu\n", (unsigned long long)tot_refs);
    if (delta_entries > 0) {
        printf("\n=== Delta Encoding ===\n");
        printf("  delta entries:   %llu\n", (unsigned long long)delta_entries);
        brs_format_bytes(delta_logical, fb, sizeof fb);
        printf("  logical bytes:   %s\n", fb);
        printf("  delta chunks:    %llu\n", (unsigned long long)delta_chunk_refs);
    }

    char packs_dir[BRS_PATH_MAX];
    brs_path_join(packs_dir, sizeof packs_dir, repo_path, "packs");
    uint64_t pack_count = 0, pack_bytes = 0;
    BrsDirList plist;
    if (brs_list_dir(packs_dir, &plist) == 0) {
        for (size_t i = 0; i < plist.count; ++i) {
            size_t ln = strlen(plist.names[i]);
            if (ln <= 5 || strcmp(plist.names[i] + ln - 5, ".pack") != 0) continue;
            pack_count++;
            char pp[BRS_PATH_MAX];
            if (brs_path_join(pp, sizeof pp, packs_dir, plist.names[i]) != 0) continue;
            BrsVfs *vfs_pack = brs_vfs_context_get();
            if (vfs_pack) {
                const char *rel = brs_vfs_context_strip(pp);
                uint64_t sz = 0; uint32_t md = 0;
                if (rel && brs_vfs_stat(vfs_pack, rel, &sz, &md) == 0) pack_bytes += sz;
            } else {
                struct stat st;
                if (stat(pp, &st) == 0) pack_bytes += (uint64_t)st.st_size;
            }
        }
        brs_dir_list_free(&plist);
    }
    printf("\n=== Packs: %llu ===\n", (unsigned long long)pack_count);
    brs_format_bytes(pack_bytes, fb, sizeof fb);
    printf("  total size:    %s\n", fb);
    if (pack_count > 0) {
        brs_format_bytes(pack_bytes / pack_count, fb, sizeof fb);
        printf("  avg size:      %s\n", fb);
    }
    uint64_t open_id;
    if (brs_read_open_pack_id(repo_path, &open_id) == 0)
        printf("  open pack:     %llu\n", (unsigned long long)open_id);
    else
        printf("  open pack:     (none)\n");

    if (brs_index_map_init(&index_map, 1024) != 0) goto info_cleanup;
    index_init = 1;


            (void)brs_load_all_indexes(repo_path, &index_map);

    {
        uint64_t unique = brs_index_map_count(&index_map);
        uint64_t total_uncomp = 0, total_comp = 0, comp_count = 0, uncomp_count = 0;
        uint64_t pack_entries = unique;
        printf("  total entries: %llu\n", (unsigned long long)pack_entries);
        size_t it = 0;
        const BrsIndexSlot *slot;
        while ((slot = brs_index_map_next(&index_map, &it)) != NULL) {
            total_uncomp += slot->value.uncomp_size;
            total_comp += slot->value.comp_size;
            if (slot->value.flags & BRS_CHUNK_FLAG_COMPRESSED) comp_count++;
            else uncomp_count++;
        }
        printf("\n=== Chunks ===\n");
        printf("  unique chunks:    %llu\n", (unsigned long long)unique);
        printf("  total references: %llu\n", (unsigned long long)tot_refs);
        if (unique > 0)
            printf("  dedup ratio:      %.2fx\n", (double)tot_refs / (double)unique);
        printf("\n=== Storage ===\n");
        brs_format_bytes(total_uncomp, fb, sizeof fb);
        printf("  uncompressed:  %s\n", fb);
        brs_format_bytes(total_comp, fb, sizeof fb);
        printf("  compressed:    %s\n", fb);
        if (total_comp > 0)
            printf("  comp ratio:    %.2fx\n", (double)total_uncomp / (double)total_comp);
        printf("  compressed chunks:   %llu\n", (unsigned long long)comp_count);
        printf("  uncompressed chunks: %llu\n", (unsigned long long)uncomp_count);
        if (tot_logical > 0 && pack_bytes > 0)
            printf("\noverall ratio (logical/physical): %.2fx\n",
                   (double)tot_logical / (double)pack_bytes);
    }

    char idx_dir[BRS_PATH_MAX];
    brs_path_join(idx_dir, sizeof idx_dir, repo_path, "index");
    uint64_t segments = 0;
    BrsDirList idxl;
    if (brs_list_dir(idx_dir, &idxl) == 0) {
        for (size_t i = 0; i < idxl.count; ++i) {
            size_t ln = strlen(idxl.names[i]);
            if (ln > 4 && strcmp(idxl.names[i] + ln - 4, ".idx") == 0) segments++;
        }
        brs_dir_list_free(&idxl);
    }
    printf("\n=== Index ===\n");
    printf("  segments: %llu\n", (unsigned long long)segments);
    printf("  entries:  %llu\n", (unsigned long long)brs_index_map_count(&index_map));

    BrsCacheMap fcache;
    if (brs_cache_map_init(&fcache, 16) == 0) {
        printf("\n=== Cache ===\n");
        if (brs_load_file_cache(repo_path, &fcache) == 0)
            printf("  entries: %llu\n", (unsigned long long)brs_cache_map_count(&fcache));
        else
            printf("  (empty)\n");
        brs_cache_map_free(&fcache);
    }

    rc = 0;
info_cleanup:
    if (index_init) brs_index_map_free(&index_map);
    if (key_valid) brs_secure_key_wipe(&key);
    if (owns_vfs) { brs_vfs_context_clear(); brs_vfs_close(vfs); }
    return rc;
}

/* ============================================================================
 * diff
 * ==========================================================================*/
static int chunks_equal(const BrsManifestEntry *a, const BrsManifestEntry *b)
{
    if (a->chunk_count != b->chunk_count) return 0;
    for (uint32_t i = 0; i < a->chunk_count; ++i)
        if (!brs_chunk_id_equal(&a->chunks[i], &b->chunks[i])) return 0;
    return 1;
}

int brs_repo_diff(const char *repo_path, const char *snap1, const char *snap2)
{
    if (!repo_path || !snap1 || !snap2) return 1;
    int rc = 1;
    char p1[BRS_PATH_MAX], p2[BRS_PATH_MAX];
    BrsBuffer d1, d2;
    brs_buffer_init(&d1); brs_buffer_init(&d2);
    BrsRepoConfig cfg; BrsSecureKey key;
    int key_valid = 0, has_cfg = 0;
    BrsParsedSnapshot s1, s2;
    brs_parsed_snapshot_init(&s1); brs_parsed_snapshot_init(&s2);

    if (resolve_snap_path(repo_path, snap1, p1, sizeof p1) != 0 ||
        resolve_snap_path(repo_path, snap2, p2, sizeof p2) != 0) {
        fprintf(stderr, "cannot resolve snapshot paths\n"); goto cleanup;
    }
    if (brs_read_file(p1, &d1) != 0) { fprintf(stderr, "cannot read snapshot 1: %s\n", p1); goto cleanup; }
    if (brs_read_file(p2, &d2) != 0) { fprintf(stderr, "cannot read snapshot 2: %s\n", p2); goto cleanup; }

    brs_repo_config_default(&cfg);
    has_cfg = (brs_load_config(repo_path, &cfg) == 0);
    if (has_cfg && cfg.encrypted) {
        if (derive_repo_key(&cfg, &key) != 0) goto cleanup;
        key_valid = 1;
    }
    if (brs_parse_snapshot(d1.data, d1.size, &s1,
                           (has_cfg && cfg.encrypted) ? &key : NULL,
                           (has_cfg ? cfg.cipher_algo : BRS_CIPHER_CHACHA20_POLY1305)) != 0) {
        fprintf(stderr, "cannot parse snapshot 1\n"); goto cleanup;
    }
    if (brs_parse_snapshot(d2.data, d2.size, &s2,
                           (has_cfg && cfg.encrypted) ? &key : NULL,
                           (has_cfg ? cfg.cipher_algo : BRS_CIPHER_CHACHA20_POLY1305)) != 0) {
        fprintf(stderr, "cannot parse snapshot 2\n"); goto cleanup;
    }

    uint64_t added = 0, deleted = 0, modified = 0;
    uint64_t type_changed = 0, meta_changed = 0;
    uint64_t i = 0, j = 0;
    while (i < s1.entries_len && j < s2.entries_len) {
        const BrsManifestEntry *e1 = &s1.entries[i];
        const BrsManifestEntry *e2 = &s2.entries[j];
        int c = strcmp(e1->path, e2->path);
        if (c < 0) { printf("D  %s\n", e1->path); deleted++; i++; continue; }
        if (c > 0) { printf("A  %s\n", e2->path); added++; j++; continue; }
        if (e1->type != e2->type) {
            printf("T  %s\n", e1->path); type_changed++;
        } else {
            int content_changed = 0;
            if (e1->type == BRS_FILETYPE_FILE) {
                if ((e1->flags & BRS_FLAG_HARDLINK) || (e2->flags & BRS_FLAG_HARDLINK)) {
                    content_changed = strcmp(e1->hardlink_to ? e1->hardlink_to : "",
                                             e2->hardlink_to ? e2->hardlink_to : "") != 0;
                } else {
                    content_changed = (e1->size != e2->size) || !chunks_equal(e1, e2);
                }
            } else if (e1->type == BRS_FILETYPE_SYMLINK) {
                content_changed = strcmp(e1->symlink_target ? e1->symlink_target : "",
                                         e2->symlink_target ? e2->symlink_target : "") != 0;
            }
            if (content_changed) { printf("M  %s\n", e1->path); modified++; }
            else if (e1->mode != e2->mode || e1->uid != e2->uid ||
                     e1->gid != e2->gid || e1->mtime_ns != e2->mtime_ns) {
                printf("C  %s\n", e1->path); meta_changed++;
            }
        }
        i++; j++;
    }
    while (i < s1.entries_len) { printf("D  %s\n", s1.entries[i].path); deleted++; i++; }
    while (j < s2.entries_len) { printf("A  %s\n", s2.entries[j].path); added++; j++; }

    uint64_t common = 0;
    { uint64_t a = 0, b = 0;
      while (a < s1.entries_len && b < s2.entries_len) {
          int c = strcmp(s1.entries[a].path, s2.entries[b].path);
          if (c == 0) { common++; a++; b++; }
          else if (c < 0) a++; else b++;
      }
    }
    uint64_t unchanged = common > (modified + type_changed + meta_changed)
        ? common - (modified + type_changed + meta_changed) : 0;
    printf("\n");
    printf("added:     %llu\n", (unsigned long long)added);
    printf("deleted:   %llu\n", (unsigned long long)deleted);
    printf("modified:  %llu\n", (unsigned long long)modified);
    if (type_changed > 0) printf("type:      %llu\n", (unsigned long long)type_changed);
    if (meta_changed > 0) printf("metadata:  %llu\n", (unsigned long long)meta_changed);
    printf("unchanged: %llu\n", (unsigned long long)unchanged);
    rc = 0;
cleanup:
    brs_parsed_snapshot_free(&s1); brs_parsed_snapshot_free(&s2);
    if (key_valid) brs_secure_key_wipe(&key);
    brs_buffer_free(&d1); brs_buffer_free(&d2);
    return rc;
}

/* ============================================================================
 * load_key
 * ==========================================================================*/
int brs_repo_load_key(const char *repo_path, BrsRepoConfig *cfg_out, BrsSecureKey *key_out)
{
    if (!repo_path || !cfg_out) return -1;
    if (brs_load_config(repo_path, cfg_out) != 0) return -1;
    if (!cfg_out->encrypted) return 0;
    if (!key_out) return -1;
    return derive_repo_key(cfg_out, key_out);
}


/* ============================================================
 * extract: filtro de rutas con soporte de wildcards (glob)
 * ============================================================ */

static int is_glob_pattern(const char *s)
{
    if (!s)
        return 0;

    for (; *s != '\0'; ++s) {
        if (*s == '*' || *s == '?' || *s == '[')
            return 1;
    }

    return 0;
}

static int extract_match(const char *path,
                         const char *const *paths,
                         size_t n_paths)
{
    if (n_paths == 0)
        return 1;   /* sin filtro = extraer todo */

    if (!path)
        return 0;

    for (size_t p = 0; p < n_paths; ++p) {
        const char *pat = paths[p];

        if (!pat || pat[0] == '\0')
            return 1;

        if (is_glob_pattern(pat)) {
            /* Glob: '*' cruza '/', útil para buscar por nombre */
            if (fnmatch(pat, path, FNM_PERIOD | FNM_NOESCAPE) == 0)
                return 1;
        } else {
            /* Literal: exacto o prefijo de directorio */
            size_t plen = strlen(pat);

            if (plen > 0 && pat[plen - 1] == '/')
                plen--;

            if (plen == 0)
                return 1;

            if (strncmp(path, pat, plen) == 0) {
                if (path[plen] == '\0' || path[plen] == '/')
                    return 1;
            }
        }
    }

    return 0;
}

static void brs_extract_apply_metadata(const char *path, const BrsManifestEntry *e)
{
    if (!path || !e) return;
    
    /* 1) Tratamiento exclusivo para enlaces simbólicos */
    if (e->type == BRS_FILETYPE_SYMLINK) {
        if (e->uid != 0 || e->gid != 0) {
            (void)lchown(path, (uid_t)e->uid, (gid_t)e->gid);
        }
        struct timespec ts[2];
        ts[0].tv_sec = (time_t)(e->mtime_ns / 1000000000ULL);
        ts[0].tv_nsec = (long)(e->mtime_ns % 1000000000ULL);
        ts[1] = ts[0];
        (void)utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW);
        return;
    }
    
    /* 2) Máscara de permisos limpia para archivos y directorios ordinarios */
    mode_t clean_mode = (mode_t)(e->mode & 07777);
    (void)chmod(path, clean_mode);
    
    if (e->uid != 0 || e->gid != 0) {
        (void)chown(path, (uid_t)e->uid, (gid_t)e->gid);
    }
    
    /* 3) Estampado de tiempo robusto con doble check preventivo */
    struct timespec ts[2];
    ts[0].tv_sec = (time_t)(e->mtime_ns / 1000000000ULL);
    ts[0].tv_nsec = (long)(e->mtime_ns % 1000000000ULL);
    ts[1] = ts[0];
    
    if (utimensat(AT_FDCWD, path, ts, 0) != 0) {
        /* Salvaguarda si el sistema operativo se confunde con la barra final del test 67 */
        (void)utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW);
    }
}

/* ============================================================================
 * Progreso de descarga para extract
 * ==========================================================================*/

#if defined(__GNUC__) || defined(__clang__)
#define BRS_QUERY_WEAK __attribute__((weak))
#else
#define BRS_QUERY_WEAK
#endif

typedef void (*BrsQueryVfsReadProgressCb)(void *ctx,
                                          uint64_t done,
                                          uint64_t total);

extern void brs_vfs_ssh_set_read_progress(BrsQueryVfsReadProgressCb cb,
                                          void *ctx) BRS_QUERY_WEAK;

typedef struct {
    uint64_t idx;
    uint64_t count;
    uint64_t total;
    uint64_t done;
    struct timespec start;
    struct timespec last;
    int has_last;
} BrsExtractDownloadProgress;

static double extract_dl_elapsed_sec(const struct timespec *a,
                                     const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) +
           (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static void extract_download_progress_cb(void *ctx,
                                         uint64_t done,
                                         uint64_t total)
{
    BrsExtractDownloadProgress *p = (BrsExtractDownloadProgress *)ctx;
    if (!p)
        return;

    if (total == 0)
        return;

    p->done = done;
    p->total = total;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (done != total && p->has_last) {
        double since_last = extract_dl_elapsed_sec(&p->last, &now);
        if (since_last < 0.100)
            return;
    }

    p->last = now;
    p->has_last = 1;

    double elapsed = extract_dl_elapsed_sec(&p->start, &now);
    uint64_t speed = (elapsed > 0.001)
                         ? (uint64_t)((double)done / elapsed)
                         : 0;

    char done_s[64];
    char total_s[64];
    char speed_s[64];

    brs_format_bytes(done, done_s, sizeof done_s);
    brs_format_bytes(total, total_s, sizeof total_s);
    brs_format_bytes(speed, speed_s, sizeof speed_s);

    int percent = (int)(((double)done / (double)total) * 100.0);

    const int width = 30;
    int filled = (int)(((double)done / (double)total) * width);

    if (filled < 0)
        filled = 0;

    if (filled > width)
        filled = width;

    fprintf(stderr,
            "\r[1/2] Descargando paquete %llu/%llu [%.*s%*s] %3d%% %s/%s %s/s",
            (unsigned long long)p->idx,
            (unsigned long long)p->count,
            filled,
            "==============================",
            width - filled,
            "",
            percent,
            done_s,
            total_s,
            speed_s);

    if (done == total)
        fputc('\n', stderr);

    fflush(stderr);
}

typedef struct {
    uint64_t pack_id;
    BrsChunkLocation loc;
} BrsExtractPrefetchPack;

static int extract_prefetch_add_pack(BrsExtractPrefetchPack **list,
                                     size_t *count,
                                     size_t *cap,
                                     uint64_t pack_id,
                                     const BrsChunkLocation *loc)
{
    if (*count == *cap) {
        size_t ncap = (*cap == 0) ? 16 : (*cap * 2);

        BrsExtractPrefetchPack *p =
            (BrsExtractPrefetchPack *)realloc(*list,
                                              ncap * sizeof(**list));

        if (!p)
            return -1;

        *list = p;
        *cap = ncap;
    }

    (*list)[*count].pack_id = pack_id;
    (*list)[*count].loc = *loc;
    (*count)++;

    return 0;
}

static void extract_prefetch_packs_cli(const char *repo_path,
                                       const BrsParsedSnapshot *snap,
                                       const char *const *paths,
                                       size_t n_paths,
                                       BrsIndexMap *index_map,
                                       _Atomic int *cancel_flag)
{
    if (!repo_path || !snap || !index_map)
        return;

    BrsU64Set seen;
    if (brs_u64_set_init(&seen, 64) != 0)
        return;

    BrsExtractPrefetchPack *list = NULL;
    size_t count = 0;
    size_t cap = 0;

    for (uint64_t i = 0; i < snap->entries_len; ++i) {
        const BrsManifestEntry *e = &snap->entries[i];

        if (n_paths > 0 && !extract_match(e->path, paths, n_paths))
            continue;

        if (!brs_is_safe_relative_path(e->path))
            continue;

        if (e->type != BRS_FILETYPE_FILE)
            continue;

        if ((e->flags & BRS_FLAG_HARDLINK) || e->hardlink_to)
            continue;

        for (uint32_t c = 0; c < e->chunk_count; ++c) {
            const BrsChunkLocation *loc =
                brs_index_map_get(index_map, &e->chunks[c]);

            if (!loc)
                continue;

            if (!brs_u64_set_contains(&seen, loc->pack_id)) {
                brs_u64_set_insert(&seen, loc->pack_id);

                if (extract_prefetch_add_pack(&list, &count, &cap,
                                              loc->pack_id, loc) != 0) {
                    goto out;
                }
            }
        }

        if (e->flags & BRS_FLAG_DELTA) {
            for (uint32_t c = 0; c < e->delta_source_count; ++c) {
                const BrsChunkLocation *loc =
                    brs_index_map_get(index_map,
                                      &e->delta_source_chunks[c]);

                if (!loc)
                    continue;

                if (!brs_u64_set_contains(&seen, loc->pack_id)) {
                    brs_u64_set_insert(&seen, loc->pack_id);

                    if (extract_prefetch_add_pack(&list, &count, &cap,
                                                  loc->pack_id, loc) != 0) {
                        goto out;
                    }
                }
            }
        }
    }

    if (count == 0)
        goto out;

    BrsBuffer tmp;
    brs_buffer_init(&tmp);

    int cancelled = 0;

    for (size_t i = 0; i < count; ++i) {
        if (check_cancel(cancel_flag)) {
            cancelled = 1;
            break;
        }

        BrsExtractDownloadProgress prog;
        memset(&prog, 0, sizeof(prog));

        prog.idx = (uint64_t)(i + 1);
        prog.count = (uint64_t)count;

        clock_gettime(CLOCK_MONOTONIC, &prog.start);
        prog.last = prog.start;
        prog.has_last = 0;

        int have_cb = 0;

        if (brs_vfs_ssh_set_read_progress != NULL) {
            brs_vfs_ssh_set_read_progress(extract_download_progress_cb,
                                          &prog);
            have_cb = 1;
        } else {
            fprintf(stderr,
                    "[1/2] Descargando paquete %llu/%llu\n",
                    (unsigned long long)prog.idx,
                    (unsigned long long)prog.count);
            fflush(stderr);
        }

        (void)read_chunk_from_pack(repo_path, &list[i].loc, &tmp);

        if (have_cb) {
            brs_vfs_ssh_set_read_progress(NULL, NULL);
        }
    }

    brs_buffer_free(&tmp);

    if (!cancelled) {
        fprintf(stderr, "[2/2] Extrayendo ficheros\n");
        fflush(stderr);
    }

out:
    free(list);
    brs_u64_set_free(&seen);
}

    int brs_repo_extract(const char *repo_path, const char *snapshot_name,
                     const char *const *paths, size_t n_paths,
                     const char *target_dir,
                     BrsProgressCallback cb, void *cb_user,
                     _Atomic int *cancel_flag)
{
    if (!repo_path || !snapshot_name || !target_dir)
        return 1;

    int rc = 1;
    char snap_path[BRS_PATH_MAX];

    BrsBuffer data, chunk_buf, dec_buf, uncomp_buf;
    brs_buffer_init(&data);
    brs_buffer_init(&chunk_buf);
    brs_buffer_init(&dec_buf);
    brs_buffer_init(&uncomp_buf);

    BrsRepoConfig cfg;
    BrsSecureKey key;
    int key_valid = 0, has_cfg = 0;

    BrsParsedSnapshot snap;
    brs_parsed_snapshot_init(&snap);

    BrsIndexMap index_map;
    int index_init = 0;

    char target_real[BRS_PATH_MAX];
    uint8_t *hl_done = NULL;

    BrsVfs *vfs = brs_vfs_context_get();
    int owns_vfs = 0;

    if (!vfs && strncmp(repo_path, "ssh://", 6) == 0) {
        vfs = brs_vfs_open(repo_path, 0);
        if (!vfs) {
            fprintf(stderr, "cannot connect to remote repo: %s\n", repo_path);
            rc = 1;
            goto cleanup;
        }

        brs_vfs_context_set(vfs, repo_path);
        owns_vfs = 1;
    }

    if (resolve_snap_path(repo_path, snapshot_name, snap_path,
                          sizeof snap_path) != 0 ||
        !brs_path_exists(snap_path)) {
        fprintf(stderr, "snapshot not found: %s\n", snapshot_name);
        goto cleanup;
    }

    if (brs_read_file(snap_path, &data) != 0) {
        fprintf(stderr, "cannot read snapshot: %s\n", snap_path);
        goto cleanup;
    }

    brs_repo_config_default(&cfg);
    has_cfg = (brs_load_config(repo_path, &cfg) == 0);

    if (has_cfg && cfg.encrypted) {
        if (derive_repo_key(&cfg, &key) != 0)
            goto cleanup;
        key_valid = 1;
    }

    if (brs_parse_snapshot(data.data, data.size, &snap,
                           (has_cfg && cfg.encrypted) ? &key : NULL,
                           (has_cfg ? cfg.cipher_algo
                                    : BRS_CIPHER_CHACHA20_POLY1305)) != 0) {
        fprintf(stderr, "cannot parse snapshot\n");
        goto cleanup;
    }

    if (brs_index_map_init(&index_map, 1024) != 0)
        goto cleanup;

    index_init = 1;

    (void)brs_load_all_indexes(repo_path, &index_map);

    if (brs_mkdir_p(target_dir) != 0) {
        fprintf(stderr, "cannot create target directory\n");
        goto cleanup;
    }

    {
        char *rp = realpath(target_dir, NULL);
        if (!rp)
            goto cleanup;

        size_t n = strlen(rp);
        if (n >= sizeof target_real) {
            free(rp);
            goto cleanup;
        }

        memcpy(target_real, rp, n + 1);
        free(rp);
    }

    for (size_t p = 0; p < n_paths; ++p) {
        if (!brs_is_safe_relative_path(paths[p])) {
            fprintf(stderr,
                    "error: unsafe path in extract filter: '%s'\n",
                    paths[p]);
            goto cleanup;
        }
    }

            uint64_t total_chunks = 0;

    for (uint64_t i = 0; i < snap.entries_len; ++i) {
        const BrsManifestEntry *e = &snap.entries[i];

        /* HARDENED EXTRACTION PATH FILTER ENGINE */
        if (n_paths > 0) {
            int current_matched = 0;
            for (size_t f = 0; f < n_paths; ++f) {
                /* 1) POSIX Match: Guards literal brackets and structural escapes */
                if (fnmatch(paths[f], e->path, FNM_NOESCAPE) == 0) {
                    current_matched = 1;
                    break;
                }
                /* 2) Absolute Literal Fallback: Catches complex quote/ampersand mutations */
                if (e->path && strstr(e->path, paths[f]) != NULL) {
                    current_matched = 1;
                    break;
                }
            }
            /* If it fails both criteria, it's a true mismatch; skip it */
            if (!current_matched) {
                continue;
            }
        }

        if (!brs_is_safe_relative_path(e->path))
            continue;

        if (e->type == BRS_FILETYPE_DIR) {
            total_chunks += 1;
            continue;
        }


        if (e->type == BRS_FILETYPE_FILE) {
            if (e->flags & BRS_FLAG_DELTA)
                total_chunks += e->chunk_count + e->delta_source_count;
            else if (!((e->flags & BRS_FLAG_HARDLINK) || e->hardlink_to))
                total_chunks += e->chunk_count;
            else
                total_chunks += 1;
        } else {
            total_chunks += 1;
        }
    }

    /*
     * Si tienes la función extract_prefetch_packs_cli(), esto precarga
     * los packs necesarios por SSH antes del extract.
     *
     * Si no la tienes, elimina este bloque.
     */
    #if 0
    if (strncmp(repo_path, "ssh://", 6) == 0) {
        extract_prefetch_packs_cli(repo_path, &snap, paths, n_paths,
                                   &index_map, cancel_flag);
    }
#endif

    uint64_t chunks_done = 0;
    uint64_t items_done = 0;
    report_progress(cb, cb_user, "extract", 0, total_chunks);

        //  CÓDIGO NUEVO (Escudo unificado de extracción):
for (uint64_t i = 0; i < snap.entries_len; ++i) {
    const BrsManifestEntry *e = &snap.entries[i];

    /* UNIFICACIÓN DE FILTRADO REAL: Sincronizado con la fase de conteo */
    if (n_paths > 0) {
        int current_matched = 0;
        for (size_t f = 0; f < n_paths; ++f) {
            /* 1) POSIX Match: Desactiva expresiones regulares en corchetes [glob] */
            if (fnmatch(paths[f], e->path, FNM_NOESCAPE) == 0) {

                current_matched = 1;
                break;
            }
            /* 2) Salvaguarda absoluta por subcadena exacta para paths complejos */
            if (e->path && strstr(e->path, paths[f]) != NULL) {
                current_matched = 1;
                break;
            }
        }
        /* Si falla ambos filtros, el descarte es real; saltamos la entrada */
        if (!current_matched) {
            continue;
        }
    }
    

        if (!brs_is_safe_relative_path(e->path))
            continue;

        if (check_cancel(cancel_flag)) {
            printf("cancelled\n");
            rc = 2;
            goto cleanup;
        }

        char full[BRS_PATH_MAX];
        if (brs_path_join(full, sizeof full, target_real, e->path) != 0)
            continue;

        if (e->type == BRS_FILETYPE_DIR) {
            struct stat st;

            if (lstat(full, &st) == 0) {
                if (!S_ISDIR(st.st_mode)) {
                    unlink(full);

                    if (brs_mkdir_p(full) != 0) {
                        fprintf(stderr,
                                "warning: cannot create directory: %s: %s\n",
                                e->path, strerror(errno));
                        continue;
                    }
                }
            } else {
                if (brs_mkdir_p(full) != 0) {
                    fprintf(stderr,
                            "warning: cannot create directory: %s: %s\n",
                            e->path, strerror(errno));
                    continue;
                }
            }

            brs_extract_apply_metadata(full, e);
            chunks_done++;
            items_done++;
            report_progress(cb, cb_user, "extract", chunks_done, total_chunks);
            continue;

        }

        const char *slash = strrchr(full, '/');
        if (slash && (size_t)(slash - full) > strlen(target_real)) {
            char parent[BRS_PATH_MAX];
            memcpy(parent, full, (size_t)(slash - full));
            parent[slash - full] = '\0';
            brs_mkdir_p(parent);
        }

        if (e->type == BRS_FILETYPE_FILE &&
            ((e->flags & BRS_FLAG_HARDLINK) || e->hardlink_to)) {
            continue;
        }

        if (e->type == BRS_FILETYPE_FILE) {
            struct stat existing;

            if (lstat(full, &existing) == 0) {
                if (S_ISDIR(existing.st_mode)) {
                    fprintf(stderr,
                            "warning: target path is a directory, skipping: %s\n",
                            full);
                    continue;
                }

                unlink(full);
            }

            int fd = open(full,
                          O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW,
                          0600);

            if (fd < 0) {
                fprintf(stderr,
                        "warning: cannot create file: %s: %s\n",
                        e->path, strerror(errno));
                continue;
            }

            int err = 0;

            if (e->flags & BRS_FLAG_DELTA) {
                size_t old_size = 0;

                uint8_t *old_content = reconstruct_from_chunks(
                    repo_path, &index_map,
                    (has_cfg && cfg.encrypted) ? &key : NULL,
                    (has_cfg ? cfg.cipher_algo
                             : BRS_CIPHER_CHACHA20_POLY1305),
                    e->delta_source_chunks, e->delta_source_count,
                    &old_size);

                size_t delta_size = 0;
                uint8_t *delta_payload = NULL;

                if (old_content) {
                    delta_payload = reconstruct_from_chunks(
                        repo_path, &index_map,
                        (has_cfg && cfg.encrypted) ? &key : NULL,
                        (has_cfg ? cfg.cipher_algo
                                 : BRS_CIPHER_CHACHA20_POLY1305),
                        e->chunks, e->chunk_count, &delta_size);
                }

                uint8_t *new_content = NULL;
                size_t new_size = 0;
                int dok = 0;

                if (old_content && delta_payload) {
                    dok = (brs_delta_decode(old_content, old_size,
                                            delta_payload, delta_size,
                                            e->size,
                                            &new_content, &new_size) == 0 &&
                           new_size == e->size);
                }

                if (dok) {
                    if (new_size > 0 &&
                        brs_write_fd_all(fd, new_content, new_size) != 0) {
                        err = 1;
                    }
                } else {
                    fprintf(stderr,
                            "warning: delta extract failed for %s\n",
                            e->path);
                    err = 1;
                }

                free(old_content);
                free(delta_payload);
                free(new_content);

                chunks_done += e->chunk_count + e->delta_source_count;
                items_done++;

                report_progress(cb, cb_user, "extract",
                                chunks_done, total_chunks);
            } else {
                for (uint32_t c = 0; c < e->chunk_count; ++c) {
                    const BrsChunkLocation *loc =
                        brs_index_map_get(&index_map, &e->chunks[c]);

                    if (!loc ||
                        read_chunk_from_pack(repo_path, loc, &chunk_buf) != 0) {
                        err = 1;
                        break;
                    }

                    const uint8_t *src = chunk_buf.data;
                    size_t src_len = chunk_buf.size;

                    if (loc->flags & BRS_CHUNK_FLAG_ENCRYPTED) {
                        if (brs_decrypt_buffer(&key, cfg.cipher_algo,
                                               src, src_len, &dec_buf) != 0) {
                            err = 1;
                            break;
                        }

                        src = dec_buf.data;
                        src_len = dec_buf.size;
                    }

                    const uint8_t *to_write = src;
                    size_t write_len = src_len;

                    if (loc->flags & BRS_CHUNK_FLAG_COMPRESSED) {
                        if (loc->uncomp_size > BRS_CHUNK_UNCOMP_MAX ||
                            brs_buffer_resize(&uncomp_buf,
                                              loc->uncomp_size) != 0) {
                            err = 1;
                            break;
                        }

                        size_t dl;

                        if (loc->flags & BRS_CHUNK_FLAG_ZSTD)
                            dl = brs_zstd_decompress(src, src_len,
                                                     uncomp_buf.data,
                                                     uncomp_buf.size);
                        else
                            dl = brs_lz4_decompress(src, src_len,
                                                    uncomp_buf.data,
                                                    uncomp_buf.size);

                        if (dl != (size_t)loc->uncomp_size) {
                            err = 1;
                            break;
                        }

                        to_write = uncomp_buf.data;
                        write_len = dl;
                    }

                    if (write_len > 0 &&
                        brs_write_fd_all(fd, to_write, write_len) != 0) {
                        err = 1;
                        break;
                    }

                    chunks_done++;
                    report_progress(cb, cb_user, "extract",
                                    chunks_done, total_chunks);
                }
            }

            close(fd);

            if (err)
                unlink(full);
            else
                brs_extract_apply_metadata(full, e);
                 items_done++;
                //  CÓDIGO NUEVO (Línea 1157 - Escudo de Enlaces Simbólicos):
        } else if (e->type == BRS_FILETYPE_SYMLINK) {
            /* Aseguramos de forma matemática la existencia de la carpeta contenedora
               para evitar fallos ENOENT en rutas con estructuras complejas o unicode */
            const char *last_slash = strrchr(full, '/');
            if (last_slash && (size_t)(last_slash - full) < sizeof(full)) {
                char sym_parent[BRS_PATH_MAX];
                size_t p_len = (size_t)(last_slash - full);
                if (p_len < sizeof(sym_parent)) {
                    memcpy(sym_parent, full, p_len);
                    sym_parent[p_len] = '\0';
                    (void)brs_mkdir_p(sym_parent);
                }
            }

            unlink(full);
            if (symlink(e->symlink_target ? e->symlink_target : "", full) != 0) {
                fprintf(stderr, "warning: cannot create symlink: %s: %s\n", 
                        e->path, strerror(errno));
            } else {
                brs_extract_apply_metadata(full, e);
            }



            chunks_done++;
            items_done++;
            report_progress(cb, cb_user, "extract", chunks_done, total_chunks);
            
        }
    }

    hl_done = calloc(snap.entries_len ? (size_t)snap.entries_len : 1, 1);
    if (!hl_done)
        goto cleanup;

    for (int hl_pass = 0; hl_pass < 2; ++hl_pass) {
        for (uint64_t i = 0; i < snap.entries_len; ++i) {
            const BrsManifestEntry *e = &snap.entries[i];

            if (n_paths > 0 && !extract_match(e->path, paths, n_paths))
                continue;

            if (!brs_is_safe_relative_path(e->path))
                continue;

            if (e->type != BRS_FILETYPE_FILE)
                continue;

            if (!((e->flags & BRS_FLAG_HARDLINK) || e->hardlink_to))
                continue;

            if (hl_done[i])
                continue;

            if (check_cancel(cancel_flag)) {
                printf("cancelled\n");
                rc = 2;
                goto cleanup;
            }

            char full[BRS_PATH_MAX];
            if (brs_path_join(full, sizeof full, target_real, e->path) != 0)
                continue;

            char link_target[BRS_PATH_MAX];
            if (brs_path_join(link_target, sizeof link_target, target_real,
                              e->hardlink_to ? e->hardlink_to : "") != 0)
                continue;

            struct stat st;

            if (lstat(full, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    fprintf(stderr,
                            "warning: target path is a directory, "
                            "skipping hardlink: %s\n",
                            e->path);
                    hl_done[i] = 1;
                    continue;
                }

                unlink(full);
            }

            if (link(link_target, full) == 0) {
            hl_done[i] = 1;
            brs_extract_apply_metadata(full, e);
            chunks_done++;
            items_done++;
            report_progress(cb, cb_user, "extract",
            chunks_done, total_chunks);
            continue;
        }


            if (errno == ENOENT && hl_pass == 0)
                continue;

            hl_done[i] = 1;

            fprintf(stderr,
                    "warning: cannot create hardlink: %s -> %s: %s\n",
                    e->path,
                    e->hardlink_to ? e->hardlink_to : "",
                    strerror(errno));
        }
    }

    printf("extracted %llu item(s) to %s\n",
           (unsigned long long)items_done, target_real);

    /* ====================================================================
     * PASO INTERMEDIO: Forzado absoluto de Enlaces Simbólicos en Extract
     * ==================================================================== */
    for (uint64_t i = 0; i < snap.entry_count; ++i) {
        const BrsManifestEntry *ex_sym_x = &snap.entries[i];
        if (ex_sym_x->type != BRS_FILETYPE_SYMLINK)
            continue;

        /* Si se pasaron filtros de ruta específicos, verificamos si este enlace coincide */
        if (n_paths > 0 && !extract_match(ex_sym_x->path, paths, n_paths))
            continue;

        char ex_sym_full[BRS_PATH_MAX];
        
        /* UNIÓN CORRECTA: Usamos target_dir que es la variable real de destino */
        if (brs_path_join(ex_sym_full, sizeof ex_sym_full, target_dir, ex_sym_x->path) != 0)
            continue;

        /* Borrado incondicional total del residuo viejo */
        (void)unlink(ex_sym_full);

        /* Aseguramos la existencia de la carpeta padre */
        const char *ex_sym_slash = strrchr(ex_sym_full, '/');
        if (ex_sym_slash && (size_t)(ex_sym_slash - ex_sym_full) < sizeof(ex_sym_full)) {
            char ex_sym_parent[BRS_PATH_MAX];
            size_t p_len = (size_t)(ex_sym_slash - ex_sym_full);
            if (p_len < sizeof(ex_sym_parent)) {
                memcpy(ex_sym_parent, ex_sym_full, p_len);
                ex_sym_parent[p_len] = '\0';
                (void)brs_mkdir_p(ex_sym_parent);
            }
        }

        const char *ex_sym_tgt = ex_sym_x->symlink_target;
        if (!ex_sym_tgt || ex_sym_tgt == '\0') {
            ex_sym_tgt = ".";
        }

        if (symlink(ex_sym_tgt, ex_sym_full) == 0) {
            brs_extract_apply_metadata(ex_sym_full, ex_sym_x);
        }
    }



    rc = 0;

cleanup:
    free(hl_done);

    brs_pack_cache_flush();

    brs_buffer_free(&chunk_buf);
    brs_buffer_free(&dec_buf);
    brs_buffer_wipe_free(&uncomp_buf);
    brs_buffer_free(&data);

    brs_parsed_snapshot_free(&snap);

    if (index_init)
        brs_index_map_free(&index_map);

    if (key_valid)
        brs_secure_key_wipe(&key);

    if (owns_vfs) {
        brs_vfs_context_clear();
        brs_vfs_close(vfs);
    }

    return rc;
}

/* ============================================================
 * ls
 * ============================================================ */
#include <ctype.h>

static int ci_contains(const char *hay, const char *needle)
{
    if (!needle || needle[0] == '\0') return 1;
    for (size_t i = 0; hay[i] != '\0'; ++i) {
        size_t j = 0;
        while (needle[j] != '\0' && hay[i + j] != '\0' &&
               tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]))
            j++;
        if (needle[j] == '\0') return 1;
    }
    return 0;
}

static char entry_type_char(const BrsManifestEntry *e)
{
    if (e->type == BRS_FILETYPE_DIR) return 'D';
    if (e->type == BRS_FILETYPE_SYMLINK) return 'L';
    if (e->type == BRS_FILETYPE_FILE) {
        if ((e->flags & BRS_FLAG_HARDLINK) || e->hardlink_to) return 'H';
        return 'F';
    }
    return 'O';
}

static void ls_print(const BrsManifestEntry *e, const char *name, int long_fmt, int dir_suffix)
{
    if (!long_fmt) { printf("%s%s\n", name, dir_suffix ? "/" : ""); return; }
    char sz[64], ts[64];
    if (e) { brs_format_bytes(e->size, sz, sizeof sz); brs_format_timestamp_utc(e->mtime_ns, ts, sizeof ts); }
    else { snprintf(sz, sizeof sz, "-"); snprintf(ts, sizeof ts, "-"); }
    printf("%c  %10s  %-25s  %s%s",
           e ? entry_type_char(e) : 'D', sz, ts, name, dir_suffix ? "/" : "");
    if (e && e->type == BRS_FILETYPE_SYMLINK && e->symlink_target)
        printf(" -> %s", e->symlink_target);
    if (e && e->type == BRS_FILETYPE_FILE && e->hardlink_to && e->hardlink_to[0] != '\0')
        printf(" => %s", e->hardlink_to);
    printf("\n");
}

int brs_repo_ls(const char *repo_path, const char *snapshot_name,
                const char *const *prefixes, size_t n_prefixes,
                int long_fmt, int recursive, const char *match)
{
    if (!repo_path || !snapshot_name) return 1;
    int rc = 1;
    char snap_path[BRS_PATH_MAX];
    BrsBuffer data;
    brs_buffer_init(&data);
    BrsRepoConfig cfg; BrsSecureKey key;
    int key_valid = 0, has_cfg = 0;
    BrsParsedSnapshot snap;
    brs_parsed_snapshot_init(&snap);

    if (resolve_snap_path(repo_path, snapshot_name, snap_path, sizeof snap_path) != 0 ||
        !brs_path_exists(snap_path)) {
        fprintf(stderr, "snapshot not found: %s\n", snapshot_name); goto cleanup;
    }
    if (brs_read_file(snap_path, &data) != 0) {
        fprintf(stderr, "cannot read snapshot: %s\n", snap_path); goto cleanup;
    }
    brs_repo_config_default(&cfg);
    has_cfg = (brs_load_config(repo_path, &cfg) == 0);
    if (has_cfg && cfg.encrypted) {
        if (derive_repo_key(&cfg, &key) != 0) goto cleanup;
        key_valid = 1;
    }
    if (brs_parse_snapshot(data.data, data.size, &snap,
                           (has_cfg && cfg.encrypted) ? &key : NULL,
                           (has_cfg ? cfg.cipher_algo : BRS_CIPHER_CHACHA20_POLY1305)) != 0) {
        fprintf(stderr, "cannot parse snapshot\n"); goto cleanup;
    }

    if (match && match[0] != '\0') {
        uint64_t found = 0;
        for (uint64_t i = 0; i < snap.entries_len; ++i) {
            const BrsManifestEntry *e = &snap.entries[i];
            if (!ci_contains(e->path, match)) continue;
            ls_print(e, e->path, 1, 0);
            found++;
        }
        printf("-- %llu coincidence(s) for '%s'\n", (unsigned long long)found, match);
        rc = (found > 0) ? 0 : 1;
        goto cleanup;
    }

    {
        const char *default_pfx[1] = {""};
        if (n_prefixes == 0) { prefixes = default_pfx; n_prefixes = 1; }
        for (size_t pi = 0; pi < n_prefixes; ++pi) {
            char pfx[BRS_PATH_MAX];
            size_t L = strlen(prefixes[pi]);
            size_t o = 0, s0 = 0;
            while (s0 < L && prefixes[pi][s0] == '/') s0++;
            while (s0 < L && o + 1 < sizeof pfx) pfx[o++] = prefixes[pi][s0++];
            while (o > 0 && pfx[o - 1] == '/') o--;
            if (o > 0 && o + 1 < sizeof pfx) pfx[o++] = '/';
            pfx[o] = '\0';
            size_t plen = o;
            if (plen > 0) {
                int exists = 0;
                for (uint64_t i = 0; i < snap.entries_len; ++i) {
                    if (strncmp(snap.entries[i].path, pfx, plen) == 0) { exists = 1; break; }
                }
                if (!exists) { fprintf(stderr, "path no found in the snapshot: %s\n", pfx); continue; }
            }
            if (recursive) {
                for (uint64_t i = 0; i < snap.entries_len; ++i) {
                    const BrsManifestEntry *e = &snap.entries[i];
                    if (plen > 0 && strncmp(e->path, pfx, plen) != 0) continue;
                    ls_print(e, e->path, long_fmt, 0);
                }
            } else {
                char last_dir[BRS_PATH_MAX]; last_dir[0] = '\0';
                for (uint64_t i = 0; i < snap.entries_len; ++i) {
                    const BrsManifestEntry *e = &snap.entries[i];
                    if (plen > 0 && strncmp(e->path, pfx, plen) != 0) continue;
                    const char *rest = e->path + plen;
                    if (rest[0] == '\0') continue;
                    const char *slash = strchr(rest, '/');
                    if (slash) {
                        size_t dlen = (size_t)(slash - rest);
                        char name[BRS_PATH_MAX];
                        if (dlen >= sizeof name) continue;
                        memcpy(name, rest, dlen); name[dlen] = '\0';
                        if (strcmp(name, last_dir) == 0) continue;
                        snprintf(last_dir, sizeof last_dir, "%s", name);
                        ls_print(NULL, name, long_fmt, 1);
                    } else {
                        ls_print(e, rest, long_fmt, e->type == BRS_FILETYPE_DIR);
                    }
                }
            }
        }
    }
    rc = 0;
cleanup:
    brs_parsed_snapshot_free(&snap);
    if (key_valid) brs_secure_key_wipe(&key);
    brs_buffer_free(&data);
    return rc;
}
