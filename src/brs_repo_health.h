#ifndef BRS_REPO_HEALTH_H
#define BRS_REPO_HEALTH_H

#include "brs_types.h"
#include "brs_index.h"
#include <stdint.h>
#include <stddef.h>

/* Informe de anomalías detectadas en el repositorio */
typedef struct {
    /* Index segments cuyo pack no existe */
    uint64_t *orphan_idx;
    size_t    orphan_idx_count;
    size_t    orphan_idx_cap;

    /* Packs que no tienen index segment */
    uint64_t *indexless_pack;
    size_t    indexless_pack_count;
    size_t    indexless_pack_cap;

    /* Bloom filters sin index segment */
    uint64_t *orphan_blm;
    size_t    orphan_blm_count;
    size_t    orphan_blm_cap;

    /* open_pack_id apunta a un pack inexistente */
        int      orphan_open_pack;
        uint64_t open_pack_id;
    
        int      missing_config;
    

    /* Chunks de snapshots que no están en ningún index segment */
    uint64_t missing_index_chunks;

    /* Chunks cuyo pack referenciado no existe en disco */
    uint64_t missing_pack_chunks;

    /* Número de snapshots con algún problema */
    uint64_t affected_snapshots;

    /* Nombres de snapshots corruptos (se moverán a damaged/) */
    char   **corrupt_snaps;
    size_t   corrupt_snap_count;
    size_t   corrupt_snap_cap;

    /* Total de anomalías */
    size_t   total_issues;
} BrsHealthReport;

void brs_health_report_init(BrsHealthReport *r);
void brs_health_report_free(BrsHealthReport *r);

/* Escanea el repo y llena el informe. Devuelve 0 si OK, -1 en error de I/O. */
int brs_health_check(const char *repo_path, BrsHealthReport *report);

/* Imprime el informe a stderr. */
void brs_health_print_report(const BrsHealthReport *report);

/* Repara las anomalías. Devuelve 0 si todo OK, -1 si hubo errores. */
int brs_health_repair(const char *repo_path, BrsHealthReport *report);

/* Pregunta al usuario si quiere reparar (solo si stdin es tty).
 * Devuelve 1 = sí, 0 = no. Si no es tty, devuelve use_default. */
int brs_health_ask_repair(int use_default);

#endif /* BRS_REPO_HEALTH_H */
