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
#ifndef BRS_VFS_CONTEXT_H
#define BRS_VFS_CONTEXT_H

#include "brs_vfs.h"

/* Contexto VFS global.
   Se establece al inicio de cada operación pública.
   Si es NULL, todas las funciones de I/O usan POSIX directo. */
void        brs_vfs_context_set(BrsVfs *vfs, const char *base_path);
void        brs_vfs_context_clear(void);
BrsVfs     *brs_vfs_context_get(void);
const char *brs_vfs_context_base(void);

/* Devuelve el path relativo al repo si 'path' pertenece al repo remoto.
   Devuelve NULL si el path no pertenece al repo (ej: target del restore). */
const char *brs_vfs_context_strip(const char *path);

/* Conveniencia: ¿este path es del repo remoto? */
int brs_vfs_context_is_remote_path(const char *path);

#endif /* BRS_VFS_CONTEXT_H */
