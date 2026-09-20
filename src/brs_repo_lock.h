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
