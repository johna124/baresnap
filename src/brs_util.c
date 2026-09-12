/* brs_util.c — utilidades base de BareSnap (C11, musl/POSIX) */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "brs_util.h"
#include "brs_vfs_context.h" 
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/random.h>
#define BRS_HAVE_GETRANDOM 1
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/* Tope para ficheros de gestion (config/index/cache/snapshots). */
#define BRS_FILE_READ_MAX (4ULL * 1024ULL * 1024ULL * 1024ULL)

/* ---- Borrado seguro: explicit_bzero nativo de musl (fix CWE-14) ---- */
#ifdef BRS_NO_EXPLICIT_BZERO
static void *(*volatile brs_memset_v)(void *, int, size_t) = memset;
void brs_secure_wipe(void *p, size_t n)
{
    if (!p || n == 0) return;
    brs_memset_v(p, 0, n);
}
#else
void brs_secure_wipe(void *p, size_t n)
{
    if (!p || n == 0) return;
    explicit_bzero(p, n);
}
#endif

/* ---- RNG: sin fallback determinista para material criptografico ---- */
int brs_random_bytes(void *buf, size_t n)
{
    if (!buf) return -1;
    if (n == 0) return 0;

    uint8_t *p = (uint8_t *)buf;
    size_t remaining = n;

#ifdef BRS_HAVE_GETRANDOM
    while (remaining > 0) {
        ssize_t r = getrandom(p, remaining, 0);
        if (r >= 0) {
            p += (size_t)r;
            remaining -= (size_t)r;
            continue;
        }
        if (errno == EINTR) continue;
        break;  /* ENOSYS u otro: caer a /dev/urandom */
    }
    if (remaining == 0) return 0;
#endif

    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    while (remaining > 0) {
        ssize_t r = read(fd, p, remaining);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (r == 0) { close(fd); return -1; }
        p += (size_t)r;
        remaining -= (size_t)r;
    }
    close(fd);
    return 0;
}



static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

int brs_make_uuid(uint8_t out[BRS_UUID_LEN])
{
    if (!out) return -1;
    if (brs_random_bytes(out, BRS_UUID_LEN) == 0) return 0;
    /* Fallback SOLO para identificadores no secretos. */
    uint64_t st = brs_now_ns() ^ ((uint64_t)getpid() << 32) ^ (uintptr_t)out;
    for (int i = 0; i < 2; ++i) {
        uint64_t v = splitmix64(&st);
        memcpy(out + i * 8, &v, 8);
    }
    return 0;
}

void brs_clear_status_line(void)
{
    fputs("\r\033[2K", stderr);
    fflush(stderr);
}

/* ---- Tiempo ---- */
uint64_t brs_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct timespec brs_ns_to_timespec(uint64_t ns)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    return ts;
}

uint64_t brs_timespec_to_ns(const struct timespec *ts)
{
    if (!ts) return 0;
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

/* ---- Formato / sistema ---- */
void brs_to_hex(const uint8_t *data, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[2 * i]     = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

uint64_t brs_fnv1a_64(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

int brs_get_hostname(char *out, size_t out_size)
{
    if (!out || out_size == 0) return -1;
    char host[BRS_HOSTNAME_MAX];
    if (gethostname(host, sizeof host) != 0) {
        static const char def[] = "localhost";
        size_t n = sizeof def - 1;
        if (n >= out_size) n = out_size - 1;
        memcpy(out, def, n);
        out[n] = '\0';
        return 0;
    }
    host[sizeof host - 1] = '\0';
    size_t n = strlen(host);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, host, n);
    out[n] = '\0';
    return 0;
}

size_t brs_format_bytes(uint64_t bytes, char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    double val = (double)bytes;
    while (val >= 1024.0 && u < 4) { val /= 1024.0; ++u; }
    int n;
    if (u == 0)
        n = snprintf(out, out_size, "%llu B", (unsigned long long)bytes);
    else
        n = snprintf(out, out_size, "%.2f %s", val, units[u]);
    if (n < 0) return 0;
    return ((size_t)n < out_size) ? (size_t)n : out_size - 1;
}

size_t brs_format_timestamp_utc(uint64_t ns, char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    time_t sec = (time_t)(ns / 1000000000ULL);
    struct tm tm_buf;
    if (!gmtime_r(&sec, &tm_buf)) { out[0] = '\0'; return 0; }
    return strftime(out, out_size, "%Y-%m-%d %H:%M:%S UTC", &tm_buf);
}

/* ---- I/O ---- */
int brs_write_fd_all(int fd, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

int brs_write_file(const char *path, const void *data, size_t n)
{
    if (!path) return -1;
    if (n > 0 && !data) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int rc = (n > 0) ? brs_write_fd_all(fd, data, n) : 0;
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    return rc;
}

int brs_read_file(const char *path, BrsBuffer *out)
{
    /* VFS: si el path pertenece al repo remoto, leer via VFS */
    {
        BrsVfs *vfs = brs_vfs_context_get();
        const char *rel = brs_vfs_context_strip(path);
        if (vfs && rel) {
            BrsVfsFile *vf = brs_vfs_fopen(vfs, rel, BRS_VFS_OPEN_READ);
            if (!vf) return -1;
            uint64_t sz = 0; uint32_t md = 0;
            if (brs_vfs_stat(vfs, rel, &sz, &md) != 0) {
                brs_vfs_fclose(vf);
                return -1;
            }
            if (brs_buffer_resize(out, (size_t)sz) != 0) {
                brs_vfs_fclose(vf);
                return -1;
            }
            ssize_t r = brs_vfs_fread(vf, out->data, (size_t)sz, 0);
            brs_vfs_fclose(vf);
            if (r < 0) return -1;
            out->size = (size_t)r;
            return 0;
        }
    }
    if (!path || !out) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > BRS_FILE_READ_MAX) {
        close(fd);
        return -1;
    }

    size_t total = (size_t)st.st_size;
    if (brs_buffer_resize(out, total) != 0) { close(fd); return -1; }

    size_t off = 0;
    while (off < total) {
        ssize_t r = read(fd, out->data + off, total - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (r == 0) break;
        off += (size_t)r;
    }
    close(fd);
    out->size = off;
    return 0;
}

/* ---- Rutas seguras (anti path-traversal) ---- */
int brs_is_safe_relative_path(const char *path)
{
    if (!path || path[0] == '\0') return 0;
    if (path[0] == '/') return 0;
    const char *p = path;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t seglen = (size_t)(p - seg);
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') return 0;
        while (*p == '/') p++;
    }
    return 1;
}

/* ---- KVV: unica ruta de empaquetado (fix overflow, 72 bytes fijos) ---- */
int brs_kvv_pack(uint8_t kvv[BRS_KVV_SIZE],
                 const uint8_t nonce[BRS_KVV_NONCE_LEN],
                 const uint8_t ct[BRS_KVV_CT_LEN],
                 const uint8_t mac[BRS_KVV_MAC_LEN])
{
    if (!kvv || !nonce || !ct || !mac) return -1;
    memcpy(kvv + BRS_KVV_OFF_NONCE, nonce, BRS_KVV_NONCE_LEN);
    memcpy(kvv + BRS_KVV_OFF_CT,    ct,    BRS_KVV_CT_LEN);
    memcpy(kvv + BRS_KVV_OFF_MAC,   mac,   BRS_KVV_MAC_LEN);
    return 0;
}

int brs_kvv_unpack(const uint8_t kvv[BRS_KVV_SIZE],
                   uint8_t nonce[BRS_KVV_NONCE_LEN],
                   uint8_t ct[BRS_KVV_CT_LEN],
                   uint8_t mac[BRS_KVV_MAC_LEN])
{
    if (!kvv || !nonce || !ct || !mac) return -1;
    memcpy(nonce, kvv + BRS_KVV_OFF_NONCE, BRS_KVV_NONCE_LEN);
    memcpy(ct,    kvv + BRS_KVV_OFF_CT,    BRS_KVV_CT_LEN);
    memcpy(mac,   kvv + BRS_KVV_OFF_MAC,   BRS_KVV_MAC_LEN);
    return 0;
}

/* ---- Lector little-endian con limites ---- */
void brs_reader_init(BrsReader *r, const void *data, size_t len)
{
    if (!r) return;
    r->data = (const uint8_t *)data;
    r->len  = data ? len : 0;
    r->pos  = 0;
}

size_t brs_reader_remaining(const BrsReader *r)
{
    return r ? r->len - r->pos : 0;
}

int brs_reader_bytes(BrsReader *r, size_t n, const uint8_t **out)
{
    if (!r || brs_reader_remaining(r) < n) return -1;
    if (out) *out = r->data + r->pos;
    r->pos += n;
    return 0;
}

int brs_reader_skip(BrsReader *r, size_t n)
{
    return brs_reader_bytes(r, n, NULL);
}

int brs_reader_u8(BrsReader *r, uint8_t *v)
{
    const uint8_t *p;
    if (brs_reader_bytes(r, 1, &p) != 0) return -1;
    if (v) *v = p[0];
    return 0;
}

int brs_reader_u16_le(BrsReader *r, uint16_t *v)
{
    const uint8_t *p;
    if (brs_reader_bytes(r, 2, &p) != 0) return -1;
    if (v)
        *v = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return 0;
}

int brs_reader_u32_le(BrsReader *r, uint32_t *v)
{
    const uint8_t *p;
    if (brs_reader_bytes(r, 4, &p) != 0) return -1;
    if (v)
        *v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
             ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return 0;
}

int brs_reader_u64_le(BrsReader *r, uint64_t *v)
{
    const uint8_t *p;
    if (brs_reader_bytes(r, 8, &p) != 0) return -1;
    if (v) {
        uint64_t x = 0;
        for (int i = 0; i < 8; ++i)
            x |= (uint64_t)p[i] << (8 * i);
        *v = x;
    }
    return 0;
}

/* ---- Helpers de tipos (declarados en brs_types.h) ---- */
void brs_secure_key_wipe(BrsSecureKey *k)
{
    if (k) brs_secure_wipe(k->bytes, sizeof k->bytes);
}

void brs_repo_config_default(BrsRepoConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->chunk_min     = 4096;     /* 4 KB (antes: 16 KB) - mejor dedup */
    cfg->chunk_avg     = 32768;    /* 32 KB (antes: 64 KB) - ajustado */
    cfg->chunk_max     = 262144;   /* 256 KB - sin cambio */
    cfg->hash_algo     = BRS_HASH_FNV1A_128;
    cfg->compression   = BRS_COMPRESSION_LZ4;
    cfg->kdf_nb_blocks = BRS_KDF_DEFAULT_NB_BLOCKS;
    cfg->kdf_nb_passes = BRS_KDF_DEFAULT_NB_PASSES;
    cfg->zstd_level = 3;
    cfg->cipher_algo = BRS_CIPHER_CHACHA20_POLY1305;

}

static int brs_dup_bytes(char **dst, const char *src, size_t len)
{
    if (!dst || (!src && len > 0)) return -1;
    char *p = (char *)malloc(len + 1);
    if (!p) return -1;
    if (len > 0) memcpy(p, src, len);
    p[len] = '\0';
    free(*dst);
    *dst = p;
    return 0;
}

void brs_manifest_entry_init(BrsManifestEntry *e)
{
    if (!e) return;
    memset(e, 0, sizeof *e);
    e->type = BRS_FILETYPE_OTHER;
}

void brs_manifest_entry_free(BrsManifestEntry *e)
{
    if (!e) return;
    free(e->path);
    free(e->symlink_target);
    free(e->hardlink_to);
    free(e->chunks);
    free(e->delta_source_chunks);
    brs_manifest_entry_init(e);
}

int brs_manifest_entry_set_path(BrsManifestEntry *e, const char *s, size_t len)
{
    if (!e) return -1;
    return brs_dup_bytes(&e->path, s, len);
}

int brs_manifest_entry_set_symlink_target(BrsManifestEntry *e, const char *s, size_t len)
{
    if (!e) return -1;
    return brs_dup_bytes(&e->symlink_target, s, len);
}

int brs_manifest_entry_set_hardlink_to(BrsManifestEntry *e, const char *s, size_t len)
{
    if (!e) return -1;
    return brs_dup_bytes(&e->hardlink_to, s, len);
}

int brs_manifest_entry_add_chunk(BrsManifestEntry *e, const BrsChunkId *id)
{
    if (!e || !id) return -1;
    if (e->chunk_count == e->chunk_cap) {
        uint32_t ncap = e->chunk_cap ? e->chunk_cap * 2 : 8;
        if (ncap < e->chunk_cap) return -1;
        BrsChunkId *p = (BrsChunkId *)realloc(e->chunks, (size_t)ncap * sizeof *p);
        if (!p) return -1;
        e->chunks = p;
        e->chunk_cap = ncap;
    }
    e->chunks[e->chunk_count++] = *id;
    return 0;
}

void brs_file_cache_entry_init(BrsFileCacheEntry *ce)
{
    if (ce) memset(ce, 0, sizeof *ce);
}

void brs_file_cache_entry_free(BrsFileCacheEntry *ce)
{
    if (!ce) return;
    free(ce->path);
    free(ce->chunks);
    brs_file_cache_entry_init(ce);
}

void brs_parsed_snapshot_init(BrsParsedSnapshot *s)
{
    if (s) memset(s, 0, sizeof *s);
}

void brs_parsed_snapshot_free(BrsParsedSnapshot *s)
{
    if (!s) return;
    free(s->hostname);
    free(s->root_path);
    for (uint64_t i = 0; i < s->entries_len; ++i)
        brs_manifest_entry_free(&s->entries[i]);
    free(s->entries);
    brs_parsed_snapshot_init(s);
}
