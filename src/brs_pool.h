/* brs_pool.h — pool de buffers preasignados (pthread mutex + cond) */
#ifndef BRS_POOL_H
#define BRS_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t slot_size;
    size_t num_slots;
    uint8_t *data;
    uint32_t *free_indices;
    size_t free_count;
    int sync_init;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
} BrsBufferPool;

int brs_buffer_pool_init(BrsBufferPool *p, size_t num_slots, size_t slot_size);
void brs_buffer_pool_free(BrsBufferPool *p);
uint32_t brs_buffer_pool_acquire(BrsBufferPool *p);   /* bloqueante */
void brs_buffer_pool_release(BrsBufferPool *p, uint32_t index);
uint8_t *brs_buffer_pool_get(BrsBufferPool *p, uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* BRS_POOL_H */