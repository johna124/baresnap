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

void brs_parsed_snapshot_free(BrsParsedSnapshot *snap);

#ifdef __cplusplus
}
#endif

#endif /* BRS_MANIFEST_H */
