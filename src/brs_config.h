/* brs_config.h — config del repositorio + open_pack_id */
#ifndef BRS_CONFIG_H
#define BRS_CONFIG_H

#include <stdint.h>
#include "brs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Formato binario identico al motor C++ (magic BSCFG001, LE, FNV1a al final).
 * Devuelven 0 ok, -1 error. */
int brs_write_config(const char *repo_path, const BrsRepoConfig *cfg);
int brs_load_config(const char *repo_path, BrsRepoConfig *cfg);

/* Fichero de texto "open_pack_id" con el pack id en decimal. */
int brs_read_open_pack_id(const char *repo_path, uint64_t *pack_id);
int brs_write_open_pack_id(const char *repo_path, uint64_t pack_id);
int brs_remove_open_pack_id(const char *repo_path);

#ifdef __cplusplus
}
#endif

#endif /* BRS_CONFIG_H */