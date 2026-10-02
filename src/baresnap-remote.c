/*
 * BareSnap — bare-metal snapshot and backup system.
 * Copyright (C) 2026  John (johna124)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Source, issues, contact: https://github.com/johna124
 */
/* baresnap-remote.c — agente remoto con pwrite + ACK + fsync */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "brs_remote_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#define MAX_HANDLES 1024
#define UPLOAD_BUF_SIZE (4 * 1024 * 1024)

static int g_handles[MAX_HANDLES];
static char g_repo_path[4096];
static int g_no_sync = 0;

static void init_handles(void) {
    for (int i = 0; i < MAX_HANDLES; i++) g_handles[i] = -1;
}

static int allocate_handle(int fd) {
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (g_handles[i] == -1) { g_handles[i] = fd; return i; }
    }
    return -1;
}

static int get_handle(int idx) {
    if (idx < 0 || idx >= MAX_HANDLES) return -1;
    return g_handles[idx];
}

static void release_handle(int idx) {
    if (idx >= 0 && idx < MAX_HANDLES) {
        if (g_handles[idx] != -1) {
            if (!g_no_sync) fsync(g_handles[idx]);
            close(g_handles[idx]);
            g_handles[idx] = -1;
        }
    }
}

static int build_path(char *out, size_t out_size, const char *rel_path) {
    if (strstr(rel_path, "..") != NULL) return -1;
    if (rel_path[0] == '/') return -1;
    int n = snprintf(out, out_size, "%s/%s", g_repo_path, rel_path);
    if (n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}

static void handle_open(uint8_t flags, const void *payload, uint32_t len) {
    if (len < 2) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    const uint8_t *p = (const uint8_t *)payload;
    uint16_t path_len = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (len < (uint32_t)(2 + path_len)) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    char rel_path[4096];
    if (path_len >= sizeof(rel_path)) { brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "path too long"); return; }
    memcpy(rel_path, p + 2, path_len); rel_path[path_len] = '\0';
    char full_path[8192];
    if (build_path(full_path, sizeof(full_path), rel_path) != 0) {
        brs_proto_send_error(STDOUT_FILENO, EACCES, "unsafe path"); return;
    }
    int oflags = 0;
    if ((flags & BRS_OPEN_READ) && (flags & BRS_OPEN_WRITE)) oflags = O_RDWR;
    else if (flags & BRS_OPEN_WRITE) oflags = O_WRONLY;
    else oflags = O_RDONLY;
    if (flags & BRS_OPEN_APPEND) oflags |= O_APPEND;
    if (flags & BRS_OPEN_TRUNC)  oflags |= O_TRUNC;
    if (flags & BRS_OPEN_CREAT)  oflags |= O_CREAT;
    int fd = open(full_path, oflags, 0644);
    if (fd < 0) { brs_proto_send_error(STDOUT_FILENO, errno, strerror(errno)); return; }
    int handle = allocate_handle(fd);
    if (handle < 0) { close(fd); brs_proto_send_error(STDOUT_FILENO, EMFILE, "too many handles"); return; }
    brs_proto_send_handle(STDOUT_FILENO, (uint32_t)handle);
}

static void handle_close(const void *payload, uint32_t len) {
    if (len < 4) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    const uint8_t *p = (const uint8_t *)payload;
    uint32_t handle = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    release_handle((int)handle);
    brs_proto_send_ok(STDOUT_FILENO, 0);
}

static void handle_read(const void *payload, uint32_t len) {
    if (len < 16) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    const uint8_t *p = (const uint8_t *)payload;
    uint32_t handle = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint64_t offset = 0;
    for (int i = 0; i < 8; i++) offset |= ((uint64_t)p[4 + i]) << (i * 8);
    uint32_t read_len = (uint32_t)p[12] | ((uint32_t)p[13] << 8) | ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    int fd = get_handle((int)handle);
    if (fd < 0) { brs_proto_send_error(STDOUT_FILENO, EBADF, "bad handle"); return; }
    if (read_len > BRS_MSG_MAX_PAYLOAD) { brs_proto_send_error(STDOUT_FILENO, E2BIG, "read too large"); return; }
    if (read_len == 0) { brs_proto_send_data(STDOUT_FILENO, NULL, 0); return; }
    uint8_t *buf = (uint8_t *)malloc(read_len);
    if (!buf) { brs_proto_send_error(STDOUT_FILENO, ENOMEM, "out of memory"); return; }
    size_t off = 0;
    while (off < read_len) {
        ssize_t r = pread(fd, buf + off, read_len - off, (off_t)(offset + off));
        if (r < 0) { if (errno == EINTR) continue; free(buf); brs_proto_send_error(STDOUT_FILENO, errno, strerror(errno)); return; }
        if (r == 0) break;
        off += (size_t)r;
    }
    brs_proto_send_data(STDOUT_FILENO, buf, (uint32_t)off);
    free(buf);
}

static void handle_write(const void *payload, uint32_t len) {
    if (len < 12) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    const uint8_t *p = (const uint8_t *)payload;
    uint32_t handle = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint64_t offset = 0;
    for (int i = 0; i < 8; i++) offset |= ((uint64_t)p[4 + i]) << (i * 8);
    const uint8_t *data = p + 12;
    uint32_t data_len = len - 12;
    int fd = get_handle((int)handle);
    if (fd < 0) { brs_proto_send_error(STDOUT_FILENO, EBADF, "bad handle"); return; }
    size_t off = 0;
    while (off < data_len) {
        ssize_t w = pwrite(fd, data + off, data_len - off, (off_t)(offset + off));
        if (w < 0) { if (errno == EINTR) continue; brs_proto_send_error(STDOUT_FILENO, errno, strerror(errno)); return; }
        if (w == 0) { brs_proto_send_error(STDOUT_FILENO, EIO, "write failed"); return; }
        off += (size_t)w;
    }
    if (!g_no_sync) {
        if (fsync(fd) != 0) {
            brs_proto_send_error(STDOUT_FILENO, errno, strerror(errno));
            return;
        }
    }
    brs_proto_send_ok(STDOUT_FILENO, (int32_t)off);
}

static void handle_rename(const void *payload, uint32_t len) {
    if (len < 4) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    const uint8_t *p = (const uint8_t *)payload;
    uint16_t from_len = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (len < (uint32_t)(2 + from_len + 2)) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    char from[4096];
    if (from_len >= sizeof(from)) { brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "path too long"); return; }
    memcpy(from, p + 2, from_len); from[from_len] = '\0';
    uint16_t to_len = (uint16_t)p[2 + from_len] | ((uint16_t)p[2 + from_len + 1] << 8);
    if (len < (uint32_t)(2 + from_len + 2 + to_len)) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    char to[4096];
    if (to_len >= sizeof(to)) { brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "path too long"); return; }
    memcpy(to, p + 2 + from_len + 2, to_len); to[to_len] = '\0';
    char full_from[8192], full_to[8192];
    if (build_path(full_from, sizeof(full_from), from) != 0 || build_path(full_to, sizeof(full_to), to) != 0) {
        brs_proto_send_error(STDOUT_FILENO, EACCES, "unsafe path"); return;
    }
    if (rename(full_from, full_to) != 0) { brs_proto_send_error(STDOUT_FILENO, errno, strerror(errno)); return; }
    brs_proto_send_ok(STDOUT_FILENO, 0);
}

static void handle_unlink(const void *payload, uint32_t len) {
    if (len < 2) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    const uint8_t *p = (const uint8_t *)payload;
    uint16_t path_len = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (len < (uint32_t)(2 + path_len)) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    char rel_path[4096];
    if (path_len >= sizeof(rel_path)) { brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "path too long"); return; }
    memcpy(rel_path, p + 2, path_len); rel_path[path_len] = '\0';
    char full_path[8192];
    if (build_path(full_path, sizeof(full_path), rel_path) != 0) {
        brs_proto_send_error(STDOUT_FILENO, EACCES, "unsafe path"); return;
    }
    if (unlink(full_path) != 0) {
        if (errno == ENOENT) { brs_proto_send_ok(STDOUT_FILENO, 0); return; }
        brs_proto_send_error(STDOUT_FILENO, errno, strerror(errno));
        return;
    }
    brs_proto_send_ok(STDOUT_FILENO, 0);
}

static void handle_list(const void *payload, uint32_t len)
{
    if (len < 2) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    const uint8_t *p = (const uint8_t *)payload;
    uint16_t path_len = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (len < (uint32_t)(2 + path_len)) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    char rel_path[4096];
    if (path_len >= sizeof(rel_path)) { brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "path too long"); return; }
    memcpy(rel_path, p + 2, path_len); rel_path[path_len] = '\0';
    char full_path[8192];
    if (build_path(full_path, sizeof(full_path), rel_path) != 0) {
        brs_proto_send_error(STDOUT_FILENO, EACCES, "unsafe path"); return;
    }
    DIR *dir = opendir(full_path);
    if (!dir) { brs_proto_send_error(STDOUT_FILENO, errno, strerror(errno)); return; }
    struct dirent *ent;
    size_t count = 0;
    size_t total_size = 4;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        count++;
        total_size += 2 + strlen(ent->d_name) + 4 + 8;
    }
    uint8_t *buf = (uint8_t *)malloc(total_size);
    if (!buf) { closedir(dir); brs_proto_send_error(STDOUT_FILENO, ENOMEM, "out of memory"); return; }
    size_t off = 0;
    buf[off++] = (uint8_t)(count & 0xFF);
    buf[off++] = (uint8_t)((count >> 8) & 0xFF);
    buf[off++] = (uint8_t)((count >> 16) & 0xFF);
    buf[off++] = (uint8_t)((count >> 24) & 0xFF);
    rewinddir(dir);
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        size_t name_len = strlen(ent->d_name);
        buf[off++] = (uint8_t)(name_len & 0xFF);
        buf[off++] = (uint8_t)((name_len >> 8) & 0xFF);
        memcpy(buf + off, ent->d_name, name_len);
        off += name_len;
        char ent_path[8192];
        snprintf(ent_path, sizeof(ent_path), "%s/%s", full_path, ent->d_name);
        struct stat st;
        uint32_t mode = 0;
        uint64_t size = 0;
        if (stat(ent_path, &st) == 0) {
            mode = (uint32_t)st.st_mode;
            size = (uint64_t)st.st_size;
        }
        for (int i = 0; i < 4; i++) buf[off++] = (uint8_t)((mode >> (i * 8)) & 0xFF);
        for (int i = 0; i < 8; i++) buf[off++] = (uint8_t)((size >> (i * 8)) & 0xFF);
    }
    closedir(dir);
    brs_proto_send_msg(STDOUT_FILENO, BRS_RSP_ENTRIES, 0, buf, (uint32_t)off);
    free(buf);
}

static void handle_stat(const void *payload, uint32_t len) {
    if (len < 2) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    const uint8_t *p = (const uint8_t *)payload;
    uint16_t path_len = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (len < (uint32_t)(2 + path_len)) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    char rel_path[4096];
    if (path_len >= sizeof(rel_path)) { brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "path too long"); return; }
    memcpy(rel_path, p + 2, path_len); rel_path[path_len] = '\0';
    char full_path[8192];
    if (build_path(full_path, sizeof(full_path), rel_path) != 0) {
        brs_proto_send_error(STDOUT_FILENO, EACCES, "unsafe path"); return;
    }
    struct stat st;
    if (stat(full_path, &st) != 0) { brs_proto_send_error(STDOUT_FILENO, errno, strerror(errno)); return; }
    uint8_t buf[12];
    uint64_t size = (uint64_t)st.st_size;
    uint32_t mode = (uint32_t)st.st_mode;
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)((size >> (i * 8)) & 0xFF);
    for (int i = 0; i < 4; i++) buf[8 + i] = (uint8_t)((mode >> (i * 8)) & 0xFF);
    brs_proto_send_msg(STDOUT_FILENO, BRS_RSP_STAT, 0, buf, 12);
}

static void handle_hardlink(const void *payload, uint32_t len) {
    if (len < 4) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    const uint8_t *p = (const uint8_t *)payload;
    uint16_t existing_len = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (len < (uint32_t)(2 + existing_len + 2)) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    char existing[4096];
    if (existing_len >= sizeof(existing)) { brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "path too long"); return; }
    memcpy(existing, p + 2, existing_len); existing[existing_len] = '\0';
    uint16_t newpath_len = (uint16_t)p[2 + existing_len] | ((uint16_t)p[2 + existing_len + 1] << 8);
    if (len < (uint32_t)(2 + existing_len + 2 + newpath_len)) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    char newpath[4096];
    if (newpath_len >= sizeof(newpath)) { brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "path too long"); return; }
    memcpy(newpath, p + 2 + existing_len + 2, newpath_len); newpath[newpath_len] = '\0';
    char full_existing[8192], full_newpath[8192];
    if (build_path(full_existing, sizeof(full_existing), existing) != 0 || build_path(full_newpath, sizeof(full_newpath), newpath) != 0) {
        brs_proto_send_error(STDOUT_FILENO, EACCES, "unsafe path"); return;
    }
    if (link(full_existing, full_newpath) != 0) { brs_proto_send_error(STDOUT_FILENO, errno, strerror(errno)); return; }
    brs_proto_send_ok(STDOUT_FILENO, 0);
}

static void handle_mkdir(const void *payload, uint32_t len) {
    if (len < 6) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    const uint8_t *p = (const uint8_t *)payload;
    uint16_t path_len = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (len < (uint32_t)(2 + path_len + 4)) { brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload"); return; }
    char rel_path[4096];
    if (path_len >= sizeof(rel_path)) { brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "path too long"); return; }
    memcpy(rel_path, p + 2, path_len); rel_path[path_len] = '\0';
    uint32_t mode = (uint32_t)p[2 + path_len] | ((uint32_t)p[2 + path_len + 1] << 8) | ((uint32_t)p[2 + path_len + 2] << 16) | ((uint32_t)p[2 + path_len + 3] << 24);
    char full_path[8192];
    if (build_path(full_path, sizeof(full_path), rel_path) != 0) {
        brs_proto_send_error(STDOUT_FILENO, EACCES, "unsafe path"); return;
    }
    if (mkdir(full_path, (mode_t)mode) != 0) {
        if (errno == EEXIST) {
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                brs_proto_send_ok(STDOUT_FILENO, 0); return;
            }
        }
        brs_proto_send_error(STDOUT_FILENO, errno, strerror(errno)); return;
    }
    brs_proto_send_ok(STDOUT_FILENO, 0);
}

static int parse_idx_seg_id(const char *name, unsigned long long *out)
{
    if (!name || !out) return -1;
    size_t len = strlen(name);
    if (len <= 4 || strcmp(name + len - 4, ".idx") != 0) return -1;
    if (name[0] < '0' || name[0] > '9') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(name, &end, 10);
    if (errno != 0 || end == name || strcmp(end, ".idx") != 0) return -1;
    *out = v;
    return 0;
}

static void handle_sync_index(const void *payload, uint32_t len)
{
    (void)payload;
    (void)len;

    char idx_dir[sizeof(g_repo_path) + 8];
    int n = snprintf(idx_dir, sizeof(idx_dir), "%s/index", g_repo_path);
    if (n < 0 || (size_t)n >= sizeof(idx_dir)) {
        brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "repo path too long");
        return;
    }

    DIR *dir = opendir(idx_dir);
    if (!dir) {
        brs_proto_send_error(STDOUT_FILENO, errno, "no index dir");
        return;
    }

    struct dirent *ent;
    size_t total_size = 4;
    uint32_t seg_count = 0;

    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen <= 4 || strcmp(ent->d_name + nlen - 4, ".idx") != 0) continue;
        errno = 0;
        char *end = NULL;
        unsigned long long seg_id_check = strtoull(ent->d_name, &end, 10);
        (void)seg_id_check;
        if (errno != 0 || end == ent->d_name || strcmp(end, ".idx") != 0) continue;
        char full[sizeof(idx_dir) + 256 + 2];
        int fn = snprintf(full, sizeof(full), "%s/%s", idx_dir, ent->d_name);
        if (fn < 0 || (size_t)fn >= sizeof(full)) {
            closedir(dir);
            brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "index path too long");
            return;
        }
        struct stat st;
        if (stat(full, &st) != 0 || st.st_size < 0) continue;
        size_t add = 8 + 4 + (size_t)st.st_size;
        if (add > BRS_MSG_MAX_PAYLOAD || total_size > BRS_MSG_MAX_PAYLOAD - add) {
            closedir(dir);
            brs_proto_send_error(STDOUT_FILENO, E2BIG, "index dump too large");
            return;
        }
        total_size += add;
        seg_count++;
    }

    uint8_t *buf = (uint8_t *)malloc(total_size);
    if (!buf) {
        closedir(dir);
        brs_proto_send_error(STDOUT_FILENO, ENOMEM, "oom");
        return;
    }

    buf[0] = (uint8_t)(seg_count & 0xFF);
    buf[1] = (uint8_t)((seg_count >> 8) & 0xFF);
    buf[2] = (uint8_t)((seg_count >> 16) & 0xFF);
    buf[3] = (uint8_t)((seg_count >> 24) & 0xFF);

    rewinddir(dir);
    size_t off = 4;

    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen <= 4 || strcmp(ent->d_name + nlen - 4, ".idx") != 0) continue;
        errno = 0;
        char *end = NULL;
        unsigned long long seg_id = strtoull(ent->d_name, &end, 10);
        if (errno != 0 || end == ent->d_name || strcmp(end, ".idx") != 0) continue;
        char full_path[sizeof(idx_dir) + 256 + 2];
        int fn = snprintf(full_path, sizeof(full_path), "%s/%s", idx_dir, ent->d_name);
        if (fn < 0 || (size_t)fn >= sizeof(full_path)) {
            free(buf);
            closedir(dir);
            brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "index path too long");
            return;
        }
        FILE *f = fopen(full_path, "rb");
        if (!f) {
            free(buf);
            closedir(dir);
            brs_proto_send_error(STDOUT_FILENO, errno, "cannot open index");
            return;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            free(buf);
            closedir(dir);
            brs_proto_send_error(STDOUT_FILENO, errno, "cannot seek index");
            return;
        }
        long fsize = ftell(f);
        if (fsize < 0) {
            fclose(f);
            free(buf);
            closedir(dir);
            brs_proto_send_error(STDOUT_FILENO, errno, "cannot tell index size");
            return;
        }
        if (fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            free(buf);
            closedir(dir);
            brs_proto_send_error(STDOUT_FILENO, errno, "cannot rewind index");
            return;
        }
        size_t need = 8 + 4 + (size_t)fsize;
        if (off + need > total_size) {
            fclose(f);
            free(buf);
            closedir(dir);
            brs_proto_send_error(STDOUT_FILENO, E2BIG, "index dump overflow");
            return;
        }
        for (int i = 0; i < 8; i++) buf[off++] = (uint8_t)((seg_id >> (i * 8)) & 0xFF);
        for (int i = 0; i < 4; i++) buf[off++] = (uint8_t)(((uint32_t)fsize >> (i * 8)) & 0xFF);
        size_t rd = fread(buf + off, 1, (size_t)fsize, f);
        if (rd != (size_t)fsize) {
            fclose(f);
            free(buf);
            closedir(dir);
            brs_proto_send_error(STDOUT_FILENO, EIO, "short index read");
            return;
        }
        off += rd;
        fclose(f);
    }
    closedir(dir);
    brs_proto_send_msg(STDOUT_FILENO, BRS_RSP_INDEX_DUMP, 0, buf, (uint32_t)off);
    free(buf);
}

static void handle_upload_pack(const void *payload, uint32_t len) {
    int fd = -1;
    char rel_path[4096];
    char full_path[8192];
    int file_created = 0;
    uint64_t total_received = 0;
    int upload_done = 0;

    full_path[0] = '\0';
    rel_path[0] = '\0';

    if (payload == NULL || len < 2) {
        brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload");
        return;
    }

    const uint8_t *p = (const uint8_t *)payload;
    uint16_t name_len = (uint16_t)p[0] | ((uint16_t)p[1] << 8);

    if ((uint64_t)len < (uint64_t)(2 + name_len)) {
        brs_proto_send_error(STDOUT_FILENO, EINVAL, "bad payload");
        return;
    }

    if (name_len >= sizeof(rel_path)) {
        brs_proto_send_error(STDOUT_FILENO, ENAMETOOLONG, "path too long");
        return;
    }

    memcpy(rel_path, p + 2, name_len);
    rel_path[name_len] = '\0';

    if (build_path(full_path, sizeof(full_path), rel_path) != 0) {
        brs_proto_send_error(STDOUT_FILENO, EACCES, "unsafe path");
        return;
    }

    fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        brs_proto_send_error(STDOUT_FILENO, errno, strerror(errno));
        return;
    }
    file_created = 1;

    while (!upload_done) {
        uint8_t type, flags;
        void *chunk_payload = NULL;
        uint32_t chunk_len = 0;

        if (brs_proto_recv_msg(STDIN_FILENO, &type, &flags,
                               &chunk_payload, &chunk_len) != 0) {
            goto upload_fail;
        }

        if (type == BRS_MSG_DATA) {
            size_t woff = 0;
            int write_error = 0;
            uint64_t abs_off = total_received;

            while (woff < (size_t)chunk_len) {
                ssize_t w = pwrite(fd,
                                   (uint8_t *)chunk_payload + woff,
                                   (size_t)chunk_len - woff,
                                   (off_t)(abs_off + woff));
                if (w < 0) {
                    if (errno == EINTR)
                        continue;
                    write_error = 1;
                    break;
                }
                if (w == 0) {
                    write_error = 1;
                    break;
                }
                woff += (size_t)w;
            }

            if (write_error || woff < (size_t)chunk_len) {
                free(chunk_payload);
                goto upload_fail;
            }

            total_received += (uint64_t)chunk_len;

            free(chunk_payload);

            /* ACK al cliente */
            brs_proto_send_ok(STDOUT_FILENO, 0);

        } else if (type == BRS_MSG_DONE) {
            upload_done = 1;
            free(chunk_payload);

        } else {
            free(chunk_payload);
            goto upload_fail;
        }
    }

    if (!g_no_sync) {
        if (fsync(fd) != 0) {
            close(fd);
            goto upload_fail_unlink;
        }
    }

    if (close(fd) != 0) {
        fd = -1;
        goto upload_fail_unlink;
    }
    fd = -1;

    brs_proto_send_ok(STDOUT_FILENO, (int32_t)total_received);
    return;

upload_fail:
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }

upload_fail_unlink:
    if (file_created && full_path[0] != '\0')
        unlink(full_path);

    brs_proto_send_error(STDOUT_FILENO, EIO, "upload interrupted");
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--test") == 0) {
        fprintf(stderr, "OK\n");
        fflush(stderr);
        return 0;
    }

    if (argc < 3 || strcmp(argv[1], "--repo") != 0) {
        fprintf(stderr, "Usage: baresnap-remote --repo /path/to/repo\n");
        fprintf(stderr, "       baresnap-remote --test\n");
        return 1;
    }

    strncpy(g_repo_path, argv[2], sizeof(g_repo_path) - 1);
    g_repo_path[sizeof(g_repo_path) - 1] = '\0';

    const char *no_sync = getenv("BRS_REMOTE_NO_SYNC");
    if (no_sync && no_sync[0] != '\0' && strcmp(no_sync, "0") != 0) {
        g_no_sync = 1;
    }

    init_handles();

    for (;;) {
        uint8_t type, flags;
        void *payload = NULL;
        uint32_t length = 0;
        if (brs_proto_recv_msg(STDIN_FILENO, &type, &flags, &payload, &length) != 0)
            break;

        switch (type) {
        case BRS_MSG_OPEN:        handle_open(flags, payload, length); break;
        case BRS_MSG_CLOSE:       handle_close(payload, length); break;
        case BRS_MSG_READ:        handle_read(payload, length); break;
        case BRS_MSG_WRITE:       handle_write(payload, length); break;
        case BRS_MSG_RENAME:      handle_rename(payload, length); break;
        case BRS_MSG_UNLINK:      handle_unlink(payload, length); break;
        case BRS_MSG_LIST:        handle_list(payload, length); break;
        case BRS_MSG_STAT:        handle_stat(payload, length); break;
        case BRS_MSG_HARDLINK:    handle_hardlink(payload, length); break;
        case BRS_MSG_MKDIR:       handle_mkdir(payload, length); break;
        case BRS_MSG_SYNC_INDEX:  handle_sync_index(payload, length); break;
        case BRS_MSG_UPLOAD_PACK: handle_upload_pack(payload, length); break;
        case BRS_MSG_BYE:
            brs_proto_send_ok(STDOUT_FILENO, 0);
            free(payload);
            return 0;
        default:
            brs_proto_send_error(STDOUT_FILENO, EINVAL, "unknown message type");
            break;
        }
        free(payload);
    }
    return 0;
}
