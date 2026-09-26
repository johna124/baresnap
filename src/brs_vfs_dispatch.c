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
#include "brs_vfs.h"
#include "brs_vfs_context.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "brs_vfs.h"
#include "brs_vfs_context.h"
#include <string.h>
#include <stdlib.h>

/* Declaraciones de los backends */
extern BrsVfs *brs_vfs_local_open(const char *uri, int flags);
extern void    brs_vfs_local_close(BrsVfs *vfs);
extern BrsVfsFile *brs_vfs_local_fopen(BrsVfs *vfs, const char *path, int flags);
extern int     brs_vfs_local_fclose(BrsVfsFile *f);
extern ssize_t brs_vfs_local_fread(BrsVfsFile *f, void *buf, size_t n, uint64_t off);
extern ssize_t brs_vfs_local_fwrite(BrsVfsFile *f, const void *buf, size_t n, uint64_t off);
extern int     brs_vfs_local_fsync(BrsVfsFile *f);
extern int     brs_vfs_local_rename(BrsVfs *vfs, const char *from, const char *to);
extern int     brs_vfs_local_unlink(BrsVfs *vfs, const char *path);
extern int     brs_vfs_local_mkdir(BrsVfs *vfs, const char *path, uint32_t mode);
extern int     brs_vfs_local_link(BrsVfs *vfs, const char *existing, const char *newpath);
extern int     brs_vfs_local_list(BrsVfs *vfs, const char *path, BrsVfsList *out);
extern void    brs_vfs_local_list_free(BrsVfsList *list);
extern int     brs_vfs_local_stat(BrsVfs *vfs, const char *path, uint64_t *size, uint32_t *mode);
extern int     brs_vfs_local_exists(BrsVfs *vfs, const char *path);
extern const char *brs_vfs_local_backend_name(BrsVfs *vfs);
extern int     brs_vfs_local_supports_hardlinks(BrsVfs *vfs);
extern int     brs_vfs_local_supports_atomic_rename(BrsVfs *vfs);

extern BrsVfs *brs_vfs_ssh_open(const char *uri, int flags);
extern void    brs_vfs_ssh_close(BrsVfs *vfs);
extern BrsVfsFile *brs_vfs_ssh_fopen(BrsVfs *vfs, const char *path, int flags);
extern int     brs_vfs_ssh_fclose(BrsVfsFile *f);
extern ssize_t brs_vfs_ssh_fread(BrsVfsFile *f, void *buf, size_t n, uint64_t off);
extern ssize_t brs_vfs_ssh_fwrite(BrsVfsFile *f, const void *buf, size_t n, uint64_t off);
extern int     brs_vfs_ssh_fsync(BrsVfsFile *f);
extern int     brs_vfs_ssh_upload_pack(BrsVfs *vfs, const char *remote_path,
                                       const char *local_path, uint64_t *out_size);
extern int     brs_vfs_ssh_rename(BrsVfs *vfs, const char *from, const char *to);
extern int     brs_vfs_ssh_unlink(BrsVfs *vfs, const char *path);
extern int     brs_vfs_ssh_mkdir(BrsVfs *vfs, const char *path, uint32_t mode);
extern int     brs_vfs_ssh_link(BrsVfs *vfs, const char *existing, const char *newpath);
extern int     brs_vfs_ssh_list(BrsVfs *vfs, const char *path, BrsVfsList *out);
extern void    brs_vfs_ssh_list_free(BrsVfsList *list);
extern int     brs_vfs_ssh_stat(BrsVfs *vfs, const char *path, uint64_t *size, uint32_t *mode);
extern int     brs_vfs_ssh_exists(BrsVfs *vfs, const char *path);
extern const char *brs_vfs_ssh_backend_name(BrsVfs *vfs);
extern int     brs_vfs_ssh_supports_hardlinks(BrsVfs *vfs);
extern int     brs_vfs_ssh_supports_atomic_rename(BrsVfs *vfs);

/* Vtable */
typedef struct {
    const char *name;
    BrsVfs *(*open)(const char *uri, int flags);
    void (*close)(BrsVfs *vfs);
    BrsVfsFile *(*fopen)(BrsVfs *vfs, const char *path, int flags);
    int (*fclose)(BrsVfsFile *f);
    ssize_t (*fread)(BrsVfsFile *f, void *buf, size_t n, uint64_t off);
    ssize_t (*fwrite)(BrsVfsFile *f, const void *buf, size_t n, uint64_t off);
    int (*fsync)(BrsVfsFile *f);
    int (*upload_pack)(BrsVfs *vfs, const char *remote_path,
                       const char *local_path, uint64_t *out_size);
    int (*rename)(BrsVfs *vfs, const char *from, const char *to);
    int (*unlink)(BrsVfs *vfs, const char *path);
    int (*mkdir)(BrsVfs *vfs, const char *path, uint32_t mode);
    int (*link)(BrsVfs *vfs, const char *existing, const char *newpath);
    int (*list)(BrsVfs *vfs, const char *path, BrsVfsList *out);
    void (*list_free)(BrsVfsList *list);
    int (*stat)(BrsVfs *vfs, const char *path, uint64_t *size, uint32_t *mode);
    int (*exists)(BrsVfs *vfs, const char *path);
    const char *(*backend_name)(BrsVfs *vfs);
    int (*supports_hardlinks)(BrsVfs *vfs);
    int (*supports_atomic_rename)(BrsVfs *vfs);
} BrsVfsVtable;

static const BrsVfsVtable g_vfs_local_vt = {
    .name = "local",
    .open = brs_vfs_local_open,
    .close = brs_vfs_local_close,
    .fopen = brs_vfs_local_fopen,
    .fclose = brs_vfs_local_fclose,
    .fread = brs_vfs_local_fread,
    .fwrite = brs_vfs_local_fwrite,
    .fsync = brs_vfs_local_fsync,
    .upload_pack = NULL,
    .rename = brs_vfs_local_rename,
    .unlink = brs_vfs_local_unlink,
    .mkdir = brs_vfs_local_mkdir,
    .link = brs_vfs_local_link,
    .list = brs_vfs_local_list,
    .list_free = brs_vfs_local_list_free,
    .stat = brs_vfs_local_stat,
    .exists = brs_vfs_local_exists,
    .backend_name = brs_vfs_local_backend_name,
    .supports_hardlinks = brs_vfs_local_supports_hardlinks,
    .supports_atomic_rename = brs_vfs_local_supports_atomic_rename,
};

static const BrsVfsVtable g_vfs_ssh_vt = {
    .name = "ssh",
    .open = brs_vfs_ssh_open,
    .close = brs_vfs_ssh_close,
    .fopen = brs_vfs_ssh_fopen,
    .fclose = brs_vfs_ssh_fclose,
    .fread = brs_vfs_ssh_fread,
    .fwrite = brs_vfs_ssh_fwrite,
    .fsync = brs_vfs_ssh_fsync,
    .upload_pack = brs_vfs_ssh_upload_pack,
    .rename = brs_vfs_ssh_rename,
    .unlink = brs_vfs_ssh_unlink,
    .mkdir = brs_vfs_ssh_mkdir,
    .link = brs_vfs_ssh_link,
    .list = brs_vfs_ssh_list,
    .list_free = brs_vfs_ssh_list_free,
    .stat = brs_vfs_ssh_stat,
    .exists = brs_vfs_ssh_exists,
    .backend_name = brs_vfs_ssh_backend_name,
    .supports_hardlinks = brs_vfs_ssh_supports_hardlinks,
    .supports_atomic_rename = brs_vfs_ssh_supports_atomic_rename,
};

/* Wrapper interno: guarda el vtable junto al backend */
typedef struct {
    const BrsVfsVtable *vt;
    BrsVfs *backend;
} BrsVfsInternal;

/* Helper: pela el prefijo ssh://... si el contexto VFS está activo */
static const char *strip_if_needed(const char *path) {
    if (!path) return NULL;
    const char *rel = brs_vfs_context_strip(path);
    return rel ? rel : path;
}

/* ================================================================
   API pública
   ================================================================ */

BrsVfs *brs_vfs_open(const char *uri, int flags)
{
    if (!uri) return NULL;

    const BrsVfsVtable *vt = NULL;
    if (strncmp(uri, "ssh://", 6) == 0)
    vt = &g_vfs_ssh_vt;
    else if (strncmp(uri, "tls://", 6) == 0 || strncmp(uri, "sftp://", 7) == 0) {
    fprintf(stderr, "error: unsupported VFS scheme: %s\n", uri);
    return NULL;
    } else vt = &g_vfs_local_vt;

    BrsVfsInternal *internal = (BrsVfsInternal *)calloc(1, sizeof(BrsVfsInternal));
    if (!internal) return NULL;

    internal->backend = vt->open(uri, flags);
    if (!internal->backend) {
        free(internal);
        return NULL;
    }
    internal->vt = vt;
    return (BrsVfs *)internal;
}

void brs_vfs_close(BrsVfs *vfs)
{
    void brs_clear_cached_pack_handle(void);
    brs_clear_cached_pack_handle();

    if (!vfs) {
        return;
    }

    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;

    if (internal->vt && internal->vt->close) {
        internal->vt->close(internal->backend);
    }
    brs_vfs_context_clear();
    free(internal);
}

BrsVfsFile *brs_vfs_fopen(BrsVfs *vfs, const char *path, int flags)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    BrsVfsFile *f = internal->vt->fopen(internal->backend, strip_if_needed(path), flags);
    if (f) f->vfs_parent = vfs;
    return f;
}

int brs_vfs_fclose(BrsVfsFile *f)
{
    if (!f) return -1;
    BrsVfsInternal *internal = (BrsVfsInternal *)f->vfs_parent;
    return internal->vt->fclose(f);
}

ssize_t brs_vfs_fread(BrsVfsFile *f, void *buf, size_t n, uint64_t off)
{
    if (!f) return -1;
    BrsVfsInternal *internal = (BrsVfsInternal *)f->vfs_parent;
    return internal->vt->fread(f, buf, n, off);
}

ssize_t brs_vfs_fwrite(BrsVfsFile *f, const void *buf, size_t n, uint64_t off)
{
    if (!f) return -1;
    BrsVfsInternal *internal = (BrsVfsInternal *)f->vfs_parent;
    return internal->vt->fwrite(f, buf, n, off);
}

int brs_vfs_fsync(BrsVfsFile *f)
{
    if (!f) return -1;
    BrsVfsInternal *internal = (BrsVfsInternal *)f->vfs_parent;
    return internal->vt->fsync(f);
}

int brs_vfs_rename(BrsVfs *vfs, const char *from, const char *to)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    return internal->vt->rename(internal->backend,
                                strip_if_needed(from),
                                strip_if_needed(to));
}

int brs_vfs_unlink(BrsVfs *vfs, const char *path)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    return internal->vt->unlink(internal->backend, strip_if_needed(path));
}

int brs_vfs_mkdir(BrsVfs *vfs, const char *path, uint32_t mode)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    return internal->vt->mkdir(internal->backend, strip_if_needed(path), mode);
}

int brs_vfs_link(BrsVfs *vfs, const char *existing, const char *newpath)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    return internal->vt->link(internal->backend,
                              strip_if_needed(existing),
                              strip_if_needed(newpath));
}

int brs_vfs_list(BrsVfs *vfs, const char *path, BrsVfsList *out)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    return internal->vt->list(internal->backend, strip_if_needed(path), out);
}

void brs_vfs_list_free(BrsVfsList *list)
{
    if (list) {
        free(list->items);
        list->items = NULL;
        list->count = 0;
    }
}

int brs_vfs_upload_pack(BrsVfs *vfs, const char *remote_path,
                        const char *local_path, uint64_t *out_size)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    if (!internal->vt->upload_pack)
        return -1;
    return internal->vt->upload_pack(internal->backend,
                                     strip_if_needed(remote_path),
                                     local_path, out_size);
}

int brs_vfs_stat(BrsVfs *vfs, const char *path, uint64_t *size, uint32_t *mode)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    return internal->vt->stat(internal->backend, strip_if_needed(path), size, mode);
}

int brs_vfs_exists(BrsVfs *vfs, const char *path)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    return internal->vt->exists(internal->backend, strip_if_needed(path));
}

const char *brs_vfs_backend_name(BrsVfs *vfs)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    return internal->vt->backend_name(internal->backend);
}

int brs_vfs_supports_hardlinks(BrsVfs *vfs)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    return internal->vt->supports_hardlinks(internal->backend);
}

int brs_vfs_supports_atomic_rename(BrsVfs *vfs)
{
    BrsVfsInternal *internal = (BrsVfsInternal *)vfs;
    return internal->vt->supports_atomic_rename(internal->backend);
}

