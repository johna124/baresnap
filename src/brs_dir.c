/* brs_dir.c — listado de directorios con soporte VFS */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "brs_dir.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include "brs_vfs_context.h"

static int dir_list_push(BrsDirList *l, const char *name)
{
    if (l->count == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        char **p = (char **)realloc(l->names, ncap * sizeof *p);
        if (!p) return -1;
        l->names = p;
        l->cap = ncap;
    }
    char *dup = strdup(name);
    if (!dup) return -1;
    l->names[l->count++] = dup;
    return 0;
}

int brs_list_dir(const char *dir, BrsDirList *out)
{
    /* VFS: si el contexto esta activo, listar via VFS */
    {
        BrsVfs *vfs = brs_vfs_context_get();
        const char *rel = brs_vfs_context_strip(dir);
        if (vfs && rel) {
            BrsVfsList vlist;
            memset(out, 0, sizeof *out);
            if (brs_vfs_list(vfs, rel, &vlist) != 0) return -1;
            if (vlist.count == 0) { brs_vfs_list_free(&vlist); return 0; }
            out->names = (char **)calloc(vlist.count, sizeof(char *));
            if (!out->names) { brs_vfs_list_free(&vlist); return -1; }
            for (size_t i = 0; i < vlist.count; ++i) {
                out->names[i] = strdup(vlist.items[i].name);
                if (!out->names[i]) {
                    for (size_t j = 0; j < i; ++j) free(out->names[j]);
                    free(out->names);
                    out->names = NULL;
                    brs_vfs_list_free(&vlist);
                    return -1;
                }
                out->count++;
            }
            brs_vfs_list_free(&vlist);
            return 0;
        }
    }

    if (!dir || !out) return -1;
    memset(out, 0, sizeof *out);
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (dir_list_push(out, e->d_name) != 0) {
            closedir(d);
            brs_dir_list_free(out);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

void brs_dir_list_free(BrsDirList *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i)
        free(list->names[i]);
    free(list->names);
    memset(list, 0, sizeof *list);
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void brs_dir_list_sort(BrsDirList *list)
{
    if (list && list->count > 1)
        qsort(list->names, list->count, sizeof *list->names, name_cmp);
}
