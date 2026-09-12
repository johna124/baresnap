/* brs_pack.h — PackWriter (streaming puro remoto o tmp + rename atómico local)
 * y parseo de packs.
 */

#ifndef BRS_PACK_H
#define BRS_PACK_H

#include <stddef.h>
#include <stdint.h>

#include "brs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration para evitar dependencias circulares con brs_vfs.h */
struct BrsVfsFile;

typedef struct {
    char repo_path[BRS_PATH_MAX];

    /*
     * En modo local: ruta temporal local dentro de repo/tmp.
     * En modo remoto: ruta relativa del pack remoto, usada solo para limpieza.
     */
    char tmp_path[BRS_PATH_MAX];

    int fd;              /* Descriptor local. */
    void *remote;        /* Puntero opaco a BrsVfsFile* en modo streaming remoto. */

    uint64_t pack_id;
    uint64_t current_offset;
    uint32_t crc_state;  /* CRC32C incremental, inicia 0xFFFFFFFF. */

    BrsPackEntry *entries;
    uint32_t entry_count;
    uint32_t entry_cap;
    BrsIndexMap *index_map;
} BrsPackWriter;

/* pack_id == 0 genera uno basado en brs_now_ns(). */
int brs_pack_writer_init(BrsPackWriter *w, const char *repo_path,
                         uint64_t pack_id);

int brs_pack_writer_add_chunk(BrsPackWriter *w, const BrsChunkId *id,
                              const uint8_t *data, size_t size,
                              uint64_t uncomp_size, uint8_t flags);

/* Para reescritura en prune: copia data y entries de un pack existente. */
int brs_pack_writer_open_for_append(BrsPackWriter *w,
                                    const uint8_t *existing_data,
                                    const BrsPackEntry *existing_entries,
                                    uint32_t existing_count,
                                    size_t data_start, size_t data_end);

uint64_t brs_pack_writer_current_size(const BrsPackWriter *w);

/*
 * Escribe footer + CRC.
 * En remoto: lo emite por el túnel VFS/SSH y cierra el stream.
 * En local: cierra y renombra tmp -> packs/<id>.pack.
 * out_entries pasa a ser del llamador (free).
 */
int brs_pack_writer_finalize(BrsPackWriter *w, BrsPackEntry **out_entries,
                             uint32_t *out_count, uint64_t *out_pack_id);

void brs_pack_writer_abort(BrsPackWriter *w);

/* out_entries es malloc: el llamador hace free. */
int brs_parse_pack(const uint8_t *data, size_t size,
                   uint64_t *out_pack_id,
                   BrsPackEntry **out_entries, uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* BRS_PACK_H */
