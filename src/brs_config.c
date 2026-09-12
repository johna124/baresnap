/* brs_config.c — lectura/escritura de config y open_pack_id
 * FIX KVV: la seccion de cifrado escribe/lee exactamente BRS_KVV_SIZE (72)
 * bytes mediante offsets fijos; ya no existe la doble ruta de init_crypto. */
#include "brs_config.h"
#include "brs_crypto.h"
#include "brs_buffer.h"
#include "brs_fsutil.h"
#include "brs_util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int brs_write_config(const char *repo_path, const BrsRepoConfig *cfg)
{
    if (!repo_path || !cfg) return -1;
    char path[BRS_PATH_MAX];
    if (brs_path_join(path, sizeof path, repo_path, "config") != 0) return -1;
    BrsBuffer buf;
    brs_buffer_init(&buf);
    int ok =
        brs_buffer_append(&buf, BRS_MAGIC_CONFIG, BRS_MAGIC_LEN) == 0 &&
        brs_buffer_append_u32_le(&buf, BRS_FORMAT_VERSION) == 0 &&
        brs_buffer_append(&buf, cfg->uuid, BRS_UUID_LEN) == 0 &&
        brs_buffer_append_u64_le(&buf, cfg->created_ns) == 0 &&
        brs_buffer_append_u32_le(&buf, cfg->chunk_min) == 0 &&
        brs_buffer_append_u32_le(&buf, cfg->chunk_avg) == 0 &&
        brs_buffer_append_u32_le(&buf, cfg->chunk_max) == 0 &&
        brs_buffer_append_u8(&buf, cfg->hash_algo) == 0 &&
        brs_buffer_append_u8(&buf, cfg->compression) == 0 &&
        brs_buffer_append_u32_le(&buf, cfg->flags) == 0 &&
        brs_buffer_append_u8(&buf, cfg->encrypted ? ((cfg->cipher_algo == BRS_CIPHER_AES256_GCM) ? 2 : 1) : 0) == 0; /* FIX P10: persist cipher_algo */
    if (ok && cfg->encrypted) {
        ok =
            brs_buffer_append(&buf, cfg->crypto_salt, BRS_CRYPTO_SALT_LEN) == 0 &&
            brs_buffer_append_u32_le(&buf, cfg->kdf_nb_blocks) == 0 &&
            brs_buffer_append_u32_le(&buf, cfg->kdf_nb_passes) == 0 &&
            brs_buffer_append(&buf, cfg->kvv, BRS_KVV_SIZE) == 0;
    }
    /* zstd_level: campo nuevo (v1.1+) */
    if (ok)
        ok = brs_buffer_append_u8(&buf, cfg->zstd_level) == 0;
    if (ok) {
        uint64_t csum = brs_fnv1a_64(buf.data, buf.size);
        ok = brs_buffer_append_u64_le(&buf, csum) == 0;
    }
    int rc = ok ? brs_write_file_atomic(path, buf.data, buf.size) : -1;
    brs_buffer_free(&buf);
    return rc;
}

int brs_load_config(const char *repo_path, BrsRepoConfig *cfg)
{
    if (!repo_path || !cfg) return -1;
    char path[BRS_PATH_MAX];
    if (brs_path_join(path, sizeof path, repo_path, "config") != 0) return -1;
    BrsBuffer data;
    brs_buffer_init(&data);
    if (brs_read_file(path, &data) != 0) {
        brs_buffer_free(&data);
        return -1;
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->cipher_algo = BRS_CIPHER_CHACHA20_POLY1305; /* FIX P10 */
    int rc = -1;
    BrsReader r;
    brs_reader_init(&r, data.data, data.size);
    do {
        const uint8_t *magic;
        uint32_t version;
        if (brs_reader_bytes(&r, BRS_MAGIC_LEN, &magic) != 0) break;
        if (memcmp(magic, BRS_MAGIC_CONFIG, BRS_MAGIC_LEN) != 0) break;
        if (brs_reader_u32_le(&r, &version) != 0) break;
        if (version != BRS_FORMAT_VERSION) break;
        const uint8_t *uuid;
        if (brs_reader_bytes(&r, BRS_UUID_LEN, &uuid) != 0) break;
        memcpy(cfg->uuid, uuid, BRS_UUID_LEN);
        if (brs_reader_u64_le(&r, &cfg->created_ns) != 0) break;
        if (brs_reader_u32_le(&r, &cfg->chunk_min) != 0) break;
        if (brs_reader_u32_le(&r, &cfg->chunk_avg) != 0) break;
        if (brs_reader_u32_le(&r, &cfg->chunk_max) != 0) break;
        uint8_t hash_algo = 0, compression = 0;
        if (brs_reader_u8(&r, &hash_algo) != 0) break;
        if (brs_reader_u8(&r, &compression) != 0) break;
        cfg->hash_algo = hash_algo;
        cfg->compression = compression;
        if (brs_reader_u32_le(&r, &cfg->flags) != 0) break;
        /* Seccion de cifrado: opcional por compatibilidad hacia atras
         * (configs antiguas terminan justo despues de flags). */
        uint8_t enc_flag = 0;
        if (brs_reader_u8(&r, &enc_flag) != 0) {
            cfg->encrypted = 0;
        } else {
            cfg->encrypted = (enc_flag != 0);
            cfg->cipher_algo = (enc_flag == 2) ? BRS_CIPHER_AES256_GCM : BRS_CIPHER_CHACHA20_POLY1305; /* FIX P10 */
            if (cfg->encrypted) {
                const uint8_t *salt, *kvv;
                if (brs_reader_bytes(&r, BRS_CRYPTO_SALT_LEN, &salt) != 0) break;
                memcpy(cfg->crypto_salt, salt, BRS_CRYPTO_SALT_LEN);
                if (brs_reader_u32_le(&r, &cfg->kdf_nb_blocks) != 0) break;
                if (brs_reader_u32_le(&r, &cfg->kdf_nb_passes) != 0) break;
                if (brs_reader_bytes(&r, BRS_KVV_SIZE, &kvv) != 0) break;
                memcpy(cfg->kvv, kvv, BRS_KVV_SIZE);
            }
        }
        /* zstd_level: campo nuevo (v1.1+).
         * Configs antiguos no lo tienen: si solo quedan 8 bytes
         * (el checksum), usamos el valor por defecto. */
        cfg->zstd_level = 3;
        {
            size_t remaining = data.size - r.pos;
            if (remaining > 8) {
                uint8_t zl = 3;
                if (brs_reader_u8(&r, &zl) == 0)
                    cfg->zstd_level = zl;
            }
        }
        uint64_t stored = 0;
        if (brs_reader_u64_le(&r, &stored) != 0) break;
        if (stored != 0) {
            if (data.size < 8) break;
            if (brs_fnv1a_64(data.data, data.size - 8) != stored) break;
        }
        rc = 0;
    } while (0);
    brs_buffer_free(&data);
    return rc;
}

int brs_read_open_pack_id(const char *repo_path, uint64_t *pack_id)
{
    if (!repo_path || !pack_id) return -1;
    char path[BRS_PATH_MAX];
    if (brs_path_join(path, sizeof path, repo_path, "open_pack_id") != 0) return -1;
    BrsBuffer data;
    brs_buffer_init(&data);
    if (brs_read_file(path, &data) != 0) {
        brs_buffer_free(&data);
        return -1;
    }
    int rc = -1;
    if (data.size > 0 && data.size < 64) {
        char tmp[64];
        memcpy(tmp, data.data, data.size);
        tmp[data.size] = '\0';
        errno = 0;
        char *end = NULL;
        unsigned long long v = strtoull(tmp, &end, 10);
        if (errno == 0 && end != tmp) {
            while (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')
                end++;
            if (*end == '\0') {
                *pack_id = (uint64_t)v;
                rc = 0;
            }
        }
    }
    brs_buffer_free(&data);
    return rc;
}

int brs_write_open_pack_id(const char *repo_path, uint64_t pack_id)
{
    if (!repo_path) return -1;
    char path[BRS_PATH_MAX];
    if (brs_path_join(path, sizeof path, repo_path, "open_pack_id") != 0) return -1;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)pack_id);
    if (n <= 0 || (size_t)n >= sizeof buf) return -1;
    return brs_write_file_atomic(path, buf, (size_t)n);
}

int brs_remove_open_pack_id(const char *repo_path)
{
    if (!repo_path) return -1;
    char path[BRS_PATH_MAX];
    if (brs_path_join(path, sizeof path, repo_path, "open_pack_id") != 0) return -1;
    return brs_remove_file(path);
}
