/* src/brs_delta.h — Interfaz para Delta Encoding (xdelta3) */
#ifndef BRS_DELTA_H
#define BRS_DELTA_H

#include <stddef.h>
#include <stdint.h>

/* Codifica un delta entre old_data y new_data. 
 * Devuelve 0 en éxito, -1 en error. El llamador debe free() a delta_out. */
int brs_delta_encode(const uint8_t *old_data, size_t old_size,
                     const uint8_t *new_data, size_t new_size,
                     uint8_t **delta_out, size_t *delta_size_out);

/* Decodifica un delta aplicándolo sobre old_data.
 * Devuelve 0 en éxito, -1 en error. El llamador debe free() a new_out. */
int brs_delta_decode(const uint8_t *old_data, size_t old_size,
                     const uint8_t *delta_data, size_t delta_size,
                     size_t expected_new_size,
                     uint8_t **new_out, size_t *new_size_out);

#endif /* BRS_DELTA_H */
