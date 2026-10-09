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
#ifndef BRS_URI_H
#define BRS_URI_H

#include <stddef.h>

#define BRS_URI_MAX_PART 256

/* URI parseada: ssh://user@host/path */
typedef struct {
    char user[BRS_URI_MAX_PART];
    char host[BRS_URI_MAX_PART];
    char path[4096];
    int  port;          /* 0 = default (22) */
} BrsUri;

/* Devuelve 1 si la URI es remota (ssh://, tls://, sftp://). */
int brs_uri_is_remote(const char *uri);

/* Parsea una URI ssh:// en sus componentes.
   Devuelve 0 en exito, -1 en error.
   Acepta: ssh://host/path
           ssh://user@host/path
           ssh://user@host:port/path
           local:/path  (explicito, tratado como local) */
int brs_uri_parse(const char *uri, BrsUri *out);

/* Construye la URI completa a partir de componentes. */
int brs_uri_build(const BrsUri *uri, char *out, size_t out_size);

#endif /* BRS_URI_H */
