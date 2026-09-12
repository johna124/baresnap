/* brs_manifest.h — escaneo de fuentes y snapshots */
#ifndef BRS_MANIFEST_H
#define BRS_MANIFEST_H

#include <stddef.h>
#include <stdint.h>
#include "brs_types.h"   /* BrsManifestEntry ya viene de aquí (con campos delta) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    BrsManifestEntry *items;
    uint64_t count;
    uint64_t cap;
} BrsManifestList;

/* Escaneo recursivo (opendir/lstat, sin seguir symlinks). */
int  brs_scan_source(const char *source, BrsManifestList *out);
void brs_manifest_list_free(BrsManifestList *list);

/* Nombre: [label_]YYYY-MM-DD_HH-MM-SS_<hash8>.snap */
int brs_make_snapshot_name(char *out, size_t out_size, uint64_t created_ns,
                           const uint8_t snapshot_id[BRS_UUID_LEN],
                           const char *label);

int brs_write_snapshot_manifest(const char *repo_path,
                                const BrsRepoConfig *cfg,
                                const char *source_path,
                                const BrsManifestEntry *entries,
                                uint64_t entry_count,
                                const BrsSecureKey *crypto_key,
                                const char *label,
                                char *out_path, size_t out_path_size);

int brs_parse_snapshot(const uint8_t *data, size_t size,
                       BrsParsedSnapshot *snap,
                       const BrsSecureKey *crypto_key,
                       int cipher_algo); /* FIX AES SNAPSHOT CIPHER */

#ifdef __cplusplus
}
#endif

#endif /* BRS_MANIFEST_H */
