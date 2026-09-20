#include "tt_types.h"
#include <stdlib.h>
#include <string.h>
#include "xdelta3.h"

int tt_delta_encode(const uint8_t *old_data, size_t old_size,
                    const uint8_t *new_data, size_t new_size,
                    uint8_t **delta_out, size_t *delta_size_out) {
    if (!delta_out || !delta_size_out) return -1;
    *delta_out = NULL; *delta_size_out = 0;
    if (!new_data || new_size == 0) return 0;
    size_t out_cap = new_size + (new_size / 8) + 64;
    uint8_t *out = malloc(out_cap);
    if (!out) return -1;
    size_t out_size = out_cap;
    if (xd3_encode_memory(new_data, new_size, old_data, old_size,
                          out, &out_size, out_cap, 0) != 0) { free(out); return -1; }
    *delta_out = out; *delta_size_out = out_size;
    return 0;
}

int tt_delta_decode(const uint8_t *old_data, size_t old_size,
                    const uint8_t *delta_data, size_t delta_size,
                    size_t expected_new_size,
                    uint8_t **new_out, size_t *new_size_out) {
    if (!new_out || !new_size_out) return -1;
    *new_out = NULL; *new_size_out = 0;
    if (expected_new_size == 0) return 0;
    if (!delta_data || delta_size == 0) return -1;
    uint8_t *out = malloc(expected_new_size);
    if (!out) return -1;
    size_t out_size = expected_new_size;
    if (xd3_decode_memory(delta_data, delta_size, old_data, old_size,
                          out, &out_size, expected_new_size, 0) != 0 ||
        out_size != expected_new_size) { free(out); return -1; }
    *new_out = out; *new_size_out = out_size;
    return 0;
}
