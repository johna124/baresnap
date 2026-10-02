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
/* brs_lz4.c — LZ4 embebido. Hash table thread-local (__thread, musl). */
#include "brs_lz4.h"

#include <string.h>

#define BRS_LZ4_HASH_LOG    14
#define BRS_LZ4_HASH_SIZE   (1 << BRS_LZ4_HASH_LOG)
#define BRS_LZ4_MIN_MATCH   4
#define BRS_LZ4_LAST_LITS   5
#define BRS_LZ4_MAX_DIST    65535

static __thread int32_t g_hash_table[BRS_LZ4_HASH_SIZE];

static uint32_t hash4(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - BRS_LZ4_HASH_LOG);
}

static uint8_t *emit_sequence(uint8_t *op, const uint8_t *literals,
                              size_t lit_len, uint16_t offset,
                              size_t match_len, int is_last)
{
    size_t ll_code = (lit_len >= 15) ? 15 : lit_len;
    size_t ml_code = 0;
    if (!is_last && match_len >= BRS_LZ4_MIN_MATCH) {
        size_t ml = match_len - BRS_LZ4_MIN_MATCH;
        ml_code = (ml >= 15) ? 15 : ml;
    }
    *op++ = (uint8_t)((ll_code << 4) | ml_code);

    if (ll_code == 15) {
        size_t remaining = lit_len - 15;
        while (remaining >= 255) { *op++ = 255; remaining -= 255; }
        *op++ = (uint8_t)remaining;
    }
    if (lit_len > 0) {
        memcpy(op, literals, lit_len);
        op += lit_len;
    }
    if (is_last) return op;

    *op++ = (uint8_t)(offset & 0xFF);
    *op++ = (uint8_t)((offset >> 8) & 0xFF);

    if (ml_code == 15) {
        size_t remaining = (match_len - BRS_LZ4_MIN_MATCH) - 15;
        while (remaining >= 255) { *op++ = 255; remaining -= 255; }
        *op++ = (uint8_t)remaining;
    }
    return op;
}

size_t brs_lz4_compress_bound(size_t input_size)
{
    return input_size + (input_size / 255) + 16;
}

size_t brs_lz4_compress(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0) return 0;
    if (dst_cap < brs_lz4_compress_bound(src_len)) return 0;

    memset(g_hash_table, 0xFF, sizeof g_hash_table);   /* -1 */

    const uint8_t *const ip_end = src + src_len;
    const uint8_t *const ip_limit = ip_end - BRS_LZ4_LAST_LITS;
    const uint8_t *ip = src;
    const uint8_t *anchor = src;
    uint8_t *op = dst;

    if (src_len < BRS_LZ4_MIN_MATCH + BRS_LZ4_LAST_LITS) {
        op = emit_sequence(op, src, src_len, 0, 0, 1);
        return (size_t)(op - dst);
    }

    ip++;   /* el primer byte no puede iniciar match */
    while (ip < ip_limit) {
        uint32_t h = hash4(ip);
        int32_t ref = g_hash_table[h];
        g_hash_table[h] = (int32_t)(ip - src);
        int matched = 0;

        if (ref >= 0) {
            size_t dist = (size_t)((ip - src) - ref);
            if (dist > 0 && dist <= BRS_LZ4_MAX_DIST) {
                if (memcmp(src + ref, ip, BRS_LZ4_MIN_MATCH) == 0) {
                    size_t match_len = BRS_LZ4_MIN_MATCH;
                    while (ip + match_len < ip_limit &&
                           src[ref + match_len] == ip[match_len]) {
                        match_len++;
                    }
                    size_t lit_len = (size_t)(ip - anchor);
                    op = emit_sequence(op, anchor, lit_len,
                                       (uint16_t)dist, match_len, 0);
                    ip += match_len;
                    anchor = ip;
                    if (ip < ip_limit) {
                        g_hash_table[hash4(ip - 2)] =
                            (int32_t)((ip - 2) - src);
                    }
                    matched = 1;
                }
            }
        }
        if (!matched) ip++;
    }

    size_t last_lit_len = (size_t)(ip_end - anchor);
    op = emit_sequence(op, anchor, last_lit_len, 0, 0, 1);
    return (size_t)(op - dst);
}

size_t brs_lz4_decompress(const uint8_t *src, size_t src_len,
                          uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0) return 0;
    const uint8_t *ip = src;
    const uint8_t *const ip_end = src + src_len;
    uint8_t *op = dst;
    uint8_t *const op_end = dst + dst_cap;

    while (ip < ip_end) {
        uint8_t token = *ip++;

        size_t lit_len = token >> 4;
        if (lit_len == 15) {
            uint8_t s;
            do {
                if (ip >= ip_end) return 0;
                s = *ip++;
                lit_len += s;
            } while (s == 255);
        }
        if (ip + lit_len > ip_end) return 0;
        if (op + lit_len > op_end) return 0;
        if (lit_len > 0) {
            memcpy(op, ip, lit_len);
            ip += lit_len;
            op += lit_len;
        }
        if (ip >= ip_end) break;   /* ultima secuencia: solo literales */

        if (ip + 2 > ip_end) return 0;
        uint16_t offset = (uint16_t)(ip[0] | (ip[1] << 8));
        ip += 2;
        if (offset == 0) return 0;
        uint8_t *match = op - offset;
        if (match < dst) return 0;

        size_t match_len = (token & 15) + BRS_LZ4_MIN_MATCH;
        if ((token & 15) == 15) {
            uint8_t s;
            do {
                if (ip >= ip_end) return 0;
                s = *ip++;
                match_len += s;
            } while (s == 255);
        }
        if (op + match_len > op_end) return 0;

        if (offset >= match_len) {
            memcpy(op, match, match_len);
        } else {
            for (size_t i = 0; i < match_len; ++i) op[i] = match[i];
        }
        op += match_len;
    }
    return (size_t)(op - dst);
}
