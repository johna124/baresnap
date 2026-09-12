#include "brs_zstd.h"
#include <zstd.h>

size_t brs_zstd_compress_bound(size_t src_size)
{
    return ZSTD_compressBound(src_size);
}

size_t brs_zstd_compress(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_cap, int level)
{
    if (level < 1) level = 1;
    if (level > 22) level = 22;
    size_t r = ZSTD_compress(dst, dst_cap, src, src_len, level);
    return ZSTD_isError(r) ? 0 : r;
}

size_t brs_zstd_decompress(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap)
{
    size_t r = ZSTD_decompress(dst, dst_cap, src, src_len);
    return ZSTD_isError(r) ? 0 : r;
}
