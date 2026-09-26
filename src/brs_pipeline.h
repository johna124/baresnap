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
/* brs_pipeline.h — tipos del pipeline create (FileTask / ChunkItem) */
#ifndef BRS_PIPELINE_H
#define BRS_PIPELINE_H

#include <stdint.h>
#include "brs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t entry_index;   /* indice dentro del array de entries */
    char *path;             /* path absoluto (dinamico) */
    uint64_t size;
} BrsFileTask;

typedef struct {
    uint32_t buffer_slot;   /* indice en el BufferPool */
    uint32_t size;          /* tamano real del chunk */
    uint32_t entry_index;
    BrsChunkId id;
} BrsChunkItem;

#define BRS_SENTINEL_SLOT UINT32_MAX

#ifdef __cplusplus
}
#endif

#endif /* BRS_PIPELINE_H */