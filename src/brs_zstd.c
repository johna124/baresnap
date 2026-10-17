/*
 * BareSnap — bare-metal snapshot and backup system.
 * Copyright (C) 2026  John (johna124)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Source, issues, contact: https://github.com/johna124
 */
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
