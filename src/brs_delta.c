/* src/brs_delta.c — Wrapper sobre xdelta3 para BareSnap */
#include "brs_delta.h"
#include "xdelta3.h"
#include <stdlib.h>
#include <string.h>

int brs_delta_encode(const uint8_t *old_data, size_t old_size,
                     const uint8_t *new_data, size_t new_size,
                     uint8_t **delta_out, size_t *delta_size_out) {
    if (!old_data || !new_data || !delta_out || !delta_size_out) return -1;
    
    // Peor caso: el delta es ligeramente mayor que el fichero nuevo + overhead
    size_t out_size = new_size + (new_size / 10) + 64;
    uint8_t *out_buf = (uint8_t *)malloc(out_size);
    if (!out_buf) return -1;

    int ret = xd3_encode_memory(new_data, new_size,
                                old_data, old_size,
                                out_buf, &out_size,
                                out_size, 0);
    if (ret != 0) {
        free(out_buf);
        return -1;
    }

    *delta_out = out_buf;
    *delta_size_out = out_size;
    return 0;
}

int brs_delta_decode(const uint8_t *old_data, size_t old_size,
                     const uint8_t *delta_data, size_t delta_size,
                     size_t expected_new_size,
                     uint8_t **new_out, size_t *new_size_out) {
    if (!old_data || !delta_data || !new_out || !new_size_out) return -1;
    
    uint8_t *out_buf = (uint8_t *)malloc(expected_new_size);
    if (!out_buf) return -1;

    size_t out_size = expected_new_size;
    int ret = xd3_decode_memory(delta_data, delta_size,
                                old_data, old_size,
                                out_buf, &out_size,
                                out_size, 0);
    if (ret != 0) {
        free(out_buf);
        return -1;
    }

    *new_out = out_buf;
    *new_size_out = out_size;
    return 0;
}
