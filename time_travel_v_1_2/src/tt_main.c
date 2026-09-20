#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "tt_types.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

extern int tt_delta_encode(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t**, size_t*);
extern int tt_delta_decode(const uint8_t*, size_t, const uint8_t*, size_t, uint64_t, uint8_t**, size_t*);
extern int tt_store_init(const char*);
extern int tt_store_init_writer(const char*, int);
extern void tt_store_free(void);
extern int tt_store_write(const TtDeltaHeader*, const char*, const uint8_t*);
extern int tt_store_reader_init(void);
extern int tt_store_reader_next(TtDeltaHeader*, char*, size_t, uint8_t**, size_t*);
extern void tt_store_reader_free(void);
extern int tt_store_scan_stats(uint64_t*, uint64_t*, uint64_t*, uint64_t*);
extern int tt_restore_file(const char*, const char*, uint64_t, const char*);
extern int tt_restore_dir(const char*, const char*, uint64_t, const char*);
extern int tt_restore_dir_per_file(const char*, const char*, const char*, int);
extern int tt_list_history(const char*, const char*);
extern int tt_compact_run(TtDaemon*);
extern int tt_is_excluded(const char*);
extern void tt_debounce_init(TtDebounce*);
extern void tt_debounce_free(TtDebounce*);
extern void tt_debounce_add(TtDebounce*, const char*, uint8_t);
extern int  tt_debounce_process(TtDaemon*, TtProcessCb, void*);
extern int  tt_watcher_init(TtDaemon*);
extern void tt_watcher_free(TtDaemon*);
extern int  tt_watcher_add_root(TtDaemon*);
extern int  tt_watcher_rebuild(TtDaemon*);
extern void tt_watcher_process_events(TtDaemon*);

static TtDaemon g_daemon;
static volatile sig_atomic_t g_signal_received = 0;
static uint64_t g_daemon_start_ns = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_signal_received = 1;
}

static void tt_log(const char *fmt, ...) {
    char tsbuf[32];
    time_t t = time(NULL);
    struct tm tmv;
    if (localtime_r(&t, &tmv)) {
        strftime(tsbuf, sizeof tsbuf, "%Y-%m-%d %H:%M:%S", &tmv);
    } else {
        snprintf(tsbuf, sizeof tsbuf, "?");
    }
    fprintf(stderr, "[%s] ", tsbuf);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static uint64_t next_ts(void) {
    static uint64_t last_ts = 0;
    uint64_t now = tt_now_ns();
    if (now <= last_ts) now = last_ts + 1;
    last_ts = now;
    return now;
}

static void cache_init(TtStateCache *c) {
    memset(c, 0, sizeof *c);
}

static void cache_free(TtStateCache *c) {
    if (!c) return;
    for (size_t i = 0; i < c->count; ++i) {
        if (c->entries[i].active) free(c->entries[i].data);
    }
    free(c->entries);
    memset(c, 0, sizeof *c);
}

static TtStateEntry *cache_find(TtStateCache *c, const char *path) {
    for (size_t i = 0; i < c->count; ++i) {
        if (c->entries[i].active && strcmp(c->entries[i].path, path) == 0) return &c->entries[i];
    }
    return NULL;
}

static void cache_remove(TtStateCache *c, const char *path) {
    TtStateEntry *e = cache_find(c, path);
    if (!e) return;
    free(e->data);
    memset(e, 0, sizeof *e);
    if (c->active_count > 0) c->active_count--;
}

static int cache_put(TtStateCache *c, const char *path, const uint8_t *data, size_t size, int anchored, int baseline_pending) {
    TtStateEntry *e = cache_find(c, path);
    if (!e) {
        size_t slot = c->count;
        for (size_t i = 0; i < c->count; ++i) {
            if (!c->entries[i].active) {
                slot = i;
                break;
            }
        }
        if (slot == c->count) {
            if (c->count == c->cap) {
                size_t ncap = c->cap ? c->cap * 2 : 64;
                TtStateEntry *ne = realloc(c->entries, ncap * sizeof(TtStateEntry));
                if (!ne) return -1;
                c->entries = ne;
                c->cap = ncap;
            }
            c->count++;
        }
        e = &c->entries[slot];
        memset(e, 0, sizeof *e);
        e->active = 1;
        snprintf(e->path, sizeof e->path, "%s", path);
        c->active_count++;
    } else {
        free(e->data);
        e->data = NULL;
        e->size = 0;
    }
    if (size > 0 && data) {
        e->data = malloc(size);
        if (!e->data) return -1;
        memcpy(e->data, data, size);
    }
    e->size = size;
    e->anchored = anchored;
    e->baseline_pending = baseline_pending;
    return 0;
}

static int read_file_all(const char *full, uint8_t **out, size_t *out_size) {
    *out = NULL;
    *out_size = 0;
    int fd = open(full, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        close(fd);
        return -1;
    }
    size_t sz = (size_t)st.st_size;
    if (sz == 0) {
        close(fd);
        return 0;
    }
    uint8_t *buf = malloc(sz);
    if (!buf) {
        close(fd);
        return -1;
    }
    size_t off = 0;
    while (off < sz) {
        ssize_t r = read(fd, buf + off, sz - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0) break;
        off += (size_t)r;
    }
    close(fd);
    *out = buf;
    *out_size = off;
    return 0;
}

static int write_create_record_ts(TtDaemon *d, const char *rel, const uint8_t *data, size_t size, uint64_t ts) {
    TtDeltaHeader hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.timestamp_ns = ts;
    hdr.event_type = TT_EV_CREATE;
    hdr.path_len = (uint32_t)strlen(rel);
    hdr.delta_size = (uint32_t)size;
    hdr.file_size = (uint64_t)size;
    int rc = tt_store_write(&hdr, rel, size > 0 ? data : NULL);
    if (rc == 0) {
        d->deltas_written++;
        d->bytes_stored += sizeof(hdr) + strlen(rel) + size;
    }
    return rc;
}

static int write_create_record(TtDaemon *d, const char *rel, const uint8_t *data, size_t size) {
    return write_create_record_ts(d, rel, data, size, next_ts());
}

static int write_version_record(TtDaemon *d, const char *rel, const uint8_t *old, size_t old_size, const uint8_t *newd, size_t new_size) {
    if (old && old_size > 0) {
        uint8_t *delta = NULL;
        size_t dsz = 0;
        if (tt_delta_encode(old, old_size, newd, new_size, &delta, &dsz) == 0 && dsz > 0 && dsz < new_size) {
            TtDeltaHeader hdr;
            memset(&hdr, 0, sizeof hdr);
            hdr.timestamp_ns = next_ts();
            hdr.event_type = TT_EV_MODIFY;
            hdr.path_len = (uint32_t)strlen(rel);
            hdr.delta_size = (uint32_t)dsz;
            hdr.file_size = (uint64_t)new_size;
            int rc = tt_store_write(&hdr, rel, delta);
            free(delta);
            if (rc == 0) {
                d->deltas_written++;
                d->bytes_stored += sizeof(hdr) + strlen(rel) + dsz;
            }
            return rc;
        }
        free(delta);
    }
    return write_create_record(d, rel, newd, new_size);
}

static int capture_path(TtDaemon *d, const char *rel, int quiet) {
    if (!d || !rel || !rel[0]) return -1;
    if (tt_is_excluded(rel)) return -1;
    char full[TT_PATH_MAX * 2];
    snprintf(full, sizeof full, "%s/%s", d->watch_dir, rel);
    struct stat st;
    if (lstat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        TtStateEntry *e = cache_find(&d->cache, rel);
        if (!e) return 0;
        if (e->baseline_pending) {
            cache_remove(&d->cache, rel);
            return 0;
        }
        TtDeltaHeader hdr;
        memset(&hdr, 0, sizeof hdr);
        hdr.timestamp_ns = next_ts();
        hdr.event_type = TT_EV_DELETE;
        hdr.path_len = (uint32_t)strlen(rel);
        if (tt_store_write(&hdr, rel, NULL) == 0) {
            cache_remove(&d->cache, rel);
            d->deltas_written++;
            d->bytes_stored += sizeof(hdr) + strlen(rel);
            tt_log("DELETE  %s\n", rel);
            return 1;
        }
        return -1;
    }
    if ((uint64_t)st.st_size > TT_MAX_CAPTURE_SIZE) {
        if (!quiet) tt_log("warning: '%s' exceeds maximum size; ignored\n", rel);
        return 0;
    }
    uint8_t *new_data = NULL;
    size_t new_size = 0;
    if (read_file_all(full, &new_data, &new_size) != 0) return -1;
    struct stat st2;
    if (lstat(full, &st2) != 0 || st2.st_size != st.st_size || st2.st_mtim.tv_sec != st.st_mtim.tv_sec || st2.st_mtim.tv_nsec != st.st_mtim.tv_nsec) {
        free(new_data);
        tt_debounce_add(&d->debounce, full, TT_EV_MODIFY);
        return 0;
    }
    TtStateEntry *e = cache_find(&d->cache, rel);
    if (!e) {
        int rc = write_create_record(d, rel, new_data, new_size);
        if (rc == 0) {
            cache_put(&d->cache, rel, new_data, new_size, 1, 0);
            tt_log("CREATE  %s (%zu bytes)\n", rel, new_size);
            free(new_data);
            return 1;
        }
        free(new_data);
        return -1;
    }
    if (e->size == new_size && (new_size == 0 || (e->data && memcmp(e->data, new_data, new_size) == 0))) {
        free(new_data);
        return 0;
    }
    int rc;
    if (e->baseline_pending) {
        uint64_t orig_ts = g_daemon_start_ns ? g_daemon_start_ns : next_ts();
        rc = write_create_record_ts(d, rel, e->data, e->size, orig_ts);
        if (rc == 0) {
            tt_log("CREATE  %s (%zu bytes) [original]\n", rel, e->size);
            rc = write_version_record(d, rel, e->data, e->size, new_data, new_size);
            if (rc == 0) {
                cache_put(&d->cache, rel, new_data, new_size, 1, 0);
                tt_log("MODIFY  %s (%zu bytes)\n", rel, new_size);
                free(new_data);
                return 1;
            }
            e->baseline_pending = 0;
            e->anchored = 0;
        }
        free(new_data);
        return -1;
    }
    if (!e->anchored) {
        rc = write_create_record(d, rel, new_data, new_size);
        if (rc == 0) {
            cache_put(&d->cache, rel, new_data, new_size, 1, 0);
            tt_log("CREATE  %s (%zu bytes) [reanchor]\n", rel, new_size);
            free(new_data);
            return 1;
        }
        free(new_data);
        return -1;
    }
    rc = write_version_record(d, rel, e->data, e->size, new_data, new_size);
    if (rc == 0) {
        cache_put(&d->cache, rel, new_data, new_size, 1, 0);
        tt_log("MODIFY  %s (%zu bytes)\n", rel, new_size);
        free(new_data);
        return 1;
    }
    free(new_data);
    return -1;
}

typedef struct {
    char **v;
    size_t n, cap;
} TtPathSet;

static int pathset_has(TtPathSet *s, const char *p) {
    for (size_t i = 0; i < s->n; ++i) {
        if (strcmp(s->v[i], p) == 0) return 1;
    }
    return 0;
}

static void pathset_add(TtPathSet *s, const char *p) {
    if (pathset_has(s, p)) return;
    if (s->n == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 256;
        char **nv = realloc(s->v, ncap * sizeof(char *));
        if (!nv) return;
        s->v = nv;
        s->cap = ncap;
    }
    s->v[s->n] = strdup(p);
    if (s->v[s->n]) s->n++;
}

static void pathset_free(TtPathSet *s) {
    for (size_t i = 0; i < s->n; ++i) free(s->v[i]);
    free(s->v);
    memset(s, 0, sizeof *s);
}

static void store_collect_paths(TtPathSet *out) {
    memset(out, 0, sizeof *out);
    if (tt_store_reader_init() != 0) return;
    for (;;) {
        TtDeltaHeader hdr;
        char path[TT_PATH_MAX];
        uint8_t *pl = NULL;
        size_t plsz = 0;
        int rc = tt_store_reader_next(&hdr, path, sizeof path, &pl, &plsz);
        if (rc <= 0) break;
        pathset_add(out, path);
        free(pl);
    }
    tt_store_reader_free();
}

static void cache_build_walk(TtDaemon *d, TtPathSet *hist, const char *dir_full, const char *rel_prefix, size_t *n_files) {
    DIR *dp = opendir(dir_full);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char rel[TT_PATH_MAX];
        if (rel_prefix[0]) snprintf(rel, sizeof rel, "%s/%s", rel_prefix, de->d_name);
        else snprintf(rel, sizeof rel, "%s", de->d_name);
        if (tt_is_excluded(rel)) continue;
        char full[TT_PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", dir_full, de->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            cache_build_walk(d, hist, full, rel, n_files);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        if ((uint64_t)st.st_size > TT_MAX_CAPTURE_SIZE) continue;
        uint8_t *data = NULL;
        size_t size = 0;
        if (read_file_all(full, &data, &size) != 0) continue;
        int has_hist = pathset_has(hist, rel);
        cache_put(&d->cache, rel, data, size, has_hist ? 0 : 1, has_hist ? 0 : 1);
        free(data);
        (*n_files)++;
    }
    closedir(dp);
}

static void cache_build_initial(TtDaemon *d) {
    TtPathSet hist;
    store_collect_paths(&hist);
    size_t n_files = 0;
    struct stat st;
    if (stat(d->watch_dir, &st) == 0 && S_ISDIR(st.st_mode)) cache_build_walk(d, &hist, d->watch_dir, "", &n_files);
    tt_log("in memory: %zu file(s); no initial records\n", n_files);
    pathset_free(&hist);
}

static void rescan_walk(TtDaemon *d, const char *dir_full, const char *rel_prefix, int *written) {
    DIR *dp = opendir(dir_full);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char rel[TT_PATH_MAX];
        if (rel_prefix[0]) snprintf(rel, sizeof rel, "%s/%s", rel_prefix, de->d_name);
        else snprintf(rel, sizeof rel, "%s", de->d_name);
        if (tt_is_excluded(rel)) continue;
        char full[TT_PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", dir_full, de->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            rescan_walk(d, full, rel, written);
        } else if (S_ISREG(st.st_mode)) {
            int r = capture_path(d, rel, 1);
            if (r > 0 && written) (*written)++;
        }
    }
    closedir(dp);
}

static void rescan_and_sync(TtDaemon *d) {
    struct stat st;
    if (stat(d->watch_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        d->root_lost = 1;
        return;
    }
    int written = 0;
    rescan_walk(d, d->watch_dir, "", &written);
    size_t i = 0;
    while (i < d->cache.count) {
        TtStateEntry *e = &d->cache.entries[i];
        if (!e->active) {
            i++;
            continue;
        }
        char full[TT_PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", d->watch_dir, e->path);
        struct stat st2;
        if (lstat(full, &st2) != 0 || !S_ISREG(st2.st_mode)) {
            int r = capture_path(d, e->path, 1);
            if (r > 0) written++;
        }
        i++;
    }
    if (written > 0) tt_log("rescan: %d record(s)\n", written);
}

static void on_debounced(TtDaemon *d, const char *full_path, uint8_t event_type, void *user) {
    (void)event_type;
    (void)user;
    size_t wlen = strlen(d->watch_dir);
    if (strncmp(full_path, d->watch_dir, wlen) != 0) return;
    const char *rel = full_path + wlen;
    while (*rel == '/') rel++;
    if (!*rel) return;
    capture_path(d, rel, 0);
}

static void handle_root_health(TtDaemon *d) {
    struct stat st;
    int exists = (stat(d->watch_dir, &st) == 0 && S_ISDIR(st.st_mode));
    if (!exists) {
        if (!d->root_lost) {
            d->root_lost = 1;
            tt_log("WARNING: watched directory '%s' deleted; waiting for recovery...\n", d->watch_dir);
        }
        return;
    }
    if (!d->root_lost) return;
    tt_log("watched directory recovered; reindexing...\n");
    tt_store_free();
    if (tt_store_init_writer(d->store_dir, 1) != 0) return;
    cache_free(&d->cache);
    cache_init(&d->cache);
    g_daemon_start_ns = tt_now_ns();
    cache_build_initial(d);
    tt_watcher_rebuild(d);
    d->root_lost = 0;
}

static int find_repo_dir(const char *path, char *out, size_t out_size) {
    char tmp[TT_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    struct stat st;
    if (stat(tmp, &st) == 0 && S_ISREG(st.st_mode)) {
        char *s = strrchr(tmp, '/');
        if (s) *s = '\0';
        else snprintf(tmp, sizeof tmp, ".");
    }
    while (tmp[0] != '\0') {
        char cand[TT_PATH_MAX * 2];
        snprintf(cand, sizeof cand, "%s/.timetravel", tmp);
        if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(out, out_size, "%s", tmp);
            return 0;
        }
        char *s = strrchr(tmp, '/');
        if (!s || s == tmp) {
            if (tmp[0] == '/') tmp[1] = '\0';
            else tmp[0] = '\0';
        } else {
            *s = '\0';
        }
    }
    return -1;
}

static int normalize_dir(const char *in, char *out, size_t outsz) {
    if (!in || !in[0]) return -1;
    char *rp = realpath(in, NULL);
    if (!rp) return -1;
    snprintf(out, outsz, "%s", rp);
    free(rp);
    return 0;
}

static int resolve_repo_arg(const char *repo_dir, const char *fallback_path, char *wd, size_t wdsz) {
    char raw[TT_PATH_MAX];
    if (repo_dir && repo_dir[0]) {
        snprintf(raw, sizeof raw, "%s", repo_dir);
    } else {
        const char *p = (fallback_path && fallback_path[0]) ? fallback_path : ".";
        if (find_repo_dir(p, raw, sizeof raw) != 0) return -1;
    }
    if (normalize_dir(raw, wd, wdsz) == 0) return 0;
    snprintf(wd, wdsz, "%s", raw);
    return 0;
}

static int make_rel_from_arg(const char *wd, const char *path, char *rel, size_t relsz) {
    if (!path || !path[0] || strcmp(path, ".") == 0) {
        rel[0] = '\0';
        return 0;
    }
    char abs[TT_PATH_MAX * 2];
    if (path[0] == '/') {
        snprintf(abs, sizeof abs, "%s", path);
    } else {
        char cwd[TT_PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return -1;
        snprintf(abs, sizeof abs, "%s/%s", cwd, path);
    }
    char *rp = realpath(abs, NULL);
    if (rp) {
        snprintf(abs, sizeof abs, "%s", rp);
        free(rp);
    }
    size_t wlen = strlen(wd);
    if (strncmp(abs, wd, wlen) == 0 && (abs[wlen] == '\0' || abs[wlen] == '/')) {
        const char *r = abs + wlen;
        while (*r == '/') r++;
        snprintf(rel, relsz, "%s", r);
        return 0;
    }
    snprintf(rel, relsz, "%s", path);
    return 0;
}

static uint64_t parse_time_expr(const char *expr, int *ok) {
    if (ok) *ok = 1;
    if (!expr || !expr[0] || strcmp(expr, "now") == 0) return tt_now_ns();

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_isdst = -1;
    if (strptime(expr, "%Y-%m-%d %H:%M:%S", &tm) != NULL) {
        time_t t = mktime(&tm);
        if (t != (time_t)-1) {
            return (uint64_t)t * 1000000000ULL;
        }
    }

    long long val = 0;
    char unit[32] = {0}, extra[32] = {0};
    int n = sscanf(expr, "%lld %31s %31s", &val, unit, extra);
    if (n >= 2) {
        long long sec = 0;
        if (strncmp(unit, "sec", 3) == 0) sec = val;
        else if (strncmp(unit, "min", 3) == 0) sec = val * 60;
        else if (strncmp(unit, "hour", 4) == 0) sec = val * 3600;
        else if (strncmp(unit, "day", 3) == 0) sec = val * 86400;
        else if (strncmp(unit, "week", 4) == 0) sec = val * 604800;
        else {
            if (ok) *ok = 0;
            return 0;
        }
        if (sec < 0) sec = -sec;
        uint64_t delta = (uint64_t)sec * 1000000000ULL;
        uint64_t now = tt_now_ns();
        return (delta >= now) ? 0 : (now - delta);
    }

    if (ok) *ok = 0;
    return 0;
}

static void format_timestamp(uint64_t ns, char *out, size_t sz) {
    time_t s = (time_t)(ns / 1000000000ULL);
    struct tm t;
    if (localtime_r(&s, &t)) strftime(out, sz, "%Y-%m-%d %H:%M:%S", &t);
    else snprintf(out, sz, "%llu", (unsigned long long)ns);
}

static void format_bytes(uint64_t b, char *out, size_t sz) {
    if (b < 1024ULL) snprintf(out, sz, "%llu B", (unsigned long long)b);
    else if (b < 1024ULL * 1024ULL) snprintf(out, sz, "%.1f KiB", (double)b / 1024.0);
    else if (b < 1024ULL * 1024ULL * 1024ULL) snprintf(out, sz, "%.1f MiB", (double)b / (1024.0 * 1024.0));
    else snprintf(out, sz, "%.2f GiB", (double)b / (1024.0 * 1024.0 * 1024.0));
}

static void format_ns_duration(uint64_t ns, char *out, size_t sz) {
    uint64_t s = ns / 1000000000ULL;
    uint64_t d = s / 86400ULL;
    s %= 86400ULL;
    uint64_t h = s / 3600ULL;
    s %= 3600ULL;
    uint64_t m = s / 60ULL;
    s %= 60ULL;
    if (d) snprintf(out, sz, "%llud %lluh %llum %llus", (unsigned long long)d, (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
    else if (h) snprintf(out, sz, "%lluh %llum %llus", (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
    else if (m) snprintf(out, sz, "%llum %llus", (unsigned long long)m, (unsigned long long)s);
    else snprintf(out, sz, "%llus", (unsigned long long)s);
}

static void pid_file_path(char *out, size_t n, const char *store_dir) {
    snprintf(out, n, "%s/timetravel.pid", store_dir);
}

static int write_pid_file(const char *store_dir) {
    char pf[TT_PATH_MAX + 64];
    pid_file_path(pf, sizeof pf, store_dir);
    int fd = open(pf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%d %llu\n", (int)getpid(), (unsigned long long)tt_now_ns());
    if (write(fd, buf, (size_t)n) != n) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static pid_t read_pid_file(const char *store_dir, uint64_t *start_ns) {
    char pf[TT_PATH_MAX + 64];
    pid_file_path(pf, sizeof pf, store_dir);
    FILE *f = fopen(pf, "r");
    if (!f) return -1;
    pid_t pid = -1;
    unsigned long long sns = 0;
    if (fscanf(f, "%d %llu", &pid, &sns) < 1) pid = -1;
    fclose(f);
    if (start_ns) *start_ns = (uint64_t)sns;
    return pid;
}

static pid_t find_running_daemon(const char *store_dir, uint64_t *start_ns) {
    pid_t pid = read_pid_file(store_dir, start_ns);
    if (pid > 0 && kill(pid, 0) == 0) return pid;
    return -1;
}

static void tags_file_path(char *out, size_t n, const char *store_dir) {
    snprintf(out, n, "%s/tags", store_dir);
}

static int tag_add(const char *store_dir, const char *name, uint64_t ts) {
    char tf[TT_PATH_MAX + 64];
    tags_file_path(tf, sizeof tf, store_dir);
    FILE *f = fopen(tf, "a");
    if (!f) return -1;
    fprintf(f, "%s\t%llu\n", name, (unsigned long long)ts);
    fclose(f);
    return 0;
}

static int tag_lookup(const char *store_dir, const char *name, uint64_t *out_ts) {
    char tf[TT_PATH_MAX + 64];
    tags_file_path(tf, sizeof tf, store_dir);
    FILE *f = fopen(tf, "r");
    if (!f) return -1;
    char line[TT_PATH_MAX + 64];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        if (strcmp(line, name) == 0) {
            *out_ts = strtoull(tab + 1, NULL, 10);
            found = 0;
        }
    }
    fclose(f);
    return found;
}

static int tag_list(const char *store_dir) {
    char tf[TT_PATH_MAX + 64];
    tags_file_path(tf, sizeof tf, store_dir);
    FILE *f = fopen(tf, "r");
    if (!f) {
        printf("No tags.\n");
        return 0;
    }
    char line[TT_PATH_MAX + 64];
    int any = 0;
    printf("Tags:\n");
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        char *nl = strchr(tab + 1, '\n');
        if (nl) *nl = '\0';
        uint64_t ts = strtoull(tab + 1, NULL, 10);
        char tsbuf[64];
        format_timestamp(ts, tsbuf, sizeof tsbuf);
        printf("  %-20s  %s\n", line, tsbuf);
        any = 1;
    }
    fclose(f);
    if (!any) printf("  (none)\n");
    return 0;
}

static int load_version_content(const char *rel_path, uint64_t target_ns, uint8_t **out, size_t *out_size, int *exists) {
    *out = NULL;
    *out_size = 0;
    *exists = 0;
    if (tt_store_reader_init() != 0) return -1;
    uint8_t *state = NULL;
    size_t state_size = 0;
    int have = 0;
    for (;;) {
        TtDeltaHeader hdr;
        char path[TT_PATH_MAX];
        uint8_t *pl = NULL;
        size_t plsz = 0;
        int rc = tt_store_reader_next(&hdr, path, sizeof path, &pl, &plsz);
        if (rc <= 0) break;
        if (strcmp(path, rel_path) != 0) {
            free(pl);
            continue;
        }
        if (hdr.timestamp_ns > target_ns) {
            free(pl);
            continue;
        }
        if (hdr.event_type == TT_EV_DELETE) {
            free(state);
            state = NULL;
            state_size = 0;
            have = 0;
        } else if (hdr.event_type == TT_EV_CREATE) {
            free(state);
            state = NULL;
            state_size = 0;
            if (plsz > 0 && pl) {
                state = malloc(plsz);
                if (!state) {
                    free(pl);
                    tt_store_reader_free();
                    return -1;
                }
                memcpy(state, pl, plsz);
                state_size = plsz;
            }
            have = 1;
        } else if (hdr.event_type == TT_EV_MODIFY && have && plsz > 0) {
            uint8_t *ns = NULL;
            size_t nss = 0;
            if (tt_delta_decode(state, state_size, pl, plsz, hdr.file_size, &ns, &nss) == 0) {
                free(state);
                state = ns;
                state_size = nss;
            }
        }
        free(pl);
    }
    tt_store_reader_free();
    if (!have) {
        free(state);
        return 0;
    }
    *exists = 1;
    *out = state;
    *out_size = state_size;
    return 0;
}

static int find_last_two_ts(const char *rel_path, uint64_t *prev_ts, uint64_t *last_ts) {
    *prev_ts = 0;
    *last_ts = 0;
    if (tt_store_reader_init() != 0) return -1;
    uint64_t a = 0, b = 0;
    for (;;) {
        TtDeltaHeader hdr;
        char path[TT_PATH_MAX];
        uint8_t *pl = NULL;
        size_t plsz = 0;
        int rc = tt_store_reader_next(&hdr, path, sizeof path, &pl, &plsz);
        if (rc <= 0) break;
        if (strcmp(path, rel_path) == 0) {
            a = b;
            b = hdr.timestamp_ns;
        }
        free(pl);
    }
    tt_store_reader_free();
    if (b == 0) return -1;
    *prev_ts = a;
    *last_ts = b;
    return 0;
}

static int scan_timestamps(const char *rel, int prefix_mode, uint64_t **out_ts, size_t *out_n) {
    *out_ts = NULL;
    *out_n = 0;
    if (tt_store_reader_init() != 0) return -1;
    uint64_t *ts = NULL;
    size_t n = 0, cap = 0;
    size_t plen = rel ? strlen(rel) : 0;
    for (;;) {
        TtDeltaHeader hdr;
        char path[TT_PATH_MAX];
        uint8_t *pl = NULL;
        size_t plsz = 0;
        int rc = tt_store_reader_next(&hdr, path, sizeof path, &pl, &plsz);
        if (rc <= 0) break;
        int m = 0;
        if (!rel || !rel[0]) m = 1;
        else if (strcmp(path, rel) == 0) m = 1;
        else if (prefix_mode && strncmp(path, rel, plen) == 0 && path[plen] == '/') m = 1;
        free(pl);
        if (!m) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            uint64_t *nt = realloc(ts, cap * sizeof(uint64_t));
            if (!nt) {
                free(ts);
                tt_store_reader_free();
                return -1;
            }
            ts = nt;
        }
        ts[n++] = hdr.timestamp_ns;
    }
    tt_store_reader_free();
    if (n > 1) {
        for (size_t i = 1; i < n; ++i) {
            uint64_t k = ts[i];
            size_t j = i;
            while (j > 0 && ts[j - 1] > k) {
                ts[j] = ts[j - 1];
                j--;
            }
            ts[j] = k;
        }
    }
    *out_ts = ts;
    *out_n = n;
    return 0;
}

static int mkdir_p(const char *path) {
    char tmp[TT_PATH_MAX * 2];
    int n = snprintf(tmp, sizeof tmp, "%s", path);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int write_file_all(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (size > 0 && fwrite(data, 1, size, f) != size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static void sanitize_component(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    if (!in || !in[0]) in = "root";
    for (const char *p = in; *p && o + 1 < outsz; ++p) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') {
            out[o++] = (char)c;
        } else {
            out[o++] = '_';
        }
    }
    out[o] = '\0';
}

static int run_diff_file(const char *a, const char *b, const char *out) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
        }
        execlp("diff", "diff", "-u", a, b, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    if (!WIFEXITED(st)) return -1;
    int code = WEXITSTATUS(st);
    return (code == 0 || code == 1) ? 0 : -1;
}

static int proc_alive(pid_t pid) {
    if (pid <= 0) return 0;
    if (kill(pid, 0) == 0) return 1;
    return errno == EPERM;
}

static long proc_rss_kb(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/status", (long)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "VmRSS: %ld kB", &rss) == 1) break;
    }
    fclose(f);
    return rss;
}

static int proc_stat_fields(pid_t pid, unsigned long *utime, unsigned long *stime, unsigned long long *starttime) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[4096];
    if (!fgets(buf, sizeof buf, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    char *rp = strrchr(buf, ')');
    if (!rp) return -1;
    rp += 2;
    int field = 3;
    char *save = NULL;
    char *tok = strtok_r(rp, " ", &save);
    unsigned long u = 0, s = 0;
    unsigned long long st = 0;
    while (tok) {
        if (field == 14) u = strtoul(tok, NULL, 10);
        else if (field == 15) s = strtoul(tok, NULL, 10);
        else if (field == 22) st = strtoull(tok, NULL, 10);
        field++;
        tok = strtok_r(NULL, " ", &save);
    }
    if (utime) *utime = u;
    if (stime) *stime = s;
    if (starttime) *starttime = st;
    return 0;
}

static double proc_cpu_total_sec(pid_t pid) {
    unsigned long u = 0, s = 0;
    if (proc_stat_fields(pid, &u, &s, NULL) != 0) return -1.0;
    static long hz = 0;
    if (!hz) hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    return (double)(u + s) / (double)hz;
}

static double proc_cpu_percent(pid_t pid) {
    double c1 = proc_cpu_total_sec(pid);
    if (c1 < 0.0) return -1.0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    usleep(200 * 1000);
    double c2 = proc_cpu_total_sec(pid);
    if (c2 < 0.0) return -1.0;
    struct timespec t2;
    clock_gettime(CLOCK_MONOTONIC, &t2);
    double wall = (double)(t2.tv_sec - t1.tv_sec) + (double)(t2.tv_nsec - t1.tv_nsec) / 1e9;
    if (wall <= 0.0001) return 0.0;
    double pct = (c2 - c1) / wall * 100.0;
    if (pct < 0.0) pct = 0.0;
    return pct;
}

static void status_file_path(char *out, size_t n, const char *store_dir) {
    snprintf(out, n, "%s/timetravel.status", store_dir);
}

static void status_write(TtDaemon *d) {
    char fin[TT_PATH_MAX + 64];
    char tmp[TT_PATH_MAX + 64];
    status_file_path(fin, sizeof fin, d->store_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", fin);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "pid=%d\nstart_ns=%llu\ndeltas=%llu\nbytes=%llu\npending=%llu\nwatch_dir=%s\nupdated_ns=%llu\n",
            (int)getpid(), (unsigned long long)g_daemon_start_ns, (unsigned long long)d->deltas_written,
            (unsigned long long)d->bytes_stored, (unsigned long long)d->debounce.count, d->watch_dir,
            (unsigned long long)tt_now_ns());
    fclose(f);
    if (rename(tmp, fin) != 0) unlink(tmp);
}

typedef struct {
    int valid;
    pid_t pid;
    uint64_t start_ns;
    uint64_t deltas;
    uint64_t bytes;
    uint64_t pending;
    uint64_t updated_ns;
    char watch_dir[TT_PATH_MAX];
} TtStatusFile;

static int status_read(const char *store_dir, TtStatusFile *sf) {
    memset(sf, 0, sizeof *sf);
    char path[TT_PATH_MAX + 64];
    status_file_path(path, sizeof path, store_dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[TT_PATH_MAX + 128];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "pid=", 4)) sf->pid = (pid_t)atol(line + 4);
        else if (!strncmp(line, "start_ns=", 9)) sf->start_ns = strtoull(line + 9, NULL, 10);
        else if (!strncmp(line, "deltas=", 7)) sf->deltas = strtoull(line + 7, NULL, 10);
        else if (!strncmp(line, "bytes=", 6)) sf->bytes = strtoull(line + 6, NULL, 10);
        else if (!strncmp(line, "pending=", 8)) sf->pending = strtoull(line + 8, NULL, 10);
        else if (!strncmp(line, "updated_ns=", 11)) sf->updated_ns = strtoull(line + 11, NULL, 10);
        else if (!strncmp(line, "watch_dir=", 10)) snprintf(sf->watch_dir, sizeof sf->watch_dir, "%s", line + 10);
    }
    fclose(f);
    sf->valid = (sf->pid > 0);
    return sf->valid ? 0 : -1;
}

static int is_integer(const char *s, long long *out) {
    if (!s || !s[0]) return 0;
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return 0;
    *out = v;
    return 1;
}

static int resolve_file_target_ns(const char *store_dir, const char *rel, const char *expr, uint64_t *out, char *desc, size_t descsz) {
    if (!expr || !expr[0]) {
        uint64_t prev = 0, last = 0;
        if (find_last_two_ts(rel, &prev, &last) != 0) return -1;
        *out = prev ? prev : last;
        snprintf(desc, descsz, "%s", prev ? "previous" : "first");
        return 0;
    }
    if (!strcasecmp(expr, "now") || !strcasecmp(expr, "head") || !strcasecmp(expr, "latest") || !strcasecmp(expr, "last")) {
        uint64_t prev = 0, last = 0;
        if (find_last_two_ts(rel, &prev, &last) != 0) return -1;
        *out = last;
        snprintf(desc, descsz, "last");
        return 0;
    }
    if (!strcasecmp(expr, "prev") || !strcasecmp(expr, "previous")) {
        uint64_t prev = 0, last = 0;
        if (find_last_two_ts(rel, &prev, &last) != 0) return -1;
        if (prev == 0) {
            *out = 0;
            snprintf(desc, descsz, "empty");
        } else {
            *out = prev;
            snprintf(desc, descsz, "previous");
        }
        return 0;
    }
    if (!strcasecmp(expr, "first") || !strcasecmp(expr, "initial")) {
        uint64_t *ts = NULL;
        size_t n = 0;
        if (scan_timestamps(rel, 0, &ts, &n) != 0 || n == 0) {
            free(ts);
            return -1;
        }
        *out = ts[0];
        snprintf(desc, descsz, "first");
        free(ts);
        return 0;
    }
    if (!strncmp(expr, "tag:", 4)) {
        uint64_t ts = 0;
        if (tag_lookup(store_dir, expr + 4, &ts) != 0) return -1;
        *out = ts;
        snprintf(desc, descsz, "tag:%s", expr + 4);
        return 0;
    }
    if (expr[0] == '@') {
        long long v = 0;
        if (!is_integer(expr + 1, &v)) return -1;
        uint64_t ns = (v < 10000000000LL) ? (uint64_t)v * 1000000000ULL : (uint64_t)v;
        *out = ns;
        snprintf(desc, descsz, "timestamp");
        return 0;
    }
    long long rev = 0;
    if (is_integer(expr, &rev)) {
        uint64_t *ts = NULL;
        size_t n = 0;
        if (scan_timestamps(rel, 0, &ts, &n) != 0 || n == 0) {
            free(ts);
            return -1;
        }
        if (rev == 0) {
            *out = 0;
            snprintf(desc, descsz, "empty");
        } else {
            long long idx;
            if (rev > 0) idx = rev - 1;
            else idx = (long long)n - 1 + rev;
            if (idx < 0) {
                *out = 0;
                snprintf(desc, descsz, "empty");
            } else if ((size_t)idx >= n) {
                *out = ts[n - 1];
                snprintf(desc, descsz, "last");
            } else {
                *out = ts[idx];
                snprintf(desc, descsz, "revision %lld", rev);
            }
        }
        free(ts);
        return 0;
    }
    int ok = 0;
    uint64_t t = parse_time_expr(expr, &ok);
    if (ok) {
        *out = t;
        snprintf(desc, descsz, "%s", expr);
        return 0;
    }
    return -1;
}

static void store_collect_matching(TtPathSet *out, const char *rel) {
    memset(out, 0, sizeof *out);
    if (tt_store_reader_init() != 0) return;
    size_t plen = rel ? strlen(rel) : 0;
    for (;;) {
        TtDeltaHeader hdr;
        char path[TT_PATH_MAX];
        uint8_t *pl = NULL;
        size_t plsz = 0;
        int rc = tt_store_reader_next(&hdr, path, sizeof path, &pl, &plsz);
        if (rc <= 0) break;
        int m = 0;
        if (!rel || !rel[0]) m = 1;
        else if (strcmp(path, rel) == 0) m = 1;
        else if (plen && strncmp(path, rel, plen) == 0 && path[plen] == '/') m = 1;
        if (m) pathset_add(out, path);
        free(pl);
    }
    tt_store_reader_free();
}

static int dump_one_file(const char *store_dir, const char *rel, const char *outdir, int with_diff, FILE *manifest) {
    (void)store_dir;
    char safe[TT_PATH_MAX];
    sanitize_component(rel[0] ? rel : "root", safe, sizeof safe);
    char fdir[TT_PATH_MAX * 2];
    char ddir[TT_PATH_MAX * 2];
    snprintf(fdir, sizeof fdir, "%s/files/%s", outdir, safe);
    mkdir_p(fdir);
    if (with_diff) {
        snprintf(ddir, sizeof ddir, "%s/diffs/%s", outdir, safe);
        mkdir_p(ddir);
    }
    if (tt_store_reader_init() != 0) return -1;
    uint8_t *state = NULL;
    size_t state_size = 0;
    int have = 0;
    uint64_t seq = 0;
    char prev_file[TT_PATH_MAX] = "";
    for (;;) {
        TtDeltaHeader hdr;
        char path[TT_PATH_MAX];
        uint8_t *pl = NULL;
        size_t plsz = 0;
        int rc = tt_store_reader_next(&hdr, path, sizeof path, &pl, &plsz);
        if (rc <= 0) break;
        if (strcmp(path, rel) != 0) {
            free(pl);
            continue;
        }
        int changed = 0;
        if (hdr.event_type == TT_EV_DELETE) {
            free(state);
            state = NULL;
            state_size = 0;
            have = 0;
            if (manifest) fprintf(manifest, "%s\t%llu\tDELETE\t0\t-\n", rel, (unsigned long long)hdr.timestamp_ns);
        } else if (hdr.event_type == TT_EV_CREATE) {
            free(state);
            state = NULL;
            state_size = 0;
            if (plsz > 0 && pl) {
                state = malloc(plsz);
                if (state) {
                    memcpy(state, pl, plsz);
                    state_size = plsz;
                }
            }
            have = 1;
            changed = 1;
        } else if (hdr.event_type == TT_EV_MODIFY) {
            if (plsz > 0) {
                if (have) {
                    uint8_t *ns = NULL;
                    size_t nss = 0;
                    if (tt_delta_decode(state, state_size, pl, plsz, hdr.file_size, &ns, &nss) == 0) {
                        free(state);
                        state = ns;
                        state_size = nss;
                        changed = 1;
                    }
                }
                if (!changed && plsz == hdr.file_size) {
                    free(state);
                    state = malloc(plsz);
                    if (state) {
                        memcpy(state, pl, plsz);
                        state_size = plsz;
                        have = 1;
                        changed = 1;
                    }
                }
            }
        }
        free(pl);
        if (changed) {
            seq++;
            char cur[TT_PATH_MAX * 2];
            snprintf(cur, sizeof cur, "%s/v%06llu_%llu", fdir, (unsigned long long)seq, (unsigned long long)hdr.timestamp_ns);
            write_file_all(cur, state, state_size);
            if (with_diff && prev_file[0]) {
                char diff_file[TT_PATH_MAX * 2];
                snprintf(diff_file, sizeof diff_file, "%s/%06llu_to_%06llu.diff", ddir, (unsigned long long)(seq - 1), (unsigned long long)seq);
                run_diff_file(prev_file, cur, diff_file);
            }
            snprintf(prev_file, sizeof prev_file, "%s", cur);
            if (manifest) {
                fprintf(manifest, "%s\t%llu\t%s\t%zu\t%s\n", rel, (unsigned long long)hdr.timestamp_ns,
                        hdr.event_type == TT_EV_CREATE ? "CREATE" : "MODIFY", state_size, cur);
            }
        }
    }
    tt_store_reader_free();
    free(state);
    return seq > 0 ? 0 : 1;
}

typedef enum {
    UNDO_MODE_TIME,
    UNDO_MODE_LAST,
    UNDO_MODE_INITIAL,
    UNDO_MODE_TAG
} TtUndoMode;

static int run_watch_loop(TtDaemon *d) {
    if (tt_store_init_writer(d->store_dir, 1) != 0) return 1;
    write_pid_file(d->store_dir);
    cache_init(&d->cache);
    tt_debounce_init(&d->debounce);
    if (tt_watcher_init(d) != 0) return 1;
    tt_watcher_add_root(d);
    g_daemon_start_ns = tt_now_ns();
    cache_build_initial(d);
    status_write(d);
    fprintf(stderr, "=== Time-Travel CLI ===\nWatching: %s\nCtrl+C to exit.\n", d->watch_dir);
    static int last_compact_day = -1;
    static uint64_t last_status_ns = 0;
    struct pollfd fds[2] = {
        { .fd = d->inotify_fd, .events = POLLIN },
        { .fd = d->timer_fd,   .events = POLLIN }
    };
    while (d->running && !g_signal_received) {
        if (poll(fds, 2, 1000) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & POLLIN) tt_watcher_process_events(d);
        if (fds[1].revents & POLLIN) {
            uint64_t exp = 0;
            if (read(d->timer_fd, &exp, sizeof exp) == (ssize_t)sizeof exp) {
                tt_debounce_process(d, on_debounced, NULL);
                d->rescan_accum_ms += exp * TT_TICK_MS;
                if (d->rescan_needed || d->rescan_accum_ms >= TT_RESCAN_MS) {
                    d->rescan_needed = 0;
                    d->rescan_accum_ms = 0;
                    rescan_and_sync(d);
                }
                handle_root_health(d);
                uint64_t now_status = tt_now_ns();
                if (now_status - last_status_ns >= 1000000000ULL) {
                    status_write(d);
                    last_status_ns = now_status;
                }
                time_t now_t = time(NULL);
                struct tm tmn;
                localtime_r(&now_t, &tmn);
                if (tmn.tm_hour == TT_COMPACT_HOUR && last_compact_day != tmn.tm_mday) {
                    last_compact_day = tmn.tm_mday;
                    tt_log("scheduled compaction\n");
                    int nc = tt_compact_run(d);
                    if (nc >= 0) tt_log("compaction: %d chain(s)\n", nc);
                }
            }
        }
    }
    tt_watcher_free(d);
    tt_store_free();
    cache_free(&d->cache);
    tt_debounce_free(&d->debounce);
    char pf[TT_PATH_MAX + 64];
    pid_file_path(pf, sizeof pf, d->store_dir);
    unlink(pf);
    char sf[TT_PATH_MAX + 64];
    status_file_path(sf, sizeof sf, d->store_dir);
    unlink(sf);
    return 0;
}

static int cmd_watch(const char *dir, int fg) {
    memset(&g_daemon, 0, sizeof g_daemon);
    char wd[TT_PATH_MAX];
    if (!realpath(dir, wd)) {
        fprintf(stderr, "error: cannot resolve '%s': %s\n", dir, strerror(errno));
        return 1;
    }
    snprintf(g_daemon.watch_dir, sizeof g_daemon.watch_dir, "%s", wd);
    snprintf(g_daemon.store_dir, sizeof g_daemon.store_dir, "%s/.timetravel", wd);
    mkdir(g_daemon.store_dir, 0700);
    if (find_running_daemon(g_daemon.store_dir, NULL) > 0) {
        printf("Daemon is already running in %s\nUse 'timetravel stop --repo %s'\n", wd, wd);
        return 0;
    }
    g_daemon.running = 1;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    if (!fg) {
        pid_t p = fork();
        if (p < 0) return 1;
        if (p > 0) {
            char pf[TT_PATH_MAX + 64];
            pid_file_path(pf, sizeof pf, g_daemon.store_dir);
            for (int i = 0; i < 20 && access(pf, F_OK) != 0; ++i) usleep(100 * 1000);
            printf("Time-Travel started in background\n  PID:   %d\n  Repo:  %s\n  Log:   %s/timetravel.log\n  Stop:  timetravel stop --repo %s\n", p, wd, g_daemon.store_dir, wd);
            return 0;
        }
        setsid();
        char lp[TT_PATH_MAX + 64];
        snprintf(lp, sizeof lp, "%s/timetravel.log", g_daemon.store_dir);
        int lf = open(lp, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lf >= 0) {
            dup2(lf, 1);
            dup2(lf, 2);
            if (lf > 2) close(lf);
        }
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) {
            dup2(dn, 0);
            if (dn > 2) close(dn);
        }
    }
    return run_watch_loop(&g_daemon);
}

static int cmd_stop(const char *repo_dir) {
    char wd[TT_PATH_MAX], sd[TT_PATH_MAX * 2];
    if (resolve_repo_arg(repo_dir, ".", wd, sizeof wd) != 0) {
        fprintf(stderr, "error: .timetravel not found\n");
        return 1;
    }
    snprintf(sd, sizeof sd, "%s/.timetravel", wd);
    pid_t pid = find_running_daemon(sd, NULL);
    if (pid < 0) {
        printf("No daemon running in %s\n", wd);
        return 0;
    }
    printf("Stopping Time-Travel (pid=%d)...\n", (int)pid);
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            char pf[TT_PATH_MAX + 64];
            pid_file_path(pf, sizeof pf, sd);
            unlink(pf);
            printf("✓ Daemon stopped.\n");
            return 0;
        }
        usleep(100 * 1000);
    }
    fprintf(stderr, "warning: did not respond to SIGTERM; sending SIGKILL\n");
    kill(pid, SIGKILL);
    char pf[TT_PATH_MAX + 64];
    pid_file_path(pf, sizeof pf, sd);
    unlink(pf);
    return 0;
}

static int cmd_undo(const char *path, const char *time_expr, const char *repo_dir, TtUndoMode mode, const char *tag_name, int force) {
    char watch_dir[TT_PATH_MAX];
    if (resolve_repo_arg(repo_dir, path, watch_dir, sizeof watch_dir) != 0) {
        fprintf(stderr, "error: .timetravel not found\n");
        return 1;
    }
    char store_dir[TT_PATH_MAX * 2];
    snprintf(store_dir, sizeof store_dir, "%s/.timetravel", watch_dir);
    if (tt_store_init(store_dir) != 0) return 1;
    char rel_path[TT_PATH_MAX];
    if (make_rel_from_arg(watch_dir, path, rel_path, sizeof rel_path) != 0) rel_path[0] = '\0';
    char full_path[TT_PATH_MAX * 2];
    if (rel_path[0]) snprintf(full_path, sizeof full_path, "%s/%s", watch_dir, rel_path);
    else snprintf(full_path, sizeof full_path, "%s", watch_dir);
    struct stat st;
    int is_dir = (rel_path[0] == '\0') || (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode));
    uint64_t target_ns = 0;
    int v_idx = 0;
    size_t v_total = 0;
    if (mode == UNDO_MODE_TIME) {
        char tdesc[128];
        int resolved = 0;
        if (!is_dir && rel_path[0]) {
            resolved = (resolve_file_target_ns(store_dir, rel_path, time_expr, &target_ns, tdesc, sizeof tdesc) == 0);
        }
        if (!resolved) {
            int tok = 0;
            target_ns = parse_time_expr(time_expr, &tok);
            if (!tok) {
                fprintf(stderr, "error: invalid time expression: '%s'\n", time_expr ? time_expr : "");
                tt_store_free();
                return 1;
            }
        }
    } else if (mode == UNDO_MODE_TAG) {
        if (tag_lookup(store_dir, tag_name, &target_ns) != 0) {
            fprintf(stderr, "error: tag '%s' not found (use 'timetravel tags')\n", tag_name);
            tt_store_free();
            return 1;
        }
    } else if (!(is_dir && (mode == UNDO_MODE_LAST || mode == UNDO_MODE_INITIAL))) {
        uint64_t *ts = NULL;
        size_t n = 0;
        if (scan_timestamps(rel_path, 0, &ts, &n) != 0 || n == 0) {
            fprintf(stderr, "error: no history for '%s'\n", path);
            free(ts);
            tt_store_free();
            return 1;
        }
        if (mode == UNDO_MODE_INITIAL) {
            target_ns = ts[0];
            v_idx = 1;
            v_total = n;
        } else {
            if (n < 2) {
                fprintf(stderr, "error: only %zu record(s); no previous version\n", n);
                free(ts);
                tt_store_free();
                return 1;
            }
            target_ns = ts[n - 2];
            v_idx = (int)n - 1;
            v_total = n;
        }
        free(ts);
    }
    if (is_dir && !force) {
        fprintf(stderr, "⚠ You are about to restore directory '%s' (files will be overwritten).\n  Continue? [y/N] ", path[0] ? path : ".");
        fflush(stderr);
        char ans[8] = {0};
        if (!fgets(ans, sizeof ans, stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
            printf("Canceled.\n");
            tt_store_free();
            return 0;
        }
    }
    int rc;
    if (is_dir && (mode == UNDO_MODE_LAST || mode == UNDO_MODE_INITIAL)) rc = tt_restore_dir_per_file(store_dir, rel_path, watch_dir, mode == UNDO_MODE_INITIAL ? 1 : 0);
    else if (is_dir) rc = tt_restore_dir(store_dir, rel_path, target_ns, watch_dir);
    else rc = tt_restore_file(store_dir, rel_path, target_ns, full_path);
    tt_store_free();
    if (rc == 0) {
        char ts_str[64];
        format_timestamp(target_ns, ts_str, sizeof ts_str);
        printf("✓ '%s' restored successfully\n", path);
        if (mode == UNDO_MODE_TIME || mode == UNDO_MODE_TAG) printf("  Time point: %s\n", ts_str);
        if (mode == UNDO_MODE_TAG) printf("  Tag:            %s\n", tag_name);
        else if (mode != UNDO_MODE_TIME && v_total > 0) printf("  Version:        %d of %zu\n", v_idx, v_total);
        printf("  Destination:    %s\n", is_dir ? watch_dir : full_path);
    } else if (rc == 1) {
        fprintf(stderr, "info: '%s' did not exist at that time point.\n", path);
    } else {
        fprintf(stderr, "error: failed to restore '%s'.\n", path);
    }
    return (rc == 0 || rc == 1) ? 0 : 1;
}

static int cmd_status(const char *repo_dir) {
    char wd[TT_PATH_MAX], sd[TT_PATH_MAX * 2];
    if (resolve_repo_arg(repo_dir, ".", wd, sizeof wd) != 0) {
        fprintf(stderr, "error: .timetravel not found\n");
        return 1;
    }
    snprintf(sd, sizeof sd, "%s/.timetravel", wd);
    TtStatusFile sf;
    int have_sf = (status_read(sd, &sf) == 0);
    uint64_t pid_start = 0;
    pid_t pid = find_running_daemon(sd, &pid_start);
    if (pid <= 0 && have_sf && proc_alive(sf.pid)) {
        pid = sf.pid;
        pid_start = sf.start_ns;
    }
    if (tt_store_init(sd) != 0) {
        fprintf(stderr, "error: could not open store\n");
        return 1;
    }
    uint64_t nrecords = 0, nbytes = 0, fts = 0, lts = 0;
    tt_store_scan_stats(&nrecords, &nbytes, &fts, &lts);
    tt_store_free();
    printf("=== Time-Travel Status ===\nRepo:      %s\n", wd);
    if (pid > 0 && proc_alive(pid)) {
        uint64_t start_ns = have_sf ? sf.start_ns : pid_start;
        uint64_t now = tt_now_ns();
        uint64_t up = (now > start_ns) ? (now - start_ns) : 0;
        char upbuf[128];
        format_ns_duration(up, upbuf, sizeof upbuf);
        double cpu = proc_cpu_percent(pid);
        long rss = proc_rss_kb(pid);
        printf("Daemon:    ✓ running (pid=%d)\nUptime:    %s\n", (int)pid, upbuf);
        if (cpu >= 0.0) printf("CPU:       %.1f%%\n", cpu);
        else printf("CPU:       ?\n");
        if (rss >= 0) {
            char mem[64];
            format_bytes((uint64_t)rss * 1024ULL, mem, sizeof mem);
            printf("RAM:       %s\n", mem);
        } else {
            printf("RAM:       ?\n");
        }
        if (have_sf) {
            char b[64];
            format_bytes(sf.bytes, b, sizeof b);
            printf("Written:   %llu deltas, %s\nPending:   %llu\n", (unsigned long long)sf.deltas, b, (unsigned long long)sf.pending);
        }
    } else {
        printf("Daemon:    ✗ not running\n");
    }
    char b[64];
    format_bytes(nbytes, b, sizeof b);
    printf("Records:   %llu\nBytes:     %s\n", (unsigned long long)nrecords, b);
    char b1[64], b2[64];
    if (fts) format_timestamp(fts, b1, sizeof b1);
    else snprintf(b1, sizeof b1, "-");
    if (lts) format_timestamp(lts, b2, sizeof b2);
    else snprintf(b2, sizeof b2, "-");
    printf("First:     %s\nLast:      %s\n==========================\n", b1, b2);
    return 0;
}

static int cmd_log(const char *path, const char *repo_dir, const char *since_expr) {
    char wd[TT_PATH_MAX], sd[TT_PATH_MAX * 2], rel[TT_PATH_MAX];
    if (resolve_repo_arg(repo_dir, path, wd, sizeof wd) != 0) {
        fprintf(stderr, "error: .timetravel not found\n");
        return 1;
    }
    snprintf(sd, sizeof sd, "%s/.timetravel", wd);
    if (tt_store_init(sd) != 0) return 1;
    if (make_rel_from_arg(wd, path, rel, sizeof rel) != 0) rel[0] = '\0';
    if (!since_expr) {
        int rc = tt_list_history(sd, rel[0] ? rel : NULL);
        tt_store_free();
        return rc;
    }
    int tok = 0;
    uint64_t since_ns = parse_time_expr(since_expr, &tok);
    if (!tok) {
        fprintf(stderr, "error: invalid time expression: '%s'\n", since_expr);
        tt_store_free();
        return 1;
    }
    if (tt_store_reader_init() != 0) {
        tt_store_free();
        return 1;
    }
    printf("History of: %s (since %s)\n", rel[0] ? rel : "(all)", since_expr);
    int any = 0;
    for (;;) {
        TtDeltaHeader hdr;
        char p2[TT_PATH_MAX];
        uint8_t *pl = NULL;
        size_t plsz = 0;
        int rc = tt_store_reader_next(&hdr, p2, sizeof p2, &pl, &plsz);
        if (rc <= 0) break;
        if (rel[0] && strcmp(p2, rel) != 0) {
            free(pl);
            continue;
        }
        if (hdr.timestamp_ns < since_ns) {
            free(pl);
            continue;
        }
        char ts[64];
        format_timestamp(hdr.timestamp_ns, ts, sizeof ts);
        const char *ev = hdr.event_type == TT_EV_CREATE ? "CREATE" : hdr.event_type == TT_EV_MODIFY ? "MODIFY" : hdr.event_type == TT_EV_DELETE ? "DELETE" : "???";
        printf("  %-20s  %-8s  delta=%10u  file=%10llu  %s\n", ts, ev, hdr.delta_size, (unsigned long long)hdr.file_size, p2);
        any = 1;
        free(pl);
    }
    tt_store_reader_free();
    tt_store_free();
    if (!any) printf("  (no events in that range)\n");
    return 0;
}

static int cmd_compact(const char *repo_dir) {
    char wd[TT_PATH_MAX];
    if (resolve_repo_arg(repo_dir, ".", wd, sizeof wd) != 0) {
        fprintf(stderr, "error: .timetravel not found\n");
        return 1;
    }
    memset(&g_daemon, 0, sizeof g_daemon);
    snprintf(g_daemon.store_dir, sizeof g_daemon.store_dir, "%s/.timetravel", wd);
    if (tt_store_init(g_daemon.store_dir) != 0) return 1;
    int rc = tt_compact_run(&g_daemon);
    tt_store_free();
    if (rc < 0) {
        fprintf(stderr, "error: compaction failed\n");
        return 1;
    }
    printf("compaction: %d chain(s) collapsed\n", rc);
    return 0;
}

static int cmd_tag(const char *name, const char *repo_dir) {
    char wd[TT_PATH_MAX], sd[TT_PATH_MAX * 2];
    if (resolve_repo_arg(repo_dir, ".", wd, sizeof wd) != 0) {
        fprintf(stderr, "error: .timetravel not found\n");
        return 1;
    }
    snprintf(sd, sizeof sd, "%s/.timetravel", wd);
    mkdir(sd, 0700);
    uint64_t ts = tt_now_ns();
    if (tag_add(sd, name, ts) != 0) {
        fprintf(stderr, "error: could not write tag\n");
        return 1;
    }
    char tsbuf[64];
    format_timestamp(ts, tsbuf, sizeof tsbuf);
    printf("✓ Tag '%s' created at %s\n  Restore: timetravel undo <path> --tag %s --repo %s\n", name, tsbuf, name, wd);
    return 0;
}

static int cmd_diff(const char *path, const char *repo_dir, const char *time_expr) {
    char wd[TT_PATH_MAX], sd[TT_PATH_MAX * 2], rel[TT_PATH_MAX];
    if (resolve_repo_arg(repo_dir, path, wd, sizeof wd) != 0) {
        fprintf(stderr, "error: .timetravel not found\n");
        return 1;
    }
    snprintf(sd, sizeof sd, "%s/.timetravel", wd);
    if (tt_store_init(sd) != 0) return 1;
    if (make_rel_from_arg(wd, path, rel, sizeof rel) != 0 || rel[0] == '\0') {
        fprintf(stderr, "error: diff requires a file inside the repo\n");
        tt_store_free();
        return 1;
    }
    uint64_t target_ns = 0;
    char tdesc[128];
    if (resolve_file_target_ns(sd, rel, time_expr, &target_ns, tdesc, sizeof tdesc) != 0) {
        fprintf(stderr, "error: no history for '%s'\n", path);
        tt_store_free();
        return 1;
    }
    uint8_t *old_data = NULL;
    size_t old_size = 0;
    int exists = 0;
    load_version_content(rel, target_ns, &old_data, &old_size, &exists);
    tt_store_free();
    char fp[TT_PATH_MAX * 2];
    snprintf(fp, sizeof fp, "%s/%s", wd, rel);
    uint8_t *cur_data = NULL;
    size_t cur_size = 0;
    read_file_all(fp, &cur_data, &cur_size);
    char ta[] = "/tmp/tt_diff_old_XXXXXX";
    char tb[] = "/tmp/tt_diff_new_XXXXXX";
    int fda = mkstemp(ta);
    int fdb = mkstemp(tb);
    if (fda < 0 || fdb < 0) {
        fprintf(stderr, "error: could not create temporaries (%s)\n", strerror(errno));
        if (fda >= 0) close(fda);
        if (fdb >= 0) close(fdb);
        free(old_data);
        free(cur_data);
        return 1;
    }
    if (old_size > 0) {
        ssize_t wr = write(fda, old_data, old_size);
        (void)wr;
    }
    if (cur_size > 0) {
        ssize_t wr = write(fdb, cur_data, cur_size);
        (void)wr;
    }
    close(fda);
    close(fdb);
    char cmdbuf[1024];
    snprintf(cmdbuf, sizeof cmdbuf, "diff -u --label \"%s (%s)\" --label \"%s (current)\" \"%s\" \"%s\" 2>/dev/null", path, tdesc, path, ta, tb);
    int rc = system(cmdbuf);
    if (rc == -1) fprintf(stderr, "warning: could not execute 'diff'\n");
    else if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0) printf("(no differences)\n");
    unlink(ta);
    unlink(tb);
    free(old_data);
    free(cur_data);
    return 0;
}

static int cmd_dump(const char *path, const char *repo_dir, const char *outdir, int with_diff) {
    if (!outdir || !outdir[0]) {
        fprintf(stderr, "error: dump requires --out <dir>\n");
        return 1;
    }
    char wd[TT_PATH_MAX];
    char sd[TT_PATH_MAX * 2];
    char rel[TT_PATH_MAX];
    if (resolve_repo_arg(repo_dir, path, wd, sizeof wd) != 0) {
        fprintf(stderr, "error: .timetravel not found\n");
        return 1;
    }
    snprintf(sd, sizeof sd, "%s/.timetravel", wd);
    if (tt_store_init(sd) != 0) return 1;
    if (make_rel_from_arg(wd, path, rel, sizeof rel) != 0) rel[0] = '\0';
    if (mkdir_p(outdir) != 0) {
        fprintf(stderr, "error: could not create '%s'\n", outdir);
        tt_store_free();
        return 1;
    }
    char manifest_path[TT_PATH_MAX * 2];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.tsv", outdir);
    FILE *manifest = fopen(manifest_path, "w");
    if (!manifest) {
        fprintf(stderr, "error: could not create manifest.tsv\n");
        tt_store_free();
        return 1;
    }
    fprintf(manifest, "path\ttimestamp\tevent\tsize\tversion_file\n");
    TtPathSet set;
    store_collect_matching(&set, rel);
    if (set.n == 0) {
        printf("no history for '%s'\n", path);
        fclose(manifest);
        pathset_free(&set);
        tt_store_free();
        return 1;
    }
    int ok = 0;
    for (size_t i = 0; i < set.n; ++i) {
        int rc = dump_one_file(sd, set.v[i], outdir, with_diff, manifest);
        if (rc == 0) ok++;
    }
    fclose(manifest);
    pathset_free(&set);
    tt_store_free();
    printf("dump: %d file(s) with history in %s\n", ok, outdir);
    return ok > 0 ? 0 : 1;
}

static void usage(void) {
    printf("\n"
           "============================================================\n"
           " Time-Travel CLI — Filesystem-level Ctrl+Z\n"
           "============================================================\n"
           "USAGE:\n"
           "  timetravel start <dir>\n"
           "  timetravel stop [--repo <dir>]\n"
           "  timetravel restart <dir>\n"
           "  timetravel watch <dir> [-f]\n"
           "  timetravel undo <path> [--to <expr> | --last | --initial | --tag <name>] [--repo <dir>] [--force]\n"
           "  timetravel diff <path> [--to <expr>] [--repo <dir>]\n"
           "  timetravel dump <path> --out <dir> [--with-diff] [--repo <dir>]\n"
           "  timetravel tag <name> [--repo <dir>]\n"
           "  timetravel tags [--repo <dir>]\n"
           "  timetravel status [--repo <dir>]\n"
           "  timetravel log <path> [--since <expr>] [--repo <dir>]\n"
           "  timetravel compact [--repo <dir>]\n"
           "\n\n"
           "--to accepts:\n"
           "  Exact timestamp: \"2026-09-13 00:07:08\"\n"
           "  Relative time:   \"10 minutes ago\", \"1 hour ago\"\n"
           "  Keywords:        now, head, latest, prev, first, N, -N, tag:<name>, @<ts>\n"
           "\n\n"
           "EXAMPLES:\n"
           "  timetravel start /tmp/project\n"
           "  timetravel status --repo /tmp/project\n"
           "  timetravel log /tmp/project/main.c --repo /tmp/project\n"
           "  timetravel diff /tmp/project/main.c --to \"2026-09-13 00:07:08\" --repo /tmp/project\n"
           "  timetravel undo /tmp/project/main.c --to \"10 minutes ago\" --repo /tmp/project\n"
           "  timetravel undo /tmp/project --last --force --repo /tmp/project\n"
           "  timetravel tag release-v1.0 --repo /tmp/project\n"
           "  timetravel dump /tmp/project --repo /tmp/project --out /tmp/tt_dump --with-diff\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(cmd, "start") == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        return cmd_watch(argv[2], 0);
    }
    if (strcmp(cmd, "stop") == 0) {
        const char *r = NULL;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) r = argv[++i];
        }
        return cmd_stop(r);
    }
    if (strcmp(cmd, "restart") == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        cmd_stop(argv[2]);
        return cmd_watch(argv[2], 0);
    }
    if (strcmp(cmd, "watch") == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        int fg = 0;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-f") == 0) fg = 1;
        }
        return cmd_watch(argv[2], fg);
    }
    if (strcmp(cmd, "undo") == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        const char *path = argv[2];
        const char *expr = "now";
        const char *repo = NULL;
        const char *tag = NULL;
        TtUndoMode mode = UNDO_MODE_LAST;
        int force = 0;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
                expr = argv[++i];
                mode = UNDO_MODE_TIME;
            } else if (strcmp(argv[i], "--last") == 0) {
                mode = UNDO_MODE_LAST;
            } else if (strcmp(argv[i], "--initial") == 0) {
                mode = UNDO_MODE_INITIAL;
            } else if (strcmp(argv[i], "--tag") == 0 && i + 1 < argc) {
                tag = argv[++i];
                mode = UNDO_MODE_TAG;
            } else if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
                repo = argv[++i];
            } else if (strcmp(argv[i], "--force") == 0) {
                force = 1;
            }
        }
        return cmd_undo(path, expr, repo, mode, tag, force);
    }
    if (strcmp(cmd, "diff") == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        const char *repo = NULL;
        const char *expr = NULL;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) repo = argv[++i];
            else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc) expr = argv[++i];
        }
        return cmd_diff(argv[2], repo, expr);
    }
    if (strcmp(cmd, "dump") == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        const char *repo = NULL;
        const char *out = NULL;
        int with_diff = 0;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) repo = argv[++i];
            else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
            else if (strcmp(argv[i], "--with-diff") == 0) with_diff = 1;
        }
        return cmd_dump(argv[2], repo, out, with_diff);
    }
    if (strcmp(cmd, "tag") == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        const char *repo = NULL;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) repo = argv[++i];
        }
        return cmd_tag(argv[2], repo);
    }
    if (strcmp(cmd, "tags") == 0) {
        char wd[TT_PATH_MAX], sd[TT_PATH_MAX * 2];
        const char *repo = NULL;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) repo = argv[++i];
        }
        if (repo) snprintf(wd, sizeof wd, "%s", repo);
        else if (find_repo_dir(".", wd, sizeof wd) != 0) {
            fprintf(stderr, "error: .timetravel not found\n");
            return 1;
        }
        snprintf(sd, sizeof sd, "%s/.timetravel", wd);
        return tag_list(sd);
    }
    if (strcmp(cmd, "status") == 0) {
        const char *r = NULL;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) r = argv[++i];
        }
        return cmd_status(r);
    }
    if (strcmp(cmd, "log") == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        const char *repo = NULL;
        const char *since = NULL;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) repo = argv[++i];
            else if (strcmp(argv[i], "--since") == 0 && i + 1 < argc) since = argv[++i];
        }
        return cmd_log(argv[2], repo, since);
    }
    if (strcmp(cmd, "compact") == 0) {
        const char *r = NULL;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) r = argv[++i];
        }
        return cmd_compact(r);
    }
    fprintf(stderr, "error: unknown command '%s'\n", cmd);
    usage();
    return 1;
}
