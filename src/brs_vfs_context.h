#ifndef BRS_VFS_CONTEXT_H
#define BRS_VFS_CONTEXT_H

#include "brs_vfs.h"

/* Contexto VFS global.
   Se establece al inicio de cada operación pública.
   Si es NULL, todas las funciones de I/O usan POSIX directo. */
void        brs_vfs_context_set(BrsVfs *vfs, const char *base_path);
void        brs_vfs_context_clear(void);
BrsVfs     *brs_vfs_context_get(void);
const char *brs_vfs_context_base(void);

/* Devuelve el path relativo al repo si 'path' pertenece al repo remoto.
   Devuelve NULL si el path no pertenece al repo (ej: target del restore). */
const char *brs_vfs_context_strip(const char *path);

/* Conveniencia: ¿este path es del repo remoto? */
int brs_vfs_context_is_remote_path(const char *path);

#endif /* BRS_VFS_CONTEXT_H */
