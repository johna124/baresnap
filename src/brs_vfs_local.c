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
/* brs_vfs_local.c — backend POSIX directo */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "brs_vfs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>

/* El backend local guarda el path base del repo */
typedef struct {
    char base_path[4096];
} BrsVfsLocal;

/* ---- Retry EIO con backoff exponencial (max 3 reintentos) ---- */
#define BRS_VFS_RETRY_MAX 3
#define BRS_VFS_RETRY_BASE_MS 50

static int brs_vfs_io_transient(int err)
{
    return err == EIO || err == EAGAIN;
}

static void brs_vfs_io_backoff(int attempt)
{
    unsigned int ms = BRS_VFS_RETRY_BASE_MS << attempt;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Funciones del backend local (prefijo brs_vfs_local_) */
BrsVfs *brs_vfs_local_open(const char *uri, int flags);
void    brs_vfs_local_close(BrsVfs *vfs);
BrsVfsFile *brs_vfs_local_fopen(BrsVfs *vfs, const char *path, int flags);
int     brs_vfs_local_fclose(BrsVfsFile *f);
ssize_t brs_vfs_local_fread(BrsVfsFile *f, void *buf, size_t n, uint64_t off);
ssize_t brs_vfs_local_fwrite(BrsVfsFile *f, const void *buf, size_t n, uint64_t off);
int     brs_vfs_local_fsync(BrsVfsFile *f);
int     brs_vfs_local_rename(BrsVfs *vfs, const char *from, const char *to);
int     brs_vfs_local_unlink(BrsVfs *vfs, const char *path);
int     brs_vfs_local_mkdir(BrsVfs *vfs, const char *path, uint32_t mode);
int     brs_vfs_local_link(BrsVfs *vfs, const char *existing, const char *newpath);
int     brs_vfs_local_list(BrsVfs *vfs, const char *path, BrsVfsList *out);
void    brs_vfs_local_list_free(BrsVfsList *list);
int     brs_vfs_local_stat(BrsVfs *vfs, const char *path, uint64_t *size, uint32_t *mode);
int     brs_vfs_local_exists(BrsVfs *vfs, const char *path);
const char *brs_vfs_local_backend_name(BrsVfs *vfs);
int     brs_vfs_local_supports_hardlinks(BrsVfs *vfs);
int     brs_vfs_local_supports_atomic_rename(BrsVfs *vfs);

/* Vtable forward declaration (definida en dispatch) */
extern const void *brs_vfs_get_local_vtable(void);

static int vfs_build_path(BrsVfs *vfs, const char *rel, char *out, size_t out_size)
{
    BrsVfsLocal *local = (BrsVfsLocal *)vfs;
    int n = snprintf(out, out_size, "%s/%s", local->base_path, rel);
    return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
}

BrsVfs *brs_vfs_local_open(const char *uri, int flags)
{
    (void)flags;
    BrsVfsLocal *local = (BrsVfsLocal *)calloc(1, sizeof(BrsVfsLocal));
    if (!local) return NULL;

    const char *path = uri;
    if (strncmp(uri, "local:", 6) == 0) path = uri + 6;
    strncpy(local->base_path, path, sizeof(local->base_path) - 1);
    local->base_path[sizeof(local->base_path) - 1] = '\0';

    return (BrsVfs *)local;
}

void brs_vfs_local_close(BrsVfs *vfs)
{
    free(vfs);
}

BrsVfsFile *brs_vfs_local_fopen(BrsVfs *vfs, const char *path, int flags)
{
    char full[8192];
    if (vfs_build_path(vfs, path, full, sizeof(full)) != 0) return NULL;

    int oflags = 0;
    if ((flags & BRS_VFS_OPEN_READ) && (flags & BRS_VFS_OPEN_WRITE)) oflags = O_RDWR;
    else if (flags & BRS_VFS_OPEN_WRITE) oflags = O_WRONLY;
    else oflags = O_RDONLY;
    if (flags & BRS_VFS_OPEN_APPEND) oflags |= O_APPEND;
    if (flags & BRS_VFS_OPEN_TRUNC) oflags |= O_TRUNC;
    if (flags & BRS_VFS_OPEN_CREAT) oflags |= O_CREAT;

    /* Retry EIO con backoff exponencial */
    int fd = -1;
    for (int attempt = 0; attempt <= BRS_VFS_RETRY_MAX; ++attempt) {
        fd = open(full, oflags, 0644);
        if (fd >= 0) break;
        if (!brs_vfs_io_transient(errno) || attempt == BRS_VFS_RETRY_MAX) return NULL;
        brs_vfs_io_backoff(attempt);
    }
    if (fd < 0) return NULL;

    BrsVfsFile *f = (BrsVfsFile *)calloc(1, sizeof(BrsVfsFile));
    if (!f) { close(fd); return NULL; }
    f->impl = (void *)(intptr_t)fd;
    f->vfs_parent = vfs;
    return f;
}

int brs_vfs_local_fclose(BrsVfsFile *f)
{
    if (!f) return -1;
    int fd = (int)(intptr_t)f->impl;
    int rc = close(fd);
    free(f);
    return rc;
}

ssize_t brs_vfs_local_fread(BrsVfsFile *f, void *buf, size_t n, uint64_t off)
{
    if (!f) return -1;
    int fd = (int)(intptr_t)f->impl;
    size_t total = 0;
    int retries = 0;
    while (total < n) {
        ssize_t r = pread(fd, (char *)buf + total, n - total, (off_t)(off + total));
        if (r < 0) {
            if (errno == EINTR) continue;
            /* Retry EIO con backoff exponencial */
            if (brs_vfs_io_transient(errno) && retries < BRS_VFS_RETRY_MAX) {
                brs_vfs_io_backoff(retries);
                retries++;
                continue;
            }
            return -1;
        }
        if (r == 0) break;
        total += (size_t)r;
        retries = 0; /* reset tras progreso */
    }
    return (ssize_t)total;
}

ssize_t brs_vfs_local_fwrite(BrsVfsFile *f, const void *buf, size_t n, uint64_t off)
{
    if (!f) return -1;
    int fd = (int)(intptr_t)f->impl;
    size_t total = 0;
    while (total < n) {
        ssize_t w = pwrite(fd, (const char *)buf + total, n - total, (off_t)(off + total));
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        total += (size_t)w;
    }
    return (ssize_t)total;
}

int brs_vfs_local_fsync(BrsVfsFile *f)
{
    if (!f) return -1;
    int fd = (int)(intptr_t)f->impl;
    return fsync(fd);
}

int brs_vfs_local_rename(BrsVfs *vfs, const char *from, const char *to)
{
    char f1[8192], f2[8192];
    if (vfs_build_path(vfs, from, f1, sizeof(f1)) != 0) return -1;
    if (vfs_build_path(vfs, to, f2, sizeof(f2)) != 0) return -1;
    return rename(f1, f2);
}

int brs_vfs_local_unlink(BrsVfs *vfs, const char *path)
{
    char full[8192];
    if (vfs_build_path(vfs, path, full, sizeof(full)) != 0) return -1;
    return unlink(full);
}

int brs_vfs_local_mkdir(BrsVfs *vfs, const char *path, uint32_t mode)
{
    char full[8192];
    if (vfs_build_path(vfs, path, full, sizeof(full)) != 0) return -1;
    return mkdir(full, (mode_t)mode);
}

int brs_vfs_local_link(BrsVfs *vfs, const char *existing, const char *newpath)
{
    char f1[8192], f2[8192];
    if (vfs_build_path(vfs, existing, f1, sizeof(f1)) != 0) return -1;
    if (vfs_build_path(vfs, newpath, f2, sizeof(f2)) != 0) return -1;
    return link(f1, f2);
}

int brs_vfs_local_list(BrsVfs *vfs, const char *path, BrsVfsList *out)
{
    char full[8192];
    if (vfs_build_path(vfs, path, full, sizeof(full)) != 0) return -1;

    DIR *dir = opendir(full);
    if (!dir) return -1;

    out->items = NULL;
    out->count = 0;
    size_t cap = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        if (out->count >= cap) {
            cap = cap ? cap * 2 : 16;
            BrsVfsEntry *ni = (BrsVfsEntry *)realloc(out->items, cap * sizeof(BrsVfsEntry));
            if (!ni) { closedir(dir); return -1; }
            out->items = ni;
        }

        /* FIX: usar snprintf en vez de strncpy para evitar truncation warning */
        snprintf(out->items[out->count].name,
                 sizeof(out->items[out->count].name), "%s", ent->d_name);

        char ent_full[16384];
        snprintf(ent_full, sizeof(ent_full), "%s/%s", full, ent->d_name);
        struct stat st;
                    if (lstat(ent_full, &st) == 0) {
            out->items[out->count].mode = (uint32_t)st.st_mode;
            out->items[out->count].size = (uint64_t)st.st_size;
        } else {
            out->items[out->count].mode = 0;
            out->items[out->count].size = 0;
        }
        out->count++;
    }
    closedir(dir);
    return 0;
}

void brs_vfs_local_list_free(BrsVfsList *list)
{
    if (list) { free(list->items); list->items = NULL; list->count = 0; }
}

int brs_vfs_local_stat(BrsVfs *vfs, const char *path, uint64_t *size, uint32_t *mode)
{
    char full[8192];
    if (vfs_build_path(vfs, path, full, sizeof(full)) != 0) return -1;
    struct stat st;
        if (lstat(full, &st) != 0) return -1;
    if (size) *size = (uint64_t)st.st_size;
    if (mode) *mode = (uint32_t)st.st_mode;
    return 0;
}

int brs_vfs_local_exists(BrsVfs *vfs, const char *path)
{
    char full[8192];
    if (vfs_build_path(vfs, path, full, sizeof(full)) != 0) return 0;
    struct stat st;
    return (stat(full, &st) == 0) ? 1 : 0;
}

const char *brs_vfs_local_backend_name(BrsVfs *vfs)
{
    (void)vfs;
    return "local";
}

int brs_vfs_local_supports_hardlinks(BrsVfs *vfs) { (void)vfs; return 1; }
int brs_vfs_local_supports_atomic_rename(BrsVfs *vfs) { (void)vfs; return 1; }
