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
/* ============================================================================
 * brs_types.h — Tipos y constantes base de BareSnap (C11 puro, musl/POSIX)
 *
 * Sustituye a include/baresnap/types.hpp del motor C++.
 * El formato binario en disco es 100% compatible con FORMAT_VERSION=1:
 * mismo layout little-endian, mismas magias, mismos tamaños.
 * ==========================================================================*/
#ifndef BRS_TYPES_H
#define BRS_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Versión de formato (compatible con el motor C++ v1.1.0)
 * -------------------------------------------------------------------------*/
#define BRS_FORMAT_VERSION 1u

/* ---------------------------------------------------------------------------
 * Longitudes fijas
 * -------------------------------------------------------------------------*/
#define BRS_CHUNK_ID_LEN    16
#define BRS_UUID_LEN        16
#define BRS_KEY_LEN         32   /* clave simétrica XChaCha20-Poly1305 */
#define BRS_CRYPTO_SALT_LEN 16

#define BRS_HOSTNAME_MAX    256
#define BRS_PATH_MAX        4096

/* Límite estricto para la lectura interactiva de passphrase.
 * FIX (init.cpp): el bucle de confirmación leía sin límite de tamaño.
 * Toda lectura de passphrase debe truncar/rechazar más allá de este valor. */
#define BRS_PASSPHRASE_MAX  1024

/* ---------------------------------------------------------------------------
 * Cifrado: KVV (Key Verification Value)
 *
 * Layout FIJO: nonce(24) | ciphertext(32) | mac(16) = 72 bytes.
 *
 * FIX (util_internal.cpp): el código C++ tenía DOS rutas de empaquetado
 * superpuestas y un comentario que mencionaba KVV_SIZE=60; el memcpy en el
 * offset 56 con 16 bytes escribía hasta el byte 72, desbordando cualquier
 * array menor. Aquí:
 *   - El tamaño es único y se valida en compilación (_Static_assert).
 *   - BrsRepoConfig.kvv tiene tamaño fijo 72.
 *   - El empaquetado pasa SIEMPRE por brs_kvv_pack()/brs_kvv_unpack()
 *     (una sola ruta, offsets constantes, sin copias parciales).
 * -------------------------------------------------------------------------*/
#define BRS_KVV_NONCE_LEN 24
#define BRS_KVV_CT_LEN    32
#define BRS_KVV_MAC_LEN   16
#define BRS_KVV_SIZE      (BRS_KVV_NONCE_LEN + BRS_KVV_CT_LEN + BRS_KVV_MAC_LEN)

#define BRS_KVV_OFF_NONCE 0
#define BRS_KVV_OFF_CT    (BRS_KVV_NONCE_LEN)
#define BRS_KVV_OFF_MAC   (BRS_KVV_NONCE_LEN + BRS_KVV_CT_LEN)

_Static_assert(BRS_KVV_NONCE_LEN + BRS_KVV_CT_LEN + BRS_KVV_MAC_LEN == 72,
               "KVV debe ser nonce(24) + ct(32) + mac(16) = 72 bytes");

/* ---------------------------------------------------------------------------
 * Magias de formato: SIEMPRE 8 bytes, sin NUL garantizado.
 * Comparar con memcmp(x, BRS_MAGIC_*, BRS_MAGIC_LEN).
 * -------------------------------------------------------------------------*/
#define BRS_MAGIC_LEN          8
#define BRS_MAGIC_CONFIG       "BSCFG001"
#define BRS_MAGIC_PACK         "BSPK0001"
#define BRS_MAGIC_PACK_FOOTER  "BSPKFT01"
#define BRS_MAGIC_INDEX        "BSIX0001"
#define BRS_MAGIC_SNAPSHOT     "BSSN0001"
#define BRS_MAGIC_CACHE        "BSCACH01"
/* Contiene 0x01: prohibido usar strlen() sobre esta magia. */
#define BRS_MAGIC_SNAPSHOT_ENC "BRSNAP2\x01"

/* ---------------------------------------------------------------------------
 * Flags
 * -------------------------------------------------------------------------*/
#define BRS_FLAG_ENCRYPTED        0x0001u  /* RepoConfig.flags             */
#define BRS_FLAG_HARDLINK         0x0002u  /* ManifestEntry.flags          */
#define BRS_CHUNK_FLAG_COMPRESSED 0x01u    /* ChunkLocation/PackEntry.flags */
#define BRS_CHUNK_FLAG_ENCRYPTED  0x02u
#define BRS_FLAG_DELTA         0x0004 
#define BRS_FLAG_DELTA         0x0004
#define BRS_CHUNK_FLAG_ZSTD       0x04u 
/* --- Delta encoding (v2) --- */
#define BRS_DELTA_MAX_SIZE         (256u * 1024u)        /* umbral: solo ficheros <= 256 KB */
#define BRS_DELTA_MAX_SIZE_BINARY  (256u * 1024u * 1024u) /* con --delta-binary: hasta 256 MB */
#define BRS_DELTA_MIN_RATIO        2 



/* ---------------------------------------------------------------------------
 * KDF Argon2 por defecto (64 MB, t=3)
 * -------------------------------------------------------------------------*/
#define BRS_KDF_DEFAULT_NB_BLOCKS 65536u
#define BRS_KDF_DEFAULT_NB_PASSES 3u

/* ---------------------------------------------------------------------------
 * Límites operativos
 * -------------------------------------------------------------------------*/
#define BRS_PACK_MAX_SIZE    (64ULL * 1024ULL * 1024ULL)
/* Límite anti-OOM/DoS para el tamaño descomprimido de un chunk.
 * FIX (repo.cpp): validar ANTES de asignar, y contra este tope duro. */
#define BRS_CHUNK_UNCOMP_MAX (64ULL * 1024ULL * 1024ULL)

/* ---------------------------------------------------------------------------
 * Enums (valores idénticos al motor C++ para compatibilidad en disco)
 * -------------------------------------------------------------------------*/
typedef enum {
    BRS_FILETYPE_DIR      = 1,
    BRS_FILETYPE_FILE     = 2,
    BRS_FILETYPE_SYMLINK  = 3,
    BRS_FILETYPE_HARDLINK = 4,
    BRS_FILETYPE_OTHER    = 5
} BrsFileType;

typedef enum {
    BRS_COMPRESSION_NONE = 0,
    BRS_COMPRESSION_LZ4  = 1,
    BRS_COMPRESSION_ZSTD = 2
} BrsCompression;

typedef enum {
    BRS_HASH_FNV1A_128   = 0,
    BRS_HASH_XXH3_128    = 1,
    BRS_HASH_BLAKE2B_128 = 2
} BrsHashAlgo;

typedef enum {
    BRS_CIPHER_CHACHA20_POLY1305 = 0,
    BRS_CIPHER_AES256_GCM = 1,
} BrsCipherAlgo;

/* ---------------------------------------------------------------------------
 * Estructuras planas
 * -------------------------------------------------------------------------*/
typedef struct {
    uint8_t bytes[BRS_CHUNK_ID_LEN];
} BrsChunkId;

static inline int brs_chunk_id_equal(const BrsChunkId *a, const BrsChunkId *b)
{
    return memcmp(a->bytes, b->bytes, BRS_CHUNK_ID_LEN) == 0;
}

static inline int brs_chunk_id_cmp(const BrsChunkId *a, const BrsChunkId *b)
{
    return memcmp(a->bytes, b->bytes, BRS_CHUNK_ID_LEN);
}

/* Localización de un chunk dentro de un pack (entrada del index). */
typedef struct {
    uint64_t pack_id;
    uint64_t offset;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint8_t  flags;
} BrsChunkLocation;

/* Entrada del footer de un pack. */
typedef struct {
    BrsChunkId chunk_id;
    uint64_t   offset;
    uint32_t   comp_size;
    uint32_t   uncomp_size;
    uint8_t    flags;
} BrsPackEntry;

/* Configuración del repositorio (fichero "config"). */
typedef struct {
    uint8_t  uuid[BRS_UUID_LEN];
    uint64_t created_ns;
    uint32_t chunk_min;
    uint32_t chunk_avg;
    uint32_t chunk_max;
    uint8_t  hash_algo;    /* BrsHashAlgo     */
    uint8_t  compression;  /* BrsCompression  */
    uint32_t flags;
    int      encrypted;    /* 0/1 */
    uint8_t  crypto_salt[BRS_CRYPTO_SALT_LEN];
    uint32_t kdf_nb_blocks;
    uint32_t kdf_nb_passes;
    uint8_t  kvv[BRS_KVV_SIZE];  /* tamaño fijo 72 — ver nota KVV arriba */
    uint8_t  zstd_level;
    BrsCipherAlgo cipher_algo;
} BrsRepoConfig;

/* Entrada de snapshot. Las cadenas son dinámicas (path puede medir hasta
 * BRS_PATH_MAX); los chunks son un array dinámico. */
typedef struct {
    char        *path;
    uint8_t      type;   /* BrsFileType */
    uint16_t     flags;
    uint32_t     mode;
    uint32_t     uid;
    uint32_t     gid;
    uint64_t     size;
    uint64_t     mtime_ns;
    uint64_t     ctime_ns;
    uint64_t     dev;
    uint64_t     ino;
    char        *symlink_target;  /* NULL si no es symlink */
    char        *hardlink_to;     /* NULL si no es hardlink */
    BrsChunkId  *chunks;
    uint32_t     chunk_count;
    uint32_t     chunk_cap;
     /* --- DELTA ENCODING (v2) --- */
    BrsChunkId  *delta_source_chunks;  
    uint32_t     delta_source_count;
    uint32_t     delta_source_cap;

} BrsManifestEntry;

/* Snapshot ya parseado en memoria. */
typedef struct {
    uint8_t  snapshot_id[BRS_UUID_LEN];
    uint8_t  repo_uuid[BRS_UUID_LEN];
    uint8_t  parent_id[BRS_UUID_LEN];
    uint64_t created_ns;
    char    *hostname;
    char    *root_path;
    uint64_t entry_count;
    uint64_t file_count;
    uint64_t dir_count;
    uint64_t symlink_count;
    uint64_t logical_bytes;
    uint64_t chunk_refs;
    BrsManifestEntry *entries;
    uint64_t entries_len;
    uint64_t entries_cap;
} BrsParsedSnapshot;

/* Entrada del file cache persistente. */
typedef struct {
    char       *path;
    uint64_t    dev;
    uint64_t    ino;
    uint64_t    size;
    uint64_t    mtime_ns;
    uint64_t    ctime_ns;
    uint32_t    mode;
    BrsChunkId *chunks;
    uint32_t    chunk_count;
    uint32_t    chunk_cap;
} BrsFileCacheEntry;

/* Clave simétrica en memoria. En C no hay destructor: el llamador DEBE
 * invocar brs_secure_key_wipe() cuando termine de usarla. */
typedef struct {
    uint8_t bytes[BRS_KEY_LEN];
} BrsSecureKey;

/* ---------------------------------------------------------------------------
 * Funciones de soporte (implementadas en brs_util.c)
 * -------------------------------------------------------------------------*/
void brs_repo_config_default(BrsRepoConfig *cfg);

void brs_manifest_entry_init(BrsManifestEntry *e);
void brs_manifest_entry_free(BrsManifestEntry *e);
int  brs_manifest_entry_set_path(BrsManifestEntry *e, const char *s, size_t len);
int  brs_manifest_entry_set_symlink_target(BrsManifestEntry *e, const char *s, size_t len);
int  brs_manifest_entry_set_hardlink_to(BrsManifestEntry *e, const char *s, size_t len);
int  brs_manifest_entry_add_chunk(BrsManifestEntry *e, const BrsChunkId *id);

void brs_file_cache_entry_init(BrsFileCacheEntry *ce);
void brs_file_cache_entry_free(BrsFileCacheEntry *ce);

void brs_parsed_snapshot_init(BrsParsedSnapshot *s);
void brs_parsed_snapshot_free(BrsParsedSnapshot *s);

void brs_secure_key_wipe(BrsSecureKey *k);

#ifdef __cplusplus
}
#endif

#endif /* BRS_TYPES_H */


