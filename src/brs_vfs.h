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
#ifndef BRS_VFS_H
#define BRS_VFS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "brs_index.h"

/* Forward declaration del backend handle (opaco para el usuario) */
typedef struct BrsVfs BrsVfs;

/* BrsVfsFile es CONCRETO: el dispatcher necesita acceder a vfs_parent */
typedef struct BrsVfsFile {
    void *impl;        /* datos del backend (fd, handle remoto, etc.) */
    void *vfs_parent;  /* puntero al BrsVfs padre (en realidad BrsVfsInternal) */
} BrsVfsFile;

typedef struct {
    char name[256];
    uint32_t mode;
    uint64_t size;
} BrsVfsEntry;

typedef struct {
    BrsVfsEntry *items;
    size_t count;
} BrsVfsList;

/* Flags de apertura */
#define BRS_VFS_OPEN_READ   0x01
#define BRS_VFS_OPEN_WRITE  0x02
#define BRS_VFS_OPEN_APPEND 0x04
#define BRS_VFS_OPEN_TRUNC  0x08
#define BRS_VFS_OPEN_CREAT  0x10

/* Apertura/cierre del VFS */
BrsVfs *brs_vfs_open(const char *uri, int flags);
void    brs_vfs_close(BrsVfs *vfs);

/* Operaciones sobre ficheros */
BrsVfsFile *brs_vfs_fopen(BrsVfs *vfs, const char *path, int flags);
int         brs_vfs_fclose(BrsVfsFile *f);
ssize_t     brs_vfs_fread(BrsVfsFile *f, void *buf, size_t n, uint64_t off);
ssize_t     brs_vfs_fwrite(BrsVfsFile *f, const void *buf, size_t n, uint64_t off);
int         brs_vfs_fsync(BrsVfsFile *f);
int brs_vfs_upload_pack(BrsVfs *vfs, const char *remote_path,
                        const char *local_path, uint64_t *out_size);

/* Operaciones atomicas */
int brs_vfs_rename(BrsVfs *vfs, const char *from, const char *to);
int brs_vfs_unlink(BrsVfs *vfs, const char *path);
int brs_vfs_mkdir(BrsVfs *vfs, const char *path, uint32_t mode);
int brs_vfs_link(BrsVfs *vfs, const char *existing, const char *newpath);

/* Listado */
int brs_vfs_list(BrsVfs *vfs, const char *path, BrsVfsList *out);
void brs_vfs_list_free(BrsVfsList *list);

/* Metadata */
int brs_vfs_stat(BrsVfs *vfs, const char *path, uint64_t *size, uint32_t *mode);
int brs_vfs_exists(BrsVfs *vfs, const char *path);

/* Info del backend */
const char *brs_vfs_backend_name(BrsVfs *vfs);
int brs_vfs_supports_hardlinks(BrsVfs *vfs);
int brs_vfs_supports_atomic_rename(BrsVfs *vfs);

/* Función puente para el Fast Path de SSH */
int brs_vfs_ssh_bulk_sync_index(BrsVfs *vfs, BrsIndexMap *out_map);
int brs_vfs_sync_index_bulk(BrsVfs *vfs, const char *repo_path, BrsIndexMap *out_map);
int brs_vfs_upload_pack(BrsVfs *vfs, const char *remote_path,
                        const char *local_path, uint64_t *out_size);


#endif /* BRS_VFS_H */
