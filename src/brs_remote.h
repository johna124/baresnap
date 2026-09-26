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
#ifndef BRS_REMOTE_H
#define BRS_REMOTE_H

#include "brs_remote_protocol.h"
#include <stdint.h>
#include <stddef.h>


/* Timeout SSH configurable (en ms). Default: 30000.
   Configurable via --timeout en CLI o BRS_SSH_TIMEOUT_MS env. */
extern int brs_ssh_timeout_ms;

/* Progreso simple CLI para VFS SSH */
void brs_vfs_ssh_cli_progress_begin(const char *title);
void brs_vfs_ssh_cli_progress_end(void);

/* Handle de conexion remota */
typedef struct BrsRemote BrsRemote;

/* Conectar a un agente remoto via file descriptors */
BrsRemote *brs_remote_connect(int read_fd, int write_fd);

/* Desconectar y liberar recursos */
void brs_remote_disconnect(BrsRemote *r);

/* Operaciones remotas */
int brs_remote_open(BrsRemote *r, const char *path, int flags);
int brs_remote_close(BrsRemote *r, int handle);
int brs_remote_read(BrsRemote *r, int handle, uint64_t offset, void *buf, size_t len);
int brs_remote_write(BrsRemote *r, int handle, uint64_t offset, const void *buf, size_t len);
int brs_remote_rename(BrsRemote *r, const char *from, const char *to);
int brs_remote_unlink(BrsRemote *r, const char *path);
int brs_remote_list(BrsRemote *r, const char *path, char ***names, size_t *count);
int brs_remote_stat(BrsRemote *r, const char *path, uint64_t *size, uint32_t *mode);
int brs_remote_hardlink(BrsRemote *r, const char *existing, const char *newpath);
int brs_remote_mkdir(BrsRemote *r, const char *path, uint32_t mode);
int brs_remote_sync_index(BrsRemote *r, uint8_t **dump, uint32_t *dump_len);
int brs_remote_upload_pack(BrsRemote *r, const char *remote_path,
                           const char *local_path, uint64_t *out_size);

#endif /* BRS_REMOTE_H */
