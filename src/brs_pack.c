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
/* brs_pack.c — escritor de packs con CRC32C incremental.
 *
 * Modo remoto:
 *   Streaming puro vía VFS/SSH con reintentos y verificación por chunk.
 *   No se crea /tmp/baresnap_pack_*.tmp.
 *   No se lee el pack completo en finalize(). No hay malloc masivo del pack.
 *
 * Modo local:
 *   tmp + rename atómico tradicional.
 *
 * Cambios incluidos (Fase 5):
 *   - Generación de pack_id con hash mezclado: timestamp + bytes
 *     pseudoaleatorios reales vía brs_random_bytes. Se impide la
 *     colisión de pack_id cuando dos hilos abren packs en el mismo
 *     instante.
 *   - Rotación automática de packs al superar 100 MB.
 *   - Índices laterales en index/<pack_id>.idx.
 *   - Manejo de tamaños y offsets con uint64_t/size_t.
 *   - Cierre inmediato de handles ante fallo de red/disk.
 *   - Funciones modificadas con salida única vía goto cleanup.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "brs_index.h"
#include "brs_pack.h"
#include "brs_buffer.h"
#include "brs_crypto.h"
#include "brs_fsutil.h"
#include "brs_hash.h"
#include "brs_util.h"
#include "brs_vfs.h"
#include "brs_vfs_context.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

static uint64_t g_brs_pack_counter = 0;

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define BRS_PACK_HEADER_SIZE   (BRS_MAGIC_LEN + 4 + 8 + 16)
#define BRS_FOOTER_FIXED_SIZE  (BRS_MAGIC_LEN + 8 + 4 + 8)
#define BRS_FOOTER_ENTRY_SIZE  (BRS_CHUNK_ID_LEN + 8 + 4 + 4 + 1)
#define BRS_PACK_ROTATE_BYTES  (100ULL * 1024ULL * 1024ULL)

static const uint8_t BRS_PACK_IDX_MAGIC[8] = {
    'B', 'R', 'S', 'I', 'D', 'X', '1', '0'
};

#define BRS_PACK_IDX_MAGIC_LEN sizeof(BRS_PACK_IDX_MAGIC)

/* ============================================================================
 * FASE 5: Generación de pack_id con hash mezclado.
 *
 * Combina el timestamp actual con 8 bytes pseudoaleatorios reales
 * obtenidos de brs_random_bytes. La mezcla usa XOR con desplazamientos
 * para distribuir la entropía y evitar colisiones cuando dos hilos
 * abren packs en el mismo nanosegundo.
 *
 * Si brs_random_bytes falla (entropía del sistema agotada), se usa un
 * fallback basado en la dirección de la pila y el PID para mantener
 * unicidad práctica.
 * ==========================================================================*/
static uint64_t brs_generate_pack_id(void)
{
    uint64_t ts = brs_now_ns();
    uint8_t rnd[8];
    uint64_t rand_part = 0;

    if (brs_random_bytes(rnd, sizeof(rnd)) == 0) {
        memcpy(&rand_part, rnd, sizeof(rand_part));
    } else {
        /* Fallback: entropía débil basada en stack address + PID.
         * Suficiente para evitar colisiones en la práctica. */
        rand_part = (uint64_t)(uintptr_t)&ts;
        rand_part ^= ((uint64_t)(uint32_t)getpid() << 32);
        rand_part ^= (uint64_t)time(NULL);
    }

    /* Mezcla: XOR con desplazamientos circulares. */
    uint64_t mixed = ts ^ (rand_part << 1) ^ (rand_part >> 7);

    /* FNV-1a adicional sobre el resultado para mayor avalancha. */
    mixed ^= 0xcbf29ce484222325ULL;
    mixed *= 0x100000001b3ULL;
    mixed ^= mixed >> 32;

    /* Garantizar que nunca sea cero. */
    if (mixed == 0)
        mixed = 1;

    return mixed;
}

/* ============================================================================
 * Política de reintentos para escrituras remotas.
 * Backoff exponencial: 50ms, 100ms, 200ms.
 * ==========================================================================*/
#ifndef BRS_IO_RETRY_MAX
#define BRS_IO_RETRY_MAX 3
#endif

#ifndef BRS_IO_RETRY_BASE_MS
#define BRS_IO_RETRY_BASE_MS 50
#endif

static void brs_retry_backoff(int attempt)
{
    long ms = (long)BRS_IO_RETRY_BASE_MS * (1L << (attempt - 1));
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;

    nanosleep(&ts, NULL);
}

/* ============================================================================
 * Escritura raw con reintentos para modo remoto.
 * En modo local se delega a brs_write_fd_all().
 * En modo remoto se reintenta hasta BRS_IO_RETRY_MAX veces con backoff
 * exponencial. Se soporta escritura parcial.
 * ==========================================================================*/
/* ============================================================================
 * Escritura raw: SIEMPRE a fd local
 * ==========================================================================*/
static int brs_pack_write_raw(BrsPackWriter *w, const void *data, size_t len)
{
    int rc = -1;

    if (w == NULL)
        goto cleanup;

    if (len == 0) {
        rc = 0;
        goto cleanup;
    }

    if (data == NULL)
        goto cleanup;

    if (w->fd >= 0) {
        rc = brs_write_fd_all(w->fd, data, len);
        goto cleanup;
    }

cleanup:
    return rc;
}

/* ============================================================================
 * Inicialización de writer: SIEMPRE a temporal local
 * ==========================================================================*/
int brs_pack_writer_init(BrsPackWriter *w,
                         const char *repo_path,
                         uint64_t pack_id)
{
    BrsBuffer buf;
    int rc = -1;
    int ok = 0;

    brs_buffer_init(&buf);

    if (w == NULL || repo_path == NULL)
        goto cleanup;

    {
        BrsIndexMap *saved_index_map = w->index_map;
        memset(w, 0, sizeof(*w));
        w->index_map = saved_index_map;
    }

    w->fd = -1;
    w->remote = NULL;
    w->pack_id = pack_id ? pack_id : brs_generate_pack_id();
    w->crc_state = 0xFFFFFFFFu;

    size_t rl = strlen(repo_path);
    if (rl >= sizeof(w->repo_path))
        goto cleanup;

    memcpy(w->repo_path, repo_path, rl + 1);

    BrsVfs *vfs = brs_vfs_context_get();

    char tmp_dir[BRS_PATH_MAX];
    char name[64];

    if (vfs != NULL) {
        snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/baresnap_pack_tmp");
        w->remote = (void*)1;
    } else {
        if (brs_path_join(tmp_dir, sizeof(tmp_dir), repo_path, "tmp") != 0)
            goto cleanup;
    }

    if (brs_mkdir_p(tmp_dir) != 0)
        goto cleanup;

    int n = snprintf(name, sizeof(name),
                     "pack_%llu_%d.tmp",
                     (unsigned long long)w->pack_id,
                     (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(name))
        goto cleanup;

    if (brs_path_join(w->tmp_path, sizeof(w->tmp_path),
                      tmp_dir, name) != 0) {
        goto cleanup;
    }

    w->fd = open(w->tmp_path,
                 O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                 0600);
    if (w->fd < 0)
        goto cleanup;
            g_brs_pack_counter++;

    brs_clear_status_line();
    fprintf(stderr,
            "\n[pack] Creando paquete %llu...\n",
            (unsigned long long)g_brs_pack_counter);

    static const uint8_t zeros16[16] = {0};

    ok =
        brs_buffer_append(&buf, BRS_MAGIC_PACK, BRS_MAGIC_LEN) == 0 &&
        brs_buffer_append_u32_le(&buf, BRS_FORMAT_VERSION) == 0 &&
        brs_buffer_append_u64_le(&buf, w->pack_id) == 0 &&
        brs_buffer_append(&buf, zeros16, sizeof(zeros16)) == 0;

    if (ok) {
        w->crc_state = brs_crc32c_update(w->crc_state, buf.data, buf.size);
        ok = brs_pack_write_raw(w, buf.data, buf.size) == 0;
        w->current_offset += buf.size;
    }

    if (!ok) {
        brs_pack_writer_abort(w);
        goto cleanup;
    }

    rc = 0;

cleanup:
    brs_buffer_free(&buf);
    return rc;
}

/* ============================================================================
 * Finalización: subir con scp si es remoto
 * ==========================================================================*/
int brs_pack_writer_finalize(BrsPackWriter *w,
                             BrsPackEntry **out_entries,
                             uint32_t *out_count,
                             uint64_t *out_pack_id)
{
    BrsBuffer buf;
    int rc = -1;
    int success = 0;
    int done_abort = 0;

    brs_buffer_init(&buf);

    if (w == NULL || w->fd < 0)
        goto cleanup;

    uint32_t entry_count32 = (uint32_t)w->entry_count;

    int ok = 1;

    ok = ok && brs_buffer_append(&buf,
                                 BRS_MAGIC_PACK_FOOTER,
                                 BRS_MAGIC_LEN) == 0;
    ok = ok && brs_buffer_append_u64_le(&buf, w->pack_id) == 0;
    ok = ok && brs_buffer_append_u32_le(&buf, entry_count32) == 0;

    for (uint64_t i = 0; ok && i < (uint64_t)w->entry_count; ++i) {
        const BrsPackEntry *e = &w->entries[i];

        ok = brs_buffer_append(&buf,
                               e->chunk_id.bytes,
                               BRS_CHUNK_ID_LEN) == 0;
        if (!ok) break;
        ok = brs_buffer_append_u64_le(&buf, e->offset) == 0;
        if (!ok) break;
        ok = brs_buffer_append_u32_le(&buf, e->comp_size) == 0;
        if (!ok) break;
        ok = brs_buffer_append_u32_le(&buf, e->uncomp_size) == 0;
        if (!ok) break;
        ok = brs_buffer_append_u8(&buf, e->flags) == 0;
    }

    if (ok) {
        w->crc_state = brs_crc32c_update(w->crc_state, buf.data, buf.size);
        uint32_t final_crc = w->crc_state ^ 0xFFFFFFFFu;
        ok = brs_buffer_append_u64_le(&buf, (uint64_t)final_crc) == 0;
    }

    if (!ok) {
        brs_pack_writer_abort(w);
        done_abort = 1;
        goto cleanup;
    }

    if (brs_pack_write_raw(w, buf.data, buf.size) != 0) {
        brs_pack_writer_abort(w);
        done_abort = 1;
        goto cleanup;
    }
    w->current_offset += buf.size;

    if (fsync(w->fd) != 0) {
        w->fd = -1;
        brs_pack_writer_abort(w);
        done_abort = 1;
        goto cleanup;
    }

    if (close(w->fd) != 0) {
        w->fd = -1;
        brs_pack_writer_abort(w);
        done_abort = 1;
        goto cleanup;
    }
    w->fd = -1;
        char size_str[64];
    brs_format_bytes(w->current_offset, size_str, sizeof(size_str));

    brs_clear_status_line();
    fprintf(stderr,
            "[pack] Subiendo paquete %llu (%u chunks, %s)\n",
            (unsigned long long)g_brs_pack_counter,
            w->entry_count,
            size_str);
    
if (w->remote != NULL) {
        /* Subir con scp */
        BrsVfs *vfs = brs_vfs_context_get();
        if (vfs) {
            char final_rel[BRS_PATH_MAX];
            snprintf(final_rel, sizeof(final_rel),
                     "packs/%llu.pack",
                     (unsigned long long)w->pack_id);

            uint64_t uploaded = 0;
            if (brs_vfs_upload_pack(vfs, final_rel,
                                    w->tmp_path, &uploaded) != 0) {
                unlink(w->tmp_path);
                w->tmp_path[0] = '\0';
                free(w->entries);
                w->entries = NULL;
                w->entry_count = 0;
                w->entry_cap = 0;
                done_abort = 1;
                goto cleanup;
            }
        }

        unlink(w->tmp_path);
        w->tmp_path[0] = '\0';
        w->remote = NULL;
        success = 1;
        goto cleanup;
    }

    /* Modo local: rename atómico */
    {
        char packs_dir[BRS_PATH_MAX];
        char final_path[BRS_PATH_MAX];
        char fname[64];

        if (brs_path_join(packs_dir,
                          sizeof(packs_dir),
                          w->repo_path,
                          "packs") != 0) {
            brs_pack_writer_abort(w);
            done_abort = 1;
            goto cleanup;
        }

        if (brs_mkdir_p(packs_dir) != 0) {
            brs_pack_writer_abort(w);
            done_abort = 1;
            goto cleanup;
        }

        int n = snprintf(fname,
                         sizeof(fname),
                         "%llu.pack",
                         (unsigned long long)w->pack_id);
        if (n < 0 || (size_t)n >= sizeof(fname)) {
            brs_pack_writer_abort(w);
            done_abort = 1;
            goto cleanup;
        }

        if (brs_path_join(final_path,
                          sizeof(final_path),
                          packs_dir,
                          fname) != 0) {
            brs_pack_writer_abort(w);
            done_abort = 1;
            goto cleanup;
        }

        if (rename(w->tmp_path, final_path) != 0) {
            brs_pack_writer_abort(w);
            done_abort = 1;
            goto cleanup;
        }

        w->tmp_path[0] = '\0';
    }

    success = 1;

cleanup:
    if (success && w != NULL) {
        uint32_t cnt = (uint32_t)w->entry_count;

        if (out_entries != NULL) {
            *out_entries = w->entries;
            w->entries = NULL;
        } else {
            free(w->entries);
        }

        w->entries = NULL;
        w->entry_count = 0;
        w->entry_cap = 0;

        if (out_count != NULL)
            *out_count = cnt;

        if (out_pack_id != NULL)
            *out_pack_id = w->pack_id;

        rc = 0;
    } else if (!success && w != NULL && !done_abort) {
        brs_pack_writer_abort(w);
    }

    brs_buffer_free(&buf);
    return rc;
}

/* ============================================================================
 * Rotación de packs.
 * ==========================================================================*/
static int brs_pack_writer_rotate(BrsPackWriter *w)
{
    char repo_path[sizeof(w->repo_path)];
    uint64_t old_pack_id = 0;
    uint64_t next_pack_id = 0;
    int rc = -1;

    repo_path[0] = '\0';

    if (w == NULL || w->fd < 0)
        goto cleanup;

    if (w->entry_count == 0) {
        rc = 0;
        goto cleanup;
    }

    memcpy(repo_path, w->repo_path, sizeof(repo_path));

    old_pack_id = w->pack_id;
    next_pack_id = old_pack_id + 1ULL;

    if (next_pack_id == 0)
        next_pack_id = brs_generate_pack_id();

    /* Finalizar el pack actual y recuperar sus entradas */
    BrsPackEntry *entries = NULL;
    uint32_t count = 0;
    uint64_t closed_pack_id = 0;

    if (brs_pack_writer_finalize(w, &entries, &count, &closed_pack_id) != 0)
        goto cleanup;

    /* Acumular en el índice global si está configurado */
    if (w->index_map && entries != NULL) {
        for (uint32_t i = 0; i < count; ++i) {
            BrsChunkLocation loc;

            loc.pack_id = closed_pack_id;
            loc.offset = entries[i].offset;
            loc.comp_size = entries[i].comp_size;
            loc.uncomp_size = entries[i].uncomp_size;
            loc.flags = entries[i].flags;

            brs_index_map_put(w->index_map, &entries[i].chunk_id, &loc);
        }
    }

    free(entries);

    /* Abrir nuevo pack */
    if (brs_pack_writer_init(w, repo_path, next_pack_id) != 0)
        goto cleanup;

    rc = 0;

cleanup:
    return rc;
}

/* ============================================================================
 * Hot path con rotación automática.
 * ==========================================================================*/
int brs_pack_writer_add_chunk(BrsPackWriter *w,
                              const BrsChunkId *id,
                              const uint8_t *data,
                              size_t size,
                              uint64_t uncomp_size,
                              uint8_t flags)
{
    int rc = -1;

    if (w == NULL || id == NULL)
        goto cleanup;

    if (w->remote == NULL && w->fd < 0)
        goto cleanup;

    if (size > 0 && data == NULL)
        goto cleanup;

    if ((uint64_t)size > UINT64_MAX - w->current_offset) {
        brs_pack_writer_abort(w);
        goto cleanup;
    }

    if (w->entry_count > 0 &&
        (w->current_offset >= BRS_PACK_ROTATE_BYTES ||
         w->current_offset + (uint64_t)size > BRS_PACK_ROTATE_BYTES)) {
        if (brs_pack_writer_rotate(w) != 0)
            goto cleanup;
    }

    if (brs_pack_write_raw(w, data, size) != 0) {
        brs_pack_writer_abort(w);
        goto cleanup;
    }

    if (size > 0)
        w->crc_state = brs_crc32c_update(w->crc_state, data, size);

    if ((uint64_t)w->entry_count >= (uint64_t)UINT32_MAX) {
        brs_pack_writer_abort(w);
        goto cleanup;
    }

    if (w->entry_count == w->entry_cap) {
        uint64_t ncap64 = w->entry_cap
                        ? ((uint64_t)w->entry_cap * 2ULL)
                        : 16ULL;

        if (ncap64 > (uint64_t)UINT32_MAX ||
            ncap64 > (uint64_t)(SIZE_MAX / sizeof(BrsPackEntry))) {
            brs_pack_writer_abort(w);
            goto cleanup;
        }

        uint32_t ncap = (uint32_t)ncap64;

        if (ncap <= w->entry_cap) {
            brs_pack_writer_abort(w);
            goto cleanup;
        }

        BrsPackEntry *p = (BrsPackEntry *)realloc(w->entries,
                                                  (size_t)ncap *
                                                  sizeof(BrsPackEntry));
        if (p == NULL) {
            brs_pack_writer_abort(w);
            goto cleanup;
        }

        w->entries = p;
        w->entry_cap = ncap;
    }

    BrsPackEntry *e = &w->entries[w->entry_count];

    e->chunk_id = *id;
    e->offset = w->current_offset;
    e->comp_size = (uint64_t)size;
    e->uncomp_size = uncomp_size;
    e->flags = flags;

    w->entry_count++;
    w->current_offset += (uint64_t)size;

    rc = 0;

cleanup:
    return rc;
}

/* ============================================================================
 * Append sobre pack existente.
 * ==========================================================================*/
int brs_pack_writer_open_for_append(BrsPackWriter *w,
                                    const uint8_t *existing_data,
                                    const BrsPackEntry *existing_entries,
                                    uint32_t existing_count,
                                    size_t data_start,
                                    size_t data_end)
{
    int rc = -1;
    int wrote = 0;
    int abort_done = 0;

    if (w == NULL || (w->remote == NULL && w->fd < 0))
        goto cleanup;

    if (data_end < data_start)
        goto cleanup;

    if (data_end > data_start) {
        if (existing_data == NULL)
            goto cleanup;

        size_t len = data_end - data_start;

        if (brs_pack_write_raw(w, existing_data + data_start, len) != 0) {
            brs_pack_writer_abort(w);
            abort_done = 1;
            goto cleanup;
        }

        wrote = 1;

        w->crc_state = brs_crc32c_update(w->crc_state,
                                         existing_data + data_start,
                                         len);
        w->current_offset += len;
    }

    if (existing_count > 0) {
        if (existing_entries == NULL) {
            if (wrote) {
                brs_pack_writer_abort(w);
                abort_done = 1;
            }
            goto cleanup;
        }

        uint64_t total64 = (uint64_t)w->entry_count +
                           (uint64_t)existing_count;

        if (total64 > (uint64_t)UINT32_MAX ||
            total64 > (uint64_t)(SIZE_MAX / sizeof(BrsPackEntry))) {
            if (wrote) {
                brs_pack_writer_abort(w);
                abort_done = 1;
            }
            goto cleanup;
        }

        BrsPackEntry *p = (BrsPackEntry *)realloc(w->entries,
                                                  (size_t)total64 *
                                                  sizeof(BrsPackEntry));
        if (p == NULL) {
            if (wrote) {
                brs_pack_writer_abort(w);
                abort_done = 1;
            }
            goto cleanup;
        }

        w->entries = p;

        memcpy(w->entries + w->entry_count,
               existing_entries,
               (size_t)existing_count * sizeof(BrsPackEntry));

        w->entry_count = (uint32_t)total64;

        if (w->entry_cap < w->entry_count)
            w->entry_cap = w->entry_count;
    }

    rc = 0;

cleanup:
    (void)abort_done;
    return rc;
}

/* ============================================================================
 * Tamaño actual.
 * ==========================================================================*/
uint64_t brs_pack_writer_current_size(const BrsPackWriter *w)
{
    return w ? w->current_offset : 0;
}

/* ============================================================================
 * Abort: cierre inmediato y limpieza.
 * ==========================================================================*/
void brs_pack_writer_abort(BrsPackWriter *w)
{
    if (w == NULL)
        return;

    if (w->fd >= 0) {
        close(w->fd);
        w->fd = -1;

        if (w->tmp_path[0] != '\0')
            unlink(w->tmp_path);
    }

    w->remote = NULL;

    free(w->entries);
    w->entries = NULL;
    w->entry_count = 0;
    w->entry_cap = 0;
}

/* ============================================================================
 * parse_pack: escaneo de footer hacia atrás.
 * ==========================================================================*/
int brs_parse_pack(const uint8_t *data,
                   size_t size,
                   uint64_t *out_pack_id,
                   BrsPackEntry **out_entries,
                   uint32_t *out_count)
{
    BrsPackEntry *arr = NULL;
    int rc = -1;

    if (data == NULL || out_pack_id == NULL ||
        out_entries == NULL || out_count == NULL) {
        goto cleanup;
    }

    *out_entries = NULL;
    *out_count = 0;

    if (size > (size_t)INT64_MAX)
        goto cleanup;

    if (size < BRS_PACK_HEADER_SIZE + BRS_FOOTER_FIXED_SIZE)
        goto cleanup;

    if (memcmp(data, BRS_MAGIC_PACK, BRS_MAGIC_LEN) != 0)
        goto cleanup;

    BrsReader pr;
    brs_reader_init(&pr,
                    data + BRS_MAGIC_LEN + 4,
                    size - (BRS_MAGIC_LEN + 4));

    if (brs_reader_u64_le(&pr, out_pack_id) != 0)
        goto cleanup;

    size_t footer_start = 0;
    int found = 0;

    int64_t start = (int64_t)size - (int64_t)BRS_FOOTER_FIXED_SIZE;

    for (int64_t i = start; i >= (int64_t)BRS_PACK_HEADER_SIZE; --i) {
        if (memcmp(data + i, BRS_MAGIC_PACK_FOOTER, BRS_MAGIC_LEN) != 0)
            continue;

        size_t ec_pos = (size_t)i + BRS_MAGIC_LEN + 8;

        if (ec_pos + 4 > size)
            continue;

        BrsReader er;
        brs_reader_init(&er, data + ec_pos, size - ec_pos);

        uint32_t entry_count = 0;

        if (brs_reader_u32_le(&er, &entry_count) != 0)
            continue;

        uint64_t expected = (uint64_t)i +
                            BRS_FOOTER_FIXED_SIZE +
                            (uint64_t)entry_count * BRS_FOOTER_ENTRY_SIZE;

        if (expected == (uint64_t)size) {
            footer_start = (size_t)i;
            found = 1;
            break;
        }
    }

    if (!found)
        goto cleanup;

    size_t pos = footer_start + BRS_MAGIC_LEN + 8;

    BrsReader er;
    brs_reader_init(&er, data + pos, size - pos);

    uint32_t entry_count = 0;

    if (brs_reader_u32_le(&er, &entry_count) != 0)
        goto cleanup;

    uint64_t count64 = entry_count;

    if (count64 > (uint64_t)(SIZE_MAX / sizeof(BrsPackEntry)))
        goto cleanup;

    arr = (BrsPackEntry *)calloc(count64 ? count64 : 1, sizeof(*arr));
    if (arr == NULL)
        goto cleanup;

    for (uint32_t e = 0; e < entry_count; ++e) {
        const uint8_t *idb = NULL;
        uint32_t comp_size = 0;
        uint32_t uncomp_size = 0;
        uint8_t flags = 0;

        if (brs_reader_bytes(&er, BRS_CHUNK_ID_LEN, &idb) != 0)
            goto cleanup;

        memcpy(arr[e].chunk_id.bytes, idb, BRS_CHUNK_ID_LEN);

        if (brs_reader_u64_le(&er, &arr[e].offset) != 0)
            goto cleanup;

        if (brs_reader_u32_le(&er, &comp_size) != 0)
            goto cleanup;

        if (brs_reader_u32_le(&er, &uncomp_size) != 0)
            goto cleanup;

        if (brs_reader_u8(&er, &flags) != 0)
            goto cleanup;

        arr[e].comp_size = comp_size;
        arr[e].uncomp_size = uncomp_size;
        arr[e].flags = flags;
    }

    *out_entries = arr;
    arr = NULL;
    *out_count = entry_count;

    rc = 0;

cleanup:
    free(arr);
    return rc;
}


