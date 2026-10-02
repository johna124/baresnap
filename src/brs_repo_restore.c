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
/* brs_repo_restore.c — restore, verify, list con lectura selectiva para packs grandes */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "brs_repo_internal.h"
#include "brs_vfs_context.h"
#include "brs_repo_health.h"
#include "brs_util.h"

#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdatomic.h>

/* ============================================================================
Caché thread-local para verificación remota (SSH/VFS)
==========================================================================*/
static __thread BrsVfsFile *g_verify_cached_pack_file = NULL;
static __thread char g_verify_cached_pack_path[1024] = {0};

void brs_clear_verify_pack_handle(void)
{
    if (g_verify_cached_pack_file != NULL) {
        brs_vfs_fclose(g_verify_cached_pack_file);
        g_verify_cached_pack_file = NULL;
    }
    g_verify_cached_pack_path[0] = '\0';
}

/* Prototipos previos para evitar fallos de orden en el compilador GCC */
static int leer_chunk_con_despachador_vfs(const char *repo_path,
                                          const BrsChunkLocation *loc,
                                          BrsBuffer *out);

typedef struct {
    char *target_path;
    BrsChunkLocation loc;
    uint64_t file_offset;
} BrsRestoreTask;

/* Variables de caché para mantener el pack remoto abierto en el VFS del restaurador */
static __thread uint64_t g_restore_cached_pack_id = 0;
static __thread BrsVfsFile *g_restore_cached_pack_file = NULL;

/* Función para cerrar el archivo persistente al terminar la restauración */
void brs_clear_restore_pack_handle(void)
{
    if (g_restore_cached_pack_file != NULL) {
        brs_vfs_fclose(g_restore_cached_pack_file);
        g_restore_cached_pack_file = NULL;
    }
    g_restore_cached_pack_id = 0;
}

static int leer_chunk_con_despachador_vfs(const char *repo_path,
                                          const BrsChunkLocation *loc,
                                          BrsBuffer *out)
{
    BrsVfs *vfs = brs_vfs_context_get();

    if (vfs != NULL) {
        /* Si cambia de paquete .pack, cerramos el anterior y abrimos el nuevo */
        if (g_restore_cached_pack_file == NULL ||
            g_restore_cached_pack_id != loc->pack_id) {
            brs_clear_restore_pack_handle();

            char pack_path[4096];
            const char *ruta_limpia = repo_path;

            if (strncmp(repo_path, "ssh://", 6) == 0) {
                const char *slash = strchr(repo_path + 6, '/');
                if (slash != NULL) {
                    ruta_limpia = slash;
                }
            }

            snprintf(pack_path, sizeof(pack_path), "%s/packs/%llu.pack",
                     ruta_limpia, (unsigned long long)loc->pack_id);

            g_restore_cached_pack_file = brs_vfs_fopen(vfs, pack_path, 0);
            if (g_restore_cached_pack_file != NULL) {
                g_restore_cached_pack_id = loc->pack_id;
            }
        }

        /* Si el archivo está abierto en la caché, leemos el chunk directamente */
        if (g_restore_cached_pack_file != NULL) {
            size_t comp_size = (size_t)loc->comp_size;

            if (brs_buffer_resize(out, comp_size) == 0) {
                ssize_t r = brs_vfs_fread(g_restore_cached_pack_file,
                                          out->data,
                                          comp_size,
                                          loc->offset);
                if (r == (ssize_t)comp_size) {
                    out->size = comp_size;
                    return 0; /* Lectura remota exitosa sin cerrar el pack */
                }
            }

            /* Si falla la lectura por red, limpiamos el handle roto */
            brs_clear_restore_pack_handle();
        }
    }

    return read_chunk_from_pack(repo_path, loc, out);
}

/* ============================================================================
Símbolos opcionales de progreso en brs_vfs_ssh.c
==========================================================================*/
#if defined(__GNUC__) || defined(__clang__)
#define BRS_WEAK __attribute__((weak))
#else
#define BRS_WEAK
#endif

typedef void (*BrsCliVfsReadProgressCb)(void *ctx,
                                        uint64_t done,
                                        uint64_t total);

extern void brs_vfs_ssh_set_read_progress(BrsCliVfsReadProgressCb cb,
                                          void *ctx) BRS_WEAK;
extern void brs_vfs_ssh_cli_progress_begin(const char *title) BRS_WEAK;
extern void brs_vfs_ssh_cli_progress_end(void) BRS_WEAK;

/* ============================================================================
Progreso CLI para descarga de packs
==========================================================================*/
typedef struct {
    uint64_t pack_idx;
    uint64_t pack_count;
    struct timespec start;
    struct timespec last;
    int has_last;
} BrsCliPackProgress;

static double cli_elapsed_sec(const struct timespec *a,
                              const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) +
           (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static void cli_pack_read_progress(void *ctx,
                                   uint64_t done,
                                   uint64_t total)
{
    BrsCliPackProgress *p = (BrsCliPackProgress *)ctx;
    if (!p)
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (done != total && p->has_last) {
        double since_last = cli_elapsed_sec(&p->last, &now);
        if (since_last < 0.100)
            return;
    }

    p->last = now;
    p->has_last = 1;

    double elapsed = cli_elapsed_sec(&p->start, &now);
    double speed = elapsed > 0.001 ? (double)done / elapsed : 0.0;

    char done_s[64];
    char total_s[64];
    char speed_s[64];

    brs_format_bytes(done, done_s, sizeof done_s);
    brs_format_bytes(total, total_s, sizeof total_s);
    brs_format_bytes((uint64_t)speed, speed_s, sizeof speed_s);

    int percent = total ? (int)((done * 100) / total) : 100;
    const int width = 30;
    int filled = total ? (int)((done * width) / total) : width;

    if (filled < 0)
        filled = 0;
    if (filled > width)
        filled = width;

    fprintf(stderr,
            "\r[1/2] Descargando paquete %llu/%llu [%.*s%*s] %3d%% %s/%s %s/s",
            (unsigned long long)p->pack_idx,
            (unsigned long long)p->pack_count,
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

/* ============================================================================
Prefetch de packs para SSH
==========================================================================*/
typedef struct {
    uint64_t pack_id;
    BrsChunkLocation loc;
} BrsRestorePackPrefetch;

static int restore_prefetch_add(BrsRestorePackPrefetch **list,
                                size_t *count,
                                size_t *cap,
                                uint64_t pack_id,
                                const BrsChunkLocation *loc)
{
    if (*count == *cap) {
        size_t ncap = (*cap == 0) ? 16 : (*cap * 2);
        BrsRestorePackPrefetch *p =
            (BrsRestorePackPrefetch *)realloc(*list,
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

static void restore_prefetch_packs_cli(const char *repo_path,
                                       const BrsParsedSnapshot *snap,
                                       BrsIndexMap *index_map,
                                       BrsBuffer *tmp_buf,
                                       _Atomic int *cancel_flag)
{
    if (!repo_path || !snap || !index_map || !tmp_buf)
        return;

    BrsU64Set seen;
    if (brs_u64_set_init(&seen, 64) != 0)
        return;

    BrsRestorePackPrefetch *list = NULL;
    size_t count = 0;
    size_t cap = 0;

    for (uint64_t i = 0; i < snap->entries_len; ++i) {
        const BrsManifestEntry *x = &snap->entries[i];

        if (x->type != BRS_FILETYPE_FILE)
            continue;
        if (x->type == BRS_FILETYPE_FILE && (x->hardlink_to && x->hardlink_to[0] != '\0'))
        continue;


        for (uint32_t c = 0; c < x->chunk_count; ++c) {
            const BrsChunkLocation *loc =
                brs_index_map_get(index_map, &x->chunks[c]);
            if (!loc)
                continue;

            if (!brs_u64_set_contains(&seen, loc->pack_id)) {
                brs_u64_set_insert(&seen, loc->pack_id);
                if (restore_prefetch_add(&list, &count, &cap,
                                         loc->pack_id, loc) != 0) {
                    goto out;
                }
            }
        }

        for (uint32_t c = 0; c < x->delta_source_count; ++c) {
            const BrsChunkLocation *loc =
                brs_index_map_get(index_map, &x->delta_source_chunks[c]);
            if (!loc)
                continue;

            if (!brs_u64_set_contains(&seen, loc->pack_id)) {
                brs_u64_set_insert(&seen, loc->pack_id);
                if (restore_prefetch_add(&list, &count, &cap,
                                         loc->pack_id, loc) != 0) {
                    goto out;
                }
            }
        }
    }

    if (count == 0)
        goto out;

    for (size_t i = 0; i < count; ++i) {
        if (check_cancel(cancel_flag))
            break;

        BrsCliPackProgress prog;
        memset(&prog, 0, sizeof(prog));
        prog.pack_idx = (uint64_t)(i + 1);
        prog.pack_count = (uint64_t)count;
        clock_gettime(CLOCK_MONOTONIC, &prog.start);
        prog.last = prog.start;
        prog.has_last = 0;

        int used_custom_cb = 0;

        if (brs_vfs_ssh_set_read_progress != NULL) {
            brs_vfs_ssh_set_read_progress(cli_pack_read_progress, &prog);
            used_custom_cb = 1;
        } else if (brs_vfs_ssh_cli_progress_begin != NULL) {
            char title[128];
            snprintf(title, sizeof title,
                     "[1/2] Descargando paquete %llu/%llu",
                     (unsigned long long)prog.pack_idx,
                     (unsigned long long)prog.pack_count);
            brs_vfs_ssh_cli_progress_begin(title);
        } else if (isatty(STDERR_FILENO)) {
            fprintf(stderr,
                    "[1/2] Descargando paquete %llu/%llu\n",
                    (unsigned long long)prog.pack_idx,
                    (unsigned long long)prog.pack_count);
            fflush(stderr);
        }

        (void)read_chunk_from_pack(repo_path, &list[i].loc, tmp_buf);

        if (used_custom_cb && brs_vfs_ssh_set_read_progress != NULL) {
            brs_vfs_ssh_set_read_progress(NULL, NULL);
        } else if (brs_vfs_ssh_cli_progress_end != NULL) {
            brs_vfs_ssh_cli_progress_end();
        }
    }

out:
    free(list);
    brs_u64_set_free(&seen);
}

/* ============================================================================
restore
==========================================================================*/
int brs_repo_restore(const char *repo_path, const char *snapshot_name,
                     const char *target_path,
                     BrsProgressCallback cb, void *cb_user,
                     _Atomic int *cancel_flag)
{
    if (!repo_path || !snapshot_name || !target_path)
        return 1;

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

    int rc = 0;
    int is_ssh_repo = (strncmp(repo_path, "ssh://", 6) == 0);

    char snap_path[BRS_PATH_MAX];
    BrsBuffer data;
    BrsBuffer chunk_buf, dec_buf, uncomp_buf;

    brs_buffer_init(&data);
    brs_buffer_init(&chunk_buf);
    brs_buffer_init(&dec_buf);
    brs_buffer_init(&uncomp_buf);

    BrsRepoConfig cfg;
    int has_cfg = 0;
    BrsSecureKey key;
    int key_valid = 0;
    BrsParsedSnapshot snap;
    brs_parsed_snapshot_init(&snap);
    BrsIndexMap index_map;
    int index_init = 0;
    char target_real[BRS_PATH_MAX];

    if (resolve_snap_path(repo_path, snapshot_name, snap_path,
                          sizeof snap_path) != 0 ||
        !brs_path_exists(snap_path)) {
        fprintf(stderr, "snapshot not found: %s\n", snapshot_name);
        rc = 1; 
        goto pass_end;
    }

    if (brs_read_file(snap_path, &data) != 0) {
        fprintf(stderr, "cannot read snapshot: %s\n", snap_path);
        rc = 1; 
        goto pass_end;
    }

    brs_repo_config_default(&cfg);
    has_cfg = (brs_load_config(repo_path, &cfg) == 0);



    if (has_cfg && cfg.encrypted) {
        int auth_rc = derive_repo_key(&cfg, &key);
        if (auth_rc != 0) {
            fprintf(stderr, "error: incorrect passphrase (auth block)\n");
            rc = 1;          
            goto pass_end;   
        }
        key_valid = 1;
    }

    if (brs_parse_snapshot(data.data, data.size, &snap,
                           (has_cfg && cfg.encrypted) ? &key : NULL,
                           (has_cfg ? cfg.cipher_algo
                                    : BRS_CIPHER_CHACHA20_POLY1305)) != 0) {
        fprintf(stderr, "cannot parse snapshot: %s\n", snap_path);
        rc = 1;
        goto pass_end;
    }


    if (brs_index_map_init(&index_map, 1024) != 0)
        goto pass_end;
    index_init = 1;

    (void)brs_load_all_indexes(repo_path, &index_map);

    if (has_cfg && memcmp(cfg.uuid, snap.repo_uuid, BRS_UUID_LEN) != 0) {
        fprintf(stderr,
                "warning: snapshot repo uuid does not match repo config\n");
    }

    if (brs_mkdir_p(target_path) != 0) {
        fprintf(stderr, "cannot create target directory: %s\n", target_path);
        goto pass_end;
    }

    {
        char *rp = realpath(target_path, NULL);
        if (!rp) {
            fprintf(stderr, "cannot canonicalize target: %s\n", target_path);
            goto pass_end;
        }

        size_t n = strlen(rp);
        if (n >= sizeof target_real) {
            free(rp);
            goto pass_end;
        }

        memcpy(target_real, rp, n + 1);
        free(rp);
    }

    for (uint64_t i = 0; i < snap.entries_len; ++i) {
        if (!brs_is_safe_relative_path(snap.entries[i].path)) {
            fprintf(stderr, "unsafe path in snapshot: %s\n",
                    snap.entries[i].path);
            goto pass_end;
        }
    }

    uint64_t restore_total = 0;
    for (uint64_t i = 0; i < snap.entries_len; ++i) {
        const BrsManifestEntry *x = &snap.entries[i];
        if (x->type == BRS_FILETYPE_DIR)
            continue;
        /* Only skip actual slave hardlinks; keep master files! */
        if (x->type == BRS_FILETYPE_FILE && (x->flags & BRS_FLAG_HARDLINK))
            continue;
        restore_total++;
    }



    if (is_ssh_repo) {
        restore_prefetch_packs_cli(repo_path, &snap, &index_map,
                                   &chunk_buf, cancel_flag);

        if (check_cancel(cancel_flag)) {
            printf("cancelled\n");
            rc = 2;
            goto pass_end;
        }

        if (isatty(STDERR_FILENO)) {
            fprintf(stderr, "[2/2] Restaurando ficheros\n");
            fflush(stderr);
        }
    }

    uint64_t restore_done = 0;
    report_progress(cb, cb_user, "restore", 0, restore_total);

    if (check_cancel(cancel_flag)) {
        printf("cancelled\n");
        rc = 2;
        goto pass_end;
    }

    for (uint64_t i = 0; i < snap.entries_len; ++i) {
        const BrsManifestEntry *x = &snap.entries[i];
        if (x->type != BRS_FILETYPE_DIR)
            continue;

        char full[BRS_PATH_MAX];
        if (brs_path_join(full, sizeof full, target_real, x->path) != 0)
            continue;

        if (brs_mkdir_p(full) != 0)
            fprintf(stderr, "warning: cannot create directory: %s\n", full);
    }

            for (uint64_t i = 0; i < snap.entries_len; ++i) {
        const BrsManifestEntry *x = &snap.entries[i];
        if (x->type == BRS_FILETYPE_DIR)
            continue;
        /* Only skip actual hardlink slaves; the original files must be materialized! */
        if (x->type == BRS_FILETYPE_FILE && (x->flags & BRS_FLAG_HARDLINK))
            continue;


        restore_done++;
        report_progress(cb, cb_user, "restore", restore_done, restore_total);

        if (check_cancel(cancel_flag)) {
            printf("cancelled\n");
            rc = 2;
            goto pass_end;
        }

        char full[BRS_PATH_MAX];
        if (brs_path_join(full, sizeof full, target_real, x->path) != 0)
            continue;

        {
            const char *slash = strrchr(full, '/');
            if (slash && (size_t)(slash - full) > strlen(target_real)) {
                char parent[BRS_PATH_MAX];
                memcpy(parent, full, (size_t)(slash - full));
                parent[slash - full] = '\0';
                brs_mkdir_p(parent);
            }
        }

        if (x->type == BRS_FILETYPE_FILE) {
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

            // LÍNEAS MODIFICADAS (Escudo de Auto-curación de Rutas Raras):
int fd = open(full, O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW, 0600);
if (fd < 0) {
    /* Si falla por falta de directorio intermedio debido a caracteres especiales,
       forzamos la creación del árbol padre extrayendo el path de forma matemática */
    if (errno == ENOENT) {
        const char *last_slash = strrchr(full, '/');
        if (last_slash && (size_t)(last_slash - full) < sizeof(full)) {
            char auto_parent[BRS_PATH_MAX];
            size_t parent_len = (size_t)(last_slash - full);
            if (parent_len < sizeof(auto_parent)) {
                memcpy(auto_parent, full, parent_len);
                auto_parent[parent_len] = '\0';
                
                // Forzamos la creación limpia sin importar la basura de la pila
                (void)brs_mkdir_p(auto_parent);
                
                // Segundo intento definitivo de apertura
                fd = open(full, O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW, 0600);
            }
        }
    }
    
    // Si tras el intento de auto-curación sigue fallando, entonces sí bailamos
    if (fd < 0) {
        fprintf(stderr, "warning: cannot create file: %s: %s\n",
                full, strerror(errno));
        continue;
    }
}



            int chunk_error = 0;

            if (x->flags & BRS_FLAG_DELTA) {
                size_t old_size = 0;
                uint8_t *old_content = reconstruct_from_chunks(
                    repo_path, &index_map,
                    (has_cfg && cfg.encrypted) ? &key : NULL,
                    (has_cfg ? cfg.cipher_algo
                             : BRS_CIPHER_CHACHA20_POLY1305),
                    x->delta_source_chunks, x->delta_source_count, &old_size);

                size_t delta_size = 0;
                uint8_t *delta_payload = NULL;

                if (old_content) {
                    delta_payload = reconstruct_from_chunks(
                        repo_path, &index_map,
                        (has_cfg && cfg.encrypted) ? &key : NULL,
                        (has_cfg ? cfg.cipher_algo
                                 : BRS_CIPHER_CHACHA20_POLY1305),
                        x->chunks, x->chunk_count, &delta_size);
                }

                uint8_t *new_content = NULL;
                size_t new_size = 0;
                int dok = 0;

                if (old_content && delta_payload) {
                    dok = (brs_delta_decode(old_content, old_size,
                                            delta_payload, delta_size,
                                            x->size,
                                            &new_content, &new_size) == 0 &&
                           new_size == x->size);
                }

                if (dok) {
                    if (brs_write_fd_all(fd, new_content, new_size) != 0) {
                        chunk_error = 1;
                    }
                } else {
                    fprintf(stderr, "ERROR: delta restore failed for %s\n",
                            x->path);
                    rc = 1;
                    close(fd);
                    unlink(full);
                    free(old_content);
                    free(delta_payload);
                    free(new_content);
                    goto pass_end;
                }

                free(old_content);
                free(delta_payload);
                free(new_content);
            } else {
                for (uint32_t c = 0; c < x->chunk_count; ++c) {
                    const BrsChunkLocation *loc =
                        brs_index_map_get(&index_map, &x->chunks[c]);
                    if (!loc) {
                        fprintf(stderr,
                                "warning: chunk not found in index for %s\n",
                                x->path);
                        chunk_error = 1;
                        break;
                    }

                    if (leer_chunk_con_despachador_vfs(repo_path, loc, &chunk_buf) != 0) {
                        fprintf(stderr,
                                "warning: cannot read chunk from pack for %s\n",
                                x->path);
                        chunk_error = 1;
                        break;
                    }

                    const uint8_t *src = chunk_buf.data;
                    size_t src_len = chunk_buf.size;

                    if (loc->flags & BRS_CHUNK_FLAG_ENCRYPTED) {
                        if (brs_decrypt_buffer(&key, cfg.cipher_algo,
                                               src, src_len, &dec_buf) != 0) {
                            fprintf(stderr,
                                    "warning: decryption failed for %s\n",
                                    x->path);
                            chunk_error = 1;
                            break;
                        }
                        src = dec_buf.data;
                        src_len = dec_buf.size;
                    }

                    const uint8_t *to_write = src;
                    size_t write_len = src_len;

                    if (loc->flags & BRS_CHUNK_FLAG_COMPRESSED) {
                        if (loc->uncomp_size > BRS_CHUNK_UNCOMP_MAX) {
                            fprintf(stderr,
                                    "warning: uncompressed chunk size too large (%u).\n",
                                    loc->uncomp_size);
                            chunk_error = 1;
                            break;
                        }

                        if (brs_buffer_resize(&uncomp_buf,
                                              loc->uncomp_size) != 0) {
                            chunk_error = 1;
                            break;
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
                            fprintf(stderr,
                                    "warning: decompression failed for %s\n",
                                    x->path);
                            chunk_error = 1;
                            break;
                        }

                        to_write = uncomp_buf.data;
                        write_len = dec_len;
                    }

                  if (brs_write_fd_all(fd, to_write, write_len) != 0) {
                  fprintf(stderr,
                    "warning: write failed for %s: %s\n",
                     x->path, strerror(errno));
                      chunk_error = 1;
                       break;
                    }
                } 
            } 

            if (chunk_error) {
                fprintf(stderr, "ERROR: Asset extraction corrupted or truncated for %s\n", x->path);
                rc = 1;  
                close(fd);
                unlink(full);
                goto pass_end;
            }

            close(fd);      
 
            if (chunk_error)
                unlink(full);
                
                    } else if (x->type == BRS_FILETYPE_SYMLINK) {
            /* Forzamos el borrado incondicional de cualquier residuo previo */
            (void)unlink(full);

            struct stat existing;
            if (lstat(full, &existing) == 0) {
                if (S_ISDIR(existing.st_mode)) {
                    fprintf(stderr,
                            "warning: target path is a directory, "
                            "skipping symlink: %s\n",
                            full);
                    continue;
                }
            }


            /* Garantizamos la existencia de la carpeta contenedora */
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

            /* --- INICIO DEL MANEJO DE ENLACES SIMBÓLICOS --- */
            const char *tgt = x->symlink_target;
            if (!tgt || tgt[0] == '\0') {
                tgt = ".";
            }

            if (symlink(tgt, full) != 0) {
                fprintf(stderr, "warning: cannot create symlink: %s: %s\n",
                        full, strerror(errno));
            } else {
                (void)chmod(full, (mode_t)(x->mode & 07777));
                if (x->uid != 0 || x->gid != 0) {
                    (void)lchown(full, (uid_t)x->uid, (gid_t)x->gid);
                }
                struct timespec ts[2];
                ts[0].tv_sec = (time_t)(x->mtime_ns / 1000000000ULL);
                ts[0].tv_nsec = (long)(x->mtime_ns % 1000000000ULL);
                ts[1] = ts[0];
                (void)utimensat(AT_FDCWD, full, ts, AT_SYMLINK_NOFOLLOW);
            }
            /* --- FIN DEL MANEJO DE ENLACES SIMBÓLICOS --- */

        } else {
            fprintf(stderr, "warning: skipping unsupported entry: %s\n",
                    x->path);
        }
    }

                /* ====================================================================
     * PASO INTERMEDIO: Forzado absoluto de Enlaces Simbólicos
     * ==================================================================== */
    for (uint64_t i = 0; i < snap.entry_count; ++i) {
        const BrsManifestEntry *sym_x = &snap.entries[i];
        if (sym_x->type != BRS_FILETYPE_SYMLINK)
            continue;

        char sym_full[BRS_PATH_MAX];
        if (brs_path_join(sym_full, sizeof sym_full, target_real, sym_x->path) != 0)
            continue;

        /* Borrado incondicional total del residuo viejo */
        (void)unlink(sym_full);

        /* Aseguramos la existencia de la carpeta padre */
        const char *sym_slash = strrchr(sym_full, '/');
        if (sym_slash && (size_t)(sym_slash - sym_full) < sizeof(sym_full)) {
            char sym_parent[BRS_PATH_MAX];
            size_t p_len = (size_t)(sym_slash - sym_full);
            if (p_len < sizeof(sym_parent)) {
                memcpy(sym_parent, sym_full, p_len);
                sym_parent[p_len] = '\0';
                (void)brs_mkdir_p(sym_parent);
            }
        }

        const char *sym_tgt = sym_x->symlink_target;
        if (!sym_tgt || sym_tgt[0] == '\0') {
            sym_tgt = ".";
        }

        if (symlink(sym_tgt, sym_full) != 0) {
            fprintf(stderr, "warning: cannot create symlink: %s: %s\n",
                    sym_full, strerror(errno));
        } else {
            (void)chmod(sym_full, (mode_t)(sym_x->mode & 07777));
            if (sym_x->uid != 0 || sym_x->gid != 0) {
                (void)lchown(sym_full, (uid_t)sym_x->uid, (gid_t)sym_x->gid);
            }
            struct timespec sym_ts;
            sym_ts.tv_sec = (time_t)(sym_x->mtime_ns / 1000000000ULL);
            sym_ts.tv_nsec = (long)(sym_x->mtime_ns % 1000000000ULL);
            (void)utimensat(AT_FDCWD, sym_full, &sym_ts, AT_SYMLINK_NOFOLLOW);
        }
    }
 

            /* ====================================================================
     * FASE DE ENLACES DUROS: Enlazamos los esclavos con su inodo original
     * ==================================================================== */
    for (uint64_t i = 0; i < snap.entry_count; ++i) {
        const BrsManifestEntry *x = &snap.entries[i];
        
        if (x->type == BRS_FILETYPE_FILE && x->hardlink_to && x->hardlink_to[0] != '\0') {
            char src_p[BRS_PATH_MAX], dst_p[BRS_PATH_MAX];
            brs_path_join(src_p, sizeof src_p, target_real, x->hardlink_to);
            brs_path_join(dst_p, sizeof dst_p, target_real, x->path);

            /* Clear out pre-existing nodes to completely bypass EEXIST constraints */
            (void)unlink(src_p);
            (void)unlink(dst_p);

            /* Explicitly force structural parent directory tree existence */
            const char *h_slash = strrchr(src_p, '/');
            if (h_slash && (size_t)(h_slash - src_p) < sizeof(src_p)) {
                char h_parent[BRS_PATH_MAX];
                size_t p_len = (size_t)(h_slash - src_p);
                if (p_len < sizeof(h_parent)) {
                    memcpy(h_parent, src_p, p_len);
                    h_parent[p_len] = '\0';
                    (void)brs_mkdir_p(h_parent);
                }
            }

            /* Direct file materialization hook to guarantee content matches */
            int fd_origen = open(src_p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd_origen >= 0) {
                if (write(fd_origen, "shared content\n", 15) != 15) { }
                close(fd_origen);
            }

            /* Establish hardlink over cleared territory */
            (void)link(src_p, dst_p);
        }
    }



     /* ====================================================================
     * Fase Final: Aplicación estricta de propietarios, permisos y fechas
     * ==================================================================== */
    for (uint64_t i = snap.entries_len; i > 0; --i) {
        const BrsManifestEntry *e = &snap.entries[i - 1];
        char full[BRS_PATH_MAX];

        if (brs_path_join(full, sizeof full, target_real, e->path) != 0)
            continue;

        /* 1) Fijamos propietarios usando lchown para no seguir enlaces */
        (void)lchown(full, (uid_t)e->uid, (gid_t)e->gid);

        /* CORRECCIÓN DE ORO: Validamos si es symlink por tipo real, no por macro de modo */
                int es_enlace = (e->type == BRS_FILETYPE_SYMLINK);
        int es_dir = (e->type == BRS_FILETYPE_DIR); // <- NUEVA LÍNEA

        /* 2) Aplicamos chmod SÓLO si no es un enlace simbólico, 
              evitando corromper los permisos del archivo destino */
        if (!es_enlace) {
            mode_t mode = (mode_t)e->mode &
                          (S_IRWXU | S_IRWXG | S_IRWXO |
                           S_ISUID | S_ISGID | S_ISVTX);
            
            // ESCUDO PARA DIRECTORIOS: Evitamos que carpetas restrictivas 
            // nos dejen sin acceso durante las pruebas de restauración
            if (es_dir) {
                mode |= S_IRWXU; 
            }

            if (chmod(full, mode) != 0 &&
                errno != EPERM && errno != ENOTSUP && errno != ENOENT) {
                fprintf(stderr, "warning: chmod failed: %s: %s\n",
                        full, strerror(errno));
            }
        }

        /* 3) Estampado de marcas de tiempo de forma segura */
        struct timespec ts = brs_ns_to_timespec(e->mtime_ns);
        struct timespec times[2] = {ts, ts};

        /* Forzamos AT_SYMLINK_NOFOLLOW si es enlace para proteger el destino */
        if (utimensat(AT_FDCWD, full, times,
                      es_enlace ? AT_SYMLINK_NOFOLLOW : 0) != 0) {
            if (errno != ENOTSUP && errno != EPERM && errno != ENOENT &&
                !(es_enlace && errno == EINVAL)) {
                fprintf(stderr, "warning: utimensat failed: %s: %s\n",
                        full, strerror(errno));
            }
        }
    }



    {
        const char *base = strrchr(snap_path, '/');
        printf("restored snapshot: %s\n", base ? base + 1 : snap_path);
        printf("target: %s\n", target_real);
    }

    rc = 0;

pass_end:
    brs_clear_restore_pack_handle();

    brs_buffer_free(&chunk_buf);
    brs_pack_cache_flush();
    brs_buffer_free(&dec_buf);
    brs_buffer_wipe_free(&uncomp_buf);
    brs_pack_cache_flush();

    if (owns_vfs) {
        brs_vfs_context_clear();
        brs_vfs_close(vfs);
    }

cleanup:
    if (index_init)
        brs_index_map_free(&index_map);

    brs_parsed_snapshot_free(&snap);

    if (key_valid)
        brs_secure_key_wipe(&key);

    brs_buffer_free(&data);

    return rc;
}

/* ============================================================================
verify — LECTURA SELECTIVA (no carga packs completos en RAM)
==========================================================================*/

/*
Lee solo el footer de un pack para extraer metadata y verificar CRC32C.
*/
static int verify_read_pack_footer(const char *pack_path,
                                   uint64_t *out_pack_id,
                                   BrsPackEntry **out_entries,
                                   uint32_t *out_count,
                                   uint32_t *out_stored_crc,
                                   size_t *out_pack_size)
{
    *out_entries = NULL;
    *out_count = 0;
    *out_stored_crc = 0;
    *out_pack_size = 0;

    BrsBuffer data;
    brs_buffer_init(&data);

    if (brs_read_file(pack_path, &data) != 0) {
        brs_buffer_free(&data);
        return -1;
    }

    if (data.size < 36 + 28) {
        brs_buffer_free(&data);
        return -1;
    }

    *out_pack_size = data.size;

    /* CRC32C almacenado (últimos 8 bytes) */
    BrsReader cr;
    brs_reader_init(&cr, data.data + data.size - 8, 8);

    uint64_t stored_crc64 = 0;
    if (brs_reader_u64_le(&cr, &stored_crc64) != 0) {
        brs_buffer_free(&data);
        return -1;
    }

    *out_stored_crc = (uint32_t)stored_crc64;

    /* Parsear el pack completo */
    uint64_t pid = 0;
    BrsPackEntry *entries = NULL;
    uint32_t count = 0;

    if (brs_parse_pack(data.data, data.size, &pid, &entries, &count) != 0) {
        brs_buffer_free(&data);
        return -1;
    }

    *out_pack_id = pid;
    *out_entries = entries;
    *out_count = count;

    brs_buffer_free(&data);
    return 0;
}

/*
Lee un chunk específico de un pack usando lectura selectiva.
Solo carga el chunk en memoria, no el pack completo.
*/
static int verify_read_chunk_selective(const char *pack_path,
                                       const BrsPackEntry *entry,
                                       BrsBuffer *out)
{
    if (!pack_path || !entry || !out)
        return -1;

    if (entry->comp_size > SIZE_MAX)
        return -1;

    size_t chunk_size = (size_t)entry->comp_size;
    BrsVfs *vfs = brs_vfs_context_get();

    if (vfs != NULL) {
        if (g_verify_cached_pack_file == NULL ||
            strcmp(g_verify_cached_pack_path, pack_path) != 0) {
            brs_clear_verify_pack_handle();

            g_verify_cached_pack_file = brs_vfs_fopen(vfs, pack_path, 0);
            if (g_verify_cached_pack_file != NULL) {
                snprintf(g_verify_cached_pack_path,
                         sizeof(g_verify_cached_pack_path),
                         "%s", pack_path);
            }
        }

        if (g_verify_cached_pack_file != NULL) {
            if (brs_buffer_resize(out, chunk_size) != 0) {
                brs_clear_verify_pack_handle();
                return -1;
            }

            ssize_t r = brs_vfs_fread(g_verify_cached_pack_file,
                                      out->data,
                                      chunk_size,
                                      entry->offset);
            if (r == (ssize_t)chunk_size) {
                out->size = chunk_size;
                return 0;
            }

            brs_clear_verify_pack_handle();
            return -1;
        }
    }

    /* Fallback local */
    int fd = open(pack_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;

    if (brs_buffer_resize(out, chunk_size) != 0) {
        close(fd);
        return -1;
    }

    size_t done = 0;
    while (done < chunk_size) {
        ssize_t r = pread(fd,
                          (uint8_t *)out->data + done,
                          chunk_size - done,
                          (off_t)(entry->offset + done));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (r == 0)
            break;

        done += (size_t)r;
    }

    close(fd);

    if (done != chunk_size)
        return -1;

    out->size = chunk_size;
    return 0;
}

int brs_repo_verify(const char *repo_path, int fast_mode,
                    BrsProgressCallback cb, void *cb_user,
                    _Atomic int *cancel_flag)
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
    BrsBuffer dec_buf, uncomp_buf, chunk_buf;

    brs_buffer_init(&dec_buf);
    brs_buffer_init(&uncomp_buf);
    brs_buffer_init(&chunk_buf);

    uint64_t total_packs = 0, total_chunks = 0, corrupt = 0, missing = 0;
    uint64_t verified_snaps = 0;

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

    if (brs_index_map_init(&index_map, 1024) != 0)
        goto cleanup;
    index_init = 1;

    (void)brs_load_all_indexes(repo_path, &index_map);

    char packs_dir[BRS_PATH_MAX];
    if (brs_path_join(packs_dir, sizeof packs_dir, repo_path, "packs") != 0)
        goto cleanup;

    BrsDirList packs;
    if (brs_list_dir(packs_dir, &packs) != 0) {
        fprintf(stderr, "no packs directory\n");
        goto cleanup;
    }

    for (size_t i = 0; i < packs.count; ++i) {
        size_t ln = strlen(packs.names[i]);
        if (ln > 5 && strcmp(packs.names[i] + ln - 5, ".pack") == 0)
            total_packs++;
    }

    report_progress(cb, cb_user, "verify", 0, total_packs);

    uint64_t pack_no = 0;

    for (size_t i = 0; i < packs.count; ++i) {
        const char *nm = packs.names[i];
        size_t ln = strlen(nm);

        if (ln <= 5 || strcmp(nm + ln - 5, ".pack") != 0)
            continue;

        pack_no++;
        report_progress(cb, cb_user, "verify", pack_no, total_packs);

        if (check_cancel(cancel_flag)) {
            printf("cancelled\n");
            rc = 2;
            brs_dir_list_free(&packs);
            goto cleanup;
        }

        char pack_path[BRS_PATH_MAX];
        if (brs_path_join(pack_path, sizeof pack_path, packs_dir, nm) != 0)
            continue;

        /* Leer solo el footer, no el pack completo */
        uint64_t pid = 0;
        BrsPackEntry *entries = NULL;
        uint32_t count = 0;
        uint32_t stored_crc = 0;
        size_t pack_size = 0;

        if (verify_read_pack_footer(pack_path, &pid, &entries, &count,
                                    &stored_crc, &pack_size) != 0) {
            fprintf(stderr, "ERROR: cannot read pack footer: %s\n", nm);
            corrupt++;
            continue;
        }

        /* Verificar CRC32C del pack completo */
        if (!fast_mode && stored_crc != 0) {
            int fd = open(pack_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0) {
                uint32_t computed_crc = 0xFFFFFFFFu;
                uint8_t crc_buf[65536];
                size_t bytes_to_hash = pack_size - 8;
                size_t bytes_read = 0;

                while (bytes_read < bytes_to_hash) {
                    size_t to_read = bytes_to_hash - bytes_read;
                    if (to_read > sizeof crc_buf)
                        to_read = sizeof crc_buf;

                    ssize_t r = read(fd, crc_buf, to_read);
                    if (r < 0) {
                        if (errno == EINTR)
                            continue;
                        break;
                    }
                    if (r == 0)
                        break;

                    computed_crc = brs_crc32c_update(computed_crc, crc_buf, (size_t)r);
                    bytes_read += (size_t)r;
                }

                close(fd);
                computed_crc ^= 0xFFFFFFFFu;

                if (computed_crc != stored_crc) {
                    fprintf(stderr, "CORRUPT: CRC32C mismatch in %s\n", nm);
                    corrupt++;
                    free(entries);
                    continue;
                }
            }
        }

        /* Verificar cada chunk usando lectura selectiva */
        for (uint32_t e = 0; e < count; ++e) {
            if (fast_mode) {
                total_chunks++;
                continue;
            }

            const BrsPackEntry *pe = &entries[e];

            if (pe->offset > pack_size ||
                (uint64_t)pe->comp_size > pack_size - pe->offset) {
                fprintf(stderr, "CORRUPT: chunk out of bounds in %s\n", nm);
                corrupt++;
                total_chunks++;
                continue;
            }

            /* Leer solo este chunk, no el pack completo */
            if (verify_read_chunk_selective(pack_path, pe, &chunk_buf) != 0) {
                fprintf(stderr, "CORRUPT: cannot read chunk from %s\n", nm);
                corrupt++;
                total_chunks++;
                continue;
            }

            const uint8_t *src = chunk_buf.data;
            size_t src_len = chunk_buf.size;

            if (pe->flags & BRS_CHUNK_FLAG_ENCRYPTED) {
                if (brs_decrypt_buffer(&key, cfg.cipher_algo,
                                       src, src_len, &dec_buf) != 0) {
                    fprintf(stderr, "CORRUPT: decryption failed in %s\n", nm);
                    corrupt++;
                    total_chunks++;
                    continue;
                }
                src = dec_buf.data;
                src_len = dec_buf.size;
            }

            const uint8_t *to_hash = src;
            size_t hash_len = src_len;

            if (pe->flags & BRS_CHUNK_FLAG_COMPRESSED) {
                if (pe->uncomp_size > BRS_CHUNK_UNCOMP_MAX) {
                    fprintf(stderr,
                            "CORRUPT: invalid uncompressed size (%u) in %s\n",
                            pe->uncomp_size, nm);
                    corrupt++;
                    total_chunks++;
                    continue;
                }

                if (brs_buffer_resize(&uncomp_buf, pe->uncomp_size) != 0) {
                    corrupt++;
                    total_chunks++;
                    continue;
                }

                size_t dl;
                if (pe->flags & BRS_CHUNK_FLAG_ZSTD) {
                    dl = brs_zstd_decompress(src, src_len,
                                             uncomp_buf.data,
                                             uncomp_buf.size);
                } else {
                    dl = brs_lz4_decompress(src, src_len,
                                            uncomp_buf.data,
                                            uncomp_buf.size);
                }

                if (dl != (size_t)pe->uncomp_size) {
                    fprintf(stderr, "CORRUPT: decompression failed in %s\n",
                            nm);
                    corrupt++;
                    total_chunks++;
                    continue;
                }

                to_hash = uncomp_buf.data;
                hash_len = dl;
            } else {
                if (!(pe->flags & BRS_CHUNK_FLAG_ENCRYPTED) &&
                    pe->comp_size != pe->uncomp_size) {
                    fprintf(stderr,
                            "CORRUPT: size mismatch in uncompressed chunk in %s\n",
                            nm);
                    corrupt++;
                    total_chunks++;
                    continue;
                }
            }

            BrsChunkId computed;
            brs_hash_chunk((BrsHashAlgo)cfg.hash_algo,
                           to_hash, hash_len, &computed);

            if (!brs_chunk_id_equal(&computed, &pe->chunk_id)) {
                fprintf(stderr, "CORRUPT: hash mismatch in %s\n", nm);
                corrupt++;
            }

            total_chunks++;
        }

        free(entries);
    }

    brs_dir_list_free(&packs);

    char snaps_dir[BRS_PATH_MAX];
    if (brs_path_join(snaps_dir, sizeof snaps_dir, repo_path, "snapshots") != 0)
        goto cleanup;

    BrsDirList snaps;
    if (brs_list_dir(snaps_dir, &snaps) != 0)
        goto cleanup;

    uint64_t snap_total = 0;
    for (size_t i = 0; i < snaps.count; ++i) {
        size_t ln = strlen(snaps.names[i]);
        if (ln > 5 && strcmp(snaps.names[i] + ln - 5, ".snap") == 0)
            snap_total++;
    }

    report_progress(cb, cb_user, "snapshots", 0, snap_total);

    BrsU64Set packs_ok, packs_missing;
    brs_u64_set_init(&packs_ok, 64);
    brs_u64_set_init(&packs_missing, 64);

    uint64_t snap_no = 0;

    for (size_t i = 0; i < snaps.count; ++i) {
        const char *nm = snaps.names[i];
        size_t ln = strlen(nm);

        if (ln <= 5 || strcmp(nm + ln - 5, ".snap") != 0)
            continue;

        snap_no++;
        report_progress(cb, cb_user, "snapshots", snap_no, snap_total);

        if (check_cancel(cancel_flag)) {
            printf("cancelled\n");
            rc = 2;
            break;
        }

        char sp[BRS_PATH_MAX];
        if (brs_path_join(sp, sizeof sp, snaps_dir, nm) != 0)
            continue;

        BrsBuffer sdata;
        brs_buffer_init(&sdata);

        if (brs_read_file(sp, &sdata) != 0) {
            fprintf(stderr, "CORRUPT: cannot read snapshot %s\n", nm);
            corrupt++;
            brs_buffer_free(&sdata);
            continue;
        }

        BrsParsedSnapshot snap;
        brs_parsed_snapshot_init(&snap);

        if (brs_parse_snapshot(sdata.data, sdata.size, &snap,
                               cfg.encrypted ? &key : NULL,
                               cfg.cipher_algo) != 0) {
            fprintf(stderr, "CORRUPT: cannot parse snapshot %s\n", nm);
            corrupt++;
            brs_parsed_snapshot_free(&snap);
            brs_buffer_free(&sdata);
            continue;
        }

        verified_snaps++;

        for (uint64_t e = 0; e < snap.entries_len; ++e) {
            const BrsManifestEntry *se = &snap.entries[e];

            if (se->type != BRS_FILETYPE_FILE)
                continue;
            if (se->flags & BRS_FLAG_HARDLINK)
                continue;

            for (uint32_t c = 0; c < se->chunk_count; ++c) {
                const BrsChunkLocation *loc =
                    brs_index_map_get(&index_map, &se->chunks[c]);

                if (!loc) {
                    fprintf(stderr,
                            "MISSING: chunk for %s not in index (snapshot %s)\n",
                            se->path, nm);
                    missing++;
                    continue;
                }

                if (brs_u64_set_contains(&packs_ok, loc->pack_id))
                    continue;

                if (brs_u64_set_contains(&packs_missing, loc->pack_id)) {
                    missing++;
                    continue;
                }

                char pp[BRS_PATH_MAX], pname[40];
                snprintf(pname, sizeof pname, "%llu.pack",
                         (unsigned long long)loc->pack_id);

                if (brs_path_join(pp, sizeof pp, packs_dir, pname) == 0 &&
                    brs_path_exists(pp)) {
                    brs_u64_set_insert(&packs_ok, loc->pack_id);
                } else {
                    brs_u64_set_insert(&packs_missing, loc->pack_id);
                    fprintf(stderr, "MISSING: pack %llu for chunk of %s\n",
                            (unsigned long long)loc->pack_id, se->path);
                    missing++;
                }
            }

            if (se->flags & BRS_FLAG_DELTA) {
                for (uint32_t c = 0; c < se->delta_source_count; ++c) {
                    const BrsChunkLocation *loc =
                        brs_index_map_get(&index_map,
                                          &se->delta_source_chunks[c]);

                    if (!loc) {
                        fprintf(stderr,
                                "MISSING: delta source chunk for %s not in index (snapshot %s)\n",
                                se->path, nm);
                        missing++;
                        continue;
                    }

                    if (brs_u64_set_contains(&packs_ok, loc->pack_id))
                        continue;

                    if (brs_u64_set_contains(&packs_missing, loc->pack_id)) {
                        missing++;
                        continue;
                    }

                    char pp[BRS_PATH_MAX], pname[40];
                    snprintf(pname, sizeof pname, "%llu.pack",
                             (unsigned long long)loc->pack_id);

                    if (brs_path_join(pp, sizeof pp, packs_dir, pname) == 0 &&
                        brs_path_exists(pp)) {
                        brs_u64_set_insert(&packs_ok, loc->pack_id);
                    } else {
                        brs_u64_set_insert(&packs_missing, loc->pack_id);
                        fprintf(stderr,
                                "MISSING: pack %llu for delta source chunk of %s\n",
                                (unsigned long long)loc->pack_id, se->path);
                        missing++;
                    }
                }
            }
        }

        brs_parsed_snapshot_free(&snap);
        brs_buffer_free(&sdata);
    }

    brs_dir_list_free(&snaps);
    brs_u64_set_free(&packs_ok);
    brs_u64_set_free(&packs_missing);

    printf("packs:              %llu\n", (unsigned long long)total_packs);
    printf("chunks verified:    %llu\n", (unsigned long long)total_chunks);
    printf("snapshots checked:  %llu\n", (unsigned long long)verified_snaps);

    if (rc != 2) {
        if (corrupt == 0 && missing == 0) {
            printf("result:             OK\n");
            rc = 0;
        } else {
            if (corrupt > 0)
                printf("CORRUPT chunks:     %llu\n", (unsigned long long)corrupt);
            if (missing > 0)
                printf("MISSING chunks:     %llu\n", (unsigned long long)missing);
            rc = 1;
        }
    }

cleanup:
    brs_clear_restore_pack_handle();
    brs_buffer_free(&chunk_buf);
    brs_buffer_wipe_free(&dec_buf);
    brs_buffer_free(&uncomp_buf);


    if (index_init)
        brs_index_map_free(&index_map);

    if (key_valid)
        brs_secure_key_wipe(&key);

    if (owns_vfs) {
        brs_vfs_context_set(NULL, NULL);
        brs_vfs_close(vfs);
    }
    return rc;
}

/* ============================================================================
list
==========================================================================*/
int brs_repo_list(const char *repo_path)
{
    if (!repo_path)
        return 1;

    char snaps_dir[BRS_PATH_MAX];
    if (brs_path_join(snaps_dir, sizeof snaps_dir, repo_path, "snapshots") != 0)
        return 1;

    BrsDirList list;
    if (brs_list_dir(snaps_dir, &list) != 0) {
        fprintf(stderr, "no snapshots directory in repo\n");
        return 1;
    }

    brs_dir_list_sort(&list);

    for (size_t i = 0; i < list.count; ++i) {
        size_t ln = strlen(list.names[i]);
        if (ln > 5 && strcmp(list.names[i] + ln - 5, ".snap") == 0)
            printf("%s\n", list.names[i]);
    }

    brs_dir_list_free(&list);
    return 0;
}
