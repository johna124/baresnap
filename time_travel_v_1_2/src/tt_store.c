#include "tt_types.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define TT_STORE_MAX_FILE_SIZE (64ULL * 1024ULL * 1024ULL)
#define TT_STORE_MAGIC         "TTDLTA01"
#define TT_STORE_MAGIC_LEN     8
#define TT_STORE_VERSION       1u

typedef struct {
    char     store_dir[TT_PATH_MAX];
    int      current_fd;
    uint64_t current_size;
} TtStoreInternal;

static TtStoreInternal g_store;

static int write_all(int fd, const void *data, size_t n)
{
    const uint8_t *p = data;

    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        p += w;
        n -= (size_t)w;
    }

    return 0;
}

int tt_store_init(const char *store_dir)
{
    if (!store_dir || !store_dir[0])
        return -1;

    memset(&g_store, 0, sizeof g_store);
    g_store.current_fd = -1;

    snprintf(g_store.store_dir, sizeof g_store.store_dir, "%s", store_dir);

    if (mkdir(store_dir, 0700) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

void tt_store_free(void)
{
    if (g_store.current_fd >= 0) {
        fsync(g_store.current_fd);
        close(g_store.current_fd);
        g_store.current_fd = -1;
    }
}

int tt_store_write(const TtDeltaHeader *hdr, const char *path, const uint8_t *delta_payload)
{
    if (!hdr || !path)
        return -1;

    uint32_t path_len = (uint32_t)strlen(path);
    size_t record_size = sizeof(TtDeltaHeader) + path_len + hdr->delta_size;

    if (g_store.current_fd < 0 ||
        g_store.current_size + record_size > TT_STORE_MAX_FILE_SIZE) {
        if (g_store.current_fd >= 0) {
            fsync(g_store.current_fd);
            close(g_store.current_fd);
            g_store.current_fd = -1;
        }

        char fpath[TT_PATH_MAX + 32];
        snprintf(fpath, sizeof fpath, "%s/%020llu.ttd",
                 g_store.store_dir,
                 (unsigned long long)tt_now_ns());

        int fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0)
            return -1;

        uint8_t mhdr[TT_STORE_MAGIC_LEN + 4];
        memcpy(mhdr, TT_STORE_MAGIC, TT_STORE_MAGIC_LEN);

        uint32_t ver = TT_STORE_VERSION;
        for (int i = 0; i < 4; ++i)
            mhdr[TT_STORE_MAGIC_LEN + i] = (uint8_t)((ver >> (8 * i)) & 0xFF);

        if (write_all(fd, mhdr, sizeof mhdr) != 0) {
            close(fd);
            return -1;
        }

        g_store.current_fd = fd;
        g_store.current_size = sizeof mhdr;
    }

    uint8_t hdr_buf[sizeof(TtDeltaHeader)];
    size_t off = 0;

    for (int i = 0; i < 8; ++i)
        hdr_buf[off++] = (uint8_t)((hdr->timestamp_ns >> (8 * i)) & 0xFF);

    hdr_buf[off++] = hdr->event_type;

    for (int i = 0; i < 4; ++i)
        hdr_buf[off++] = (uint8_t)((path_len >> (8 * i)) & 0xFF);

    for (int i = 0; i < 4; ++i)
        hdr_buf[off++] = (uint8_t)((hdr->delta_size >> (8 * i)) & 0xFF);

    for (int i = 0; i < 8; ++i)
        hdr_buf[off++] = (uint8_t)((hdr->file_size >> (8 * i)) & 0xFF);

    if (write_all(g_store.current_fd, hdr_buf, sizeof hdr_buf) != 0)
        return -1;

    if (path_len > 0 &&
        write_all(g_store.current_fd, path, path_len) != 0)
        return -1;

    if (hdr->delta_size > 0 && delta_payload &&
        write_all(g_store.current_fd, delta_payload, hdr->delta_size) != 0)
        return -1;

    g_store.current_size += record_size;
    return 0;
}

typedef struct {
    char     dir_path[TT_PATH_MAX];
    int      file_fd;
    uint64_t file_size;
    uint64_t file_pos;
    uint64_t *ids;
    size_t   id_count, id_index, cap;
} TtStoreReader;

static TtStoreReader g_reader;

static int id_cmp(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;

    if (va < vb)
        return -1;
    if (va > vb)
        return 1;
    return 0;
}

int tt_store_reader_init(void)
{
    TtStoreReader *rd = &g_reader;

    memset(rd, 0, sizeof *rd);
    rd->file_fd = -1;

    if (!g_store.store_dir[0])
        return -1;

    snprintf(rd->dir_path, sizeof rd->dir_path, "%s", g_store.store_dir);

    DIR *d = opendir(rd->dir_path);
    if (!d)
        return -1;

    rd->cap = 16;
    rd->ids = malloc(rd->cap * sizeof(uint64_t));
    if (!rd->ids) {
        closedir(d);
        return -1;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t ln = strlen(de->d_name);
        if (ln <= 4 || strcmp(de->d_name + ln - 4, ".ttd") != 0)
            continue;

        char *end = NULL;
        unsigned long long v = strtoull(de->d_name, &end, 10);
        if (end == de->d_name || *end != '.')
            continue;

        if (rd->id_count == rd->cap) {
            rd->cap *= 2;
            uint64_t *ni = realloc(rd->ids, rd->cap * sizeof(uint64_t));
            if (!ni)
                break;
            rd->ids = ni;
        }

        rd->ids[rd->id_count++] = (uint64_t)v;
    }

    closedir(d);

    if (rd->id_count > 1)
        qsort(rd->ids, rd->id_count, sizeof(uint64_t), id_cmp);

    rd->id_index = 0;
    return 0;
}

static int reader_open_next_file(TtStoreReader *rd)
{
    if (rd->file_fd >= 0) {
        close(rd->file_fd);
        rd->file_fd = -1;
    }

    while (rd->id_index < rd->id_count) {
        char path[TT_PATH_MAX + 32];

        snprintf(path, sizeof path, "%s/%020llu.ttd",
                 rd->dir_path,
                 (unsigned long long)rd->ids[rd->id_index++]);

        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;

        struct stat st;
        if (fstat(fd, &st) != 0 ||
            st.st_size < (off_t)(TT_STORE_MAGIC_LEN + 4)) {
            close(fd);
            continue;
        }

        uint8_t hdr[TT_STORE_MAGIC_LEN + 4];
        if (read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr ||
            memcmp(hdr, TT_STORE_MAGIC, TT_STORE_MAGIC_LEN) != 0) {
            close(fd);
            continue;
        }

        rd->file_fd = fd;
        rd->file_size = (uint64_t)st.st_size;
        rd->file_pos = TT_STORE_MAGIC_LEN + 4;
        return 0;
    }

    return -1;
}

int tt_store_reader_next(TtDeltaHeader *out_hdr,
                         char *out_path,
                         size_t out_path_size,
                         uint8_t **out_payload,
                         size_t *out_payload_size)
{
    TtStoreReader *rd = &g_reader;

    if (out_payload)
        *out_payload = NULL;
    if (out_payload_size)
        *out_payload_size = 0;

    for (;;) {
        if (rd->file_fd < 0) {
            if (reader_open_next_file(rd) != 0)
                return 0;
        }

        if (rd->file_pos + sizeof(TtDeltaHeader) > rd->file_size) {
            close(rd->file_fd);
            rd->file_fd = -1;
            continue;
        }

        uint8_t hdr_buf[sizeof(TtDeltaHeader)];
        if (pread(rd->file_fd, hdr_buf, sizeof hdr_buf,
                  (off_t)rd->file_pos) != (ssize_t)sizeof hdr_buf) {
            close(rd->file_fd);
            rd->file_fd = -1;
            continue;
        }

        size_t off = 0;

        uint64_t ts = 0;
        for (int i = 0; i < 8; ++i)
            ts |= (uint64_t)hdr_buf[off++] << (8 * i);

        uint8_t ev = hdr_buf[off++];

        uint32_t pl = 0;
        for (int i = 0; i < 4; ++i)
            pl |= (uint32_t)hdr_buf[off++] << (8 * i);

        uint32_t ds = 0;
        for (int i = 0; i < 4; ++i)
            ds |= (uint32_t)hdr_buf[off++] << (8 * i);

        uint64_t fs = 0;
        for (int i = 0; i < 8; ++i)
            fs |= (uint64_t)hdr_buf[off++] << (8 * i);

        if (pl == 0 || pl >= out_path_size ||
            rd->file_pos + sizeof(hdr_buf) + pl + ds > rd->file_size) {
            close(rd->file_fd);
            rd->file_fd = -1;
            continue;
        }

        if (pread(rd->file_fd, out_path, pl,
                  (off_t)(rd->file_pos + sizeof(hdr_buf))) != (ssize_t)pl) {
            close(rd->file_fd);
            rd->file_fd = -1;
            continue;
        }

        out_path[pl] = '\0';

        if (out_payload && out_payload_size) {
            if (ds > 0) {
                uint8_t *buf = malloc(ds);
                if (!buf)
                    return -1;

                if (pread(rd->file_fd, buf, ds,
                          (off_t)(rd->file_pos + sizeof(hdr_buf) + pl)) != (ssize_t)ds) {
                    free(buf);
                    close(rd->file_fd);
                    rd->file_fd = -1;
                    continue;
                }

                *out_payload = buf;
                *out_payload_size = ds;
            }
        }

        if (out_hdr) {
            out_hdr->timestamp_ns = ts;
            out_hdr->event_type = ev;
            out_hdr->path_len = pl;
            out_hdr->delta_size = ds;
            out_hdr->file_size = fs;
        }

        rd->file_pos += sizeof(hdr_buf) + pl + ds;
        return 1;
    }
}

void tt_store_reader_free(void)
{
    TtStoreReader *rd = &g_reader;

    if (rd->file_fd >= 0)
        close(rd->file_fd);

    free(rd->ids);
    memset(rd, 0, sizeof *rd);
}

int tt_store_scan_stats(uint64_t *nrecords,
                        uint64_t *nbytes,
                        uint64_t *first_ts,
                        uint64_t *last_ts)
{
    uint64_t n = 0, b = 0, fts = 0, lts = 0;

    if (tt_store_reader_init() != 0)
        return -1;

    for (;;) {
        TtDeltaHeader hdr;
        char path[TT_PATH_MAX];
        uint8_t *pl = NULL;
        size_t plsz = 0;

        int rc = tt_store_reader_next(&hdr, path, sizeof path, &pl, &plsz);
        if (rc <= 0)
            break;

        n++;
        b += sizeof(TtDeltaHeader) + hdr.path_len + hdr.delta_size;

        if (fts == 0 || hdr.timestamp_ns < fts)
            fts = hdr.timestamp_ns;

        if (hdr.timestamp_ns > lts)
            lts = hdr.timestamp_ns;

        free(pl);
    }

    tt_store_reader_free();

    if (nrecords)
        *nrecords = n;
    if (nbytes)
        *nbytes = b;
    if (first_ts)
        *first_ts = fts;
    if (last_ts)
        *last_ts = lts;

    return 0;
}

static int store_find_latest(char *out, size_t outsz)
{
    DIR *d = opendir(g_store.store_dir);
    if (!d)
        return -1;

    uint64_t best = 0;
    int found = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t ln = strlen(de->d_name);
        if (ln <= 4 || strcmp(de->d_name + ln - 4, ".ttd") != 0)
            continue;

        char *end = NULL;
        unsigned long long v = strtoull(de->d_name, &end, 10);
        if (end == de->d_name || *end != '.')
            continue;

        if (!found || v > best) {
            best = v;
            found = 1;
        }
    }

    closedir(d);

    if (!found)
        return -1;

    snprintf(out, outsz, "%s/%020llu.ttd",
             g_store.store_dir,
             (unsigned long long)best);

    return 0;
}

static int store_try_continue(void)
{
    char fpath[TT_PATH_MAX + 32];

    if (store_find_latest(fpath, sizeof fpath) != 0)
        return -1;

    int fd = open(fpath, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0)
        return -1;

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0)
        goto fail;

    if ((uint64_t)st.st_size < (TT_STORE_MAGIC_LEN + 4))
        goto fail;

    if ((uint64_t)st.st_size >= TT_STORE_MAX_FILE_SIZE)
        goto fail;

    uint8_t mhdr[TT_STORE_MAGIC_LEN + 4];
    if (pread(fd, mhdr, sizeof mhdr, 0) != (ssize_t)sizeof mhdr)
        goto fail;

    if (memcmp(mhdr, TT_STORE_MAGIC, TT_STORE_MAGIC_LEN) != 0)
        goto fail;

    g_store.current_fd = fd;
    g_store.current_size = (uint64_t)st.st_size;
    return 0;

fail:
    close(fd);
    return -1;
}

int tt_store_init_writer(const char *store_dir, int continue_last)
{
    if (tt_store_init(store_dir) != 0)
        return -1;

    if (continue_last)
        store_try_continue();

    return 0;
}
