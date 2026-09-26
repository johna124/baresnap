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
#include "brs_repo_internal.h"
#include <fnmatch.h>
/* ============================================================================
search — busca un patrón (substring o glob) en TODOS los snapshots del repo
==========================================================================*/
static int search_path_matches(const char *path,
                               const char *pattern,
                               int use_glob)
{
    if (!pattern || pattern[0] == '\0')
        return 1;

    if (use_glob) {
        /* 0 = match */
     return (fnmatch(pattern, path, FNM_PATHNAME | FNM_PERIOD | FNM_NOESCAPE) == 0);
    }

    /* Substring case-insensitive no, case-sensitive sí (simple y predecible) */
    return (strstr(path, pattern) != NULL);
}

int brs_repo_search(const char *repo_path,
                    const char *snapshot_name,
                    const char *pattern,
                    int use_glob,
                    BrsProgressCallback cb,
                    void *cb_user,
                    _Atomic int *cancel_flag)
{
    if (!repo_path || !pattern)
        return 1;

    int rc = 1;

    BrsVfs *vfs = brs_vfs_context_get();
    int owns_vfs = 0;
    if (!vfs && strncmp(repo_path, "ssh://", 6) == 0) {
        vfs = brs_vfs_open(repo_path, 0);
        if (!vfs) {
            fprintf(stderr, "cannot connect to remote repo: %s\n", repo_path);
            return 1;
        }
        brs_vfs_context_set(vfs, repo_path);
        owns_vfs = 1;
    }

    BrsRepoConfig cfg;
    BrsSecureKey key;
    int key_valid = 0;
    BrsBuffer sdata;
    brs_buffer_init(&sdata);

    brs_repo_config_default(&cfg);
    if (brs_load_config(repo_path, &cfg) != 0) {
        fprintf(stderr, "cannot load repo config\n");
        goto cleanup;
    }
    if (cfg.encrypted) {
        if (derive_repo_key(&cfg, &key) != 0)
            goto cleanup;
        key_valid = 1;
    }

    char snaps_dir[BRS_PATH_MAX];
    if (brs_path_join(snaps_dir, sizeof snaps_dir, repo_path, "snapshots") != 0)
        goto cleanup;

    BrsDirList snaps;
    if (brs_list_dir(snaps_dir, &snaps) != 0) {
        fprintf(stderr, "no snapshots directory\n");
        goto cleanup;
    }
    brs_dir_list_sort(&snaps);

    /* Contar snapshots válidos a procesar */
    size_t total_snaps = 0;
    for (size_t i = 0; i < snaps.count; ++i) {
        size_t ln = strlen(snaps.names[i]);
        if (ln <= 5 || strcmp(snaps.names[i] + ln - 5, ".snap") != 0)
            continue;
        if (snapshot_name && strcmp(snaps.names[i], snapshot_name) != 0)
            continue;
        total_snaps++;
    }

    uint64_t matches_found = 0;
    uint64_t snap_no = 0;

    for (size_t i = 0; i < snaps.count; ++i) {
        const char *nm = snaps.names[i];
        size_t ln = strlen(nm);
        if (ln <= 5 || strcmp(nm + ln - 5, ".snap") != 0)
            continue;
        if (snapshot_name && strcmp(nm, snapshot_name) != 0)
            continue;

        if (check_cancel(cancel_flag)) {
            rc = 2;
            break;
        }

        snap_no++;
        report_progress(cb, cb_user, "search", snap_no, total_snaps);

        char sp[BRS_PATH_MAX];
        if (brs_path_join(sp, sizeof sp, snaps_dir, nm) != 0)
            continue;

        brs_buffer_free(&sdata);
        brs_buffer_init(&sdata);
        if (brs_read_file(sp, &sdata) != 0) {
            fprintf(stderr, "warning: cannot read snapshot %s\n", nm);
            continue;
        }

        BrsParsedSnapshot snap;
        brs_parsed_snapshot_init(&snap);
        if (brs_parse_snapshot(sdata.data, sdata.size, &snap,
                               cfg.encrypted ? &key : NULL,
                               cfg.cipher_algo) != 0) {
            fprintf(stderr, "warning: cannot parse snapshot %s\n", nm);
            brs_parsed_snapshot_free(&snap);
            continue;
        }

        for (uint64_t e = 0; e < snap.entries_len; ++e) {
            const BrsManifestEntry *se = &snap.entries[e];
            if (!se->path)
                continue;

            if (!search_path_matches(se->path, pattern, use_glob))
                continue;

            const char *type_str = "?";
            switch (se->type) {
                case BRS_FILETYPE_FILE:    type_str = "F"; break;
                case BRS_FILETYPE_DIR:     type_str = "D"; break;
                case BRS_FILETYPE_SYMLINK: type_str = "L"; break;
            }

            printf("[%s]  %s  %8llu  %s\n",
                   nm,
                   type_str,
                   (unsigned long long)se->size,
                   se->path);
            matches_found++;
        }

        brs_parsed_snapshot_free(&snap);
    }

    brs_dir_list_free(&snaps);

    if (rc != 2) {
        if (matches_found == 0) {
            fprintf(stderr, "no matches found for: %s\n", pattern);
            rc = 1;
        } else {
            printf("\n%llu match(es) in %llu snapshot(s)\n",
                   (unsigned long long)matches_found,
                   (unsigned long long)snap_no);
            rc = 0;
        }
    }

cleanup:
    if (key_valid)
        brs_secure_key_wipe(&key);
    brs_buffer_free(&sdata);
    if (owns_vfs) {
        brs_vfs_context_set(NULL, NULL);
        brs_vfs_close(vfs);
    }
    return rc;
}

