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
/* brs_dir.h — listado de directorios (opendir/readdir) */
#ifndef BRS_DIR_H
#define BRS_DIR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char **names;
    size_t count;
    size_t cap;
} BrsDirList;

/* Lista nombres (sin "." ni ".."). 0 ok, -1 error. */
int  brs_list_dir(const char *dir, BrsDirList *out);
void brs_dir_list_free(BrsDirList *list);
void brs_dir_list_sort(BrsDirList *list);

#ifdef __cplusplus
}
#endif

#endif /* BRS_DIR_H */