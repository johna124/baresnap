/* brs_lock.c — Lock de repositorio con flock().
 *
 * flock(LOCK_EX|LOCK_NB):
 *   - LOCK_EX: exclusivo (solo un proceso a la vez).
 *   - LOCK_NB: no bloqueante (falla inmediatamente si otro lo tiene).
 *
 * Al cerrar el fd (o morir el proceso), el kernel libera el flock
 * automáticamente. Un kill -9 NUNCA deja el repo bloqueado.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "brs_lock.h"
#include "brs_fsutil.h"
#include "brs_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

/* Para repos SSH, el lock se basa en un hash local de la URI. */
static int lock_path_from_repo(const char *repo_path, char *out, size_t out_size)
{
    if (!repo_path || !out || out_size == 0) return -1;

    /* Si es una URI ssh://, usar un lock en /tmp basado en hash de la URI */
    if (strncmp(repo_path, "ssh://", 6) == 0 ||
        strncmp(repo_path, "tls://", 6) == 0 ||
        strncmp(repo_path, "sftp://", 7) == 0) {
        uint64_t h = brs_fnv1a_64(repo_path, strlen(repo_path));
        int n = snprintf(out, out_size, "/tmp/.baresnap_lock_%016llx",
                         (unsigned long long)h);
        if (n < 0 || (size_t)n >= out_size) return -1;
        return 0;
    }

    /* Para repos locales, el lock vive dentro del repo */
    if (brs_path_join(out, out_size, repo_path, ".lock") != 0) return -1;
    return 0;
}

void brs_repo_lock_init(BrsRepoLock *lock)
{
    if (!lock) return;
    lock->fd = -1;
    lock->path[0] = '\0';
    lock->operation[0] = '\0';
    lock->acquired_ns = 0;
}

int brs_repo_lock_acquire(BrsRepoLock *lock, const char *repo_path,
                          const char *operation)
{
    if (!lock || !repo_path || !operation) return -1;
    brs_repo_lock_init(lock);

    if (lock_path_from_repo(repo_path, lock->path, sizeof lock->path) != 0)
        return -1;

    snprintf(lock->operation, sizeof lock->operation, "%s", operation);

    /* Abrir (o crear) el fichero de lock */
    lock->fd = open(lock->path, O_RDWR | O_CREAT, 0600);
    if (lock->fd < 0) {
        fprintf(stderr, "error: cannot open lock file: %s (%s)\n",
                lock->path, strerror(errno));
        return -1;
    }

    /* Intentar adquirir el lock exclusivo de forma no bloqueante */
    if (flock(lock->fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            /* Otro proceso tiene el lock. Leer contenido para informar. */
            char buf[256];
            memset(buf, 0, sizeof buf);
            ssize_t n = pread(lock->fd, buf, sizeof buf - 1, 0);
            if (n > 0) buf[n] = '\0';

            int holder_pid = 0;
            char holder_op[64] = "unknown";
            sscanf(buf, "pid=%d ts=%*u op=%63s", &holder_pid, holder_op);

            fprintf(stderr,
                    "error: repository is locked by another process\n"
                    "  lock file: %s\n"
                    "  holder:    pid=%d operation=%s\n"
                    "  If the process is dead, the lock is stale.\n"
                    "  Wait for it to finish or kill the holder.\n",
                    lock->path, holder_pid, holder_op);

            close(lock->fd);
            lock->fd = -1;
            return -1;
        }
        fprintf(stderr, "error: flock failed: %s (%s)\n",
                lock->path, strerror(errno));
        close(lock->fd);
        lock->fd = -1;
        return -1;
    }

    /* Lock adquirido. Escribir contenido informativo. */
    lock->acquired_ns = brs_now_ns();
    char content[256];
    int len = snprintf(content, sizeof content,
                       "pid=%d ts=%llu op=%s\n",
                       (int)getpid(),
                       (unsigned long long)lock->acquired_ns,
                       lock->operation);
    if (len > 0) {
        if (ftruncate(lock->fd, 0) != 0) { /* no fatal */ }
        ssize_t w = pwrite(lock->fd, content, (size_t)len, 0);
        (void)w; /* no fatal si falla la escritura informativa */
    }

    return 0;
}

void brs_repo_lock_release(BrsRepoLock *lock)
{
    if (!lock) return;
    if (lock->fd < 0) return;

    /* Limpiar contenido del fichero (informativo) */
    if (ftruncate(lock->fd, 0) != 0) { /* no fatal */ }

    /* Cerrar el fd libera el flock automáticamente */
    close(lock->fd);
    lock->fd = -1;

    /* Para locks locales, borrar el fichero.
     * Para locks remotos (/tmp), también borrar. */
    unlink(lock->path);
}

int brs_repo_lock_is_stale(const char *repo_path)
{
    if (!repo_path) return -1;

    char lock_path[BRS_PATH_MAX];
    if (lock_path_from_repo(repo_path, lock_path, sizeof lock_path) != 0)
        return -1;

    int fd = open(lock_path, O_RDONLY);
    if (fd < 0) return -1; /* no hay lock */

    /* Intentar adquirir. Si se puede, no está en uso (stale o libre). */
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return 1; /* nadie lo retiene = stale o libre */
    }

    /* Alguien lo retiene. Verificar si el PID sigue vivo. */
    char buf[256];
    memset(buf, 0, sizeof buf);
    ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    close(fd);

    if (n <= 0) return 0;
    buf[n] = '\0';

    int holder_pid = 0;
    if (sscanf(buf, "pid=%d", &holder_pid) != 1) return 0;

    /* Verificar si el proceso existe via /proc */
    char proc_path[64];
    snprintf(proc_path, sizeof proc_path, "/proc/%d", holder_pid);
    struct stat st;
    if (stat(proc_path, &st) != 0) {
        return 1; /* el proceso ya no existe = stale */
    }

    return 0; /* el proceso sigue vivo, lock válido */
}
