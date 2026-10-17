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
/* src/brs_delta.h — Interfaz para Delta Encoding (xdelta3) */
#ifndef BRS_DELTA_H
#define BRS_DELTA_H

#include <stddef.h>
#include <stdint.h>

/* Codifica un delta entre old_data y new_data. 
 * Devuelve 0 en éxito, -1 en error. El llamador debe free() a delta_out. */
int brs_delta_encode(const uint8_t *old_data, size_t old_size,
                     const uint8_t *new_data, size_t new_size,
                     uint8_t **delta_out, size_t *delta_size_out);

/* Decodifica un delta aplicándolo sobre old_data.
 * Devuelve 0 en éxito, -1 en error. El llamador debe free() a new_out. */
int brs_delta_decode(const uint8_t *old_data, size_t old_size,
                     const uint8_t *delta_data, size_t delta_size,
                     size_t expected_new_size,
                     uint8_t **new_out, size_t *new_size_out);

#endif /* BRS_DELTA_H */
