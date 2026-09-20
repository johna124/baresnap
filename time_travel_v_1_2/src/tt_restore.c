#include "tt_types.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern int tt_store_reader_init(void);
extern int tt_store_reader_next(TtDeltaHeader *h, char *p, size_t ps, uint8_t **pl, size_t *plsz);
extern void tt_store_reader_free(void);
extern int tt_delta_decode(const uint8_t *old_data, size_t old_size, const uint8_t *delta_data, size_t delta_size, size_t expected_new_size, uint8_t **new_out, size_t *new_size_out);

typedef struct {
    TtDeltaHeader hdr;
    char path[TT_PATH_MAX];
    uint8_t *payload;
    size_t payload_size;
} TtRestoreRecord;

typedef struct {
    TtRestoreRecord *records;
    size_t count, cap;
} TtRestoreList;

static int restore_list_push(TtRestoreList *l, const TtDeltaHeader *hdr, const char *path, uint8_t *payload, size_t payload_size) {
    if (l->count == l->cap) {
        size_t n = l->cap ? l->cap * 2 : 32;
        TtRestoreRecord *nr = realloc(l->records, n * sizeof(TtRestoreRecord));
        if (!nr) return -1;
        l->records = nr;
        l->cap = n;
    }
    TtRestoreRecord *r = &l->records[l->count++];
    r->hdr = *hdr;
    snprintf(r->path, sizeof r->path, "%s", path);
    r->payload = payload;
    r->payload_size = payload_size;
    return 0;
}

static void restore_list_free(TtRestoreList *l) {
    if (!l) return;
    for (size_t i = 0; i < l->count; ++i) free(l->records[i].payload);
    free(l->records);
    memset(l, 0, sizeof *l);
}

static int restore_record_cmp(const void *a, const void *b) {
    const TtRestoreRecord *ra = a, *rb = b;
    if (ra->hdr.timestamp_ns < rb->hdr.timestamp_ns) return -1;
    if (ra->hdr.timestamp_ns > rb->hdr.timestamp_ns) return 1;
    return 0;
}

static int load_records(const char *filter_path, TtRestoreList *out) {
    memset(out, 0, sizeof *out);
    if (tt_store_reader_init() != 0) return -1;
    for (;;) {
        TtDeltaHeader hdr;
        char path[TT_PATH_MAX];
        uint8_t *pl = NULL;
        size_t plsz = 0;
        int rc = tt_store_reader_next(&hdr, path, sizeof path, &pl, &plsz);
        if (rc == 0) break;
        if (rc < 0) {
            tt_store_reader_free();
            restore_list_free(out);
            return -1;
        }
        if (filter_path && filter_path[0] && strcmp(path, filter_path) != 0) {
            free(pl);
            continue;
        }
        if (restore_list_push(out, &hdr, path, pl, plsz) != 0) {
            free(pl);
            tt_store_reader_free();
            restore_list_free(out);
            return -1;
        }
    }
    tt_store_reader_free();
    if (out->count > 1) qsort(out->records, out->count, sizeof(TtRestoreRecord), restore_record_cmp);
    return 0;
}

static int reconstruct_file(const TtRestoreRecord *records, size_t count, uint64_t target_ns,
                            uint8_t **out_data, size_t *out_size, int *out_exists) {
    *out_data = NULL;
    *out_size = 0;
    *out_exists = 0;
    uint8_t *state = NULL;
    size_t state_size = 0;
    int have = 0;
    for (size_t i = 0; i < count; ++i) {
        const TtRestoreRecord *rec = &records[i];
        if (rec->hdr.timestamp_ns > target_ns) break;
        if (rec->hdr.event_type == TT_EV_DELETE) {
            free(state);
            state = NULL;
            state_size = 0;
            have = 0;
        } else if (rec->hdr.event_type == TT_EV_CREATE) {
            free(state);
            state = NULL;
            state_size = 0;
            size_t s = rec->payload_size;
            if (s > 0 && rec->payload) {
                state = malloc(s);
                if (!state) return -1;
                memcpy(state, rec->payload, s);
                state_size = s;
            }
            have = 1;
        } else if (rec->hdr.event_type == TT_EV_MODIFY) {
            if (!have) return -1;
            if (rec->hdr.delta_size == 0) {
                free(state);
                state = NULL;
                state_size = 0;
                continue;
            }
            if (!rec->payload) return -1;
            uint8_t *ns = NULL;
            size_t nss = 0;
            if (tt_delta_decode(state, state_size, rec->payload, rec->payload_size,
                                rec->hdr.file_size, &ns, &nss) != 0) {
                free(state);
                return -1;
            }
            free(state);
            state = ns;
            state_size = nss;
        }
    }
    if (!have) {
        free(state);
        return 0;
    }
    *out_exists = 1;
    *out_data = state;
    *out_size = state_size;
    return 0;
}

static int write_file_with_parents(const char *path, const uint8_t *data, size_t size) {
    char tmp[TT_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    struct stat orig_st;
    int have_orig = (stat(path, &orig_st) == 0);
    char tmppath[TT_PATH_MAX + 40];
    snprintf(tmppath, sizeof tmppath, "%s.tt_tmp_%d", path, (int)getpid());
    int fd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < size) {
        ssize_t w = write(fd, data + off, size - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmppath);
            return -1;
        }
        off += (size_t)w;
    }
    fsync(fd);
    close(fd);
    if (have_orig) chmod(tmppath, orig_st.st_mode & 07777);
    if (rename(tmppath, path) != 0) {
        unlink(tmppath);
        return -1;
    }
    return 0;
}

int tt_restore_file(const char *store_dir, const char *file_path, uint64_t target_ns, const char *out_path) {
    (void)store_dir;
    TtRestoreList list;
    if (load_records(file_path, &list) != 0) return -1;
    if (list.count == 0) {
        restore_list_free(&list);
        return 1;
    }
    uint8_t *content = NULL;
    size_t csz = 0;
    int exists = 0;
    int rc = reconstruct_file(list.records, list.count, target_ns, &content, &csz, &exists);
    restore_list_free(&list);
    if (rc != 0) return -1;
    if (!exists) return 1;
    if (out_path && write_file_with_parents(out_path, content, csz) != 0) {
        free(content);
        return -1;
    }
    fprintf(stderr, "tt_restore: restored %s -> %s (%zu bytes)\n", file_path, out_path ? out_path : "(null)", csz);
    free(content);
    return 0;
}

int tt_restore_dir(const char *store_dir, const char *dir_prefix, uint64_t target_ns, const char *out_dir) {
    (void)store_dir;
    TtRestoreList all;
    if (load_records(NULL, &all) != 0) return -1;
    if (all.count == 0) {
        restore_list_free(&all);
        return 1;
    }
    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        restore_list_free(&all);
        return -1;
    }
    size_t plen = (dir_prefix && dir_prefix[0]) ? strlen(dir_prefix) : 0;
    char **upaths = NULL;
    size_t ucount = 0, ucap = 0;
    for (size_t i = 0; i < all.count; ++i) {
        const char *p = all.records[i].path;
        if (plen > 0 && (strncmp(p, dir_prefix, plen) != 0 || (p[plen] != '/' && p[plen] != '\0'))) continue;
        int found = 0;
        for (size_t u = 0; u < ucount; ++u) {
            if (strcmp(upaths[u], p) == 0) {
                found = 1;
                break;
            }
        }
        if (found) continue;
        if (ucount == ucap) {
            ucap = ucap ? ucap * 2 : 32;
            char **nu = realloc(upaths, ucap * sizeof(char *));
            if (!nu) break;
            upaths = nu;
        }
        upaths[ucount] = strdup(p);
        if (!upaths[ucount]) break;
        ucount++;
    }
    int restored = 0, errors = 0;
    for (size_t u = 0; u < ucount; ++u) {
        const char *fp = upaths[u];
        size_t fc = 0;
        for (size_t i = 0; i < all.count; ++i) {
            if (strcmp(all.records[i].path, fp) == 0) fc++;
        }
        if (fc == 0) continue;
        TtRestoreRecord *fr = calloc(fc, sizeof(TtRestoreRecord));
        if (!fr) {
            errors++;
            continue;
        }
        size_t fi = 0;
        for (size_t i = 0; i < all.count; ++i) {
            if (strcmp(all.records[i].path, fp) == 0) fr[fi++] = all.records[i];
        }
        uint8_t *content = NULL;
        size_t csz = 0;
        int exists = 0;
        int rc = reconstruct_file(fr, fc, target_ns, &content, &csz, &exists);
        free(fr);
        if (rc != 0) {
            errors++;
            continue;
        }
        if (!exists) continue;
        const char *rel = fp;
        if (plen > 0 && strncmp(fp, dir_prefix, plen) == 0) {
            rel = fp + plen;
            while (*rel == '/') rel++;
        }
        char out_file[TT_PATH_MAX * 2];
        snprintf(out_file, sizeof out_file, "%s/%s", out_dir, rel);
        if (write_file_with_parents(out_file, content, csz) == 0) {
            restored++;
            fprintf(stderr, "tt_restore:   %s (%zu bytes)\n", rel, csz);
        } else {
            errors++;
        }
        free(content);
    }
    for (size_t u = 0; u < ucount; ++u) free(upaths[u]);
    free(upaths);
    restore_list_free(&all);
    fprintf(stderr, "tt_restore: %d file(s) restored in %s\n", restored, out_dir);
    return errors == 0 ? 0 : -1;
}

int tt_restore_dir_per_file(const char *store_dir, const char *dir_prefix, const char *out_dir, int mode) {
    (void)store_dir;
    TtRestoreList all;
    if (load_records(NULL, &all) != 0) return -1;
    if (all.count == 0) {
        restore_list_free(&all);
        return 1;
    }
    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        restore_list_free(&all);
        return -1;
    }
    size_t plen = (dir_prefix && dir_prefix[0]) ? strlen(dir_prefix) : 0;
    char **upaths = NULL;
    size_t ucount = 0, ucap = 0;
    for (size_t i = 0; i < all.count; ++i) {
        const char *p = all.records[i].path;
        if (plen > 0 && (strncmp(p, dir_prefix, plen) != 0 || (p[plen] != '/' && p[plen] != '\0'))) continue;
        int found = 0;
        for (size_t u = 0; u < ucount; ++u) {
            if (strcmp(upaths[u], p) == 0) {
                found = 1;
                break;
            }
        }
        if (found) continue;
        if (ucount == ucap) {
            ucap = ucap ? ucap * 2 : 32;
            char **nu = realloc(upaths, ucap * sizeof(char *));
            if (!nu) break;
            upaths = nu;
        }
        upaths[ucount] = strdup(p);
        if (!upaths[ucount]) break;
        ucount++;
    }
    int restored = 0, errors = 0, skipped = 0;
    for (size_t u = 0; u < ucount; ++u) {
        const char *fp = upaths[u];
        size_t fc = 0;
        for (size_t i = 0; i < all.count; ++i) {
            if (strcmp(all.records[i].path, fp) == 0) fc++;
        }
        if (fc == 0) continue;
        if (mode == 0 && fc < 2) {
            skipped++;
            continue;
        }
        TtRestoreRecord *fr = calloc(fc, sizeof(TtRestoreRecord));
        if (!fr) {
            errors++;
            continue;
        }
        size_t fi = 0;
        for (size_t i = 0; i < all.count; ++i) {
            if (strcmp(all.records[i].path, fp) == 0) fr[fi++] = all.records[i];
        }
        uint64_t target_ns = fr[mode == 0 ? fc - 2 : 0].hdr.timestamp_ns;
        uint8_t *content = NULL;
        size_t csz = 0;
        int exists = 0;
        int rc = reconstruct_file(fr, fc, target_ns, &content, &csz, &exists);
        free(fr);
        if (rc != 0) {
            errors++;
            continue;
        }
        if (!exists) continue;
        const char *rel = fp;
        if (plen > 0 && strncmp(fp, dir_prefix, plen) == 0) {
            rel = fp + plen;
            while (*rel == '/') rel++;
        }
        char out_file[TT_PATH_MAX * 2];
        snprintf(out_file, sizeof out_file, "%s/%s", out_dir, rel);
        if (write_file_with_parents(out_file, content, csz) == 0) {
            restored++;
            fprintf(stderr, "tt_restore:   %s (%zu bytes)\n", rel, csz);
        } else {
            errors++;
        }
        free(content);
    }
    for (size_t u = 0; u < ucount; ++u) free(upaths[u]);
    free(upaths);
    restore_list_free(&all);
    fprintf(stderr, "tt_restore: %d file(s) restored in %s (%d without previous version)\n", restored, out_dir, skipped);
    return errors == 0 ? 0 : -1;
}

int tt_list_history(const char *store_dir, const char *file_path) {
    (void)store_dir;
    TtRestoreList list;
    if (load_records(file_path, &list) != 0) return -1;
    if (list.count == 0) {
        printf("No history%s%s\n", (file_path && file_path[0]) ? " for: " : "", (file_path && file_path[0]) ? file_path : "");
        restore_list_free(&list);
        return 0;
    }
    if (file_path && file_path[0]) {
        printf("History of: %s (%zu events)\n", file_path, list.count);
    } else {
        printf("Full history (%zu events)\n", list.count);
    }
    for (size_t i = 0; i < list.count; ++i) {
        const TtRestoreRecord *r = &list.records[i];
        time_t s = (time_t)(r->hdr.timestamp_ns / 1000000000ULL);
        struct tm t;
        char ts[64];
        if (localtime_r(&s, &t)) {
            strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &t);
        } else {
            snprintf(ts, sizeof ts, "%llu", (unsigned long long)r->hdr.timestamp_ns);
        }
        const char *ev = r->hdr.event_type == TT_EV_CREATE ? "CREATE" :
                         r->hdr.event_type == TT_EV_MODIFY ? "MODIFY" :
                         r->hdr.event_type == TT_EV_DELETE ? "DELETE" : "???";
        printf("  %-20s  %-8s  delta=%10u  file=%10llu  %s\n", ts, ev, r->hdr.delta_size, (unsigned long long)r->hdr.file_size, r->path);
    }
    restore_list_free(&list);
    return 0;
}
