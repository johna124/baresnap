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
/* brs_lock.h — Lock de repositorio para exclusión mutua entre procesos.
 *
 * Uso:
 *   BrsRepoLock lock;
 *   if (brs_repo_lock_acquire(&lock, repo_path, "prune") != 0) return 1;
 *   ... trabajo ...
 *   brs_repo_lock_release(&lock);
 *
 * Garantías:
 *   - Si el proceso muere (kill -9, SIGSEGV, power loss), el kernel
 *     libera el flock automáticamente al cerrar el fd.
 *   - El fichero .lock queda en disco pero con contenido vacío;
 *     el siguiente acquire lo reutiliza sin problemas.
 *   - Para repos SSH, el lock es local (evita 2 procesos locales
 *     operando sobre el mismo repo remoto simultáneamente).
 */
#ifndef BRS_LOCK_H
#define BRS_LOCK_H

#include "brs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      fd;                    /* fd del .lock (-1 = no adquirido) */
    char     path[BRS_PATH_MAX];   /* ruta completa del .lock */
    char     operation[64];        /* operación que adquirió el lock */
    uint64_t acquired_ns;           /* timestamp de adquisición */
} BrsRepoLock;

/* Adquiere el lock exclusivo del repositorio.
 * Devuelve 0 si OK, -1 si otro proceso lo tiene (imprime info del holder). */
int brs_repo_lock_acquire(BrsRepoLock *lock, const char *repo_path,
                          const char *operation);

/* Libera el lock. Seguro con fd == -1 (no-op). */
void brs_repo_lock_release(BrsRepoLock *lock);

/* Verifica si un lock existente es stale (el PID ya no vive).
 * Devuelve 1 = stale, 0 = válido, -1 = no hay lock. */
int brs_repo_lock_is_stale(const char *repo_path);

/* Inicializa a estado "no adquirido". */
void brs_repo_lock_init(BrsRepoLock *lock);

#ifdef __cplusplus
}
#endif

#endif /* BRS_LOCK_H */
