/* brs_spsc.c — SPSC: nucleo lock-free con atomics C11; el push bloqueante
usa mutex+condvar (mismo patron que el C++, sin lost wakeups). */
#include "brs_spsc.h"
#include <string.h>

_Static_assert((BRS_SPSC_CAP & (BRS_SPSC_CAP - 1)) == 0,
               "BRS_SPSC_CAP debe ser potencia de 2");
_Static_assert(BRS_SPSC_CAP >= 2, "BRS_SPSC_CAP >= 2");

#define BRS_SPSC_MASK ((size_t)BRS_SPSC_CAP - 1)

void brs_spsc_init(BrsSpscQueue *q)
{
    if (!q) return;
    memset(q->items, 0, sizeof q->items);
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    pthread_mutex_init(&q->notify_mtx, NULL);
    pthread_cond_init(&q->not_full_cv, NULL);
}

void brs_spsc_free(BrsSpscQueue *q)
{
    if (!q) return;
    pthread_mutex_destroy(&q->notify_mtx);
    pthread_cond_destroy(&q->not_full_cv);
}

int brs_spsc_try_push(BrsSpscQueue *q, const BrsChunkItem *item)
{
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t next = (tail + 1) & BRS_SPSC_MASK;
    if (next == atomic_load_explicit(&q->head, memory_order_acquire))
        return -1;   /* llena */
    q->items[tail] = *item;
    atomic_store_explicit(&q->tail, next, memory_order_release);
    return 0;
}

int brs_spsc_try_pop(BrsSpscQueue *q, BrsChunkItem *item)
{
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (head == atomic_load_explicit(&q->tail, memory_order_acquire))
        return -1;   /* vacia */
    *item = q->items[head];
    atomic_store_explicit(&q->head, (head + 1) & BRS_SPSC_MASK,
                          memory_order_release);
    return 0;
}

void brs_spsc_push(BrsSpscQueue *q, const BrsChunkItem *item)
{
    pthread_mutex_lock(&q->notify_mtx);
    while (brs_spsc_try_push(q, item) != 0)
        pthread_cond_wait(&q->not_full_cv, &q->notify_mtx);
    pthread_mutex_unlock(&q->notify_mtx);
}

int brs_spsc_pop_notify(BrsSpscQueue *q, BrsChunkItem *item)
{
    if (brs_spsc_try_pop(q, item) != 0) return -1;
    /* Serializar con el push bloqueante antes de senalar (anti lost-wakeup). */
    pthread_mutex_lock(&q->notify_mtx);
    pthread_mutex_unlock(&q->notify_mtx);
    pthread_cond_signal(&q->not_full_cv);
    return 0;
}
