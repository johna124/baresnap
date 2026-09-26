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
/* brs_repo.h — operaciones de repositorio */
#ifndef BRS_REPO_H
#define BRS_REPO_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include "brs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Callback de progreso: cb(user, fase, actual, total). Puede ser NULL. */
typedef void (*BrsProgressCallback)(void *user, const char *phase,
                                    uint64_t current, uint64_t total);

/* Cancelacion: puntero a _Atomic int o NULL. */
int brs_repo_create(const char *repo_path, const char *source_spec,
                    const char *label, int delta_binary,
                    BrsProgressCallback cb, void *cb_user,
                    _Atomic int *cancel_flag);


int brs_repo_restore(const char *repo_path, const char *snapshot_name,
                     const char *target_path,
                     BrsProgressCallback cb, void *cb_user,
                     _Atomic int *cancel_flag);

int brs_repo_verify(const char *repo_path, int fast_mode,
                    BrsProgressCallback cb, void *cb_user,
                    _Atomic int *cancel_flag);

int brs_repo_search(const char *repo_path,
                    const char *snapshot_name,   /* NULL = todos */
                    const char *pattern,
                    int use_glob,                /* 0=substring, 1=glob */
                    BrsProgressCallback cb,
                    void *cb_user,
                    _Atomic int *cancel_flag);

int brs_repo_list(const char *repo_path);

int brs_repo_prune(const char *repo_path,
                   int keep_last, int keep_daily, int keep_weekly,
                   int keep_monthly, int keep_yearly, int dry_run,
                   BrsProgressCallback cb, void *cb_user,
                   _Atomic int *cancel_flag);

int brs_repo_info(const char *repo_path);

int brs_repo_diff(const char *repo_path, const char *snap1, const char *snap2);

#ifdef __cplusplus
}
#endif


/* Extrae rutas concretas de un snapshot (admite directorios: extrae todo lo que cuelga). */
int brs_repo_extract(const char *repo_path, const char *snapshot_name,
                     const char *const *paths, size_t n_paths,
                     const char *target_dir,
                     BrsProgressCallback cb, void *cb_user,
                     _Atomic int *cancel_flag);
/* Carga config y deriva la clave si el repo es cifrado (passphrase por env o tty). */
int brs_repo_load_key(const char *repo_path, BrsRepoConfig *cfg_out,
                      BrsSecureKey *key_out);
/* Listar o buscar el contenido de un snapshot (tipo ls/find). */
int brs_repo_ls(const char *repo_path, const char *snapshot_name,
                const char *const *prefixes, size_t n_prefixes,
                int long_fmt, int recursive, const char *match);
#endif /* BRS_REPO_H */
