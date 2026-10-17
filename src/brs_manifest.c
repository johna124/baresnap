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
/* brs_manifest.c — escaneo + escritura/parseo de snapshots */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "brs_manifest.h"
#include "brs_buffer.h"
#include "brs_crypto.h"
#include "brs_fsutil.h"
#include "brs_hash.h"
#include "brs_util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ============================================================================
 * Escaneo recursivo
 * ==========================================================================*/

static BrsManifestEntry *manifest_list_push(BrsManifestList *l)
{
    if (l->count == l->cap) {
        uint64_t ncap = l->cap ? l->cap * 2 : 64;
        if (ncap < l->cap) return NULL;

        BrsManifestEntry *p =
            (BrsManifestEntry *)realloc(l->items, (size_t)ncap * sizeof *p);
        if (!p) return NULL;

        l->items = p;
        l->cap = ncap;
    }

    BrsManifestEntry *e = &l->items[l->count++];
    brs_manifest_entry_init(e);
    return e;
}

void brs_manifest_list_free(BrsManifestList *l)
{
    if (!l) return;

    for (uint64_t i = 0; i < l->count; ++i)
        brs_manifest_entry_free(&l->items[i]);

    free(l->items);
    memset(l, 0, sizeof *l);
}

static int scan_push_entry(BrsManifestList *out, const char *rel,
                           const struct stat *st, const char *full)
{
    BrsManifestEntry *e = manifest_list_push(out);
    if (!e) return -1;

    if (brs_manifest_entry_set_path(e, rel, strlen(rel)) != 0) return -1;

    e->mode = st->st_mode;
    e->uid = st->st_uid;
    e->gid = st->st_gid;
    e->mtime_ns = brs_timespec_to_ns(&st->st_mtim);
    e->ctime_ns = brs_timespec_to_ns(&st->st_ctim);
    e->dev = (uint64_t)st->st_dev;
    e->ino = (uint64_t)st->st_ino;

    if (S_ISLNK(st->st_mode)) {
        e->type = BRS_FILETYPE_SYMLINK;
        char buf[BRS_PATH_MAX];
        ssize_t n = readlink(full, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = '\0';
            (void)brs_manifest_entry_set_symlink_target(e, buf, (size_t)n);
            e->size = (uint64_t)n;
        } else {
            /* ESCUDO: Si readlink falla o devuelve vacío, inyectamos un punto */
            (void)brs_manifest_entry_set_symlink_target(e, ".", 1);
            e->size = 1;
        }
    } else if (S_ISDIR(st->st_mode)) {
        e->type = BRS_FILETYPE_DIR;
    } else if (S_ISREG(st->st_mode)) {
        e->type = BRS_FILETYPE_FILE;
        e->size = (uint64_t)st->st_size;
    } else {
        e->type = BRS_FILETYPE_OTHER;
    }

    return 0;
}


static int scan_dir_contents(const char *dir_full, const char *rel_prefix,
                             BrsManifestList *out)
{
    DIR *d = opendir(dir_full);
    if (!d) {
        fprintf(stderr, "scan warning: cannot open dir: %s (%s)\n",
                dir_full, strerror(errno));
        return 0;
    }

    struct dirent *de;

    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char full[BRS_PATH_MAX], rel[BRS_PATH_MAX];
        int n1 = snprintf(full, sizeof full, "%s/%s", dir_full, de->d_name);
        int n2 = snprintf(rel, sizeof rel, "%s/%s", rel_prefix, de->d_name);
        if (n1 < 0 || (size_t)n1 >= sizeof full) continue;
        if (n2 < 0 || (size_t)n2 >= sizeof rel) continue;

        struct stat st;
        if (lstat(full, &st) != 0) continue;

        if (scan_push_entry(out, rel, &st, full) != 0) {
            closedir(d);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            if (scan_dir_contents(full, rel, out) != 0) {
                closedir(d);
                return -1;
            }
        }
    }

    closedir(d);
    return 0;
}

static int entry_path_cmp(const void *a, const void *b)
{
    const BrsManifestEntry *ea = (const BrsManifestEntry *)a;
    const BrsManifestEntry *eb = (const BrsManifestEntry *)b;
    return strcmp(ea->path, eb->path);
}

int brs_scan_source(const char *source, BrsManifestList *out)
{
    if (!source || !out) return -1;

    char clean[BRS_PATH_MAX];

    size_t len = strlen(source);
    if (len == 0 || len >= sizeof clean) return -1;
    memcpy(clean, source, len + 1);

    while (len > 1 && clean[len - 1] == '/') clean[--len] = '\0';

    struct stat st;
    if (lstat(clean, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "cannot open source directory: %s\n", clean);
        return -1;
    }

    const char *base = strrchr(clean, '/');
    base = base ? base + 1 : clean;
    if (base[0] == '\0') return -1;

    if (scan_dir_contents(clean, base, out) != 0) {
        brs_manifest_list_free(out);
        return -1;
    }

    if (out->count > 1)
        qsort(out->items, (size_t)out->count, sizeof *out->items,
              entry_path_cmp);

    return 0;
}

/* ============================================================================
 * Nombre de snapshot
 * ==========================================================================*/

static void sanitize_label(const char *label, char *out, size_t out_size)
{
    size_t o = 0;

    for (size_t i = 0; label[i] != '\0' && o + 1 < out_size; ++i) {
        unsigned char c = (unsigned char)label[i];
        out[o++] = (isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
    }

    out[o] = '\0';
}

int brs_make_snapshot_name(char *out, size_t out_size, uint64_t created_ns,
                           const uint8_t snapshot_id[BRS_UUID_LEN],
                           const char *label)
{
    if (!out || out_size == 0 || !snapshot_id) return -1;

    time_t secs = (time_t)(created_ns / 1000000000ULL);

    struct tm tm_buf;
    char ts[64];

    if (!localtime_r(&secs, &tm_buf)) return -1;

    size_t ts_len = strftime(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S", &tm_buf);
    if (ts_len == 0) return -1;

    /* FIX: milisegundos para orden cronológico determinista */
    uint64_t ms = (created_ns / 1000000ULL) % 1000;
    snprintf(ts + ts_len, sizeof(ts) - ts_len, "-%03llu", (unsigned long long)ms);

    char id_hex[BRS_UUID_LEN * 2 + 1];
    brs_to_hex(snapshot_id, BRS_UUID_LEN, id_hex);
    id_hex[8] = '\0';

    int n;

    if (label && label[0] != '\0') {
        char clean_label[256];
        sanitize_label(label, clean_label, sizeof clean_label);
        n = snprintf(out, out_size, "%s_%s_%s.snap", clean_label, ts, id_hex);
    } else {
        n = snprintf(out, out_size, "%s_%s.snap", ts, id_hex);
    }

    if (n < 0 || (size_t)n >= out_size) return -1;

    return 0;
}
/* ============================================================================
 * Escritura del manifiesto
 * ==========================================================================*/

int brs_write_snapshot_manifest(const char *repo_path,
                                const BrsRepoConfig *cfg,
                                const char *source_path,
                                const BrsManifestEntry *entries,
                                uint64_t entry_count,
                                const BrsSecureKey *crypto_key,
                                const char *label,
                                char *out_path, size_t out_path_size)
{
    if (!repo_path || !cfg || (!entries && entry_count > 0) ||
        !out_path || out_path_size == 0)
        return -1;

    BrsBuffer buf, final_buf, enc;
    brs_buffer_init(&buf);
    brs_buffer_init(&final_buf);
    brs_buffer_init(&enc);

    BrsSecureKey derived;
    int derived_valid = 0;

    uint8_t snapshot_id[BRS_UUID_LEN];
    uint64_t created_ns = 0;
    int rc = -1;

    do {
        if (brs_make_uuid(snapshot_id) != 0) break;

        created_ns = brs_now_ns();

        static const uint8_t zeros16[BRS_UUID_LEN] = {0};

        char host[BRS_HOSTNAME_MAX];
        if (brs_get_hostname(host, sizeof host) != 0) break;

        const char *root = source_path ? source_path : "";
        size_t root_len = strlen(root);

        uint64_t file_count = 0, dir_count = 0, symlink_count = 0;
        uint64_t logical_bytes = 0, chunk_refs = 0;

        for (uint64_t i = 0; i < entry_count; ++i) {
            const BrsManifestEntry *e = &entries[i];
            if (e->type == BRS_FILETYPE_FILE) {
                file_count++;
                logical_bytes += e->size;
                chunk_refs += e->chunk_count;
            } else if (e->type == BRS_FILETYPE_DIR) {
                dir_count++;
            } else if (e->type == BRS_FILETYPE_SYMLINK) {
                symlink_count++;
            }
        }

        int ok =
            brs_buffer_append(&buf, BRS_MAGIC_SNAPSHOT, BRS_MAGIC_LEN) == 0 &&
            brs_buffer_append_u32_le(&buf, BRS_FORMAT_VERSION) == 0 &&
            brs_buffer_append(&buf, snapshot_id, BRS_UUID_LEN) == 0 &&
            brs_buffer_append(&buf, cfg->uuid, BRS_UUID_LEN) == 0 &&
            brs_buffer_append(&buf, zeros16, sizeof zeros16) == 0 &&
            brs_buffer_append_u64_le(&buf, created_ns) == 0 &&
            brs_buffer_append_u32_le(&buf, (uint32_t)strlen(host)) == 0 &&
            brs_buffer_append_str(&buf, host) == 0 &&
            brs_buffer_append_u32_le(&buf, (uint32_t)root_len) == 0 &&
            brs_buffer_append(&buf, root, root_len) == 0 &&
            brs_buffer_append_u64_le(&buf, entry_count) == 0 &&
            brs_buffer_append_u64_le(&buf, file_count) == 0 &&
            brs_buffer_append_u64_le(&buf, dir_count) == 0 &&
            brs_buffer_append_u64_le(&buf, symlink_count) == 0 &&
            brs_buffer_append_u64_le(&buf, logical_bytes) == 0 &&
            brs_buffer_append_u64_le(&buf, chunk_refs) == 0;

        for (uint64_t i = 0; ok && i < entry_count; ++i) {
            const BrsManifestEntry *e = &entries[i];
            size_t plen = e->path ? strlen(e->path) : 0;

            ok = brs_buffer_append_u32_le(&buf, (uint32_t)plen) == 0 &&
                 brs_buffer_append(&buf, e->path, plen) == 0 &&
                 brs_buffer_append_u8(&buf, e->type) == 0 &&
                 brs_buffer_append_u16_le(&buf, e->flags) == 0 &&
                 brs_buffer_append_u32_le(&buf, e->mode) == 0 &&
                 brs_buffer_append_u32_le(&buf, e->uid) == 0 &&
                 brs_buffer_append_u32_le(&buf, e->gid) == 0 &&
                 brs_buffer_append_u64_le(&buf, e->size) == 0 &&
                 brs_buffer_append_u64_le(&buf, e->mtime_ns) == 0 &&
                 brs_buffer_append_u64_le(&buf, e->ctime_ns) == 0;

            if (!ok) break;

            if (e->type == BRS_FILETYPE_SYMLINK) {
                size_t tlen = e->symlink_target ? strlen(e->symlink_target) : 0;
                ok = brs_buffer_append_u32_le(&buf, (uint32_t)tlen) == 0 &&
                     brs_buffer_append(&buf, e->symlink_target, tlen) == 0;
            } else if (e->type == BRS_FILETYPE_FILE &&
                       (e->flags & BRS_FLAG_HARDLINK)) {
                size_t hlen = e->hardlink_to ? strlen(e->hardlink_to) : 0;
                ok = brs_buffer_append_u32_le(&buf, (uint32_t)hlen) == 0 &&
                     brs_buffer_append(&buf, e->hardlink_to, hlen) == 0;
            } else if (e->type == BRS_FILETYPE_FILE) {
                if (e->flags & BRS_FLAG_DELTA) {
                    ok = brs_buffer_append_u32_le(&buf, e->delta_source_count) == 0;
                    for (uint32_t c = 0; ok && c < e->delta_source_count; ++c) {
                        ok = brs_buffer_append(&buf, e->delta_source_chunks[c].bytes,
                                               BRS_CHUNK_ID_LEN) == 0;
                    }
                }

                if (ok) ok = brs_buffer_append_u32_le(&buf, e->chunk_count) == 0;

                for (uint32_t c = 0; ok && c < e->chunk_count; ++c) {
                    ok = brs_buffer_append(&buf, e->chunks[c].bytes,
                                           BRS_CHUNK_ID_LEN) == 0;
                }
            }
        }

        if (!ok) break;

        uint64_t csum = brs_fnv1a_64(buf.data, buf.size);
        if (brs_buffer_append_u64_le(&buf, csum) != 0) break;

        char name[256];
        if (brs_make_snapshot_name(name, sizeof name, created_ns,
                                   snapshot_id, label) != 0)
            break;

        char snaps_dir[BRS_PATH_MAX];
        if (brs_path_join(snaps_dir, sizeof snaps_dir, repo_path,
                          "snapshots") != 0)
            break;

        if (brs_path_join(out_path, out_path_size, snaps_dir, name) != 0)
            break;

        if (cfg->encrypted) {
            const BrsSecureKey *key = crypto_key;

            if (!key) {
                char pass[BRS_PASSPHRASE_MAX + 1];
                if (brs_read_passphrase(pass, sizeof pass,
                                        "Passphrase: ") != 0)
                    break;

                if (brs_derive_key(cfg, pass, &derived) != 0) {
                    brs_secure_wipe(pass, sizeof pass);
                    fprintf(stderr, "error: incorrect passphrase\n");
                    break;
                }

                brs_secure_wipe(pass, sizeof pass);
                key = &derived;
                derived_valid = 1;
            }

            int eok =
                brs_encrypt_buffer(key, cfg->cipher_algo, buf.data, buf.size, &enc) == 0 &&
                brs_buffer_append(&final_buf, BRS_MAGIC_SNAPSHOT_ENC,
                                  BRS_MAGIC_LEN) == 0 &&
                brs_buffer_append(&final_buf, enc.data, enc.size) == 0;

            if (!eok) break;

            rc = brs_write_file_atomic(out_path, final_buf.data, final_buf.size);
        } else {
            rc = brs_write_file_atomic(out_path, buf.data, buf.size);
        }
    } while (0);

    if (derived_valid) brs_secure_key_wipe(&derived);

    brs_buffer_free(&buf);
    brs_buffer_free(&final_buf);
    brs_buffer_wipe_free(&enc);

    return rc;
}

/* ============================================================================
 * Parseo de snapshots
 * ==========================================================================*/

static int reader_read_string(BrsReader *r, char **out)
{
    uint32_t len;
    const uint8_t *p;

    if (brs_reader_u32_le(r, &len) != 0) return -1;
    if (len > 65536) return -1;
    if (brs_reader_bytes(r, len, &p) != 0) return -1;

    char *s = (char *)malloc((size_t)len + 1);
    if (!s) return -1;

    if (len > 0) memcpy(s, p, len);
    s[len] = '\0';

    free(*out);
    *out = s;

    return 0;
}

int brs_parse_snapshot(const uint8_t *data, size_t size,
                       BrsParsedSnapshot *snap,
                       const BrsSecureKey *crypto_key,
                       int cipher_algo)
{
    BrsBuffer dec;
    int dec_valid = 0;

    BrsReader r;
    uint32_t version;
    const uint8_t *tmp;

    if (!data || !snap) {
        fprintf(stderr, "PARSE_FAIL: null data/snap\n");
        return -1;
    }

    brs_buffer_init(&dec);

    if (size < BRS_MAGIC_LEN) {
        fprintf(stderr, "PARSE_FAIL: size %zu < 8\n", size);

    }

    if (memcmp(data, BRS_MAGIC_SNAPSHOT_ENC, BRS_MAGIC_LEN) == 0) {
        int rc;

        if (!crypto_key) {
            fprintf(stderr, "error: snapshot is encrypted but no key provided\n");
            goto parse_error;
        }

        if (brs_decrypt_buffer(crypto_key, (BrsCipherAlgo)cipher_algo,
                               data + BRS_MAGIC_LEN, size - BRS_MAGIC_LEN,
                               &dec) != 0) {
            fprintf(stderr, "PARSE_FAIL: cannot decrypt\n");
            goto parse_error;
        }

        dec_valid = 1;

        rc = brs_parse_snapshot(dec.data, dec.size, snap, NULL, cipher_algo);

        /* FIX M1: el plaintext descifrado se limpia SIEMPRE, también aquí. */
        brs_buffer_wipe_free(&dec);

        return rc; /* la llamada anidada ya saneó snap si falló */
    }

    if (memcmp(data, BRS_MAGIC_SNAPSHOT, BRS_MAGIC_LEN) != 0) {
        fprintf(stderr, "PARSE_FAIL: bad plain magic %02x %02x %02x %02x %02x %02x %02x %02x\n",
                data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
        goto parse_error;
    }

    brs_reader_init(&r, data, size);

    if (brs_reader_skip(&r, BRS_MAGIC_LEN) != 0) { fprintf(stderr, "PARSE_FAIL: skip magic\n"); goto parse_error; }
    if (brs_reader_u32_le(&r, &version) != 0) { fprintf(stderr, "PARSE_FAIL: read version\n"); goto parse_error; }
    if (version != BRS_FORMAT_VERSION) { fprintf(stderr, "PARSE_FAIL: version %u != %u\n", version, BRS_FORMAT_VERSION); goto parse_error; }

    if (brs_reader_bytes(&r, BRS_UUID_LEN, &tmp) != 0) { fprintf(stderr, "PARSE_FAIL: snapshot_id\n"); goto parse_error; }
    memcpy(snap->snapshot_id, tmp, BRS_UUID_LEN);

    if (brs_reader_bytes(&r, BRS_UUID_LEN, &tmp) != 0) { fprintf(stderr, "PARSE_FAIL: repo_uuid\n"); goto parse_error; }
    memcpy(snap->repo_uuid, tmp, BRS_UUID_LEN);

    if (brs_reader_bytes(&r, BRS_UUID_LEN, &tmp) != 0) { fprintf(stderr, "PARSE_FAIL: parent_id\n"); goto parse_error; }
    memcpy(snap->parent_id, tmp, BRS_UUID_LEN);

    if (brs_reader_u64_le(&r, &snap->created_ns) != 0) { fprintf(stderr, "PARSE_FAIL: created_ns\n"); goto parse_error; }
    if (reader_read_string(&r, &snap->hostname) != 0) { fprintf(stderr, "PARSE_FAIL: hostname\n"); goto parse_error; }
    if (reader_read_string(&r, &snap->root_path) != 0) { fprintf(stderr, "PARSE_FAIL: root_path\n"); goto parse_error; }

    if (brs_reader_u64_le(&r, &snap->entry_count) != 0) { fprintf(stderr, "PARSE_FAIL: entry_count\n"); goto parse_error; }
    if (brs_reader_u64_le(&r, &snap->file_count) != 0) { fprintf(stderr, "PARSE_FAIL: file_count\n"); goto parse_error; }
    if (brs_reader_u64_le(&r, &snap->dir_count) != 0) { fprintf(stderr, "PARSE_FAIL: dir_count\n"); goto parse_error; }
    if (brs_reader_u64_le(&r, &snap->symlink_count) != 0) { fprintf(stderr, "PARSE_FAIL: symlink_count\n"); goto parse_error; }
    if (brs_reader_u64_le(&r, &snap->logical_bytes) != 0) { fprintf(stderr, "PARSE_FAIL: logical_bytes\n"); goto parse_error; }
    if (brs_reader_u64_le(&r, &snap->chunk_refs) != 0) { fprintf(stderr, "PARSE_FAIL: chunk_refs\n"); goto parse_error; }

    /* FIX C6: límites duros antes de calloc */
    if (snap->entry_count > 10000000ULL) {
        fprintf(stderr, "PARSE_FAIL: entry_count too big\n");
        goto parse_error;
    }

    if (snap->file_count > snap->entry_count ||
        snap->dir_count > snap->entry_count ||
        snap->symlink_count > snap->entry_count) {
        fprintf(stderr, "PARSE_FAIL: inconsistent snapshot counters\n");
        goto parse_error;
    }

    if (snap->entry_count > size / 32ULL) {
        fprintf(stderr, "PARSE_FAIL: entry_count impossible for snapshot size\n");
        goto parse_error;
    }

    if (snap->entry_count > 0) {
        snap->entries = (BrsManifestEntry *)calloc(
            (size_t)snap->entry_count, sizeof(BrsManifestEntry));
        if (!snap->entries) { fprintf(stderr, "PARSE_FAIL: calloc entries\n"); goto parse_error; }
        snap->entries_cap = snap->entry_count;
    }

    for (uint64_t i = 0; i < snap->entry_count; ++i) {
        BrsManifestEntry *e = &snap->entries[i];
        uint8_t type;

        brs_manifest_entry_init(e);
        //snap->entries_len = i + 1;

        if (reader_read_string(&r, &e->path) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu path\n", (unsigned long long)i); goto parse_error; }
        if (brs_reader_u8(&r, &type) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu type\n", (unsigned long long)i); goto parse_error; }
        e->type = type;

        if (brs_reader_u16_le(&r, &e->flags) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu flags\n", (unsigned long long)i); goto parse_error; }
        if (brs_reader_u32_le(&r, &e->mode) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu mode\n", (unsigned long long)i); goto parse_error; }
        if (brs_reader_u32_le(&r, &e->uid) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu uid\n", (unsigned long long)i); goto parse_error; }
        if (brs_reader_u32_le(&r, &e->gid) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu gid\n", (unsigned long long)i); goto parse_error; }
        if (brs_reader_u64_le(&r, &e->size) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu size\n", (unsigned long long)i); goto parse_error; }
        if (brs_reader_u64_le(&r, &e->mtime_ns) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu mtime\n", (unsigned long long)i); goto parse_error; }
        if (brs_reader_u64_le(&r, &e->ctime_ns) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu ctime\n", (unsigned long long)i); goto parse_error; }

        if (e->type == BRS_FILETYPE_SYMLINK) {
            if (reader_read_string(&r, &e->symlink_target) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu symlink\n", (unsigned long long)i); goto parse_error; }
        }

        if (e->type == BRS_FILETYPE_FILE && (e->flags & BRS_FLAG_HARDLINK)) {
            if (reader_read_string(&r, &e->hardlink_to) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu hardlink\n", (unsigned long long)i); goto parse_error; }
        } else if (e->type == BRS_FILETYPE_FILE) {
            if (e->flags & BRS_FLAG_DELTA) {
                uint32_t dsc;
                if (brs_reader_u32_le(&r, &dsc) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu delta_source_count\n", (unsigned long long)i); goto parse_error; }

                if (dsc > 100000000ULL) { fprintf(stderr, "PARSE_FAIL: e%llu dsc too big\n", (unsigned long long)i); goto parse_error; }

                if (dsc > 0) {
                    e->delta_source_chunks = (BrsChunkId *)calloc(dsc, sizeof(BrsChunkId));
                    if (!e->delta_source_chunks) { fprintf(stderr, "PARSE_FAIL: e%llu delta_source_chunks calloc\n", (unsigned long long)i); goto parse_error; }
                    e->delta_source_cap = dsc;
                    e->delta_source_count = dsc;

                    for (uint32_t c = 0; c < dsc; ++c) {
                        const uint8_t *cid;
                        if (brs_reader_bytes(&r, BRS_CHUNK_ID_LEN, &cid) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu delta_source_chunk %u\n", (unsigned long long)i, c); goto parse_error; }
                        memcpy(e->delta_source_chunks[c].bytes, cid, BRS_CHUNK_ID_LEN);
                    }
                }
            }
            {
                uint32_t cc;
                if (brs_reader_u32_le(&r, &cc) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu chunk_count\n", (unsigned long long)i); goto parse_error; }

                if (cc > 100000000ULL) { fprintf(stderr, "PARSE_FAIL: e%llu cc too big\n", (unsigned long long)i); goto parse_error; }

                if (cc > 0) {
                    e->chunks = (BrsChunkId *)calloc(cc, sizeof(BrsChunkId));
                    if (!e->chunks) { fprintf(stderr, "PARSE_FAIL: e%llu chunks calloc\n", (unsigned long long)i); goto parse_error; }
                    e->chunk_cap = cc;
                    e->chunk_count = cc;

                    for (uint32_t c = 0; c < cc; ++c) {
                        const uint8_t *cid;
                        if (brs_reader_bytes(&r, BRS_CHUNK_ID_LEN, &cid) != 0) { fprintf(stderr, "PARSE_FAIL: e%llu chunk %u\n", (unsigned long long)i, c); goto parse_error; }
                        memcpy(e->chunks[c].bytes, cid, BRS_CHUNK_ID_LEN);
                    }
                }
            }
        }
    }

        snap->entries_len = snap->entry_count; 

    if (size < 8) { fprintf(stderr, "PARSE_FAIL: no room for checksum\n"); goto parse_error; }

    {
        BrsReader cr;
        uint64_t stored = 0;

        brs_reader_init(&cr, data + size - 8, 8);
        if (brs_reader_u64_le(&cr, &stored) != 0) { fprintf(stderr, "PARSE_FAIL: read checksum\n"); goto parse_error; }

        if (stored != 0 && brs_fnv1a_64(data, size - 8) != stored) {
            fprintf(stderr, "PARSE_FAIL: checksum mismatch stored=%llx computed=%llx\n",
                    (unsigned long long)stored,
                    (unsigned long long)brs_fnv1a_64(data, size - 8));
            goto parse_error;
        }
    }

    brs_buffer_free(&dec);
    return 0;

    /* ====================================================================
     * REVISIÓN CORTAFUEGOS: Limpieza absoluta e incondicional
     * ==================================================================== */
parse_error:
    /* 1) Borrado y liberación segura del buffer descifrado (dec) */
    if (dec.data != NULL) {
        if (dec.size > 0) {
            brs_secure_wipe(dec.data, dec.size);
        }
        brs_buffer_free(&dec);
    }

    /* 2) Liberación de metadatos del snapshot */
    if (snap != NULL) {
        free(snap->hostname);
        snap->hostname = NULL;
        free(snap->root_path);
        snap->root_path = NULL;


                    /* 3) BARRIDO BRUTO: Limpieza total de ranuras del heap */
        if (snap->entries != NULL) {
            uint64_t total_slots = snap->entries_len;

            for (uint64_t i = 0; i < total_slots; ++i) {
                /* Si la memoria de la entrada contiene basura del crash, la vaciamos
                   invocando al destructor seguro de brs_util.c */
                brs_manifest_entry_free(&snap->entries[i]);
            }
            free(snap->entries);
            snap->entries = NULL;
        }

        
        snap->entries_cap = 0;
        snap->entries_len = 0;
        snap->entry_count = 0;
    }

    return -1;
}

