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
/* brs_config.h — config del repositorio + open_pack_id */
#ifndef BRS_CONFIG_H
#define BRS_CONFIG_H

#include <stdint.h>
#include "brs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Formato binario identico al motor C++ (magic BSCFG001, LE, FNV1a al final).
 * Devuelven 0 ok, -1 error. */
int brs_write_config(const char *repo_path, const BrsRepoConfig *cfg);
int brs_load_config(const char *repo_path, BrsRepoConfig *cfg);

/* Fichero de texto "open_pack_id" con el pack id en decimal. */
int brs_read_open_pack_id(const char *repo_path, uint64_t *pack_id);
int brs_write_open_pack_id(const char *repo_path, uint64_t pack_id);
int brs_remove_open_pack_id(const char *repo_path);

#ifdef __cplusplus
}
#endif

#endif /* BRS_CONFIG_H */