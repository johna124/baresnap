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
#ifndef BRS_REPO_LOCK_H
#define BRS_REPO_LOCK_H
/* WARNING: Este header define un BrsRepoLock DIFERENTE al de
 * brs_lock.h. NO incluir ambos en la misma unidad de traducción.
 * Este header parece ser un residuo de la migración C++. */
#ifdef BRS_LOCK_H
#error "brs_repo_lock.h y brs_lock.h definen BrsRepoLock conflictivos"
#endif
#include "brs_types.h" /* Para BRS_PATH_MAX */#ifndef BRS_REPO_LOCK_H
#define BRS_REPO_LOCK_H

#include "brs_types.h" /* Para BRS_PATH_MAX */

typedef struct {
    int fd;
    int locked;
    char path[BRS_PATH_MAX];
} BrsRepoLock;

void brs_repo_lock_init(BrsRepoLock *lock);
int  brs_repo_lock_acquire(BrsRepoLock *lock, const char *repo_path, const char *op_name);
void brs_repo_lock_release(BrsRepoLock *lock);

#endif /* BRS_REPO_LOCK_H */
