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
#ifndef BRS_ZSTD_H
#define BRS_ZSTD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Devuelve el tamaño máximo del buffer de destino. */
size_t brs_zstd_compress_bound(size_t src_size);

/* Comprime src -> dst. Devuelve tamaño comprimido, o 0 en error. */
size_t brs_zstd_compress(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_cap, int level);

/* Descomprime src -> dst. Devuelve tamaño descomprimido, o 0 en error. */
size_t brs_zstd_decompress(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif /* BRS_ZSTD_H */
