#ifndef BRS_REMOTE_V3_H
#define BRS_REMOTE_V3_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
   Protocolo v3: operaciones de alto nivel sobre el repositorio.
   Coexiste con el protocolo v2 (compatibilidad hacia atrás).

   FIX v2.3.0: Opcodes migrados de 0x20-0x3F a 0x40-0x5F para
   eliminar colisión con Smart SSH Protocol v2 (0x20-0x30).
   Respuestas migradas de 0xA0-0xBF a 0xC0-0xDF.
   ============================================================ */

/* Mensajes v3 (rango 0x40-0x5F, sin colisión con v2) */
#define BRS3_MSG_GET_REPO_STATE    0x40
#define BRS3_MSG_PUT_REPO_UPDATE   0x41
#define BRS3_MSG_GET_PACK_DATA     0x42
#define BRS3_MSG_PUT_PACK_DATA     0x43
#define BRS3_MSG_LIST_DIR          0x44
#define BRS3_MSG_GET_FILE          0x45
#define BRS3_MSG_PUT_FILE          0x46
#define BRS3_MSG_DELETE_FILE       0x47
#define BRS3_MSG_MKDIR             0x48
#define BRS3_MSG_RENAME            0x49
#define BRS3_MSG_STAT              0x4A

/* Respuestas v3 (rango 0xC0-0xDF) */
#define BRS3_RSP_REPO_STATE        0xC0
#define BRS3_RSP_PACK_DATA         0xC1
#define BRS3_RSP_FILE_DATA         0xC2
#define BRS3_RSP_SNAPSHOTS         0xC3

/* Formato del blob de GET_REPO_STATE:
   [4 bytes: config_size][config_size bytes: config]
   [4 bytes: index_size][index_size bytes: todos los .idx concatenados]
   [4 bytes: blooms_size][blooms_size bytes: todos los .blm concatenados]
   [4 bytes: cache_size][cache_size bytes: cache file]
   [4 bytes: snapshots_size][snapshots_size bytes: lista de snapshots]

   Formato de cada archivo concatenado:
   [4 bytes: name_size][name_size bytes: nombre]
   [4 bytes: data_size][data_size bytes: contenido]
*/

/* Formato del blob de PUT_REPO_UPDATE:
   [4 bytes: index_segment_size][data: contenido del .idx]
   [4 bytes: cache_size][data: contenido del cache]
   [4 bytes: snapshot_size][data: contenido del snapshot]
   [4 bytes: open_pack_id_size][data: contenido del open_pack_id]
   [4 bytes: num_extra_files]
   Por cada extra: [4 bytes: name_size][name][4 bytes: data_size][data]
*/

/* Formato de GET_PACK_DATA request:
   [8 bytes: pack_id]
*/

/* Formato de PUT_PACK_DATA request:
   [8 bytes: pack_id]
   [4 bytes: data_size]
   [data_size bytes: contenido del pack]
*/

/* Formato de LIST_DIR / GET_FILE / PUT_FILE / DELETE_FILE / MKDIR / RENAME / STAT:
   [4 bytes: path_size][path_size bytes: path]
   (RENAME tiene dos paths)
*/

/* Formato de STAT response:
   [8 bytes: size]
   [4 bytes: mode]
*/

/* Helpers para construir/parsear blobs de archivos concatenados */
int brs3_bundle_add_file(uint8_t **blob, size_t *blob_size,
                         const char *name, const void *data, size_t data_size);

int brs3_bundle_get_file(const uint8_t *blob, size_t blob_size, size_t *offset,
                         char *name, size_t name_cap,
                         const uint8_t **data, size_t *data_size);

#endif /* BRS_REMOTE_V3_H */
