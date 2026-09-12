/* ============================================================================
 * brs_buffer.h — Buffer dinámico genérico de BareSnap
 *
 * Sustituye a std::vector<std::byte> del motor C++.
 * Crecimiento con detección de overflow: todas las operaciones devuelven
 * -1 en lugar de corromper memoria o abortar.
 * ==========================================================================*/
#ifndef BRS_BUFFER_H
#define BRS_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tope duro: evita que size + extra desborde size_t (32 y 64 bits). */
#define BRS_BUFFER_MAX ((SIZE_MAX >> 1) - 1)

typedef struct BrsBuffer {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
} BrsBuffer;

void brs_buffer_init(BrsBuffer *b);
void brs_buffer_free(BrsBuffer *b);
/* Para buffers que hayan contenido material sensible:
 * explicit_bzero sobre TODA la capacidad antes del free. */
void brs_buffer_wipe_free(BrsBuffer *b);

int  brs_buffer_reserve(BrsBuffer *b, size_t min_capacity);
/* resize hacia arriba rellena con ceros (no filtra datos del heap). */
int  brs_buffer_resize(BrsBuffer *b, size_t new_size);
void brs_buffer_clear(BrsBuffer *b);   /* size=0, conserva capacity */

int  brs_buffer_append(BrsBuffer *b, const void *data, size_t n);
/* Añade strlen(s) bytes SIN el NUL final (igual que el append_bytes
 * de string_view del C++: necesario para compatibilidad de formato). */
int  brs_buffer_append_str(BrsBuffer *b, const char *s);
int  brs_buffer_append_u8(BrsBuffer *b, uint8_t v);
int  brs_buffer_append_u16_le(BrsBuffer *b, uint16_t v);
int  brs_buffer_append_u32_le(BrsBuffer *b, uint32_t v);
int  brs_buffer_append_u64_le(BrsBuffer *b, uint64_t v);

#ifdef __cplusplus
}
#endif

#endif /* BRS_BUFFER_H */
