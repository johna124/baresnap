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
/* brs_fsutil.h — utilidades de rutas y filesystem (POSIX puro) */
#ifndef BRS_FSUTIL_H
#define BRS_FSUTIL_H

#include <stddef.h>
#include "brs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int brs_path_join(char *out, size_t out_size, const char *a, const char *b);
int brs_mkdir_p(const char *path);
int brs_path_exists(const char *path);
int brs_is_directory(const char *path);
int brs_remove_file(const char *path);

/* Escritura atomica: tmp + fsync + rename + fsync del directorio.
 * Ante corte de luz el destino queda completo-viejo o completo-nuevo,
 * nunca corrupto. */
int brs_write_file_atomic(const char *path, const void *data, size_t n);

/* Borra ficheros cuyo nombre termine en suffix dentro de dir.
 * Devuelve cuantos borro, o -1 si no pudo listar. */
int brs_remove_files_with_suffix(const char *dir, const char *suffix);

#ifdef __cplusplus
}
#endif

#endif /* BRS_FSUTIL_H */