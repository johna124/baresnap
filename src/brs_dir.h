/* brs_dir.h — listado de directorios (opendir/readdir) */
#ifndef BRS_DIR_H
#define BRS_DIR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char **names;
    size_t count;
    size_t cap;
} BrsDirList;

/* Lista nombres (sin "." ni ".."). 0 ok, -1 error. */
int  brs_list_dir(const char *dir, BrsDirList *out);
void brs_dir_list_free(BrsDirList *list);
void brs_dir_list_sort(BrsDirList *list);

#ifdef __cplusplus
}
#endif

#endif /* BRS_DIR_H */