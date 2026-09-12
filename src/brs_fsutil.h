/* brs_fsutil.h — utilidades de rutas y filesystem (POSIX puro) */
#ifndef BRS_FSUTIL_H
#define BRS_FSUTIL_H

#include <stddef.h>
#include "brs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int brs_path_join(char *out, size_t out_size, const char *a, const char *b);
int brs_mkdir_p(const char *path);
int brs_path_exists(const char *path);
int brs_is_directory(const char *path);
int brs_remove_file(const char *path);

/* Escritura atomica: tmp + fsync + rename + fsync del directorio.
 * Ante corte de luz el destino queda completo-viejo o completo-nuevo,
 * nunca corrupto. */
int brs_write_file_atomic(const char *path, const void *data, size_t n);

/* Borra ficheros cuyo nombre termine en suffix dentro de dir.
 * Devuelve cuantos borro, o -1 si no pudo listar. */
int brs_remove_files_with_suffix(const char *dir, const char *suffix);

#ifdef __cplusplus
}
#endif

#endif /* BRS_FSUTIL_H */