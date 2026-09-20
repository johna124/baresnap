/* brs_repo_prune.c — retención, orden de snapshots y prune concurrente.
 *
 * Cambios incluidos (Fase 5):
 *   - El borrado de metadatos antiguos (.idx/.blm) solo ocurre tras
 *     verificar éxito absoluto de brs_write_index_segment. Si falla,
 *     se aborta la purga para evitar dejar el repo invisible.
 *   - La rutina cleanup destruye explícitamente index_mtx tras
 *     sincronizar todos los hilos, evitando bloqueos huérfanos
 *     en ejecuciones posteriores tras Ctrl+C.
 *   - Los workers verifican cancel_flag para ser interrumpibles
 *     de forma segura.
 */

#include "brs_repo_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "brs_vfs_context.h"

/* ============================================================================
 * Parseo de timestamp de snapshot
 * Formato del nombre: [label_]YYYY-MM-DD_HH-MM-SS_<hash8>.snap
 *           o bien: [label_]YYYY-MM-DD_HH-MM-SS-mmm_<hash8>.snap
 * Devuelve epoch (segundos desde 1970) o 0 si no se pudo parsear.
 * ==========================================================================*/
static uint64_t parse_snapshot_timestamp(const char *name)
{
    if (!name) return 0;

    const char *dot = strrchr(name, '.');
    if (!dot) return 0;

    const char *hash_sep = NULL;
    for (const char *p = dot - 1; p >= name; --p) {
        if (*p == '_') {
            hash_sep = p;
            break;
        }
    }

    if (!hash_sep || (size_t)(hash_sep - name) < 19) return 0;

    const char *ts19 = hash_sep - 19;
    const char *ts23 = hash_sep - 23;

    int Y, M, D, h, m, s;
    int parsed = 0;

    if ((size_t)(hash_sep - name) >= 23) {
        if (sscanf(ts23, "%d-%d-%d_%d-%d-%d-%*d",
                   &Y, &M, &D, &h, &m, &s) == 6) {
            parsed = 1;
        }
    }

    if (!parsed) {
        if (sscanf(ts19, "%d-%d-%d_%d-%d-%d",
                   &Y, &M, &D, &h, &m, &s) == 6) {
            parsed = 1;
        }
    }

    if (!parsed) return 0;

    if (Y < 1970 || Y > 2100) return 0;
    if (M < 1 || M > 12)     return 0;
    if (D < 1 || D > 31)     return 0;
    if (h < 0 || h > 23)     return 0;
    if (m < 0 || m > 59)     return 0;
    if (s < 0 || s > 60)     return 0;

    static const int dim[12] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    int dmax = dim[M - 1];
    if (M == 2 && ((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0)) {
        dmax = 29;
    }
    if (D > dmax) return 0;

    int y = Y;
    unsigned mm = (unsigned)M;
    y -= (mm <= 2);

    int era      = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mm > 2 ? mm - 3 : mm + 9) + 2) / 5
                   + (unsigned)D - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;

    return (uint64_t)(days * 86400 + h * 3600 + m * 60 + s);
}

/* ============================================================================
 * Orden de snapshots para prune
 * ==========================================================================*/
static uint64_t key_iso_week(uint64_t epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm_buf;
    char buf[16];

    if (!localtime_r(&t, &tm_buf)) return 0;
    if (strftime(buf, sizeof buf, "%G%V", &tm_buf) == 0) return 0;

    return strtoull(buf, NULL, 10);
}

static uint64_t key_day(uint64_t epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm_buf;

    if (!localtime_r(&t, &tm_buf)) return 0;

    return (uint64_t)tm_buf.tm_year * 10000 +
           (uint64_t)tm_buf.tm_mon * 100 +
           (uint64_t)tm_buf.tm_mday;
}

static uint64_t key_month(uint64_t epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm_buf;

    if (!localtime_r(&t, &tm_buf)) return 0;

    return (uint64_t)tm_buf.tm_year * 12 + (uint64_t)tm_buf.tm_mon;
}

static uint64_t key_year(uint64_t epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm_buf;

    if (!localtime_r(&t, &tm_buf)) return 0;

    return (uint64_t)tm_buf.tm_year;
}

static void keep_by_period(int *keep, const uint64_t *ts, size_t n,
                           int max_periods, uint64_t (*period_key)(uint64_t))
{
    if (max_periods <= 0 || n == 0) return;

    int count = 0;
    uint64_t prev = UINT64_MAX;

    for (size_t idx = n; idx > 0 && count < max_periods; --idx) {
        uint64_t pk = period_key(ts[idx - 1]);
        if (pk != prev) {
            keep[idx - 1] = 1;
            prev = pk;
            count++;
        }
    }
}

static const uint64_t *g_sort_ts;
static char          **g_sort_names;

static int snap_order_cmp(const void *a, const void *b)
{
    size_t ia = *(const size_t *)a;
    size_t ib = *(const size_t *)b;

    if (g_sort_ts[ia] != g_sort_ts[ib]) {
        return g_sort_ts[ia] < g_sort_ts[ib] ? -1 : 1;
    }

    return strcmp(g_sort_names[ia], g_sort_names[ib]);
}

/* ============================================================================
 * Prune Concurrente: Estructuras y Workers
 * ==========================================================================*/
typedef struct {
    char path[BRS_PATH_MAX];
    uint64_t old_pack_id;
    size_t old_size;
} PruneTask;

typedef struct {
    PruneTask *tasks;
    atomic_size_t *next_task;
    size_t *task_count;
    BrsIndexMap *live;
    BrsIndexMap *new_index;
    pthread_mutex_t *index_mtx;
    BrsRepoConfig *cfg;
    BrsSecureKey *key;
    _Atomic uint64_t *next_pack_id;
    _Atomic uint64_t *bytes_reclaimed;
    _Atomic uint64_t *packs_rewritten;
    _Atomic int *cancel_flag;
    const char *repo_path;
} PruneWorkerCtx;

/* ============================================================================
 * FASE 5: Worker interrumpible con cancel_flag.
 * ==========================================================================*/
static void *prune_worker(void *arg)
{
    PruneWorkerCtx *ctx = (PruneWorkerCtx *)arg;

    BrsBuffer data;
    brs_buffer_init(&data);

    while (1) {
        /* FASE 5: verificación de cancelación en cada iteración. */
        if (check_cancel(ctx->cancel_flag))
            break;

        size_t t = atomic_fetch_add_explicit(ctx->next_task, 1,
                                             memory_order_relaxed);
        if (t >= *ctx->task_count)
            break;

        const PruneTask *task = &ctx->tasks[t];

        if (brs_read_file(task->path, &data) != 0)
            continue;

        uint64_t pid;
        BrsPackEntry *entries = NULL;
        uint32_t count = 0;

        if (brs_parse_pack(data.data, data.size, &pid, &entries, &count) != 0) {
            free(entries);
            continue;
        }

        uint64_t new_pid = atomic_fetch_add_explicit(ctx->next_pack_id, 1,
                                                     memory_order_relaxed);

        BrsPackWriter writer;

        if (brs_pack_writer_init(&writer, ctx->repo_path, new_pid) != 0) {
            free(entries);
            continue;
        }

        for (uint32_t e = 0; e < count; ++e) {
            /* FASE 5: verificación de cancelación por chunk. */
            if (check_cancel(ctx->cancel_flag))
                break;

            if (!brs_index_map_get(ctx->live, &entries[e].chunk_id))
                continue;

            if (entries[e].offset > data.size ||
                entries[e].comp_size > data.size - entries[e].offset)
                continue;

            const uint8_t *raw_src = data.data + entries[e].offset;

            brs_pack_writer_add_chunk(&writer,
                                      &entries[e].chunk_id,
                                      raw_src,
                                      entries[e].comp_size,
                                      entries[e].uncomp_size,
                                      entries[e].flags);
        }

        BrsPackEntry *ne = NULL;
        uint32_t nec = 0;
        uint64_t npid = 0;

        if (brs_pack_writer_finalize(&writer, &ne, &nec, &npid) == 0) {
            size_t new_size = 36 + 28 + (uint64_t)nec * 33;
            for (uint32_t e = 0; e < nec; ++e)
                new_size += ne[e].comp_size;

            if (task->old_size > new_size)
                atomic_fetch_add(ctx->bytes_reclaimed,
                                 task->old_size - new_size);

            pthread_mutex_lock(ctx->index_mtx);
            for (uint32_t e = 0; e < nec; ++e) {
                BrsChunkLocation loc;
                loc.pack_id = npid;
                loc.offset = ne[e].offset;
                loc.comp_size = ne[e].comp_size;
                loc.uncomp_size = ne[e].uncomp_size;
                loc.flags = ne[e].flags;
                brs_index_map_put(ctx->new_index, &ne[e].chunk_id, &loc);
            }
            pthread_mutex_unlock(ctx->index_mtx);

            atomic_fetch_add(ctx->packs_rewritten, 1);
            free(ne);
        } else {
            /* Si finalize falló, abortar para no dejar writer abierto. */
            brs_pack_writer_abort(&writer);
        }

        free(entries);
    }

    brs_buffer_free(&data);
    return NULL;
}

/* FIX A4: helper seguro para trackear packs viejos */
static int prune_track_old_pack(char ***arr, size_t *count, size_t *cap,
                                const char *path)
{
    if (!arr || !count || !cap || !path)
        return -1;

    if (*count == *cap) {
        size_t ncap = *cap ? *cap * 2 : 16;

        char **tmp = (char **)realloc(*arr, ncap * sizeof(char *));
        if (!tmp) {
            fprintf(stderr,
                    "WARNING: cannot track old pack for deletion: %s\n",
                    path);
            return -1;
        }

        *arr = tmp;
        *cap = ncap;
    }

    char *dup = strdup(path);
    if (!dup) {
        fprintf(stderr,
                "WARNING: cannot track old pack for deletion: %s\n",
                path);
        return -1;
    }

    (*arr)[(*count)++] = dup;
    return 0;
}

/* ============================================================================
 * Función principal
 * ==========================================================================*/
#include "brs_lock.h"

int brs_repo_prune(const char *repo_path, int keep_last, int keep_daily,
                   int keep_weekly, int keep_monthly, int keep_yearly,
                   int dry_run,
                   BrsProgressCallback cb, void *cb_user,
                   _Atomic int *cancel_flag)
{
    if (!repo_path) return 1;

    BrsRepoLock repo_lock;

    brs_repo_lock_init(&repo_lock);

    if (brs_repo_lock_acquire(&repo_lock, repo_path, "prune") != 0) {
        return 1;
    }

    int rc = 1;

    BrsRepoConfig cfg;
    BrsSecureKey key;
    int key_valid = 0;

    BrsDirList snaps;
    int snaps_loaded = 0;

    uint64_t *ts = NULL;
    size_t *order = NULL;
    int *keep = NULL;

    BrsIndexMap live, full_index, new_index;
    int live_init = 0, full_init = 0, new_init = 0;

    BrsBuffer data;
    brs_buffer_init(&data);

    /* ====================================================================
     * FASE 5: Estado de hilos y mutex a nivel de función para que
     * cleanup pueda sincronizarlos y destruirlos en cualquier ruta
     * de salida, incluyendo cancelación asíncrona por Ctrl+C.
     * ================================================================== */
    pthread_mutex_t index_mtx = PTHREAD_MUTEX_INITIALIZER;
    int index_mtx_init = 1;

    pthread_t *threads = NULL;
    int threads_started = 0;
    PruneWorkerCtx *wctx = NULL;
    PruneTask *tasks = NULL;
    size_t task_count = 0;
    atomic_size_t next_task;
    atomic_init(&next_task, 0);

    brs_repo_config_default(&cfg);

    if (brs_load_config(repo_path, &cfg) != 0) {
        fprintf(stderr, "cannot load repo config\n");
        goto cleanup;
    }

    if (cfg.encrypted) {
        if (derive_repo_key(&cfg, &key) != 0)
            goto cleanup;
        key_valid = 1;
    }

    char snaps_dir[BRS_PATH_MAX];
    if (brs_path_join(snaps_dir, sizeof snaps_dir, repo_path,
                      "snapshots") != 0)
        goto cleanup;

    if (brs_list_dir(snaps_dir, &snaps) != 0) {
        fprintf(stderr, "no snapshots directory\n");
        goto cleanup;
    }
    snaps_loaded = 1;

    size_t total = 0;
    for (size_t i = 0; i < snaps.count; ++i) {
        size_t ln = strlen(snaps.names[i]);
        if (ln > 5 && strcmp(snaps.names[i] + ln - 5, ".snap") == 0)
            total++;
    }

    ts = (uint64_t *)calloc(total ? total : 1, sizeof(uint64_t));
    order = (size_t *)calloc(total ? total : 1, sizeof(size_t));
    keep = (int *)calloc(total ? total : 1, sizeof(int));

    if (!ts || !order || !keep)
        goto cleanup;

    {
        size_t k = 0;
        for (size_t i = 0; i < snaps.count; ++i) {
            size_t ln = strlen(snaps.names[i]);
            if (ln > 5 && strcmp(snaps.names[i] + ln - 5, ".snap") == 0) {
                ts[k] = parse_snapshot_timestamp(snaps.names[i]);
                order[k] = i;
                k++;
            }
        }
    }

    {
        size_t *idx = (size_t *)malloc((total ? total : 1) * sizeof(size_t));
        if (!idx) goto cleanup;

        for (size_t i = 0; i < total; ++i) idx[i] = i;

        uint64_t *ts2 = (uint64_t *)malloc((total ? total : 1) * sizeof(uint64_t));
        char **names2 = (char **)malloc((total ? total : 1) * sizeof(char *));

        if (!ts2 || !names2) {
            free(idx); free(ts2); free(names2);
            goto cleanup;
        }

        for (size_t i = 0; i < total; ++i) {
            ts2[i] = ts[i];
            names2[i] = snaps.names[order[i]];
        }

        g_sort_ts = ts2;
        g_sort_names = names2;

        qsort(idx, total, sizeof *idx, snap_order_cmp);

        uint64_t *ts_sorted = (uint64_t *)malloc((total ? total : 1) * sizeof(uint64_t));
        char **names_sorted = (char **)malloc((total ? total : 1) * sizeof(char *));

        if (!ts_sorted || !names_sorted) {
            free(idx); free(ts2); free(names2);
            free(ts_sorted); free(names_sorted);
            goto cleanup;
        }

        for (size_t i = 0; i < total; ++i) {
            ts_sorted[i] = ts2[idx[i]];
            names_sorted[i] = names2[idx[i]];
        }

        free(ts);
        ts = ts_sorted;

        for (size_t i = 0; i < total; ++i)
            snaps.names[order[i]] = names_sorted[i];

        free(idx); free(ts2); free(names2); free(names_sorted);
    }

    if (keep_last < 0) {
        for (size_t i = 0; i < total; ++i) keep[i] = 1;
    } else {
        size_t last_count = (size_t)keep_last;
        if (last_count > total) last_count = total;

        for (size_t i = total - last_count; i < total; ++i)
            keep[i] = 1;

        keep_by_period(keep, ts, total, keep_daily, key_day);
        keep_by_period(keep, ts, total, keep_weekly, key_iso_week);
        keep_by_period(keep, ts, total, keep_monthly, key_month);
        keep_by_period(keep, ts, total, keep_yearly, key_year);
    }

    size_t to_delete = 0;
    for (size_t i = 0; i < total; ++i)
        if (!keep[i]) to_delete++;

    if (brs_index_map_init(&live, 1024) != 0)
        goto cleanup;
    live_init = 1;

    {
        uint64_t kept_total = total - to_delete, kept_done = 0;

        report_progress(cb, cb_user, "scan", 0, kept_total);

        for (size_t i = 0; i < total; ++i) {
            if (!keep[i]) continue;

            kept_done++;
            report_progress(cb, cb_user, "scan", kept_done, kept_total);

            if (check_cancel(cancel_flag)) {
                printf("cancelled\n");
                rc = 2;
                goto cleanup;
            }

            char sp[BRS_PATH_MAX];

            if (brs_path_join(sp, sizeof sp, snaps_dir,
                              snaps.names[i]) != 0)
                continue;

            BrsBuffer sdata;
            brs_buffer_init(&sdata);

            if (brs_read_file(sp, &sdata) != 0) {
                brs_buffer_free(&sdata);
                continue;
            }

            BrsParsedSnapshot snap;
            brs_parsed_snapshot_init(&snap);

            if (brs_parse_snapshot(sdata.data, sdata.size, &snap,
                                   cfg.encrypted ? &key : NULL,
                                   cfg.cipher_algo) == 0) {
                for (uint64_t e = 0; e < snap.entries_len; ++e) {
                    const BrsManifestEntry *se = &snap.entries[e];
                    if (se->type != BRS_FILETYPE_FILE) continue;

                    BrsChunkLocation zloc;
                    memset(&zloc, 0, sizeof zloc);

                    for (uint32_t c = 0; c < se->chunk_count; ++c)
                        brs_index_map_put(&live, &se->chunks[c], &zloc);

                    if (se->flags & BRS_FLAG_DELTA) {
                        for (uint32_t c = 0; c < se->delta_source_count; ++c)
                            brs_index_map_put(&live,
                                              &se->delta_source_chunks[c],
                                              &zloc);
                    }
                }

                brs_parsed_snapshot_free(&snap);
            } else {
                /* FIX P1: snapshot retenido corrupto -> abortar prune. */
                fprintf(stderr,
                    "error: cannot parse retained snapshot '%s' -- "
                    "aborting prune to prevent data loss\n",
                    snaps.names[i]);

                brs_parsed_snapshot_free(&snap);
                brs_buffer_free(&sdata);
                rc = 1;
                goto cleanup;
            }

            brs_buffer_free(&sdata);
        }
    }

    if (dry_run) {
        printf("dry run:\n");
        printf("  snapshots total:  %llu\n", (unsigned long long)total);
        printf("  snapshots keep:   %llu\n",
               (unsigned long long)(total - to_delete));
        printf("  snapshots delete: %llu\n", (unsigned long long)to_delete);
        printf("  live chunks:      %llu\n",
               (unsigned long long)brs_index_map_count(&live));

        for (size_t i = 0; i < total; ++i)
            if (!keep[i])
                printf("  would delete: %s\n", snaps.names[i]);

        rc = 0;
        goto cleanup;
    }

    uint64_t snaps_deleted = 0;

    for (size_t i = 0; i < total; ++i) {
        if (keep[i]) continue;

        if (check_cancel(cancel_flag)) {
            printf("cancelled\n");
            rc = 2;
            goto cleanup;
        }

        char sp[BRS_PATH_MAX];

        if (brs_path_join(sp, sizeof sp, snaps_dir, snaps.names[i]) == 0 &&
            brs_remove_file(sp) == 0) {
            snaps_deleted++;
        }
    }

    uint64_t open_id = 0;
    if (brs_read_open_pack_id(repo_path, &open_id) == 0)
        brs_remove_open_pack_id(repo_path);

    if (brs_index_map_init(&full_index, 1024) != 0)
        goto cleanup;
    full_init = 1;

    (void)brs_load_all_indexes(repo_path, &full_index);

    uint64_t chunks_before = brs_index_map_count(&full_index);

    if (brs_index_map_init(&new_index, chunks_before + 16) != 0)
        goto cleanup;
    new_init = 1;

    char packs_dir[BRS_PATH_MAX];
    if (brs_path_join(packs_dir, sizeof packs_dir, repo_path,
                      "packs") != 0)
        goto cleanup;

    BrsDirList pack_list;

    if (brs_list_dir(packs_dir, &pack_list) == 0) {
        brs_dir_list_sort(&pack_list);

        uint64_t packs_before = 0, packs_after = 0, packs_skipped = 0;
        _Atomic uint64_t bytes_reclaimed = 0;
        _Atomic uint64_t packs_rewritten = 0;
        _Atomic uint64_t next_pack_id = brs_now_ns();

        for (size_t i = 0; i < pack_list.count; ++i) {
            size_t ln = strlen(pack_list.names[i]);
            if (ln > 5 && strcmp(pack_list.names[i] + ln - 5, ".pack") == 0)
                packs_before++;
        }

        tasks = (PruneTask *)calloc(packs_before ? packs_before : 1,
                                    sizeof(PruneTask));
        if (!tasks) {
            brs_dir_list_free(&pack_list);
            goto cleanup;
        }

        task_count = 0;
        atomic_store(&next_task, 0);

        char **old_packs_to_delete = NULL;
        size_t old_packs_count = 0, old_packs_cap = 0;

        uint64_t rewrite_done = 0;

        report_progress(cb, cb_user, "rewrite", 0, packs_before);

        for (size_t i = 0; i < pack_list.count; ++i) {
            const char *nm = pack_list.names[i];
            size_t ln = strlen(nm);

            if (ln <= 5 || strcmp(nm + ln - 5, ".pack") != 0)
                continue;

            rewrite_done++;
            report_progress(cb, cb_user, "rewrite", rewrite_done,
                            packs_before);

            if (check_cancel(cancel_flag))
                break;

            char pp[BRS_PATH_MAX];

            if (brs_path_join(pp, sizeof pp, packs_dir, nm) != 0)
                continue;

            BrsBuffer header_data;
            brs_buffer_init(&header_data);

            if (brs_read_file(pp, &header_data) != 0) {
                fprintf(stderr,
                        "WARNING: Cannot read pack %s, marking for deletion\n",
                        nm);
                brs_buffer_free(&header_data);
                prune_track_old_pack(&old_packs_to_delete,
                                     &old_packs_count,
                                     &old_packs_cap, pp);
                continue;
            }

            uint64_t pack_id = 0;
            BrsPackEntry *entries = NULL;
            uint32_t count = 0;

            if (brs_parse_pack(header_data.data, header_data.size,
                               &pack_id, &entries, &count) != 0) {
                fprintf(stderr,
                        "WARNING: Cannot parse pack %s, "
                        "marking for deletion\n", nm);
                brs_buffer_free(&header_data);
                free(entries);
                prune_track_old_pack(&old_packs_to_delete,
                                     &old_packs_count,
                                     &old_packs_cap, pp);
                continue;
            }

            size_t dead_bytes = 0;
            uint32_t live_count = 0;

            for (uint32_t e = 0; e < count; ++e) {
                if (brs_index_map_get(&live, &entries[e].chunk_id))
                    live_count++;
                else
                    dead_bytes += entries[e].comp_size;
            }

            if (live_count == count) {
                for (uint32_t e = 0; e < count; ++e) {
                    BrsChunkLocation loc;
                    loc.pack_id = pack_id;
                    loc.offset = entries[e].offset;
                    loc.comp_size = entries[e].comp_size;
                    loc.uncomp_size = entries[e].uncomp_size;
                    loc.flags = entries[e].flags;
                    brs_index_map_put(&new_index,
                                      &entries[e].chunk_id, &loc);
                }
                packs_after++;
            } else if (live_count == 0) {
                atomic_fetch_add(&bytes_reclaimed, header_data.size);
                prune_track_old_pack(&old_packs_to_delete,
                                     &old_packs_count,
                                     &old_packs_cap, pp);
            } else {
                double dead_ratio =
                    (double)dead_bytes / (double)header_data.size;

                if (dead_ratio < BRS_REWRITE_THRESHOLD) {
                    for (uint32_t e = 0; e < count; ++e) {
                        if (!brs_index_map_get(&live,
                                               &entries[e].chunk_id))
                            continue;

                        BrsChunkLocation loc;
                        loc.pack_id = pack_id;
                        loc.offset = entries[e].offset;
                        loc.comp_size = entries[e].comp_size;
                        loc.uncomp_size = entries[e].uncomp_size;
                        loc.flags = entries[e].flags;
                        brs_index_map_put(&new_index,
                                          &entries[e].chunk_id, &loc);
                    }
                    packs_after++;
                    packs_skipped++;
                } else {
                    strncpy(tasks[task_count].path, pp,
                            sizeof(tasks[task_count].path) - 1);
                    tasks[task_count].path[
                        sizeof(tasks[task_count].path) - 1] = '\0';
                    tasks[task_count].old_pack_id = pack_id;
                    tasks[task_count].old_size = header_data.size;
                    task_count++;

                    prune_track_old_pack(&old_packs_to_delete,
                                         &old_packs_count,
                                         &old_packs_cap, pp);
                }
            }

            brs_buffer_free(&header_data);
            free(entries);
        }
        /* ==================================================================
         * INICIAR WORKERS
         * ================================================================== */
        int effective_workers = BRS_NUM_WORKERS;

        if (brs_vfs_context_get() != NULL) {
            effective_workers = 1;
        }

        threads = (pthread_t *)calloc((size_t)effective_workers,
                                      sizeof(pthread_t));
        wctx = (PruneWorkerCtx *)calloc((size_t)effective_workers,
                                        sizeof(PruneWorkerCtx));

        if (!threads || !wctx) {
            free(threads);
            free(wctx);
            threads = NULL;
            wctx = NULL;
            free(tasks);
            tasks = NULL;
            brs_dir_list_free(&pack_list);
            goto cleanup;
        }

        threads_started = 0;

        for (int w = 0; w < effective_workers; ++w) {
            wctx[w].tasks = tasks;
            wctx[w].next_task = &next_task;
            wctx[w].task_count = &task_count;
            wctx[w].live = &live;
            wctx[w].new_index = &new_index;
            wctx[w].index_mtx = &index_mtx;
            wctx[w].cfg = &cfg;
            wctx[w].key = cfg.encrypted ? &key : NULL;
            wctx[w].next_pack_id = &next_pack_id;
            wctx[w].bytes_reclaimed = &bytes_reclaimed;
            wctx[w].packs_rewritten = &packs_rewritten;
            wctx[w].cancel_flag = cancel_flag;
            wctx[w].repo_path = repo_path;

            if (pthread_create(&threads[w], NULL, prune_worker,
                               &wctx[w]) == 0) {
                threads_started++;
            }
        }

        /* Sincronizar todos los hilos antes de continuar. */
        for (int w = 0; w < threads_started; ++w)
            pthread_join(threads[w], NULL);

        threads_started = 0;

        packs_after += atomic_load(&packs_rewritten);

        free(tasks);
        tasks = NULL;

        free(threads);
        threads = NULL;

        free(wctx);
        wctx = NULL;

        for (size_t i = 0; i < old_packs_count; ++i) {
            brs_remove_file(old_packs_to_delete[i]);
            free(old_packs_to_delete[i]);
        }
        free(old_packs_to_delete);
        old_packs_to_delete = NULL;

        brs_dir_list_free(&pack_list);

        /* ==============================================================
         * FASE 5 TAREA 1: Escritura del índice nuevo con verificación
         * de éxito absoluto.
         *
         * Si brs_write_index_segment falla (disco lleno, corte de red),
         * se ABORTA la purga inmediatamente. No se borran índices ni
         * blooms antiguos. El repositorio permanece visible y
         * consistente.
         * ============================================================ */
        char new_idx_name[40] = {0};
        int index_written = 0;

        if (brs_index_map_count(&new_index) > 0) {
            uint64_t seg = brs_now_ns();

            if (brs_write_index_segment(repo_path, seg, &new_index) == 0) {
                snprintf(new_idx_name, sizeof new_idx_name,
                         "%llu.idx", (unsigned long long)seg);
                index_written = 1;
            } else {
                /* FASE 5: fallo absoluto de escritura del índice.
                 * Abortar la purga para no dejar el repositorio
                 * invisible. */
                fprintf(stderr,
                        "ERROR: Failed to write new index segment. "
                        "Aborting prune to prevent invisible repository.\n");
                rc = 1;
                goto cleanup;
            }
        }

        /* ==============================================================
         * FASE 5 TAREA 1: Borrar índices y blooms antiguos SOLO si
         * el nuevo se escribió con éxito rotundo (index_written == 1).
         * ============================================================ */
        char idx_dir[BRS_PATH_MAX];

        if (brs_path_join(idx_dir, sizeof idx_dir, repo_path,
                          "index") == 0) {
            BrsDirList idxl;

            if (brs_list_dir(idx_dir, &idxl) == 0) {
                for (size_t i = 0; i < idxl.count; ++i) {
                    size_t il = strlen(idxl.names[i]);

                    int is_idx = (il > 4 &&
                                  strcmp(idxl.names[i] + il - 4,
                                         ".idx") == 0);
                    int is_blm = (il > 4 &&
                                  strcmp(idxl.names[i] + il - 4,
                                         ".blm") == 0);

                    if (!is_idx && !is_blm)
                        continue;

                    if (is_blm) {
                        char base[40];
                        snprintf(base, sizeof base, "%.*s",
                                 (int)(il - 4), idxl.names[i]);

                        if (index_written && new_idx_name[0]) {
                            char nbase[40];
                            snprintf(nbase, sizeof nbase, "%.*s",
                                     (int)(strlen(new_idx_name) - 4),
                                     new_idx_name);

                            if (strcmp(base, nbase) == 0)
                                continue;
                        }
                    } else {
                        if (index_written && new_idx_name[0] &&
                            strcmp(idxl.names[i], new_idx_name) == 0) {
                            continue;
                        }
                    }

                    char ip[BRS_PATH_MAX];

                    if (brs_path_join(ip, sizeof ip, idx_dir,
                                      idxl.names[i]) == 0) {
                        brs_remove_file(ip);
                    }
                }

                brs_dir_list_free(&idxl);
            }
        }

        BrsCacheMap fcache;

        if (brs_cache_map_init(&fcache, 1024) == 0) {
            if (brs_load_file_cache(repo_path, &fcache) == 0) {
                char **to_remove = NULL;
                size_t rem_count = 0, rem_cap = 0;

                size_t cursor = 0;
                const BrsFileCacheEntry *ce;

                while ((ce = brs_cache_map_next(&fcache, &cursor)) != NULL) {
                    int valid = 1;

                    for (uint32_t c = 0; c < ce->chunk_count; ++c) {
                        if (!brs_index_map_get(&new_index,
                                               &ce->chunks[c])) {
                            valid = 0;
                            break;
                        }
                    }

                    if (!valid) {
                        if (rem_count == rem_cap) {
                            size_t nc = rem_cap ? rem_cap * 2 : 16;
                            char **nr = (char **)realloc(to_remove,
                                                         nc * sizeof *nr);
                            if (!nr) break;
                            to_remove = nr;
                            rem_cap = nc;
                        }

                        to_remove[rem_count] = strdup(ce->path);
                        if (to_remove[rem_count])
                            rem_count++;
                    }
                }

                int changed = 0;

                for (size_t i = 0; i < rem_count; ++i) {
                    if (brs_cache_map_remove(&fcache, to_remove[i]) == 0)
                        changed = 1;
                    free(to_remove[i]);
                }

                free(to_remove);

                if (changed)
                    brs_save_file_cache(repo_path, &fcache);
            }

            brs_cache_map_free(&fcache);
        }

        uint64_t chunks_after = brs_index_map_count(&new_index);
        uint64_t chunks_freed = chunks_before > chunks_after
                              ? chunks_before - chunks_after : 0;

        char reclaimed_str[64];
        brs_format_bytes(atomic_load(&bytes_reclaimed),
                         reclaimed_str, sizeof reclaimed_str);

        printf("snapshots: %llu total, %llu deleted, %llu kept\n",
               (unsigned long long)total,
               (unsigned long long)snaps_deleted,
               (unsigned long long)(total - snaps_deleted));

        printf("chunks:    %llu before, %llu after (%llu freed)\n",
               (unsigned long long)chunks_before,
               (unsigned long long)chunks_after,
               (unsigned long long)chunks_freed);

        printf("packs:     %llu before, %llu after",
               (unsigned long long)packs_before,
               (unsigned long long)packs_after);

        if (packs_skipped > 0)
            printf(" (%llu below rewrite threshold)",
                   (unsigned long long)packs_skipped);

        printf("\n");
        printf("reclaimed: %s\n", reclaimed_str);
    }

    rc = 0;

cleanup:
    /* ====================================================================
     * FASE 5 TAREA 2: Limpieza centralizada con destrucción explícita
     * del mutex compartido.
     *
     * Si ocurre cancelación asíncrona (Ctrl+C vía cancel_flag), los
     * hilos de prune_worker verifican el flag y terminan de forma
     * segura. Aquí se sincronizan los hilos pendientes y se destruye
     * index_mtx explícitamente, evitando bloqueos huérfanos en
     * ejecuciones posteriores.
     * ================================================================== */

    /* Sincronizar hilos pendientes si los hay. */
    if (threads != NULL && threads_started > 0) {
        for (int w = 0; w < threads_started; ++w)
            pthread_join(threads[w], NULL);
        threads_started = 0;
    }

    /* FASE 5: destrucción explícita del mutex tras sincronizar hilos. */
    if (index_mtx_init) {
        pthread_mutex_destroy(&index_mtx);
        index_mtx_init = 0;
    }

    /* Liberar recursos de hilos si aún no se liberaron. */
    if (threads != NULL) {
        free(threads);
        threads = NULL;
    }

    if (wctx != NULL) {
        free(wctx);
        wctx = NULL;
    }

    if (tasks != NULL) {
        free(tasks);
        tasks = NULL;
    }

    brs_buffer_free(&data);

    if (new_init)
        brs_index_map_free(&new_index);

    if (full_init)
        brs_index_map_free(&full_index);

    if (live_init)
        brs_index_map_free(&live);

    free(keep);
    free(order);
    free(ts);

    if (snaps_loaded)
        brs_dir_list_free(&snaps);

    if (key_valid)
        brs_secure_key_wipe(&key);

    brs_repo_lock_release(&repo_lock);

    return rc;
}
