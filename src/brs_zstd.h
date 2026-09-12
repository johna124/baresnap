#ifndef BRS_ZSTD_H
#define BRS_ZSTD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Devuelve el tamaño máximo del buffer de destino. */
size_t brs_zstd_compress_bound(size_t src_size);

/* Comprime src -> dst. Devuelve tamaño comprimido, o 0 en error. */
size_t brs_zstd_compress(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_cap, int level);

/* Descomprime src -> dst. Devuelve tamaño descomprimido, o 0 en error. */
size_t brs_zstd_decompress(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif /* BRS_ZSTD_H */
