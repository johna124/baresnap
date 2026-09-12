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

/* ============================================================================
 * Símbolos opcionales de progreso en brs_vfs_ssh.c
 * ==========================================================================*/
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
 * Progreso CLI para descarga de packs
 * ==========================================================================*/
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
 * Prefetch de packs para SSH
 * ==========================================================================*/
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
        if ((x->flags & BRS_FLAG_HARDLINK) || x->hardlink_to)
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
 * restore
 * ==========================================================================*/
int brs_repo_restore(const char *repo_path, const char *snapshot_name,
                     const char *target_path,
                     BrsProgressCallback cb, void *cb_user,
                     _Atomic int *cancel_flag)
{
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

    if (!repo_path || !snapshot_name || !target_path)
        return 1;

    int rc = 1;
    int is_ssh_repo = (strncmp(repo_path, "ssh://", 6) == 0);

    char snap_path[BRS_PATH_MAX];
    BrsBuffer data;
    brs_buffer_init(&data);

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
        fprintf(stderr, "cannot parse snapshot: %s\n", snap_path);
        goto cleanup;
    }

    if (brs_index_map_init(&index_map, 1024) != 0)
        goto cleanup;
    index_init = 1;

    (void)brs_load_all_indexes(repo_path, &index_map);

    if (has_cfg && memcmp(cfg.uuid, snap.repo_uuid, BRS_UUID_LEN) != 0) {
        fprintf(stderr,
                "warning: snapshot repo uuid does not match repo config\n");
    }

    if (brs_mkdir_p(target_path) != 0) {
        fprintf(stderr, "cannot create target directory: %s\n", target_path);
        goto cleanup;
    }

    {
        char *rp = realpath(target_path, NULL);
        if (!rp) {
            fprintf(stderr, "cannot canonicalize target: %s\n", target_path);
            goto cleanup;
        }
        size_t n = strlen(rp);
        if (n >= sizeof target_real) {
            free(rp);
            goto cleanup;
        }
        memcpy(target_real, rp, n + 1);
        free(rp);
    }

    for (uint64_t i = 0; i < snap.entries_len; ++i) {
        if (!brs_is_safe_relative_path(snap.entries[i].path)) {
            fprintf(stderr, "unsafe path in snapshot: %s\n",
                    snap.entries[i].path);
            goto cleanup;
        }
    }

    uint64_t restore_total = 0;
    for (uint64_t i = 0; i < snap.entries_len; ++i) {
        const BrsManifestEntry *x = &snap.entries[i];
        if (x->type == BRS_FILETYPE_DIR)
            continue;
        if (x->type == BRS_FILETYPE_FILE &&
            ((x->flags & BRS_FLAG_HARDLINK) || x->hardlink_to))
            continue;
        restore_total++;
    }

    BrsBuffer chunk_buf, dec_buf, uncomp_buf;
    brs_buffer_init(&chunk_buf);
    brs_buffer_init(&dec_buf);
    brs_buffer_init(&uncomp_buf);

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
        if (x->type == BRS_FILETYPE_FILE &&
            ((x->flags & BRS_FLAG_HARDLINK) || x->hardlink_to))
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

            int fd = open(full, O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW, 0600);
            if (fd < 0) {
                fprintf(stderr, "warning: cannot create file: %s: %s\n",
                        full, strerror(errno));
                continue;
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

                    if (read_chunk_from_pack(repo_path, loc, &chunk_buf) != 0) {
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

            close(fd);
            if (chunk_error)
                unlink(full);
        } else if (x->type == BRS_FILETYPE_SYMLINK) {
            struct stat existing;
            if (lstat(full, &existing) == 0) {
                if (S_ISDIR(existing.st_mode)) {
                    fprintf(stderr,
                            "warning: target path is a directory, "
                            "skipping symlink: %s\n",
                            full);
                    continue;
                }
                unlink(full);
            }

            if (symlink(x->symlink_target ? x->symlink_target : "", full) != 0) {
                fprintf(stderr, "warning: cannot create symlink: %s: %s\n",
                        full, strerror(errno));
            }
        } else {
            fprintf(stderr, "warning: skipping unsupported entry: %s\n",
                    x->path);
        }
    }

    for (uint64_t i = 0; i < snap.entries_len; ++i) {
        const BrsManifestEntry *x = &snap.entries[i];
        if (!(x->type == BRS_FILETYPE_FILE &&
              ((x->flags & BRS_FLAG_HARDLINK) || x->hardlink_to)))
            continue;

        if (check_cancel(cancel_flag)) {
            printf("cancelled\n");
            rc = 2;
            goto pass_end;
        }

        if (!x->hardlink_to || x->hardlink_to[0] == '\0') {
            fprintf(stderr, "ERROR: hardlink entry without target: %s\n",
                    x->path);
            goto pass_end;
        }

        if (!brs_is_safe_relative_path(x->hardlink_to)) {
            fprintf(stderr, "ERROR: unsafe hardlink target: %s\n",
                    x->hardlink_to);
            goto pass_end;
        }

        char full[BRS_PATH_MAX], link_to[BRS_PATH_MAX];
        if (brs_path_join(full, sizeof full, target_real, x->path) != 0)
            continue;
        if (brs_path_join(link_to, sizeof link_to, target_real,
                          x->hardlink_to) != 0)
            continue;

        struct stat existing;
        if (lstat(full, &existing) == 0) {
            if (S_ISDIR(existing.st_mode)) {
                fprintf(stderr,
                        "warning: target is a directory, skipping "
                        "hardlink: %s\n",
                        full);
                continue;
            }
            unlink(full);
        }

        if (link(link_to, full) != 0) {
            fprintf(stderr, "ERROR: cannot create hardlink: %s -> %s: %s\n",
                    full, x->hardlink_to, strerror(errno));
            goto pass_end;
        }
    }

    for (uint64_t i = snap.entries_len; i > 0; --i) {
        const BrsManifestEntry *e = &snap.entries[i - 1];
        char full[BRS_PATH_MAX];
        if (brs_path_join(full, sizeof full, target_real, e->path) != 0)
            continue;

        (void)lchown(full, (uid_t)e->uid, (gid_t)e->gid);

        if (!S_ISLNK(e->mode)) {
            mode_t mode = (mode_t)e->mode &
                          (S_IRWXU | S_IRWXG | S_IRWXO |
                           S_ISUID | S_ISGID | S_ISVTX);
            if (chmod(full, mode) != 0 &&
                errno != EPERM && errno != ENOTSUP && errno != ENOENT) {
                fprintf(stderr, "warning: chmod failed: %s: %s\n",
                        full, strerror(errno));
            }
        }

        struct timespec ts = brs_ns_to_timespec(e->mtime_ns);
        struct timespec times[2] = {ts, ts};
        if (utimensat(AT_FDCWD, full, times,
                      S_ISLNK(e->mode) ? AT_SYMLINK_NOFOLLOW : 0) != 0) {
            if (errno != ENOTSUP && errno != EPERM && errno != ENOENT &&
                !(S_ISLNK(e->mode) && errno == EINVAL)) {
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
 * verify — LECTURA SELECTIVA (no carga packs completos en RAM)
 * ==========================================================================*/

/*
 * Lee solo el footer de un pack para extraer metadata y verificar CRC32C.
 * No carga el pack completo en memoria.
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
 * Lee un chunk específico de un pack usando lectura selectiva.
 * Solo carga el chunk en memoria, no el pack completo.
 */
    static int verify_read_chunk_selective(const char *pack_path,
                                        const BrsPackEntry *entry,
                                        BrsBuffer *out)
{
    BrsVfs *vfs = brs_vfs_context_get();
    const char *rel = brs_vfs_context_strip(pack_path);

    if (vfs && rel) {
        BrsVfsFile *f = brs_vfs_fopen(vfs, rel, BRS_VFS_OPEN_READ);
        if (!f)
            return -1;

        if (brs_buffer_resize(out, entry->comp_size) != 0) {
            brs_vfs_fclose(f);
            return -1;
        }

        size_t off = 0;
        while (off < entry->comp_size) {
            ssize_t r = brs_vfs_fread(f,
                                      out->data + off,
                                      entry->comp_size - off,
                                      entry->offset + (uint64_t)off);
            if (r <= 0) {
                brs_vfs_fclose(f);
                return -1;
            }
            off += (size_t)r;
        }

        brs_vfs_fclose(f);
        out->size = entry->comp_size;
        return 0;
    }

    int fd = open(pack_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;

    if (brs_buffer_resize(out, entry->comp_size) != 0) {
        close(fd);
        return -1;
    }

    size_t offset = 0;
    while (offset < entry->comp_size) {
        ssize_t r = pread(fd,
                          out->data + offset,
                          entry->comp_size - offset,
                          (off_t)(entry->offset + (uint64_t)offset));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (r == 0) {
            close(fd);
            return -1;
        }
        offset += (size_t)r;
    }

    close(fd);
    out->size = entry->comp_size;
    return 0;
}


int brs_repo_verify(const char *repo_path,
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
        if (stored_crc != 0) {
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

cleanup:
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
 * list
 * ==========================================================================*/
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
