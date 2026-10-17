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
/* brs_repo_internal.h — Estructuras internas y declaraciones del repositorio */
#ifndef BRS_REPO_INTERNAL_H
#define BRS_REPO_INTERNAL_H

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "brs_zstd.h"
#include "brs_repo.h"
#include "brs_buffer.h"
#include "brs_cache.h"
#include "brs_chunker.h"
#include "brs_config.h"
#include "brs_crypto.h"
#include "brs_dir.h"
#include "brs_fsutil.h"
#include "brs_hash.h"
#include "brs_index.h"
#include "brs_lz4.h"
#include "brs_manifest.h"
#include "brs_pack.h"
#include "brs_pipeline.h"
#include "brs_pool.h"
#include "brs_spsc.h"
#include "brs_util.h"
#include "brs_delta.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "brs_types.h"
#include "brs_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

char *realpath(const char *path, char *resolved);

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
void brs_clear_cached_pack_handle(void);


#define BRS_NUM_WORKERS 2
#define BRS_REWRITE_THRESHOLD 0.25

/*
Tamaño dinámico del buffer de lectura.
Valor por defecto: 262144 bytes (256 KB).
Configurable vía CLI: --buffer-size=<val>[K|M]
*/
extern size_t brs_dyn_read_size;


/* Declaraciones de funciones internas del repositorio */
int brs_repo_init(const char *repo_path, int encrypt, int cipher_algo, int compression, int zstd_level, int hash_algo);
int brs_repo_create(const char *repo_path, const char *source, const char *label, int delta_binary, void (*progress_cb)(void *, const char *, uint64_t, uint64_t), void *progress_user, _Atomic int *cancel_flag);
int brs_repo_restore(const char *repo_path, const char *snapshot, const char *target, void (*progress_cb)(void *, const char *, uint64_t, uint64_t), void *progress_user, _Atomic int *cancel_flag);
int brs_repo_verify(const char *repo_path, int fast_mode, void (*progress_cb)(void *, const char *, uint64_t, uint64_t), void *progress_user, _Atomic int *cancel_flag);
int brs_repo_list(const char *repo_path);
int brs_repo_prune(const char *repo_path, int keep_last, int keep_daily, int keep_weekly, int keep_monthly, int keep_yearly, int dry_run, void (*progress_cb)(void *, const char *, uint64_t, uint64_t), void *progress_user, _Atomic int *cancel_flag);
int brs_repo_info(const char *repo_path);
int brs_repo_diff(const char *repo_path, const char *snap1, const char *snap2);
int brs_repo_ls(const char *repo_path, const char *snapshot_name, const char * const *prefixes, size_t n_prefixes, int long_fmt, int recursive, const char *match);
int brs_repo_extract(const char *repo_path, const char *snapshot_name, const char * const *paths, size_t n_paths, const char *target, void (*progress_cb)(void *, const char *, uint64_t, uint64_t), void *progress_user, _Atomic int *cancel_flag);
void report_progress(BrsProgressCallback cb, void *user, const char *phase, uint64_t cur, uint64_t total);
int check_cancel(_Atomic int *flag);
int derive_repo_key(const BrsRepoConfig *cfg, BrsSecureKey *key);
int resolve_snap_path(const char *repo, const char *arg, char *out, size_t out_size);
int copy_chunk_array(BrsChunkId **dst, uint32_t *dst_count, uint32_t *dst_cap, const BrsChunkId *src, uint32_t n);
int read_chunk_from_pack(const char *repo, const BrsChunkLocation *loc, BrsBuffer *out);
uint8_t *reconstruct_from_chunks(const char *repo_path,
                                 const BrsIndexMap *index_map,
                                 const BrsSecureKey *key,
                                 BrsCipherAlgo cipher_algo,
                                 const BrsChunkId *chunks,
                                 uint32_t chunk_count,
                                 size_t *out_size);
int find_previous_version(const BrsCacheMap *cache, const BrsIndexMap *existing_index, const char *path, BrsChunkId **out_chunks, uint32_t *out_count);
const BrsChunkLocation *lazy_get_main(const char *repo_path, BrsIndexMap *existing, BrsBloomSet *blooms, const BrsChunkId *id);
int brs_load_index_segment_from_file(const char *local_path, uint64_t seg_id, BrsIndexMap *map);
void brs_pack_cache_flush(void);
int brs_repo_search(const char *repo_path,
                    const char *snapshot_name,   /* NULL = todos los snapshots */
                    const char *pattern,
                    int use_glob,                /* 0 = substring, 1 = glob */
                    BrsProgressCallback cb,
                    void *cb_user,
                    _Atomic int *cancel_flag);

#define BRS_IO_RETRY_MAX 3
#define BRS_IO_RETRY_BASE_MS 50

#ifdef __cplusplus
}
#endif

#endif /* BRS_REPO_INTERNAL_H */
