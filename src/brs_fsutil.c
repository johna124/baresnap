/* brs_fsutil.c — rutas, mkdir -p, escrituras atomicas con soporte VFS
 *
 * FASE 3: Escritura atómica remota vía VFS/SSH.
 * Los archivos remotos se crean SIEMPRE con extensión temporal .tmp.<pid>
 * y se promueven a su nombre definitivo mediante rename() remoto síncrono.
 * Esto garantiza que ningún cliente vea archivos parciales o corruptos.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "brs_fsutil.h"
#include "brs_dir.h"
#include "brs_util.h"
#include "brs_vfs_context.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

int brs_path_join(char *out, size_t out_size, const char *a, const char *b)
{
    if (!out || out_size == 0 || !b)
        return -1;

    int n;
    if (!a || a[0] == '\0') {
        n = snprintf(out, out_size, "%s", b);
    } else {
        size_t alen = strlen(a);
        if (a[alen - 1] == '/')
            n = snprintf(out, out_size, "%s%s", a, b);
        else
            n = snprintf(out, out_size, "%s/%s", a, b);
    }

    if (n < 0 || (size_t)n >= out_size)
        return -1;

    return 0;
}

static int mkdir_one(const char *p)
{
    if (mkdir(p, 0700) == 0)
        return 0;

    if (errno == EEXIST) {
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
    }

    return -1;
}

int brs_mkdir_p(const char *path)
{
    /* VFS: si el contexto esta activo, crear via VFS */
    {
        BrsVfs *vfs = brs_vfs_context_get();
        const char *rel = brs_vfs_context_strip(path);

        if (vfs && rel) {
            /* Raiz del repo: asumir que existe */
            if (rel[0] == '\0' || (rel[0] == '.' && rel[1] == '\0'))
                return 0;

            int rc = brs_vfs_mkdir(vfs, rel, 0755);
            if (rc == 0)
                return 0;

            /* Ignorar EEXIST si ya es directorio */
            uint64_t sz;
            uint32_t md;
            if (brs_vfs_stat(vfs, rel, &sz, &md) == 0 &&
                (md & 0170000) == 0040000)
                return 0;

            return -1;
        }
    }

    /* POSIX local */
    if (!path || path[0] == '\0')
        return -1;

    char tmp[BRS_PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof tmp)
        return -1;

    memcpy(tmp, path, len + 1);

    if (len > 1 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir_one(tmp) != 0)
                return -1;
            *p = '/';
        }
    }

    return mkdir_one(tmp);
}

int brs_path_exists(const char *path)
{
    {
        BrsVfs *vfs = brs_vfs_context_get();
        const char *rel = brs_vfs_context_strip(path);

        if (vfs && rel)
            return brs_vfs_exists(vfs, rel);
    }

    struct stat st;
    if (!path)
        return 0;

    return (stat(path, &st) == 0) ? 1 : 0;
}

int brs_is_directory(const char *path)
{
    /* VFS */
    {
        BrsVfs *vfs = brs_vfs_context_get();
        const char *rel = brs_vfs_context_strip(path);

        if (vfs && rel) {
            uint64_t sz;
            uint32_t md;
            if (brs_vfs_stat(vfs, rel, &sz, &md) != 0)
                return 0;
            return (md & 0170000) == 0040000;
        }
    }

    struct stat st;
    if (!path)
        return 0;
    if (stat(path, &st) != 0)
        return 0;

    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int brs_remove_file(const char *path)
{
    {
        BrsVfs *vfs = brs_vfs_context_get();
        const char *rel = brs_vfs_context_strip(path);

        if (vfs && rel)
            return brs_vfs_unlink(vfs, rel);
    }

    if (!path)
        return -1;

    if (unlink(path) != 0) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }

    return 0;
}

/* ============================================================================
 * FASE 3: Escritura atómica con soporte VFS.
 *
 * En modo remoto (VFS/SSH):
 *   1. Se crea el archivo con extensión temporal ".tmp.<pid>"
 *   2. Se escriben los datos y se hace fsync remoto
 *   3. Se ejecuta rename() remoto síncrono al nombre definitivo
 *   4. Si el rename falla, se elimina el temporal para no dejar residuos
 *
 * En modo local POSIX:
 *   Se usa tmp + rename atómico tradicional + fsync del directorio padre.
 * ==========================================================================*/
int brs_write_file_atomic(const char *path, const void *data, size_t n)
{
    if (!path)
        return -1;
    if (n > 0 && !data)
        return -1;

    /* VFS: si el contexto esta activo, escribir directamente via VFS */
    {
        BrsVfs *vfs = brs_vfs_context_get();
        const char *rel = brs_vfs_context_strip(path);

        if (vfs && rel) {
            /* -------------------------------------------------------------
             * FASE 3: Escribir a un nombre temporal con extensión .tmp.<pid>
             * para garantizar atomicidad ante fallos.
             * ---------------------------------------------------------- */
            char rel_tmp[BRS_PATH_MAX];
            int sn = snprintf(rel_tmp, sizeof rel_tmp, "%s.tmp.%d",
                              rel, (int)getpid());
            if (sn < 0 || (size_t)sn >= sizeof rel_tmp)
                return -1;

            BrsVfsFile *f = brs_vfs_fopen(vfs, rel_tmp,
                                          BRS_VFS_OPEN_WRITE |
                                          BRS_VFS_OPEN_CREAT |
                                          BRS_VFS_OPEN_TRUNC);
            if (!f)
                return -1;

            if (n > 0) {
                ssize_t w = brs_vfs_fwrite(f, data, n, 0);
                if (w != (ssize_t)n) {
                    brs_vfs_fclose(f);
                    brs_vfs_unlink(vfs, rel_tmp);
                    return -1;
                }
            }

            brs_vfs_fsync(f);
            brs_vfs_fclose(f);

            /* -------------------------------------------------------------
             * FASE 3: rename() remoto síncrono al nombre definitivo.
             * Si falla, eliminamos el temporal para evitar residuos.
             * ---------------------------------------------------------- */
            if (brs_vfs_rename(vfs, rel_tmp, rel) != 0) {
                brs_vfs_unlink(vfs, rel_tmp);
                return -1;
            }

            return 0;
        }
    }

    /* POSIX local */
    char tmp[BRS_PATH_MAX];
    int m = snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    if (m < 0 || (size_t)m >= sizeof tmp)
        return -1;

    int fd = open(tmp,
                  O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                  0600);
    if (fd < 0)
        return -1;

    int rc = (n > 0) ? brs_write_fd_all(fd, data, n) : 0;
    if (rc == 0 && fsync(fd) != 0)
        rc = -1;
    if (close(fd) != 0)
        rc = -1;

    if (rc != 0) {
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }

    char dir[BRS_PATH_MAX];
    size_t plen = strlen(path);
    if (plen < sizeof dir) {
        memcpy(dir, path, plen + 1);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir)
            *slash = '\0';
        else if (slash == dir)
            dir[1] = '\0';

        int dfd = open(dir, O_RDONLY);
        if (dfd >= 0) {
            (void)fsync(dfd);
            close(dfd);
        }
    }

    return 0;
}

int brs_remove_files_with_suffix(const char *dir, const char *suffix)
{
    if (!dir || !suffix)
        return -1;

    BrsDirList l;
    if (brs_list_dir(dir, &l) != 0)
        return -1;

    size_t slen = strlen(suffix);
    int removed = 0;

    for (size_t i = 0; i < l.count; ++i) {
        size_t nlen = strlen(l.names[i]);
        if (nlen > slen &&
            strcmp(l.names[i] + nlen - slen, suffix) == 0) {
            char p[BRS_PATH_MAX];
            if (brs_path_join(p, sizeof p, dir, l.names[i]) == 0 &&
                brs_remove_file(p) == 0) {
                removed++;
            }
        }
    }

    brs_dir_list_free(&l);
    return removed;
}
