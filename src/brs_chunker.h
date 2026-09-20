/* brs_chunker.h — FastCDC + streaming (cortes compatibles con el C++) */
#ifndef BRS_CHUNKER_H
#define BRS_CHUNKER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t min_size;
    uint32_t avg_size;
    uint32_t max_size;
} BrsFastCDCConfig;

typedef struct {
    BrsFastCDCConfig cfg;
    uint64_t mask_s;
    uint64_t mask_l;
} BrsFastCDC;

void brs_fastcdc_init(BrsFastCDC *c, BrsFastCDCConfig cfg);

/* Callback de emision: devolver 0 para continuar, != 0 para abortar. */
typedef int (*BrsChunkEmitFn)(const uint8_t *data, size_t size, void *user);

/* Tamano del primer corte. force_cut: corta en max si no hay corte natural.
   Devuelve 0 si no hay corte posible (y force_cut == 0). */
size_t brs_fastcdc_find_first_cut(const BrsFastCDC *c,
                                  const uint8_t *data, size_t size,
                                  int force_cut);

int brs_fastcdc_chunk_buffer(const BrsFastCDC *c,
                             const uint8_t *data, size_t size,
                             BrsChunkEmitFn emit, void *user);

/* Streaming: feed() por bloques, finish() al final. Los cortes son
   IDENTICOS a los del modo batch, independiente de la granularidad. */
typedef struct {
    const BrsFastCDC *cdc;
    uint8_t *buffer;
    size_t size;
    size_t capacity;
} BrsStreamingChunker;

int  brs_streaming_chunker_init(BrsStreamingChunker *sc, const BrsFastCDC *cdc);
void brs_streaming_chunker_free(BrsStreamingChunker *sc);
void brs_streaming_chunker_reset(BrsStreamingChunker *sc);

int  brs_streaming_chunker_feed(BrsStreamingChunker *sc,
                                const uint8_t *data, size_t size,
                                BrsChunkEmitFn emit, void *user);
int  brs_streaming_chunker_finish(BrsStreamingChunker *sc,
                                  BrsChunkEmitFn emit, void *user);

/* Estimador rápido de entropía para el cortafuegos de compresión.
   Devuelve true si el chunk parece incompresible (ruido/ya comprimido). */
bool brs_is_chunk_incompressible(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BRS_CHUNKER_H */
