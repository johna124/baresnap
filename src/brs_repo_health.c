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
brs_repo_health.c — detección y reparación de anomalías del repositorio
FIXES integrados:
Detección de config ausente/ilegible como anomalía reparable.
Detección y purga de temporales zombi en tmp/ tras kill -9.
total_issues incluye config perdido y temporales zombi.
Repair recrea config si falta o no es cargable.
FIX C5: preserva el UUID original del repo al recrear config.
==========================================================================*/
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "brs_repo_health.h"
#include "brs_repo_internal.h"
#include "brs_buffer.h"
#include "brs_dir.h"
#include "brs_fsutil.h"
#include "brs_pack.h"
#include "brs_manifest.h"
#include "brs_index.h"
#include "brs_config.h"
#include "brs_crypto.h"
#include "brs_init.h"
#include "brs_hash.h"
#include "brs_vfs.h"
#include "brs_vfs_context.h"

static int      g_health_missing_config = 0;
static uint64_t g_health_tmp_zombies    = 0;

/* ============================================================================
Helpers de arrays dinámicos
==========================================================================*/
static int push_u64(uint64_t **arr, size_t *count, size_t *cap, uint64_t v)
{
    if (*count == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        uint64_t *p = (uint64_t *)realloc(*arr, nc * sizeof(uint64_t));
        if (!p) return -1;
        *arr = p;
        *cap = nc;
    }
    (*arr)[(*count)++] = v;
    return 0;
}

static int push_str(char ***arr, size_t *count, size_t *cap, const char *s)
{
    if (!s) return -1;
    if (*count == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        char **p = (char **)realloc(*arr, nc * sizeof(char *));
        if (!p) return -1;
        *arr = p;
        *cap = nc;
    }
    char *dup = strdup(s);
    if (!dup) return -1;
    (*arr)[(*count)++] = dup;
    return 0;
}

static int smart_remove_file(const char *repo_path, const char *sub_dir, const char *file_name)
{
BrsVfs *vfs = brs_vfs_context_get();char rel_path[BRS_PATH_MAX];
if (sub_dir && strlen(sub_dir) > 0) {
    snprintf(rel_path, sizeof rel_path, "%s/%s", sub_dir, file_name);
} else {
    snprintf(rel_path, sizeof rel_path, "%s", file_name);
}
    if (vfs) {
    return brs_vfs_unlink(vfs, rel_path);
}
char full_path[BRS_PATH_MAX];
if (brs_path_join(full_path, sizeof full_path, repo_path, sub_dir) != 0) return -1;
if (brs_path_join(full_path, sizeof full_path, full_path, file_name) != 0) return -1;
return brs_remove_file(full_path);
}


/* ---- FIX: parseo robusto de IDs (uint64_t, sin overflow) ---- */
static int parse_id_from_name(const char *name, const char *ext,
                              uint64_t *out)
{
    if (!name || !ext || !out) return -1;
    size_t ext_len  = strlen(ext);
    size_t name_len = strlen(name);
    if (name_len <= ext_len) return -1;
    if (strcmp(name + name_len - ext_len, ext) != 0) return -1;

    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(name, &end, 10);
    if (errno == ERANGE || end == name || *end != '.') return -1;
    *out = (uint64_t)v;
    return 0;
}

/* ============================================================================
Helpers de config / temporales
==========================================================================*/
static int health_config_loadable(const char *repo_path)
{
    if (!repo_path) return 0;
    BrsRepoConfig cfg;
    brs_repo_config_default(&cfg);
    return (brs_load_config(repo_path, &cfg) == 0);
}

static void health_backup_or_remove_bad_config(const char *repo_path)
{
    if (!repo_path) return;
    BrsVfs *vfs = brs_vfs_context_get();
    if (vfs) {
        (void)brs_vfs_unlink(vfs,"config");
        return;
    }

    char cfg_path[BRS_PATH_MAX];
    char bak_path[BRS_PATH_MAX];

    if (brs_path_join(cfg_path, sizeof cfg_path,
                      repo_path,"config") != 0) {
        return;
    }
    if (!brs_path_exists(cfg_path))
        return;

    if (brs_path_join(bak_path, sizeof bak_path,
                      repo_path,"config.damaged") == 0) {
        if (rename(cfg_path, bak_path) == 0) {
            fprintf(stderr,
                   "  [REPAIR] damaged config moved to config.damaged\n");
            return;
        }
    }
    if (brs_remove_file(cfg_path) == 0) {
        fprintf(stderr,
               "  [REPAIR] damaged config removed\n");
    }
}

static uint64_t health_count_tmp_zombies(const char *repo_path)
{
    if (!repo_path) return 0;
    BrsVfs *vfs = brs_vfs_context_get();
    if (vfs) {
        BrsVfsList lst;
        memset(&lst, 0, sizeof lst);
        if (brs_vfs_list(vfs,"tmp", &lst) != 0)
            return 0;
        uint64_t count = 0;
        for (size_t i = 0; i < lst.count; ++i) {
            const char *name = lst.items[i].name;
            size_t nlen = strlen(name);
            if (nlen > 4 && strcmp(name + nlen - 4,".tmp") == 0)
                count++;
        }
        brs_vfs_list_free(&lst);
        return count;
    }

    char tmp_dir[BRS_PATH_MAX];
    if (brs_path_join(tmp_dir, sizeof tmp_dir,
                      repo_path,"tmp") != 0) {
        return 0;
    }
    BrsDirList list;
    memset(&list, 0, sizeof list);
    if (brs_list_dir(tmp_dir, &list) != 0)
        return 0;
    uint64_t count = 0;
    for (size_t i = 0; i < list.count; ++i) {
        const char *name = list.names[i];
        size_t nlen = strlen(name);
        if (nlen > 4 && strcmp(name + nlen - 4,".tmp") == 0)
            count++;
    }
    brs_dir_list_free(&list);
    return count;
}

static void health_purge_tmp(const char *repo_path)
{
    if (!repo_path) return;
    BrsVfs *vfs = brs_vfs_context_get();
    if (vfs) {
        BrsVfsList lst;
        memset(&lst, 0, sizeof lst);
        if (brs_vfs_list(vfs,"tmp", &lst) != 0)
            return;
        for (size_t i = 0; i < lst.count; ++i) {
            const char *name = lst.items[i].name;
            size_t nlen = strlen(name);
            if (nlen > 4 && strcmp(name + nlen - 4,".tmp") == 0) {
                char rel_path[BRS_PATH_MAX];
                snprintf(rel_path, sizeof rel_path,"tmp/%s", name);
                (void)brs_vfs_unlink(vfs, rel_path);
            }
        }
        brs_vfs_list_free(&lst);
        return;
    }

    char tmp_dir[BRS_PATH_MAX];
    if (brs_path_join(tmp_dir, sizeof tmp_dir,
                      repo_path,"tmp") == 0) {
        (void)brs_remove_files_with_suffix(tmp_dir,".tmp");
    }
}

/* FIX C5: recuperar UUID del repo desde snapshots existentes */
static int health_recover_repo_uuid(const char *repo_path,
                                    uint8_t uuid_out[BRS_UUID_LEN])
{
    if (!repo_path || !uuid_out) return -1;
    char snaps_dir[BRS_PATH_MAX];
    if (brs_path_join(snaps_dir, sizeof snaps_dir,
                      repo_path,"snapshots") != 0) {
        return -1;
    }
    BrsDirList list;
    memset(&list, 0, sizeof list);
    if (brs_list_dir(snaps_dir, &list) != 0)
        return -1;
    brs_dir_list_sort(&list);

    int rc = -1;
    for (size_t i = 0; i < list.count; ++i) {
        size_t ln = strlen(list.names[i]);
        if (ln <= 5 || strcmp(list.names[i] + ln - 5,".snap") != 0)
            continue;
        char sp[BRS_PATH_MAX];
        if (brs_path_join(sp, sizeof sp, snaps_dir, list.names[i]) != 0)
            continue;
        BrsBuffer data;
        brs_buffer_init(&data);
        if (brs_read_file(sp, &data) != 0) {
            brs_buffer_free(&data);
            continue;
        }
        /* Solo snapshots planos (no cifrados) tienen el UUID legible.
         * Offset: magic(8) + version(4) + snapshot_id(16) = 28
         * repo_uuid está en offset 28..43 */
        size_t need = BRS_MAGIC_LEN + 4 + BRS_UUID_LEN + BRS_UUID_LEN;
        if (data.size >= need &&
            memcmp(data.data, BRS_MAGIC_SNAPSHOT, BRS_MAGIC_LEN) == 0) {
            memcpy(uuid_out,
                   data.data + BRS_MAGIC_LEN + 4 + BRS_UUID_LEN,
                   BRS_UUID_LEN);
            rc = 0;
            brs_buffer_free(&data);
            break;
        }
        brs_buffer_free(&data);
    }
    brs_dir_list_free(&list);
    return rc;
}

/* ============================================================================
FIX F1: detección de snapshots cifrados antes de recrear config
==========================================================================*/
static int health_detect_encrypted_snapshots(const char *repo_path)
{
    if (!repo_path) return 0;
    char snaps_dir[BRS_PATH_MAX];
    if (brs_path_join(snaps_dir, sizeof snaps_dir,
                      repo_path, "snapshots") != 0) {
        return 0;
    }
    BrsDirList list;
    memset(&list, 0, sizeof list);
    if (brs_list_dir(snaps_dir, &list) != 0)
        return 0;
    int found = 0;
    for (size_t i = 0; i < list.count; ++i) {
        size_t ln = strlen(list.names[i]);
        if (ln <= 5 || strcmp(list.names[i] + ln - 5, ".snap") != 0)
            continue;
        char sp[BRS_PATH_MAX];
        if (brs_path_join(sp, sizeof sp, snaps_dir, list.names[i]) != 0)
            continue;
        BrsBuffer data;
        brs_buffer_init(&data);
        if (brs_read_file(sp, &data) != 0) {
            brs_buffer_free(&data);
            continue;
        }
        /* Solo el magic cifrado (BRSNAP2...) demuestra que el repo
         * original estaba cifrado. Los snapshots planos no prueban nada
         * (podrían coexistir), por eso basta UN cifrado para marcarlo. */
        if (data.size >= BRS_MAGIC_LEN &&
            memcmp(data.data, BRS_MAGIC_SNAPSHOT_ENC, BRS_MAGIC_LEN) == 0) {
            found = 1;
            brs_buffer_free(&data);
            break;
        }
        brs_buffer_free(&data);
    }
    brs_dir_list_free(&list);
    return found;
}

/* ============================================================================
init/free del informe
==========================================================================*/
void brs_health_report_init(BrsHealthReport *r)
{
    memset(r, 0, sizeof *r);
}

void brs_health_report_free(BrsHealthReport *r)
{
    if (!r) return;
    free(r->orphan_idx);
    free(r->indexless_pack);
    free(r->orphan_blm);
    for (size_t i = 0; i < r->corrupt_snap_count; ++i)
        free(r->corrupt_snaps[i]);
    free(r->corrupt_snaps);
    memset(r, 0, sizeof *r);
}

/* ============================================================================
health check
==========================================================================*/
int brs_health_check(const char *repo_path, BrsHealthReport *report)
{
    if (!repo_path || !report) return -1;
    brs_health_report_init(report);

    g_health_missing_config = 0;
    g_health_tmp_zombies    = 0;

    if (!health_config_loadable(repo_path))
        g_health_missing_config = 1;

    g_health_tmp_zombies = health_count_tmp_zombies(repo_path);

    char idx_dir[BRS_PATH_MAX];
    char packs_dir[BRS_PATH_MAX];
    char snaps_dir[BRS_PATH_MAX];

    if (brs_path_join(idx_dir, sizeof idx_dir, repo_path,"index") != 0)
        return -1;
    if (brs_path_join(packs_dir, sizeof packs_dir, repo_path,"packs") != 0)
        return -1;
    if (brs_path_join(snaps_dir, sizeof snaps_dir, repo_path,"snapshots") != 0)
        return -1;

    BrsIndexMap full_index;
    int full_index_init = 0;
    if (brs_index_map_init(&full_index, 1024) == 0) {
        full_index_init = 1;
        (void)brs_load_all_indexes(repo_path, &full_index);
    }

    BrsU64Set referenced_packs;
    int ref_packs_init = 0;
    if (full_index_init && brs_u64_set_init(&referenced_packs, 64) == 0) {
        ref_packs_init = 1;
        size_t cursor = 0;
        const BrsIndexSlot *slot;
        while ((slot = brs_index_map_next(&full_index, &cursor)) != NULL) {
            brs_u64_set_insert(&referenced_packs, slot->value.pack_id);
        }
    }

    /* ================================================================
    Index segments huérfanos
    ================================================================= */
    BrsDirList idx_list;
    if (brs_list_dir(idx_dir, &idx_list) == 0) {
        for (size_t i = 0; i < idx_list.count; ++i) {
            uint64_t seg_id = 0;
            if (parse_id_from_name(idx_list.names[i],".idx", &seg_id) != 0)
                continue;

            char pack_name[64];
            char pack_path[BRS_PATH_MAX];
            snprintf(pack_name, sizeof pack_name,
                    "%llu.pack", (unsigned long long)seg_id);
            int found_by_name = 0;
            if (brs_path_join(pack_path, sizeof pack_path,
                              packs_dir, pack_name) == 0) {
                found_by_name = brs_path_exists(pack_path);
            }

            if (!found_by_name) {
                BrsIndexMap seg_map;
                int has_valid_ref = 0;
                if (brs_index_map_init(&seg_map, 16) == 0) {
                    if (brs_load_index_segment(repo_path, seg_id,
                                               &seg_map) == 0) {
                        size_t cur = 0;
                        const BrsIndexSlot *s;
                        while ((s = brs_index_map_next(&seg_map,
                                                       &cur)) != NULL) {
                            char pp[BRS_PATH_MAX];
                            char pname[64];
                            snprintf(pname, sizeof pname,
                                    "%llu.pack",
                                     (unsigned long long)s->value.pack_id);
                            if (brs_path_join(pp, sizeof pp,
                                              packs_dir, pname) == 0 &&
                                brs_path_exists(pp)) {
                                has_valid_ref = 1;
                                break;
                            }
                        }
                    }
                    brs_index_map_free(&seg_map);
                }
                if (!has_valid_ref) {
                    push_u64(&report->orphan_idx,
                             &report->orphan_idx_count,
                             &report->orphan_idx_cap,
                             seg_id);
                }
            }
        }
        brs_dir_list_free(&idx_list);
    }

    /* ================================================================
    Packs sin index segment
    ================================================================= */
    BrsDirList pack_list;
    if (brs_list_dir(packs_dir, &pack_list) == 0) {
        for (size_t i = 0; i < pack_list.count; ++i) {
            uint64_t pack_id = 0;
            if (parse_id_from_name(pack_list.names[i],".pack", &pack_id) != 0)
                continue;

            if (ref_packs_init &&
                brs_u64_set_contains(&referenced_packs, pack_id)) {
                continue;
            }

            char pp[BRS_PATH_MAX];
            if (brs_path_join(pp, sizeof pp,
                              packs_dir, pack_list.names[i]) != 0) {
                continue;
            }
            BrsBuffer pdata;
            brs_buffer_init(&pdata);
            if (brs_read_file(pp, &pdata) != 0) {
                brs_buffer_free(&pdata);
                push_u64(&report->indexless_pack,
                         &report->indexless_pack_count,
                         &report->indexless_pack_cap,
                         pack_id);
                continue;
            }
            uint64_t pid;
            BrsPackEntry *entries;
            uint32_t count;
            if (brs_parse_pack(pdata.data, pdata.size,
                               &pid, &entries, &count) != 0) {
                brs_buffer_free(&pdata);
                push_u64(&report->indexless_pack,
                         &report->indexless_pack_count,
                         &report->indexless_pack_cap,
                         pack_id);
                continue;
            }
            int any_chunk_indexed = 0;
            if (full_index_init) {
                for (uint32_t e = 0; e < count; ++e) {
                    if (brs_index_map_get(&full_index,
                                          &entries[e].chunk_id) != NULL) {
                        any_chunk_indexed = 1;
                        break;
                    }
                }
            }
            free(entries);
            brs_buffer_free(&pdata);
            if (!any_chunk_indexed) {
                push_u64(&report->indexless_pack,
                         &report->indexless_pack_count,
                         &report->indexless_pack_cap,
                         pack_id);
            }
        }
        brs_dir_list_free(&pack_list);
    }

    /* ================================================================
    Bloom filters sin index segment
    ================================================================= */
    BrsDirList blm_list;
    if (brs_list_dir(idx_dir, &blm_list) == 0) {
        for (size_t i = 0; i < blm_list.count; ++i) {
            uint64_t blm_id = 0;
            if (parse_id_from_name(blm_list.names[i],".blm",
                                   &blm_id) != 0)
                continue;

            char idx_name[64];
            char idx_path[BRS_PATH_MAX];
            snprintf(idx_name, sizeof idx_name,
                    "%llu.idx", (unsigned long long)blm_id);
            if (brs_path_join(idx_path, sizeof idx_path,
                              idx_dir, idx_name) == 0) {
                if (!brs_path_exists(idx_path)) {
                    push_u64(&report->orphan_blm,
                             &report->orphan_blm_count,
                             &report->orphan_blm_cap,
                             blm_id);
                }
            }
        }
        brs_dir_list_free(&blm_list);
    }

    /* ================================================================
    open_pack_id huérfano
    ================================================================= */
    uint64_t open_id = 0;
    if (brs_read_open_pack_id(repo_path, &open_id) == 0) {
        char pack_name[64];
        char pack_path[BRS_PATH_MAX];
        snprintf(pack_name, sizeof pack_name,
                "%llu.pack", (unsigned long long)open_id);
        if (brs_path_join(pack_path, sizeof pack_path,
                          packs_dir, pack_name) == 0) {
            if (!brs_path_exists(pack_path)) {
                report->orphan_open_pack = 1;
                report->open_pack_id = open_id;
            }
        }
    }

    /* 5. Verificar chunks de snapshots contra el index */
    if (!g_health_missing_config) {
        BrsRepoConfig cfg;
        BrsSecureKey key;
        int key_valid = 0;
        brs_repo_config_default(&cfg);
        int has_cfg = (brs_load_config(repo_path, &cfg) == 0);

        if (has_cfg && cfg.encrypted) {
            if (derive_repo_key(&cfg, &key) == 0)
                key_valid = 1;
        }

        BrsU64Set packs_ok;
        BrsU64Set packs_bad;
        brs_u64_set_init(&packs_ok, 64);
        brs_u64_set_init(&packs_bad, 64);

        BrsDirList snap_list;
        if (brs_list_dir(snaps_dir, &snap_list) == 0) {
            for (size_t i = 0; i < snap_list.count; ++i) {
                size_t ln = strlen(snap_list.names[i]);
                if (ln <= 5 ||
                    strcmp(snap_list.names[i] + ln - 5,".snap") != 0) {
                    continue;
                }
                char sp[BRS_PATH_MAX];
                if (brs_path_join(sp, sizeof sp,
                                  snaps_dir, snap_list.names[i]) != 0) {
                    continue;
                }
                BrsBuffer sdata;
                brs_buffer_init(&sdata);
                if (brs_read_file(sp, &sdata) != 0) {
                    brs_buffer_free(&sdata);
                    continue;
                }
                BrsParsedSnapshot snap;
                brs_parsed_snapshot_init(&snap);
                int use_key = (has_cfg && cfg.encrypted && key_valid);
                if (brs_parse_snapshot(sdata.data, sdata.size, &snap,
                                       use_key ? &key : NULL, (has_cfg ? cfg.cipher_algo : BRS_CIPHER_CHACHA20_POLY1305)) != 0) {
                    report->affected_snapshots++;
                    (void)push_str(&report->corrupt_snaps,
                                   &report->corrupt_snap_count,
                                   &report->corrupt_snap_cap,
                                   snap_list.names[i]);
                    brs_parsed_snapshot_free(&snap);
                    brs_buffer_free(&sdata);
                    continue;
                }

                int snap_has_issue = 0;
                for (uint64_t e = 0; e < snap.entries_len; ++e) {
                    const BrsManifestEntry *se = &snap.entries[e];
                    if (se->type != BRS_FILETYPE_FILE) continue;
                    if (se->flags & BRS_FLAG_HARDLINK) continue;

                    for (uint32_t c = 0; c < se->chunk_count; ++c) {
                        const BrsChunkLocation *loc =
                            full_index_init ?
                            brs_index_map_get(&full_index, &se->chunks[c]) :
                            NULL;
                        if (!loc) {
                            report->missing_index_chunks++;
                            snap_has_issue = 1;
                            continue;
                        }
                        if (brs_u64_set_contains(&packs_bad, loc->pack_id)) {
                            report->missing_pack_chunks++;
                            snap_has_issue = 1;
                            continue;
                        }
                        if (!brs_u64_set_contains(&packs_ok, loc->pack_id)) {
                            char pp2[BRS_PATH_MAX];
                            char pname[40];
                            snprintf(pname, sizeof pname,
                                    "%llu.pack",
                                     (unsigned long long)loc->pack_id);
                            if (brs_path_join(pp2, sizeof pp2,
                                              packs_dir, pname) == 0 &&
                                brs_path_exists(pp2)) {
                                brs_u64_set_insert(&packs_ok, loc->pack_id);
                            } else {
                                brs_u64_set_insert(&packs_bad, loc->pack_id);
                                report->missing_pack_chunks++;
                                snap_has_issue = 1;
                            }
                        }
                    }

                    if (se->flags & BRS_FLAG_DELTA) {
                        for (uint32_t c = 0;
                             c < se->delta_source_count;
                             ++c) {
                            const BrsChunkLocation *loc =
                                full_index_init ?
                                brs_index_map_get(&full_index,
                                                  &se->delta_source_chunks[c]) :
                                NULL;
                            if (!loc) {
                                report->missing_index_chunks++;
                                snap_has_issue = 1;
                                continue;
                            }
                            if (brs_u64_set_contains(&packs_bad,
                                                     loc->pack_id)) {
                                report->missing_pack_chunks++;
                                snap_has_issue = 1;
                                continue;
                            }
                            if (!brs_u64_set_contains(&packs_ok,
                                                      loc->pack_id)) {
                                char pp2[BRS_PATH_MAX];
                                char pname[40];
                                snprintf(pname, sizeof pname,
                                        "%llu.pack",
                                         (unsigned long long)loc->pack_id);
                                if (brs_path_join(pp2, sizeof pp2,
                                                  packs_dir, pname) == 0 &&
                                    brs_path_exists(pp2)) {
                                    brs_u64_set_insert(&packs_ok,
                                                       loc->pack_id);
                                } else {
                                    brs_u64_set_insert(&packs_bad,
                                                       loc->pack_id);
                                    report->missing_pack_chunks++;
                                    snap_has_issue = 1;
                                }
                            }
                        }
                    }
                }

                if (snap_has_issue) {
                    report->affected_snapshots++;
                    (void)push_str(&report->corrupt_snaps,
                                   &report->corrupt_snap_count,
                                   &report->corrupt_snap_cap,
                                   snap_list.names[i]);
                }
                brs_parsed_snapshot_free(&snap);
                brs_buffer_free(&sdata);
            }
            brs_dir_list_free(&snap_list);
        }
        brs_u64_set_free(&packs_ok);
        brs_u64_set_free(&packs_bad);
        if (key_valid)
            brs_secure_key_wipe(&key);
    }

    /* 6. Verificar CRC32C de cada pack */
    {
        BrsDirList plist;
        if (brs_list_dir(packs_dir, &plist) == 0) {
            for (size_t i = 0; i < plist.count; ++i) {
                size_t ln = strlen(plist.names[i]);
                if (ln <= 5 ||
                    strcmp(plist.names[i] + ln - 5,".pack") != 0) {
                    continue;
                }
                char pp[BRS_PATH_MAX];
                if (brs_path_join(pp, sizeof pp,
                                  packs_dir, plist.names[i]) != 0) {
                    continue;
                }
                BrsBuffer pdata;
                brs_buffer_init(&pdata);
                if (brs_read_file(pp, &pdata) != 0) {
                    brs_buffer_free(&pdata);
                    continue;
                }
                if (pdata.size >= 8) {
                    BrsReader cr;
                    brs_reader_init(&cr,
                                    pdata.data + pdata.size - 8,
                                    8);
                    uint64_t stored = 0;
                    if (brs_reader_u64_le(&cr, &stored) == 0 &&
                        stored != 0) {
                        uint32_t computed =
                            brs_crc32c(pdata.data, pdata.size - 8);
                        if ((uint64_t)computed != stored) {
                            report->missing_pack_chunks++;
                        }
                    }
                }
                brs_buffer_free(&pdata);
            }
            brs_dir_list_free(&plist);
        }
    }

    if (ref_packs_init)
        brs_u64_set_free(&referenced_packs);
    if (full_index_init)
        brs_index_map_free(&full_index);

    report->total_issues =
        report->orphan_idx_count +
        report->indexless_pack_count +
        report->orphan_blm_count +
        (report->orphan_open_pack ? 1 : 0) +
        report->corrupt_snap_count +
        report->missing_index_chunks +
        report->missing_pack_chunks +
        (size_t)g_health_tmp_zombies +
        (g_health_missing_config ? 1 : 0);

    return 0;
}

/* ============================================================================
imprimir informe
==========================================================================*/
void brs_health_print_report(const BrsHealthReport *report)
{
    if (!report || report->total_issues == 0)
        return;

    fprintf(stderr,"\n");
    fprintf(stderr,"=== ANOMALIES DETECTED IN REPOSITORY ===\n");

    if (g_health_missing_config) {
        fprintf(stderr,
               "  [1] config missing or unreadable (repository metadata lost)\n");
    }
    if (g_health_tmp_zombies > 0) {
        fprintf(stderr,
               "  [%llu] zombie temporaries in tmp/ (will be purged with --repair)\n",
                (unsigned long long)g_health_tmp_zombies);
    }
    if (report->orphan_idx_count > 0) {
        fprintf(stderr,
               "  [%zu] index segment(s) sin pack (orphan idx):\n",
                report->orphan_idx_count);
        for (size_t i = 0; i < report->orphan_idx_count; ++i) {
            fprintf(stderr,"        %llu.idx\n",
                    (unsigned long long)report->orphan_idx[i]);
        }
    }
    if (report->indexless_pack_count > 0) {
        fprintf(stderr,
               "  [%zu] pack(s) without index segment:\n",
                report->indexless_pack_count);
        for (size_t i = 0; i < report->indexless_pack_count; ++i) {
            fprintf(stderr,"        %llu.pack\n",
                    (unsigned long long)report->indexless_pack[i]);
        }
    }
    if (report->orphan_blm_count > 0) {
        fprintf(stderr,
               "  [%zu] bloom filter(s) without index:\n",
                report->orphan_blm_count);
        for (size_t i = 0; i < report->orphan_blm_count; ++i) {
            fprintf(stderr,"        %llu.blm\n",
                    (unsigned long long)report->orphan_blm[i]);
        }
    }
    if (report->orphan_open_pack) {
        fprintf(stderr,
               "  [1] open_pack_id points to nonexistent pack: %llu\n",
                (unsigned long long)report->open_pack_id);
    }
    if (report->missing_index_chunks > 0) {
        fprintf(stderr,
               "  [%llu] snapshot chunk(s) NOT in any index\n",
                (unsigned long long)report->missing_index_chunks);
    }
    if (report->missing_pack_chunks > 0) {
        fprintf(stderr,
               "  [%llu] chunk(s) pointing to packs that DO NOT exist\n",
                (unsigned long long)report->missing_pack_chunks);
    }
    if (report->corrupt_snap_count > 0) {
        fprintf(stderr,
               "  [%zu] corrupt snapshot(s) (parse fail or lost chunks):\n",
                report->corrupt_snap_count);
        for (size_t i = 0; i < report->corrupt_snap_count; ++i) {
            fprintf(stderr,"        %s\n", report->corrupt_snaps[i]);
        }
    }
    if (report->affected_snapshots > 0) {
        fprintf(stderr,
               "  [%llu] snapshot(s) affected in total\n",
                (unsigned long long)report->affected_snapshots);
    }

    fprintf(stderr,
           "  Total: %zu anomalie(s)\n",
            report->total_issues);
    fprintf(stderr,"=== END ANOMALIES ===\n");
}

/* ============================================================================
reparación
==========================================================================*/
int brs_health_repair(const char *repo_path, BrsHealthReport *report)
{
    if (!repo_path || !report) return -1;

    int errors = 0;

    /* Índice reconstruido — vive durante toda la función */
    BrsIndexMap missing_index;
    int missing_index_init = 0;
    int missing_index_written = 0;

    /* Purgar temporales zombi */
    health_purge_tmp(repo_path);
    g_health_tmp_zombies = 0;

    /* FIX C5 / FIX F1: recuperar config perdido */
    if (!health_config_loadable(repo_path)) {
        uint8_t recovered_uuid[BRS_UUID_LEN];
        int have_uuid = (health_recover_repo_uuid(repo_path, recovered_uuid) == 0);
        int was_encrypted = health_detect_encrypted_snapshots(repo_path);

        fprintf(stderr,
                "  [REPAIR] config missing or unreadable; recreating repository config\n");
        if (was_encrypted) {
            fprintf(stderr,
                    "  [REPAIR] WARNING: encrypted snapshots detected; "
                    "recreated config will be marked encrypted\n");
        }

        health_backup_or_remove_bad_config(repo_path);

        if (brs_repo_init(repo_path, 0, 0, 0, 3, 0) != 0) {
            fprintf(stderr, "  [REPAIR] ERROR: cannot recreate missing config\n");
            errors++;
        } else {
            g_health_missing_config = 0;
            if (have_uuid || was_encrypted) {
                BrsRepoConfig cfg;
                brs_repo_config_default(&cfg);
                if (brs_load_config(repo_path, &cfg) == 0) {
                    if (have_uuid)
                        memcpy(cfg.uuid, recovered_uuid, BRS_UUID_LEN);
                    if (was_encrypted)
                        cfg.encrypted = 1;
                    if (brs_write_config(repo_path, &cfg) != 0) {
                        fprintf(stderr,
                                "  [REPAIR] ERROR: cannot persist recovered config attributes\n");
                        errors++;
                    }
                } else {
                    fprintf(stderr, "  [REPAIR] ERROR: cannot reload recreated config\n");
                    errors++;
                }
            }
        }
    }

    char idx_dir[BRS_PATH_MAX];
    char packs_dir[BRS_PATH_MAX];
    if (brs_path_join(idx_dir, sizeof idx_dir, repo_path, "index") != 0)
        return -1;
    if (brs_path_join(packs_dir, sizeof packs_dir, repo_path, "packs") != 0)
        return -1;

    /* 1. Borrar index segments huérfanos + sus blooms */
    for (size_t i = 0; i < report->orphan_idx_count; ++i) {
        char n_idx[40];
        char n_blm[40];
        snprintf(n_idx, sizeof n_idx, "%llu.idx",
                 (unsigned long long)report->orphan_idx[i]);
        snprintf(n_blm, sizeof n_blm, "%llu.blm",
                 (unsigned long long)report->orphan_idx[i]);

        if (smart_remove_file(repo_path, "index", n_idx) != 0) {
            errors++;
        } else {
            fprintf(stderr, "   [REPAIR] removed index %s\n", n_idx);
        }
        if (smart_remove_file(repo_path, "index", n_blm) != 0) {
            errors++;
        } else {
            fprintf(stderr, "   [REPAIR] removed bloom %s\n", n_blm);
        }
    }

    /* ================================================================
    2. Reconstruir chunks faltantes desde TODOS los packs
    ================================================================ */
    {
        BrsIndexMap full_index;
        if (brs_index_map_init(&full_index, 1024) == 0) {
            (void)brs_load_all_indexes(repo_path, &full_index);

            if (brs_index_map_init(&missing_index, 1024) == 0) {
                missing_index_init = 1;

                BrsDirList pack_list;
                if (brs_list_dir(packs_dir, &pack_list) == 0) {
                    for (size_t i = 0; i < pack_list.count; ++i) {
                        uint64_t pack_id = 0;
                        if (parse_id_from_name(pack_list.names[i], ".pack",
                                               &pack_id) != 0)
                            continue;

                        char pp[BRS_PATH_MAX];
                        if (brs_path_join(pp, sizeof pp,
                                          packs_dir, pack_list.names[i]) != 0)
                            continue;

                        BrsBuffer pdata;
                        brs_buffer_init(&pdata);
                        if (brs_read_file(pp, &pdata) != 0) {
                            brs_buffer_free(&pdata);
                            continue;
                        }

                        uint64_t pid;
                        BrsPackEntry *entries;
                        uint32_t count;
                        if (brs_parse_pack(pdata.data, pdata.size,
                                           &pid, &entries, &count) == 0) {
                            for (uint32_t e = 0; e < count; ++e) {
                                if (!brs_index_map_get(&full_index,
                                                       &entries[e].chunk_id)) {
                                    BrsChunkLocation loc;
                                    loc.pack_id = pid;
                                    loc.offset = entries[e].offset;
                                    loc.comp_size = entries[e].comp_size;
                                    loc.uncomp_size = entries[e].uncomp_size;
                                    loc.flags = entries[e].flags;
                                    brs_index_map_put(&missing_index,
                                                      &entries[e].chunk_id, &loc);
                                }
                            }
                            free(entries);
                        } else {
                            fprintf(stderr,
                                    "   [REPAIR] Pack corrupto detectado: %s\n",
                                    pack_list.names[i]);
                            if (smart_remove_file(repo_path, "packs",
                                                  pack_list.names[i]) != 0) {
                                errors++;
                            } else {
                                fprintf(stderr,
                                        "   [REPAIR] Pack corrupto eliminado.\n");
                            }
                        }
                        brs_buffer_free(&pdata);
                    }
                    brs_dir_list_free(&pack_list);
                }

                uint64_t missing_count = brs_index_map_count(&missing_index);
                if (missing_count > 0) {
                    uint64_t new_seg_id = brs_now_ns();
                    if (brs_write_index_segment(repo_path, new_seg_id,
                                                &missing_index) == 0) {
                        fprintf(stderr,
                                "  [REPAIR] reconstruido index con %llu "
                                "chunks faltantes (segment %llu)\n",
                                (unsigned long long)missing_count,
                                (unsigned long long)new_seg_id);
                        missing_index_written = 1;
                    } else {
                        fprintf(stderr,
                                "  [REPAIR] ERROR: no se pudo escribir "
                                "el index reconstruido\n");
                        errors++;
                    }
                }
                /* NO liberar missing_index aquí; se necesita en la sección 4.5 */
            }
            brs_index_map_free(&full_index);
        }
    }

    /* 3. Borrar blooms huérfanos */
    for (size_t i = 0; i < report->orphan_blm_count; ++i) {
        char name[40];
        char path[BRS_PATH_MAX];
        snprintf(name, sizeof name, "%llu.blm",
                 (unsigned long long)report->orphan_blm[i]);
        if (brs_path_join(path, sizeof path, idx_dir, name) == 0) {
            if (brs_remove_file(path) != 0) {
                errors++;
            } else {
                fprintf(stderr, "  [REPAIR] removed %s\n", name);
            }
        }
    }

    /* 4. Borrar open_pack_id huérfano */
    if (report->orphan_open_pack) {
        if (brs_remove_open_pack_id(repo_path) != 0) {
            errors++;
        } else {
            fprintf(stderr,
                    "  [REPAIR] removed open_pack_id (pointed to pack %llu)\n",
                    (unsigned long long)report->open_pack_id);
        }
    }

    /* 4.5. Re-verificar snapshots tras reconstruir index */
    if (report->corrupt_snap_count > 0) {
        char snaps_dir_rv[BRS_PATH_MAX];
        char packs_dir_rv[BRS_PATH_MAX];

        if (brs_path_join(snaps_dir_rv, sizeof snaps_dir_rv,
                          repo_path, "snapshots") == 0 &&
            brs_path_join(packs_dir_rv, sizeof packs_dir_rv,
                          repo_path, "packs") == 0) {

            BrsIndexMap updated_idx;
            if (brs_index_map_init(&updated_idx, 1024) == 0) {
                (void)brs_load_all_indexes(repo_path, &updated_idx);

                /* FIX: fusionar el index reconstruido */
                if (missing_index_written && missing_index_init) {
                    size_t ri_cursor = 0;
                    const BrsIndexSlot *ri_slot;
                    while ((ri_slot = brs_index_map_next(&missing_index,
                                                         &ri_cursor)) != NULL) {
                        (void)brs_index_map_put(&updated_idx,
                                                &ri_slot->key, &ri_slot->value);
                    }
                }

                BrsRepoConfig rv_cfg;
                BrsSecureKey rv_key;
                int rv_key_valid = 0;
                brs_repo_config_default(&rv_cfg);
                int rv_has_cfg = (brs_load_config(repo_path, &rv_cfg) == 0);
                if (rv_has_cfg && rv_cfg.encrypted) {
                    if (derive_repo_key(&rv_cfg, &rv_key) == 0)
                        rv_key_valid = 1;
                }

                size_t still = 0;
                for (size_t i = 0; i < report->corrupt_snap_count; ++i) {
                    char *snap_name = report->corrupt_snaps[i];
                    if (!snap_name) continue;

                    char sp[BRS_PATH_MAX];
                    if (brs_path_join(sp, sizeof sp,
                                      snaps_dir_rv, snap_name) != 0)
                        continue;

                    BrsBuffer sd;
                    brs_buffer_init(&sd);
                    if (brs_read_file(sp, &sd) != 0) {
                        brs_buffer_free(&sd);
                        continue;
                    }

                    BrsParsedSnapshot ps;
                    brs_parsed_snapshot_init(&ps);
                    int use_key = (rv_has_cfg && rv_cfg.encrypted && rv_key_valid);

                    if (brs_parse_snapshot(sd.data, sd.size, &ps,
                                           use_key ? &rv_key : NULL,
                                           (rv_has_cfg ? rv_cfg.cipher_algo
                                                       : BRS_CIPHER_CHACHA20_POLY1305)) != 0) {
                        if (still != i) {
                            report->corrupt_snaps[still] = snap_name;
                            report->corrupt_snaps[i] = NULL;
                        }
                        still++;
                        brs_parsed_snapshot_free(&ps);
                        brs_buffer_free(&sd);
                        continue;
                    }

                    int ok = 1;
                    for (uint64_t e = 0; e < ps.entries_len && ok; ++e) {
                        const BrsManifestEntry *se = &ps.entries[e];
                        if (se->type != BRS_FILETYPE_FILE) continue;
                        if (se->flags & BRS_FLAG_HARDLINK) continue;

                        for (uint32_t c = 0; c < se->chunk_count; ++c) {
                            const BrsChunkLocation *loc =
                                brs_index_map_get(&updated_idx, &se->chunks[c]);
                            if (!loc) {
                                ok = 0;
                                break;
                            }
                            /* Verificar que el pack existe */
                            char pack_path[BRS_PATH_MAX];
                            char pack_name[64];
                            snprintf(pack_name, sizeof pack_name, "%llu.pack",
                                     (unsigned long long)loc->pack_id);
                            if (brs_path_join(pack_path, sizeof pack_path,
                                              packs_dir_rv, pack_name) != 0 ||
                                !brs_path_exists(pack_path)) {
                                ok = 0;
                                break;
                            }
                        }

                        if (ok && (se->flags & BRS_FLAG_DELTA)) {
                            for (uint32_t c = 0; c < se->delta_source_count; ++c) {
                                if (!brs_index_map_get(&updated_idx,
                                                       &se->delta_source_chunks[c])) {
                                    ok = 0;
                                    break;
                                }
                            }
                        }
                    }

                    if (!ok) {
                        if (still != i) {
                            report->corrupt_snaps[still] = snap_name;
                            report->corrupt_snaps[i] = NULL;
                        }
                        still++;
                    } else {
                        free(snap_name);
                        report->corrupt_snaps[i] = NULL;
                    }

                    brs_parsed_snapshot_free(&ps);
                    brs_buffer_free(&sd);
                }

                report->corrupt_snap_count = still;

                if (rv_key_valid)
                    brs_secure_key_wipe(&rv_key);

                brs_index_map_free(&updated_idx);
            }
        }
    }

    /* 5. Mover snapshots corruptos a damaged/ */
    if (report->corrupt_snap_count > 0) {
        char snaps_dir[BRS_PATH_MAX];
        char damaged_dir[BRS_PATH_MAX];
        if (brs_path_join(snaps_dir, sizeof snaps_dir,
                          repo_path, "snapshots") == 0 &&
            brs_path_join(damaged_dir, sizeof damaged_dir,
                          repo_path, "damaged") == 0) {

            brs_mkdir_p(damaged_dir);
            size_t moved = 0;

            for (size_t i = 0; i < report->corrupt_snap_count; ++i) {
                if (!report->corrupt_snaps[i]) continue;

                char src[BRS_PATH_MAX];
                char dst[BRS_PATH_MAX];
                if (brs_path_join(src, sizeof src,
                                  snaps_dir, report->corrupt_snaps[i]) != 0)
                    continue;
                if (brs_path_join(dst, sizeof dst,
                                  damaged_dir, report->corrupt_snaps[i]) != 0)
                    continue;

                if (rename(src, dst) == 0) {
                    fprintf(stderr, "  [REPAIR] moved to damaged/: %s\n",
                            report->corrupt_snaps[i]);
                    moved++;
                } else {
                    fprintf(stderr, "  [REPAIR] ERROR moving %s: %s\n",
                            report->corrupt_snaps[i], strerror(errno));
                    errors++;
                }
            }

            if (moved > 0) {
                fprintf(stderr,
                        "\n============================================\n"
                        "  WARNING: %zu corrupt snapshot(s) moved\n"
                        "  Location: %s\n"
                        "  These snapshots have unrecoverable chunks.\n"
                        "  They have NOT been deleted. To remove them:\n"
                        "      rm -rf \"%s\"\n"
                        "  ============================================\n",
                        moved, damaged_dir, damaged_dir);
            }
        }
    }

    /* 6. Aviso sobre datos irrecuperables */
    if (report->missing_index_chunks > 0 || report->missing_pack_chunks > 0) {
        fprintf(stderr,
                "  [WARNING] %llu chunk(s) without index and %llu chunk(s) without pack.\n"
                "  This data is LOST. The only solution is to recreate\n"
                "  the backup of the affected files.\n",
                (unsigned long long)report->missing_index_chunks,
                (unsigned long long)report->missing_pack_chunks);
    }

    /* Limpieza del índice reconstruido */
    if (missing_index_init)
        brs_index_map_free(&missing_index);

    if (errors == 0) {
        fprintf(stderr, "  [REPAIR] repair completed\n");
    } else {
        fprintf(stderr, "  [REPAIR] %d error(s) during repair\n", errors);
    }

    return errors == 0 ? 0 : -1;
}
/* ============================================================================
preguntar al usuario
==========================================================================*/
__attribute__((unused)) int brs_health_ask_repair(int use_default)
{
    if (!isatty(STDIN_FILENO))
        return use_default;

    fprintf(stderr,"Repair the detected anomalies? [y/n]");
    fflush(stderr);
    char buf[16];
    if (!fgets(buf, sizeof buf, stdin))
        return use_default;

    return (buf[0] == 's' || buf[0] == 'S' ||
            buf[0] == 'y' || buf[0] == 'Y') ? 1 : 0;
}
