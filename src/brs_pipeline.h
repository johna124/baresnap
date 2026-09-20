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