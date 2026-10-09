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
/* brs_pool.c — pool de buffers zero-allocation en hot path */
#include "brs_pool.h"

#include <stdlib.h>
#include <string.h>

int brs_buffer_pool_init(BrsBufferPool *p, size_t num_slots, size_t slot_size)
{
    if (!p || num_slots == 0 || slot_size == 0) return -1;
    if (num_slots > UINT32_MAX) return -1;
    if (slot_size > SIZE_MAX / num_slots) return -1;

    memset(p, 0, sizeof *p);
    p->slot_size = slot_size;
    p->num_slots = num_slots;
    p->data = (uint8_t *)malloc(num_slots * slot_size);
    p->free_indices = (uint32_t *)malloc(num_slots * sizeof(uint32_t));
    if (!p->data || !p->free_indices) {
        brs_buffer_pool_free(p);
        return -1;
    }
    for (size_t i = 0; i < num_slots; ++i)
        p->free_indices[i] = (uint32_t)(num_slots - 1 - i);
    p->free_count = num_slots;

    pthread_mutex_init(&p->mtx, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->sync_init = 1;
    return 0;
}

void brs_buffer_pool_free(BrsBufferPool *p)
{
    if (!p) return;
    free(p->data);
    free(p->free_indices);
    if (p->sync_init) {
        pthread_mutex_destroy(&p->mtx);
        pthread_cond_destroy(&p->cv);
    }
    memset(p, 0, sizeof *p);
}

uint32_t brs_buffer_pool_acquire(BrsBufferPool *p)
{
    pthread_mutex_lock(&p->mtx);
    while (p->free_count == 0)
        pthread_cond_wait(&p->cv, &p->mtx);
    uint32_t idx = p->free_indices[--p->free_count];
    pthread_mutex_unlock(&p->mtx);
    return idx;
}

void brs_buffer_pool_release(BrsBufferPool *p, uint32_t index)
{
    pthread_mutex_lock(&p->mtx);
    p->free_indices[p->free_count++] = index;
    pthread_mutex_unlock(&p->mtx);
    pthread_cond_signal(&p->cv);
}

uint8_t *brs_buffer_pool_get(BrsBufferPool *p, uint32_t index)
{
    return p->data + (size_t)index * p->slot_size;
}
