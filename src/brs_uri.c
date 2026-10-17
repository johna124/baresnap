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
#include "brs_uri.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int brs_uri_is_remote(const char *uri)
{
    if (!uri) return 0;
    if (strncmp(uri, "ssh://", 6) == 0) return 1;
    if (strncmp(uri, "tls://", 6) == 0) return 1;
    if (strncmp(uri, "sftp://", 7) == 0) return 1;
    return 0;
}

int brs_uri_parse(const char *uri, BrsUri *out)
{
    if (!uri || !out) return -1;
    memset(out, 0, sizeof *out);
    out->port = 0;

    /* Determinar el scheme */
    const char *rest = NULL;
    if (strncmp(uri, "ssh://", 6) == 0) {
        rest = uri + 6;
    } else if (strncmp(uri, "tls://", 6) == 0) {
        rest = uri + 6;
    } else if (strncmp(uri, "sftp://", 7) == 0) {
        rest = uri + 7;
    } else {
        return -1; /* no es una URI remota */
    }

    if (*rest == '\0') return -1;

    /* Buscar el primer '/' que separa host de path */
    const char *slash = strchr(rest, '/');
    if (!slash || slash == rest) return -1; /* sin host o sin path */

    /* La parte antes del '/' es [user@]host[:port] */
    const char *hostpart_start = rest;
    size_t hostpart_len = (size_t)(slash - rest);

    /* Buscar user@ */
    const char *at = memchr(hostpart_start, '@', hostpart_len);
    const char *host_start;
    size_t host_len;

    if (at) {
        size_t ulen = (size_t)(at - hostpart_start);
        if (ulen == 0 || ulen >= BRS_URI_MAX_PART) return -1;
        memcpy(out->user, hostpart_start, ulen);
        out->user[ulen] = '\0';
        host_start = at + 1;
        host_len = hostpart_len - ulen - 1;
    } else {
        host_start = hostpart_start;
        host_len = hostpart_len;
    }

    if (host_len == 0 || host_len >= BRS_URI_MAX_PART) return -1;

    /* Buscar :port dentro del host */
    const char *colon = memchr(host_start, ':', host_len);
    if (colon) {
        size_t hlen = (size_t)(colon - host_start);
        if (hlen == 0 || hlen >= BRS_URI_MAX_PART) return -1;
        memcpy(out->host, host_start, hlen);
        out->host[hlen] = '\0';
        /* Parsear puerto */
        char portbuf[16];
        size_t plen = host_len - hlen - 1;
        if (plen == 0 || plen >= sizeof portbuf) return -1;
        memcpy(portbuf, colon + 1, plen);
        portbuf[plen] = '\0';
        char *end = NULL;
        long p = strtol(portbuf, &end, 10);
        if (!end || *end != '\0' || p <= 0 || p > 65535) return -1;
        out->port = (int)p;
    } else {
        memcpy(out->host, host_start, host_len);
        out->host[host_len] = '\0';
    }

    /* Path: desde el '/' inclusive, sin trailing slash (excepto raiz) */
    size_t path_len = strlen(slash);
    if (path_len >= sizeof out->path) return -1;
    memcpy(out->path, slash, path_len + 1);
    /* Eliminar trailing slash si no es la raiz */
    while (path_len > 1 && out->path[path_len - 1] == '/') {
        out->path[path_len - 1] = '\0';
        path_len--;
    }

    return 0;
}

int brs_uri_build(const BrsUri *uri, char *out, size_t out_size)
{
    if (!uri || !out || out_size == 0) return -1;
    int n;
    if (uri->user[0] != '\0') {
        if (uri->port > 0) {
            n = snprintf(out, out_size, "ssh://%s@%s:%d%s",
                         uri->user, uri->host, uri->port, uri->path);
        } else {
            n = snprintf(out, out_size, "ssh://%s@%s%s",
                         uri->user, uri->host, uri->path);
        }
    } else {
        if (uri->port > 0) {
            n = snprintf(out, out_size, "ssh://%s:%d%s",
                         uri->host, uri->port, uri->path);
        } else {
            n = snprintf(out, out_size, "ssh://%s%s",
                         uri->host, uri->path);
        }
    }
    if (n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}
