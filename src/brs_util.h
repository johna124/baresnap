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
/* ============================================================================
 * brs_util.h — Utilidades base de BareSnap (C11 puro, musl/POSIX)
 * ==========================================================================*/
#ifndef BRS_UTIL_H
#define BRS_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "brs_types.h"
#include "brs_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Borrado seguro de memoria
 * FIX (CWE-14): el wipe con punteros volatile del C++ puede ser eliminado
 * por Dead Store Elimination. Aquí usamos explicit_bzero nativo de musl,
 * que el compilador no puede optimizar.
 * -------------------------------------------------------------------------*/
void brs_secure_wipe(void *p, size_t n);

/* ---------------------------------------------------------------------------
 * RNG: SIN fallback determinista. Si no hay entropía disponible, falla.
 * El material criptográfico (salt, nonces) jamás debe generarse con
 * fuentes predecibles.
 * -------------------------------------------------------------------------*/
int brs_random_bytes(void *out, size_t n);

/* UUID: intenta RNG real; si falla usa mezcla tiempo+pid (solo válido
 * porque el UUID es un identificador, no un secreto). */
int brs_make_uuid(uint8_t out[BRS_UUID_LEN]);

/* ---------------------------------------------------------------------------
 * Tiempo
 * -------------------------------------------------------------------------*/
uint64_t        brs_now_ns(void);
struct timespec brs_ns_to_timespec(uint64_t ns);
uint64_t        brs_timespec_to_ns(const struct timespec *ts);

/* ---------------------------------------------------------------------------
 * Formato / sistema
 * -------------------------------------------------------------------------*/
/* out debe tener 2*len+1 bytes. */
void   brs_to_hex(const uint8_t *data, size_t len, char *out);
uint64_t brs_fnv1a_64(const void *data, size_t n);
int    brs_get_hostname(char *out, size_t out_size);
size_t brs_format_bytes(uint64_t bytes, char *out, size_t out_size);
size_t brs_format_timestamp_utc(uint64_t ns, char *out, size_t out_size);

/* ---------------------------------------------------------------------------
 * I/O sobre descriptores y ficheros
 * -------------------------------------------------------------------------*/
int brs_write_fd_all(int fd, const void *data, size_t n);
int brs_write_file(const char *path, const void *data, size_t n);
int brs_read_file(const char *path, BrsBuffer *out);

/* ---------------------------------------------------------------------------
 * Validación de rutas (anti path-traversal en restore)
 * Devuelve 1 si es segura, 0 si no.
 * -------------------------------------------------------------------------*/
int brs_is_safe_relative_path(const char *path);

/* ---------------------------------------------------------------------------
 * KVV: única ruta de empaquetado/desempaquetado.
 * FIX: unifica los dos bloques duplicados de init_crypto() y garantiza
 * que nunca se escribe fuera de los 72 bytes.
 * -------------------------------------------------------------------------*/
int brs_kvv_pack(uint8_t kvv[BRS_KVV_SIZE],
                 const uint8_t nonce[BRS_KVV_NONCE_LEN],
                 const uint8_t ct[BRS_KVV_CT_LEN],
                 const uint8_t mac[BRS_KVV_MAC_LEN]);

int brs_kvv_unpack(const uint8_t kvv[BRS_KVV_SIZE],
                   uint8_t nonce[BRS_KVV_NONCE_LEN],
                   uint8_t ct[BRS_KVV_CT_LEN],
                   uint8_t mac[BRS_KVV_MAC_LEN]);

/* ---------------------------------------------------------------------------
 * Lector little-endian con límites (sustituye al patrón ByteSpan+subspan).
 * En caso de error el cursor NO avanza.
 * -------------------------------------------------------------------------*/
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} BrsReader;

void   brs_reader_init(BrsReader *r, const void *data, size_t len);
size_t brs_reader_remaining(const BrsReader *r);
int    brs_reader_bytes(BrsReader *r, size_t n, const uint8_t **out);
int    brs_reader_skip(BrsReader *r, size_t n);
int    brs_reader_u8(BrsReader *r, uint8_t *v);
int    brs_reader_u16_le(BrsReader *r, uint16_t *v);
int    brs_reader_u32_le(BrsReader *r, uint32_t *v);
int    brs_reader_u64_le(BrsReader *r, uint64_t *v);

void brs_clear_status_line(void);
#ifdef __cplusplus
}
#endif

#endif /* BRS_UTIL_H */
