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
/* brs_spsc.h — cola SPSC lock-free (C11 atomics) + espera por condvar */

#ifndef BRS_SPSC_H
#define BRS_SPSC_H

#include <stdatomic.h>
#include <pthread.h>
#include "brs_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * FASE 2: Ampliación del buffer circular a 1024 slots.
 *
 * Esto evita que los productores de FastCDC bloqueen el pipeline durmiendo
 * en pthread_cond_wait cuando el consumidor está ocupado procesando chunks.
 * ==========================================================================*/
#define BRS_SPSC_CAP 1024

typedef struct {
    _Alignas(64) BrsChunkItem items[BRS_SPSC_CAP];
    _Alignas(64) atomic_size_t head;   /* consumidor */
    _Alignas(64) atomic_size_t tail;   /* productor */
    
    /* 🛡️ CANCELLATION MATRIX POISON PILL
     * We pad this to its own cache boundary line to protect thread loops
     * from cross-talk or timing delays during execution aborts. */
    _Alignas(64) atomic_int aborted;
    
    pthread_mutex_t notify_mtx;
    pthread_cond_t not_full_cv;
} BrsSpscQueue;

void brs_spsc_init(BrsSpscQueue *q);
void brs_spsc_free(BrsSpscQueue *q);
void brs_spsc_abort(BrsSpscQueue *q); /* New asynchronous teardown channel */
int  brs_spsc_try_push(BrsSpscQueue *q, const BrsChunkItem *item);
int  brs_spsc_try_pop(BrsSpscQueue *q, BrsChunkItem *item);
void brs_spsc_push(BrsSpscQueue *q, const BrsChunkItem *item);  /* bloqueante */
int  brs_spsc_pop_notify(BrsSpscQueue *q, BrsChunkItem *item);  /* 0 ok, -1 vacia */

#ifdef __cplusplus
}
#endif

#endif /* BRS_SPSC_H */

