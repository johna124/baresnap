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
