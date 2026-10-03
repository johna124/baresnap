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
/* brs_chunker.c — FastCDC. La tabla Gear usa la MISMA semilla splitmix64
   que el C++ (0x123456789ABCDEF0), asi los puntos de corte son identicos
   y los packs antiguos siguen siendo compatibles. */

#include "brs_chunker.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_gear[256];
static pthread_once_t g_gear_once = PTHREAD_ONCE_INIT;

static void gear_build(void)
{
    uint64_t x = 0x123456789ABCDEF0ULL;
    for (int i = 0; i < 256; ++i) {
        uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        g_gear[i] = z ^ (z >> 31);
    }
}

/*
 * Cortafuegos de entropía.
 *
 * Muestrea exactamente 1024 bytes en saltos fijos y construye un histograma
 * en pila. Está pensado para detectar ruido blanco / contendores multimedia
 * ya comprimidos sin pagar un análisis completo del chunk.
 *
 * Criterios:
 *   - Más de 200 símbolos distintos en la muestra => incompresible.
 *   - Distribución suficientemente plana sobre muchos símbolos => incompresible.
 */
bool brs_is_chunk_incompressible(const uint8_t *data, size_t len)
{
    if (!data || len < 1024)
        return false;

    uint32_t histogram[256] = {0};
    const size_t step = (len / 1024) ? (len / 1024) : 1;
    uint32_t unique = 0;

    for (int i = 0; i < 1024; ++i) {
        size_t off = (size_t)i * step;
        if (off >= len)
            off = len - 1;

        uint8_t v = data[off];
        if (histogram[v] == 0)
            ++unique;
        ++histogram[v];
    }

    if (unique > 200)
        return true;

    /*
     * Segunda pasada barata para detectar "ruido blanco".
     * Con 1024 muestras y 256 buckets, la cuenta esperada por bucket es 4.
     * Una distribución muy plana con muchos símbolos distintos suele ser
     * material ya comprimido o aleatorio.
     */
    if (unique >= 192) {
        uint64_t deviation_sq = 0;
        for (int b = 0; b < 256; ++b) {
            int d = (int)histogram[b] - 4;
            deviation_sq += (uint64_t)(d * d);
        }
        if (deviation_sq <= 2048)
            return true;
    }

    return false;
}

static BrsFastCDCConfig normalize(BrsFastCDCConfig cfg)
{
    if (cfg.min_size == 0)
        cfg.min_size = 1;
    if (cfg.avg_size <= cfg.min_size)
        cfg.avg_size = cfg.min_size * 4;
    if (cfg.max_size <= cfg.avg_size)
        cfg.max_size = cfg.avg_size * 4;
    if (cfg.max_size < cfg.min_size)
        cfg.max_size = cfg.min_size;
    return cfg;
}

static uint64_t mask_from_bits(int bits)
{
    if (bits <= 0)
        return 0;
    if (bits >= 64)
        return ~(uint64_t)0;
    return (1ULL << bits) - 1;
}

void brs_fastcdc_init(BrsFastCDC *c, BrsFastCDCConfig cfg)
{
    if (!c)
        return;

    pthread_once(&g_gear_once, gear_build);
    c->cfg = normalize(cfg);

    uint64_t v = 1;
    int bits = 0;
    while (v < c->cfg.avg_size && bits < 62) {
        v <<= 1;
        ++bits;
    }

    int small_bits = bits + 2;
    int large_bits = bits - 2;

    if (small_bits < 1)
        small_bits = 1;
    if (large_bits < 1)
        large_bits = 1;
    if (small_bits > 63)
        small_bits = 63;
    if (large_bits > 63)
        large_bits = 63;

    c->mask_s = mask_from_bits(small_bits);
    c->mask_l = mask_from_bits(large_bits);
}

size_t brs_fastcdc_find_first_cut(const BrsFastCDC *c,
                                  const uint8_t *data, size_t size,
                                  int force_cut)
{
    if (!c || !data || size == 0)
        return 0;

    const size_t min_sz = c->cfg.min_size;
    const size_t max_sz = c->cfg.max_size;
    const size_t avg_sz = c->cfg.avg_size;

    if (size <= min_sz)
        return size;

    size_t limit = (size < max_sz) ? size : max_sz;
    uint64_t fp = 0;

    for (size_t i = 0; i < limit; ++i) {
        fp = (fp << 1) + g_gear[data[i]];
        size_t length = i + 1;

        if (length >= min_sz) {
            uint64_t mask = (length < avg_sz) ? c->mask_s : c->mask_l;
            if ((fp & mask) == 0)
                return length;
        }
    }

    if (force_cut)
        return limit;

    return 0;
}

int brs_fastcdc_chunk_buffer(const BrsFastCDC *c,
                             const uint8_t *data, size_t size,
                             BrsChunkEmitFn emit, void *user)
{
    if (!c || !emit)
        return -1;

    size_t pos = 0;
    while (pos < size) {
        size_t cut = brs_fastcdc_find_first_cut(c, data + pos, size - pos, 1);
        if (cut == 0)
            break;

        if (emit(data + pos, cut, user) != 0)
            return -1;

        pos += cut;
    }

    return 0;
}

/* ---- Streaming ---- */

int brs_streaming_chunker_init(BrsStreamingChunker *sc, const BrsFastCDC *cdc)
{
    if (!sc || !cdc)
        return -1;

    memset(sc, 0, sizeof *sc);
    sc->cdc = cdc;
    return 0;
}

void brs_streaming_chunker_free(BrsStreamingChunker *sc)
{
    if (!sc)
        return;

    free(sc->buffer);
    memset(sc, 0, sizeof *sc);
}

void brs_streaming_chunker_reset(BrsStreamingChunker *sc)
{
    if (sc)
        sc->size = 0;
}

static int streaming_drain(BrsStreamingChunker *sc,
                           BrsChunkEmitFn emit, void *user, int is_final)
{
    for (;;) {
        if (sc->size == 0)
            break;

        int can_force = is_final || (sc->size >= sc->cdc->cfg.max_size);
        if (!can_force && sc->size < sc->cdc->cfg.min_size)
            break;

        size_t cut = brs_fastcdc_find_first_cut(sc->cdc, sc->buffer,
                                                sc->size, can_force);
        if (cut == 0)
            break;

        if (emit(sc->buffer, cut, user) != 0)
            return -1;

        size_t remaining = sc->size - cut;
        if (remaining > 0)
            memmove(sc->buffer, sc->buffer + cut, remaining);
        sc->size = remaining;
    }

    return 0;
}

int brs_streaming_chunker_feed(BrsStreamingChunker *sc,
                               const uint8_t *data, size_t size,
                               BrsChunkEmitFn emit, void *user)
{
    if (!sc || (!data && size > 0) || !emit)
        return -1;

    if (size > 0) {
        if (sc->size + size > sc->capacity) {
            size_t ncap = sc->capacity ? sc->capacity : (64 * 1024);
            while (ncap < sc->size + size)
                ncap *= 2;

            uint8_t *p = (uint8_t *)realloc(sc->buffer, ncap);
            if (!p)
                return -1;

            sc->buffer = p;
            sc->capacity = ncap;
        }

        memcpy(sc->buffer + sc->size, data, size);
        sc->size += size;
    }

    return streaming_drain(sc, emit, user, 0);
}

int brs_streaming_chunker_finish(BrsStreamingChunker *sc,
                                 BrsChunkEmitFn emit, void *user)
{
    if (!sc || !emit)
        return -1;

    int rc = streaming_drain(sc, emit, user, 1);
    sc->size = 0;
    return rc;
}
