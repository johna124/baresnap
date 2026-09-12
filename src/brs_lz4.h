/* brs_lz4.h — LZ4 embebido (port de lz4.cpp) */
#ifndef BRS_LZ4_H
#define BRS_LZ4_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t brs_lz4_compress_bound(size_t input_size);
/* Devuelve tamano comprimido, o 0 si falla. dst_cap >= bound. */
size_t brs_lz4_compress(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_cap);
/* Devuelve tamano descomprimido, o 0 si falla. */
size_t brs_lz4_decompress(const uint8_t *src, size_t src_len,
                          uint8_t *dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif /* BRS_LZ4_H */