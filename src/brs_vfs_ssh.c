/* brs_vfs_ssh.c — backend VFS SSH con progreso CLI y límites de I/O */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "brs_remote.h"
#include "brs_remote_protocol.h"
#include "brs_vfs.h"
#include "brs_uri.h"
#include "brs_types.h"
#include "brs_util.h"
#include "brs_fsutil.h"
#include "brs_index.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdint.h>
#include <poll.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef BRS_PATH_MAX
#define BRS_PATH_MAX PATH_MAX
#endif

#define BRS_SSH_READ_LIMIT_DEFAULT  (1u * 1024u * 1024u)
#define BRS_SSH_WRITE_LIMIT_DEFAULT (4u * 1024u * 1024u)
#define BRS_SSH_IO_LIMIT_MAX        (4u * 1024u * 1024u)

static uint64_t g_ssh_rpc_count = 0;
static uint64_t g_ssh_rpc_ns = 0;
static uint64_t g_ssh_bytes_sent = 0;
static uint64_t g_ssh_bytes_recv = 0;

static void ssh_rpc_record(struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);

    g_ssh_rpc_count++;
    g_ssh_rpc_ns += (uint64_t)(t1.tv_sec - t0->tv_sec) * 1000000000ULL +
                    (uint64_t)(t1.tv_nsec - t0->tv_nsec);
}

static void ssh_rpc_log(const char *op, const char *path)
{
    static int debug = -1;

    if (debug < 0) {
        const char *e = getenv("BRS_SSH_DEBUG");
        debug = (e && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
    }

    if (debug) {
        fprintf(stderr, "[SSH-RPC] %-20s %s\n", op, path ? path : "-");
    }
}

void brs_vfs_ssh_debug_stats(void)
{
    if (g_ssh_rpc_count > 0 || g_ssh_bytes_sent > 0) {
        char sent_str[64];
        char recv_str[64];

        brs_format_bytes(g_ssh_bytes_sent, sent_str, sizeof sent_str);
        brs_format_bytes(g_ssh_bytes_recv, recv_str, sizeof recv_str);

        double avg_ms = g_ssh_rpc_count > 0
                            ? (g_ssh_rpc_ns / 1e6) / (double)g_ssh_rpc_count
                            : 0.0;

        fprintf(stderr,
                "[SSH] %llu RPCs, %.3fs total, %.1fms avg | Sent: %s, Recv: %s\n",
                (unsigned long long)g_ssh_rpc_count,
                g_ssh_rpc_ns / 1e9,
                avg_ms,
                sent_str,
                recv_str);
    }
}

typedef struct {
    BrsRemote *remote;
    pid_t agent_pid;
    int pipe_read_fd;
    int pipe_write_fd;
    char repo_path[PATH_MAX];
    char ssh_uri[PATH_MAX];
    int ssh_port;
    pthread_mutex_t mtx;
} BrsVfsSsh;

typedef struct {
    BrsVfsSsh *ssh;
    int handle;
    uint8_t *wbuf;
    size_t wbuf_cap;
    size_t wbuf_len;
    uint64_t wbuf_off;
} BrsSshFileHandle;

typedef void (*BrsVfsReadProgressCb)(void *ctx, uint64_t done, uint64_t total);

static BrsVfsReadProgressCb g_vfs_read_progress_cb = NULL;
static void *g_vfs_read_progress_ctx = NULL;

void brs_vfs_ssh_set_read_progress(BrsVfsReadProgressCb cb, void *ctx)
{
    g_vfs_read_progress_cb = cb;
    g_vfs_read_progress_ctx = ctx;
}

static size_t ssh_io_from_env(const char *env_name, size_t default_value)
{
    const char *e = getenv(env_name);
    if (!e || e[0] == '\0')
        return default_value;

    char *end = NULL;
    long v = strtol(e, &end, 10);

    if (v <= 0 || end == e || *end != '\0')
        return default_value;

    return (size_t)v;
}

static size_t ssh_io_clamp(size_t v)
{
    if (v == 0)
        v = 65536;

    if (v > BRS_SSH_IO_LIMIT_MAX)
        v = BRS_SSH_IO_LIMIT_MAX;

#ifdef BRS_MSG_MAX_PAYLOAD
    if (BRS_MSG_MAX_PAYLOAD > 16) {
        size_t proto_max = (size_t)(BRS_MSG_MAX_PAYLOAD - 16);
        if (v > proto_max)
            v = proto_max;
    }
#endif

    return v;
}

static size_t ssh_max_read(void)
{
    size_t v = ssh_io_from_env("BRS_SSH_READ_IO",
                               BRS_SSH_READ_LIMIT_DEFAULT);
    return ssh_io_clamp(v);
}

static size_t ssh_max_write(void)
{
    size_t v = ssh_io_from_env("BRS_SSH_WRITE_IO",
                               BRS_SSH_WRITE_LIMIT_DEFAULT);
    return ssh_io_clamp(v);
}

typedef void (*BrsSshCliReadProgressCb)(void *ctx,
                                        uint64_t done,
                                        uint64_t total);

static BrsSshCliReadProgressCb g_cli_read_progress_cb = NULL;
static void *g_cli_read_progress_ctx = NULL;

typedef struct {
    const char *title;
    struct timespec start;
    struct timespec last;
    uint64_t last_done;
    uint64_t last_total;
    int active;
    int rendered;
} BrsSshCliProgress;

static BrsSshCliProgress g_cli_progress;

static double ssh_cli_progress_diff_sec(const struct timespec *a,
                                        const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) +
           (double)(b->tv_nsec - a->tv_nsec) / 1000000000.0;
}

static void ssh_cli_progress_cb(void *ctx, uint64_t done, uint64_t total)
{
    BrsSshCliProgress *p = (BrsSshCliProgress *)ctx;
    if (!p || !p->active)
        return;

    p->last_done = done;
    p->last_total = total;
    p->rendered = 1;

    if (!isatty(STDERR_FILENO))
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (done != total && p->rendered) {
        double since_last = ssh_cli_progress_diff_sec(&p->last, &now);
        if (since_last < 0.100)
            return;
    }

    p->last = now;

    double elapsed = ssh_cli_progress_diff_sec(&p->start, &now);
    uint64_t speed = (elapsed > 0.001)
                         ? (uint64_t)((double)done / elapsed)
                         : 0;

    char done_s[64];
    char total_s[64];
    char speed_s[64];

    brs_format_bytes(done, done_s, sizeof done_s);
    brs_format_bytes(total, total_s, sizeof total_s);
    brs_format_bytes(speed, speed_s, sizeof speed_s);

    int percent = total ? (int)(((double)done / (double)total) * 100.0) : 100;

    const int width = 24;
    int filled = total ? (int)(((double)done / (double)total) * width) : width;

    if (filled < 0)
        filled = 0;
    if (filled > width)
        filled = width;

    fprintf(stderr,
            "\r%s [%.*s%*s] %3d%% %s/%s %s/s                    ",
            p->title ? p->title : "Progreso",
            filled,
            "================================",
            width - filled,
            "",
            percent,
            done_s,
            total_s,
            speed_s);

    fflush(stderr);
}

void brs_vfs_ssh_cli_progress_begin(const char *title)
{
    memset(&g_cli_progress, 0, sizeof(g_cli_progress));

    g_cli_progress.title = title;
    clock_gettime(CLOCK_MONOTONIC, &g_cli_progress.start);
    g_cli_progress.last = g_cli_progress.start;
    g_cli_progress.active = 1;
    g_cli_progress.rendered = 0;

    g_cli_read_progress_cb = ssh_cli_progress_cb;
    g_cli_read_progress_ctx = &g_cli_progress;
}

void brs_vfs_ssh_cli_progress_end(void)
{
    if (!g_cli_progress.active)
        return;

    if (g_cli_progress.rendered) {
        if (isatty(STDERR_FILENO)) {
            fputc('\n', stderr);
            fflush(stderr);
        } else if (g_cli_progress.last_total > 0) {
            char done_s[64];
            char total_s[64];

            brs_format_bytes(g_cli_progress.last_done,
                             done_s,
                             sizeof done_s);

            brs_format_bytes(g_cli_progress.last_total,
                             total_s,
                             sizeof total_s);

            fprintf(stderr,
                    "%s: %s/%s\n",
                    g_cli_progress.title ? g_cli_progress.title : "Progreso",
                    done_s,
                    total_s);

            fflush(stderr);
        }
    }

    g_cli_progress.active = 0;
    g_cli_progress.rendered = 0;

    g_cli_read_progress_cb = NULL;
    g_cli_read_progress_ctx = NULL;
}

static int ssh_ensure_write_buffer(BrsSshFileHandle *h)
{
    if (!h)
        return -1;

    if (h->wbuf)
        return 0;

    size_t cap = ssh_max_write();
    if (cap == 0)
        return -1;

    h->wbuf = (uint8_t *)malloc(cap);
    if (!h->wbuf)
        return -1;

    h->wbuf_cap = cap;
    h->wbuf_len = 0;
    h->wbuf_off = 0;

    return 0;
}

static int ssh_flush_write(BrsSshFileHandle *h)
{
    if (!h || !h->ssh || !h->ssh->remote)
        return -1;

    if (h->wbuf_len == 0)
        return 0;

    if (!h->wbuf)
        return -1;

    size_t max = ssh_max_write();
    if (max == 0)
        return -1;

    size_t off = 0;

    while (off < h->wbuf_len) {
        size_t piece = h->wbuf_len - off;
        if (piece > max)
            piece = max;

        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        pthread_mutex_lock(&h->ssh->mtx);
        int w = brs_remote_write(h->ssh->remote, h->handle,
                                 h->wbuf_off + (uint64_t)off,
                                 h->wbuf + off, piece);
        pthread_mutex_unlock(&h->ssh->mtx);

        ssh_rpc_record(&t0);
        ssh_rpc_log("write", NULL);

        if (w <= 0) {
            if (off > 0) {
                h->wbuf_off += (uint64_t)off;
                h->wbuf_len -= off;
                memmove(h->wbuf, h->wbuf + off, h->wbuf_len);
            }
            return -1;
        }

        g_ssh_bytes_sent += (size_t)w;
        off += (size_t)w;
    }

    h->wbuf_off += (uint64_t)off;
    h->wbuf_len = 0;
    return 0;
}

static void ssh_free_write_buffer(BrsSshFileHandle *h)
{
    if (!h)
        return;

    if (h->wbuf) {
        free(h->wbuf);
        h->wbuf = NULL;
    }

    h->wbuf_cap = 0;
    h->wbuf_len = 0;
    h->wbuf_off = 0;
}

static int ssh_lazy_fsync(void)
{
    static int lazy = -1;

    if (lazy < 0) {
        const char *e = getenv("BRS_SSH_STRICT_FSYNC");
        lazy = (e && e[0] != '\0' && strcmp(e, "0") != 0) ? 0 : 1;
    }

    return lazy;
}

static int ensure_dirs_for_file(const char *file)
{
    char tmp[PATH_MAX];
    size_t len = strlen(file);

    if (len == 0 || len >= sizeof(tmp))
        return -1;

    memcpy(tmp, file, len + 1);

    for (char *p = tmp + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }

    return 0;
}

static int get_self_path(char *out, size_t out_size)
{
    ssize_t len = readlink("/proc/self/exe", out, out_size - 1);

    if (len <= 0) {
        snprintf(out, out_size, "baresnap");
        return 0;
    }

    out[len] = '\0';
    return 0;
}

static int write_askpass_wrapper(const char *path)
{
    char self_path[BRS_PATH_MAX];

    if (get_self_path(self_path, sizeof self_path) != 0)
        return -1;

    if (ensure_dirs_for_file(path) != 0)
        return -1;

    FILE *f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f,
            "#!/bin/sh\n"
            "exec '%s' --askpass-internal \"$@\"\n",
            self_path);

    if (fclose(f) != 0)
        return -1;

    if (chmod(path, 0700) != 0)
        return -1;

    return 0;
}

static int setup_askpass(char *out, size_t out_size)
{
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0')
        return -1;

    int n = snprintf(out, out_size, "%s/.local/libexec/baresnap-askpass", home);
    if (n < 0 || (size_t)n >= out_size)
        return -1;

    if (write_askpass_wrapper(out) == 0)
        return 0;

    return (access(out, X_OK) == 0) ? 0 : -1;
}

static int shell_single_quote(const char *in, char *out, size_t out_size)
{
    size_t o = 0;

    if (!in || !out || out_size < 3)
        return -1;

    out[o++] = '\'';

    for (const char *p = in; *p != '\0'; ++p) {
        if (*p == '\'') {
            if (o + 6 > out_size)
                return -1;

            out[o++] = '\'';
            out[o++] = '\\';
            out[o++] = '\'';
            out[o++] = '\'';
        } else {
            if (o + 3 > out_size)
                return -1;

            out[o++] = *p;
        }
    }

    if (o + 2 > out_size)
        return -1;

    out[o++] = '\'';
    out[o] = '\0';

    return 0;
}

static void close_fds_except(int keep1, int keep2)
{
    DIR *d = opendir("/proc/self/fd");

    if (!d) {
        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0)
            max_fd = 1024;

        for (int fd = 3; fd < max_fd; ++fd) {
            if (fd != keep1 && fd != keep2)
                close(fd);
        }
        return;
    }

    int dir_fd = dirfd(d);
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        char *end = NULL;
        long fd = strtol(ent->d_name, &end, 10);

        if (!end || *end != '\0')
            continue;

        if (fd <= 2)
            continue;

        if ((int)fd == dir_fd)
            continue;

        if ((int)fd == keep1 || (int)fd == keep2)
            continue;

        close((int)fd);
    }

    closedir(d);
}

static int ssh_waitpid_timeout(pid_t pid)
{
    if (pid <= 0)
        return 0;

    for (int i = 0; i < 100; ++i) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid)
            return 0;

        if (r < 0)
            return 0;

        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000000L;
        nanosleep(&ts, NULL);
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    return 0;
}

BrsVfs *brs_vfs_ssh_open(const char *uri, int flags)
{
    (void)flags;

    if (!uri || strncmp(uri, "ssh://", 6) != 0)
        return NULL;

    g_ssh_rpc_count = 0;
    g_ssh_rpc_ns = 0;
    g_ssh_bytes_sent = 0;
    g_ssh_bytes_recv = 0;

    BrsUri u;
    memset(&u, 0, sizeof(u));

    if (brs_uri_parse(uri, &u) != 0)
        return NULL;

    char userhost[512];

    if (u.user[0] != '\0')
        snprintf(userhost, sizeof userhost, "%s@%s", u.user, u.host);
    else
        snprintf(userhost, sizeof userhost, "%s", u.host);

    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", u.port > 0 ? u.port : 22);

    char askpass_path[BRS_PATH_MAX];
    int have_askpass = (setup_askpass(askpass_path, sizeof askpass_path) == 0);

    int pipe_to_agent[2] = {-1, -1};
    int pipe_from_agent[2] = {-1, -1};
    pid_t pid = -1;
    BrsRemote *remote = NULL;
    BrsVfsSsh *ssh = NULL;

    if (pipe(pipe_to_agent) != 0)
        goto fail_open;

    if (pipe(pipe_from_agent) != 0)
        goto fail_open;

    char connect_timeout_opt[64];
    {
        int tsec = brs_ssh_timeout_ms / 1000;

        if (tsec < 1)
            tsec = 1;

        snprintf(connect_timeout_opt, sizeof connect_timeout_opt,
                 "ConnectTimeout=%d", tsec);
    }

    pid = fork();
    if (pid < 0)
        goto fail_open;

            if (pid == 0) {
        close_fds_except(pipe_to_agent[0], pipe_from_agent[1]);

        close(pipe_to_agent[1]);
        close(pipe_from_agent[0]);

        dup2(pipe_to_agent[0], STDIN_FILENO);
        dup2(pipe_from_agent[1], STDOUT_FILENO);

        /* ============================================================
         * SILENCIADOR DE SQUELCH: Evita que el stderr de SSH rompa ncurses
         * ============================================================ */
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd != -1) {
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        /* ============================================================ */

        close(pipe_to_agent[0]);
        close(pipe_from_agent[1]);

        if (have_askpass) {


            const char *ssh_pass_child = getenv("BARESNAP_SSH_PASSWORD");

            if (ssh_pass_child && ssh_pass_child[0] != '\0') {
                setenv("SSH_ASKPASS", askpass_path, 1);
                setenv("SSH_ASKPASS_REQUIRE", "force", 1);

                if (!getenv("DISPLAY"))
                    setenv("DISPLAY", ":0", 1);
            } else {
                unsetenv("SSH_ASKPASS");
                unsetenv("SSH_ASKPASS_REQUIRE");
                unsetenv("DISPLAY");
            }
        }

        char path_q[PATH_MAX * 2 + 10];
        char cmd[PATH_MAX * 4 + 128];

        if (shell_single_quote(u.path, path_q, sizeof(path_q)) != 0)
            _exit(127);

            snprintf(cmd, sizeof cmd,
                     "cd %s && BRS_REMOTE_NO_SYNC=1 exec "
                     "./.local/bin/baresnap-remote --repo .",
                     path_q);
        
        execlp("ssh", "ssh",
               "-T",
               "-p", port_str,
               "-o", connect_timeout_opt,
               "-o", "StrictHostKeyChecking=accept-new",
               "-o", "LogLevel=ERROR",
               "-o", "NumberOfPasswordPrompts=3",
               "-o", "IPQoS=lowdelay",
               "-o", "TCPKeepAlive=yes",
               "-o", "ServerAliveInterval=15",
               "-o", "ServerAliveCountMax=3",
               userhost, cmd, (char *)NULL);
        _exit(127);
       

    }

    close(pipe_to_agent[0]);
    pipe_to_agent[0] = -1;

    close(pipe_from_agent[1]);
    pipe_from_agent[1] = -1;

    struct pollfd pfd = { .fd = pipe_from_agent[0], .events = POLLIN };
    (void)poll(&pfd, 1, 250);

    remote = brs_remote_connect(pipe_from_agent[0], pipe_to_agent[1]);
    if (!remote) {
        fprintf(stderr, "error: brs_remote_connect failed.\n");
        goto fail_open;
    }

    ssh = (BrsVfsSsh *)calloc(1, sizeof(BrsVfsSsh));
    if (!ssh)
        goto fail_open;

    ssh->remote = remote;
    ssh->agent_pid = pid;
    ssh->pipe_read_fd = pipe_from_agent[0];
    ssh->pipe_write_fd = pipe_to_agent[1];

    pthread_mutex_init(&ssh->mtx, NULL);

    strncpy(ssh->repo_path, u.path, sizeof(ssh->repo_path) - 1);
    ssh->repo_path[sizeof(ssh->repo_path) - 1] = '\0';

    strncpy(ssh->ssh_uri, uri, sizeof(ssh->ssh_uri) - 1);
    ssh->ssh_uri[sizeof(ssh->ssh_uri) - 1] = '\0';

    ssh->ssh_port = u.port > 0 ? u.port : 22;
            return (BrsVfs *)ssh;

fail_open:
    if (remote) {
        brs_remote_disconnect(remote);
    } else {
        if (pipe_from_agent[0] != -1)
            close(pipe_from_agent[0]);

        if (pipe_to_agent[1] != -1)
            close(pipe_to_agent[1]);
    }

    if (pipe_to_agent[0] != -1)
        close(pipe_to_agent[0]);

    if (pipe_from_agent[1] != -1)
        close(pipe_from_agent[1]);

    if (pid > 0) {
        kill(pid, SIGTERM);
        ssh_waitpid_timeout(pid);
    }

    free(ssh);
    return NULL;
}

void brs_vfs_ssh_close(BrsVfs *vfs)
{
    BrsVfsSsh *ssh = (BrsVfsSsh *)vfs;
    if (!ssh)
        return;

    brs_vfs_ssh_debug_stats();

    if (ssh->remote) {
        brs_remote_disconnect(ssh->remote);
        ssh->remote = NULL;
    }

    ssh->pipe_read_fd = -1;
    ssh->pipe_write_fd = -1;

    if (ssh->agent_pid > 0)
        ssh_waitpid_timeout(ssh->agent_pid);

    pthread_mutex_destroy(&ssh->mtx);
    free(ssh);
}

BrsVfsFile *brs_vfs_ssh_fopen(BrsVfs *vfs, const char *path, int flags)
{
    ssh_rpc_log("fopen", path);

    BrsVfsSsh *ssh = (BrsVfsSsh *)vfs;
    if (!ssh || !ssh->remote)
        return NULL;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&ssh->mtx);
    int handle = brs_remote_open(ssh->remote, path, flags);
    pthread_mutex_unlock(&ssh->mtx);

    ssh_rpc_record(&t0);

    if (handle < 0)
        return NULL;

    BrsSshFileHandle *h = (BrsSshFileHandle *)calloc(1, sizeof(*h));
    if (!h) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);

        pthread_mutex_lock(&ssh->mtx);
        brs_remote_close(ssh->remote, handle);
        pthread_mutex_unlock(&ssh->mtx);

        ssh_rpc_record(&t1);
        return NULL;
    }

    h->ssh = ssh;
    h->handle = handle;

    BrsVfsFile *f = (BrsVfsFile *)calloc(1, sizeof(BrsVfsFile));
    if (!f) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);

        pthread_mutex_lock(&ssh->mtx);
        brs_remote_close(ssh->remote, handle);
        pthread_mutex_unlock(&ssh->mtx);

        ssh_rpc_record(&t1);

        free(h);
        return NULL;
    }

    f->impl = h;
    f->vfs_parent = vfs;

    return f;
}

int brs_vfs_ssh_fclose(BrsVfsFile *f)
{
    ssh_rpc_log("fclose", NULL);

    if (!f)
        return -1;

    BrsSshFileHandle *h = (BrsSshFileHandle *)f->impl;
    if (!h) {
        free(f);
        return -1;
    }

    if (!h->ssh || !h->ssh->remote) {
        ssh_free_write_buffer(h);
        free(h);
        free(f);
        return -1;
    }

    int rc_flush = ssh_flush_write(h);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&h->ssh->mtx);
    int rc_close = brs_remote_close(h->ssh->remote, h->handle);
    pthread_mutex_unlock(&h->ssh->mtx);

    ssh_rpc_record(&t0);

    ssh_free_write_buffer(h);
    free(h);
    free(f);

    return (rc_flush == 0 && rc_close == 0) ? 0 : -1;
}


ssize_t brs_vfs_ssh_fread(BrsVfsFile *f, void *buf, size_t n, uint64_t off)
{
    ssh_rpc_log("fread", NULL);

    if (!f || !f->impl)
        return -1;

    BrsSshFileHandle *h = (BrsSshFileHandle *)f->impl;
    if (!h->ssh || !h->ssh->remote)
        return -1;

    if (ssh_flush_write(h) != 0)
        return -1;

    if (n == 0)
        return 0;

    if (g_vfs_read_progress_cb) {
        g_vfs_read_progress_cb(g_vfs_read_progress_ctx, 0, (uint64_t)n);
    }

    size_t total = 0;
    int error = 0;
    size_t max_read = ssh_max_read();
    if (max_read == 0)
        max_read = 65536;

    while (total < n) {
        size_t piece = n - total;
        if (piece > max_read)
            piece = max_read;

        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        errno = 0;
        pthread_mutex_lock(&h->ssh->mtx);
        int r = brs_remote_read(h->ssh->remote, h->handle,
                                off + (uint64_t)total,
                                (uint8_t *)buf + total, piece);
        int saved_errno = errno;

        if (r < 0 && (saved_errno == EAGAIN ||
                      saved_errno == EWOULDBLOCK ||
                      saved_errno == EINTR)) {
            pthread_mutex_unlock(&h->ssh->mtx);
            ssh_rpc_record(&t0);
            usleep(1000);
            pthread_mutex_lock(&h->ssh->mtx);
            pthread_mutex_unlock(&h->ssh->mtx);
            continue;
        }

        pthread_mutex_unlock(&h->ssh->mtx);
        ssh_rpc_record(&t0);
        errno = saved_errno;

        if (r > 0) {
            g_ssh_bytes_recv += (size_t)r;
        }

        if (r == 0) {
            if (g_vfs_read_progress_cb) {
                g_vfs_read_progress_cb(g_vfs_read_progress_ctx,
                                       (uint64_t)total,
                                       (uint64_t)n);
            }
            break;
        }

        if (r < 0) {
            error = 1;
            if (g_vfs_read_progress_cb) {
                g_vfs_read_progress_cb(g_vfs_read_progress_ctx,
                                       (uint64_t)total,
                                       (uint64_t)n);
            }
            break;
        }

        total += (size_t)r;
        if (g_vfs_read_progress_cb) {
            g_vfs_read_progress_cb(g_vfs_read_progress_ctx,
                                   (uint64_t)total,
                                   (uint64_t)n);
        }

        if ((size_t)r < piece)
            break;
    }

    return error ? -1 : (ssize_t)total;
}

ssize_t brs_vfs_ssh_fwrite(BrsVfsFile *f, const void *buf, size_t n,
                           uint64_t off)
{
    ssh_rpc_log("fwrite", NULL);

    if (!f || !f->impl)
        return -1;

    BrsSshFileHandle *h = (BrsSshFileHandle *)f->impl;
    if (!h->ssh || !h->ssh->remote)
        return -1;

    if (n == 0)
        return 0;

    if (ssh_ensure_write_buffer(h) != 0)
        return -1;

    size_t max = ssh_max_write();
    if (max == 0)
        return -1;

    const uint8_t *p = (const uint8_t *)buf;
    size_t total = 0;

    while (total < n) {
        uint64_t cur = off + (uint64_t)total;
        size_t piece = n - total;

        if (h->wbuf_len == 0) {
            h->wbuf_off = cur;
        } else if (cur != h->wbuf_off + (uint64_t)h->wbuf_len) {
            if (ssh_flush_write(h) != 0)
                return -1;

            h->wbuf_off = cur;
        }

        if (piece >= h->wbuf_cap) {
            if (ssh_flush_write(h) != 0)
                return -1;

            size_t doff = 0;

            while (doff < piece) {
                size_t d = piece - doff;
                if (d > max)
                    d = max;

                struct timespec t0;
                clock_gettime(CLOCK_MONOTONIC, &t0);

                pthread_mutex_lock(&h->ssh->mtx);
                int w = brs_remote_write(h->ssh->remote, h->handle,
                                         cur + (uint64_t)doff,
                                         p + total + doff, d);
                pthread_mutex_unlock(&h->ssh->mtx);

                ssh_rpc_record(&t0);
                ssh_rpc_log("write", NULL);

                if (w > 0) {
                    g_ssh_bytes_sent += (size_t)w;
                }

                if (w <= 0)
                    return -1;

                doff += (size_t)w;
            }

            h->wbuf_off = cur + (uint64_t)piece;
            h->wbuf_len = 0;

            total += piece;
            continue;
        }

        if (h->wbuf_len == h->wbuf_cap) {
            if (ssh_flush_write(h) != 0)
                return -1;

            h->wbuf_off = cur;
        }

        size_t space = h->wbuf_cap - h->wbuf_len;
        if (piece > space)
            piece = space;

        memcpy(h->wbuf + h->wbuf_len, p + total, piece);
        h->wbuf_len += piece;
        total += piece;

        if (h->wbuf_len == h->wbuf_cap) {
            if (ssh_flush_write(h) != 0)
                return -1;
        }
    }

    return (ssize_t)n;
}

int brs_vfs_ssh_fsync(BrsVfsFile *f)
{
    if (!f || !f->impl)
        return -1;

    if (ssh_lazy_fsync())
        return 0;

    return ssh_flush_write((BrsSshFileHandle *)f->impl);
}

int brs_vfs_ssh_rename(BrsVfs *vfs, const char *from, const char *to)
{
    ssh_rpc_log("rename", from);

    BrsVfsSsh *ssh = (BrsVfsSsh *)vfs;
    if (!ssh || !ssh->remote)
        return -1;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&ssh->mtx);
    int rc = brs_remote_rename(ssh->remote, from, to);
    pthread_mutex_unlock(&ssh->mtx);

    ssh_rpc_record(&t0);

    return rc;
}

int brs_vfs_ssh_unlink(BrsVfs *vfs, const char *path)
{
    ssh_rpc_log("unlink", path);

    BrsVfsSsh *ssh = (BrsVfsSsh *)vfs;
    if (!ssh || !ssh->remote)
        return -1;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&ssh->mtx);
    int rc = brs_remote_unlink(ssh->remote, path);
    pthread_mutex_unlock(&ssh->mtx);

    ssh_rpc_record(&t0);

    return rc;
}

int brs_vfs_ssh_mkdir(BrsVfs *vfs, const char *path, uint32_t mode)
{
    ssh_rpc_log("mkdir", path);

    BrsVfsSsh *ssh = (BrsVfsSsh *)vfs;
    if (!ssh || !ssh->remote)
        return -1;

    char tmp[PATH_MAX];
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(tmp))
        return -1;

    memcpy(tmp, path, len + 1);

    if (len > 1 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    pthread_mutex_lock(&ssh->mtx);

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';

            struct timespec t0;
            clock_gettime(CLOCK_MONOTONIC, &t0);

            int rc = brs_remote_mkdir(ssh->remote, tmp, mode);
            ssh_rpc_record(&t0);

            if (rc != 0) {
                uint64_t sz;
                uint32_t md;

                struct timespec t1;
                clock_gettime(CLOCK_MONOTONIC, &t1);

                int src = brs_remote_stat(ssh->remote, tmp, &sz, &md);
                ssh_rpc_record(&t1);

                if (src != 0 || (md & 0170000) != 0040000) {
                    pthread_mutex_unlock(&ssh->mtx);
                    return -1;
                }
            }

            *p = '/';
        }
    }

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int rc = brs_remote_mkdir(ssh->remote, tmp, mode);
    ssh_rpc_record(&t0);

    if (rc != 0) {
        uint64_t sz;
        uint32_t md;

        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);

        int src = brs_remote_stat(ssh->remote, tmp, &sz, &md);
        ssh_rpc_record(&t1);

        if (src == 0 && (md & 0170000) == 0040000) {
            pthread_mutex_unlock(&ssh->mtx);
            return 0;
        }

        pthread_mutex_unlock(&ssh->mtx);
        return -1;
    }

    pthread_mutex_unlock(&ssh->mtx);
    return 0;
}

int brs_vfs_ssh_link(BrsVfs *vfs, const char *existing, const char *newpath)
{
    ssh_rpc_log("link", existing);

    BrsVfsSsh *ssh = (BrsVfsSsh *)vfs;
    if (!ssh || !ssh->remote)
        return -1;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&ssh->mtx);
    int rc = brs_remote_hardlink(ssh->remote, existing, newpath);
    pthread_mutex_unlock(&ssh->mtx);

    ssh_rpc_record(&t0);

    return rc;
}

int brs_vfs_ssh_list(BrsVfs *vfs, const char *path, BrsVfsList *out)
{
    ssh_rpc_log("list", path);

    BrsVfsSsh *ssh = (BrsVfsSsh *)vfs;
    if (!ssh || !ssh->remote || !out)
        return -1;

    out->items = NULL;
    out->count = 0;

    char **names = NULL;
    size_t count = 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&ssh->mtx);
    int rc = brs_remote_list(ssh->remote, path, &names, &count);
    pthread_mutex_unlock(&ssh->mtx);

    ssh_rpc_record(&t0);

    if (rc != 0)
        return -1;

    out->items = (BrsVfsEntry *)calloc(count ? count : 1,
                                       sizeof(BrsVfsEntry));

    if (!out->items) {
        for (size_t i = 0; i < count; i++)
            free(names[i]);

        free(names);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        snprintf(out->items[i].name, sizeof(out->items[i].name),
                 "%s", names[i]);

        out->items[i].mode = 0;
        out->items[i].size = 0;

        free(names[i]);
    }

    free(names);
    out->count = count;

    return 0;
}

void brs_vfs_ssh_list_free(BrsVfsList *list)
{
    if (!list)
        return;

    free(list->items);
    list->items = NULL;
    list->count = 0;
}

int brs_vfs_ssh_stat(BrsVfs *vfs, const char *path,
                     uint64_t *size, uint32_t *mode)
{
    ssh_rpc_log("stat", path);

    BrsVfsSsh *ssh = (BrsVfsSsh *)vfs;
    if (!ssh || !ssh->remote)
        return -1;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&ssh->mtx);
    int rc = brs_remote_stat(ssh->remote, path, size, mode);
    pthread_mutex_unlock(&ssh->mtx);

    ssh_rpc_record(&t0);

    return rc;
}

int brs_vfs_ssh_exists(BrsVfs *vfs, const char *path)
{
    ssh_rpc_log("exists", path);

    uint64_t size;
    uint32_t mode;

    return (brs_vfs_ssh_stat(vfs, path, &size, &mode) == 0) ? 1 : 0;
}

const char *brs_vfs_ssh_backend_name(BrsVfs *vfs)
{
    (void)vfs;
    return "ssh";
}

int brs_vfs_ssh_supports_hardlinks(BrsVfs *vfs)
{
    (void)vfs;
    return 1;
}

int brs_vfs_ssh_supports_atomic_rename(BrsVfs *vfs)
{
    (void)vfs;
    return 1;
}

int brs_vfs_ssh_upload_pack(BrsVfs *vfs, const char *remote_path,
                            const char *local_path, uint64_t *out_size)
{
    if (!vfs || !remote_path || !local_path)
        return -1;

    BrsVfsSsh *ssh = (BrsVfsSsh *)vfs;
    if (!ssh || !ssh->remote)
        return -1;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&ssh->mtx);
    int rc = brs_remote_upload_pack(ssh->remote, remote_path,
                                    local_path, out_size);
    pthread_mutex_unlock(&ssh->mtx);

    ssh_rpc_record(&t0);
    return rc;
}


int brs_vfs_sync_index_bulk(BrsVfs *vfs, const char *repo_path, BrsIndexMap *out_map)
{
    (void)repo_path;

    if (!vfs || !out_map)
        return -1;

    BrsVfsSsh *ssh = (BrsVfsSsh *)vfs;
    if (!ssh->remote)
        return -1;

    uint8_t *dump = NULL;
    uint32_t dump_len = 0;

    pthread_mutex_lock(&ssh->mtx);
    int rc = brs_remote_sync_index(ssh->remote, &dump, &dump_len);
    pthread_mutex_unlock(&ssh->mtx);

    if (rc != 0 || !dump) {
        if (dump)
            free(dump);
        return -1;
    }

    if (dump_len < 4) {
        free(dump);
        return -1;
    }

    uint32_t seg_count = (uint32_t)dump[0] |
                         ((uint32_t)dump[1] << 8) |
                         ((uint32_t)dump[2] << 16) |
                         ((uint32_t)dump[3] << 24);

    size_t off = 4;

    for (uint32_t s = 0; s < seg_count && off + 12 <= dump_len; ++s) {
        uint64_t seg_id = 0;

        for (int i = 0; i < 8; ++i)
            seg_id |= ((uint64_t)dump[off + i]) << (i * 8);

        off += 8;

        uint32_t seg_size = (uint32_t)dump[off] |
                            ((uint32_t)dump[off + 1] << 8) |
                            ((uint32_t)dump[off + 2] << 16) |
                            ((uint32_t)dump[off + 3] << 24);

        off += 4;

        if (off + seg_size > dump_len)
            break;

        BrsReader r;
        brs_reader_init(&r, dump + off, seg_size);

        const uint8_t *magic;
        uint32_t ver;
        uint64_t fsid;
        uint64_t count;

        if (brs_reader_bytes(&r, BRS_MAGIC_LEN, &magic) != 0)
            break;

        if (memcmp(magic, BRS_MAGIC_INDEX, BRS_MAGIC_LEN) != 0)
            break;

        if (brs_reader_u32_le(&r, &ver) != 0)
            break;

        if (brs_reader_u64_le(&r, &fsid) != 0)
            break;

        if (fsid != seg_id)
            break;

        if (brs_reader_skip(&r, 16) != 0)
            break;

        if (brs_reader_u64_le(&r, &count) != 0)
            break;

        for (uint64_t e = 0; e < count; ++e) {
            const uint8_t *idb;
            BrsChunkId cid;
            BrsChunkLocation loc;
            uint8_t fl;

            if (brs_reader_bytes(&r, 16, &idb) != 0)
                break;

            memcpy(cid.bytes, idb, 16);

            if (brs_reader_u64_le(&r, &loc.pack_id) != 0)
                break;

            if (brs_reader_u64_le(&r, &loc.offset) != 0)
                break;

            if (brs_reader_u32_le(&r, &loc.comp_size) != 0)
                break;

            if (brs_reader_u32_le(&r, &loc.uncomp_size) != 0)
                break;

            if (brs_reader_u8(&r, &fl) != 0)
                break;

            loc.flags = fl;

            (void)brs_index_map_put(out_map, &cid, &loc);
        }

        off += seg_size;
    }

    free(dump);
    return 0;
}
