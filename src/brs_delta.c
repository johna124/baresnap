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
/* src/brs_delta.c — Wrapper sobre xdelta3 para BareSnap (Thread-Safe Hardened) */
#include "brs_delta.h"
#include "xdelta3.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* 🛡️ STATIC MUTEX BOUNDARY FOR XDELTA3 ENGINE
 * Enforces strict thread isolation when multiple worker pools try to 
 * execute memory delta calculations simultaneously over the underlying library. */
static pthread_mutex_t g_xd3_mutex = PTHREAD_MUTEX_INITIALIZER;

int brs_delta_encode(const uint8_t *old_data, size_t old_size,
                     const uint8_t *new_data, size_t new_size,
                     uint8_t **delta_out, size_t *delta_size_out) {
    if (!old_data || !new_data || !delta_out || !delta_size_out) return -1;
    
    // Peor caso: el delta es ligeramente mayor que el fichero nuevo + overhead
    size_t out_size = new_size + (new_size / 10) + 64;
    uint8_t *out_buf = (uint8_t *)malloc(out_size);
    if (!out_buf) return -1;

    /* --- CRITICAL SECTION ENTRY --- */
    pthread_mutex_lock(&g_xd3_mutex);
    
    int ret = xd3_encode_memory(new_data, new_size,
                                old_data, old_size,
                                out_buf, &out_size,
                                out_size, 0);
                                
    pthread_mutex_unlock(&g_xd3_mutex);
    /* --- CRITICAL SECTION EXIT --- */

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
    
    /* --- CRITICAL SECTION ENTRY --- */
    pthread_mutex_lock(&g_xd3_mutex);
    
    int ret = xd3_decode_memory(delta_data, delta_size,
                                old_data, old_size,
                                out_buf, &out_size,
                                out_size, 0);
                                
    pthread_mutex_unlock(&g_xd3_mutex);
    /* --- CRITICAL SECTION EXIT --- */

    if (ret != 0) {
        free(out_buf);
        return -1;
    }

    *new_out = out_buf;
    *new_size_out = out_size;
    return 0;
}

