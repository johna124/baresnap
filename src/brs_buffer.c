/* ============================================================================
 * brs_buffer.c — Implementación del buffer dinámico
 * ==========================================================================*/
#include "brs_buffer.h"
#include "brs_util.h"   /* brs_secure_wipe */

#include <stdlib.h>
#include <string.h>

#define BRS_BUFFER_INIT_CAP 64

void brs_buffer_init(BrsBuffer *b)
{
    if (!b) return;
    b->data = NULL;
    b->size = 0;
    b->capacity = 0;
}

void brs_buffer_free(BrsBuffer *b)
{
    if (!b) return;
    free(b->data);
    brs_buffer_init(b);
}

void brs_buffer_wipe_free(BrsBuffer *b)
{
    if (!b) return;
    if (b->data) brs_secure_wipe(b->data, b->capacity);
    brs_buffer_free(b);
}

int brs_buffer_reserve(BrsBuffer *b, size_t min_capacity)
{
    if (!b) return -1;
    if (min_capacity > BRS_BUFFER_MAX) return -1;
    if (min_capacity <= b->capacity) return 0;

    size_t newcap = b->capacity ? b->capacity : BRS_BUFFER_INIT_CAP;
    while (newcap < min_capacity) {
        if (newcap > BRS_BUFFER_MAX / 2) {
            newcap = BRS_BUFFER_MAX;  /* último escalón sin desbordar */
            break;
        }
        newcap *= 2;
    }

    uint8_t *p = (uint8_t *)realloc(b->data, newcap);
    if (!p) return -1;
    b->data = p;
    b->capacity = newcap;
    return 0;
}

int brs_buffer_resize(BrsBuffer *b, size_t new_size)
{
    if (!b) return -1;
    if (new_size > BRS_BUFFER_MAX) return -1;
    if (new_size > b->capacity && brs_buffer_reserve(b, new_size) != 0)
        return -1;
    if (new_size > b->size)
        memset(b->data + b->size, 0, new_size - b->size);
    b->size = new_size;
    return 0;
}

void brs_buffer_clear(BrsBuffer *b)
{
    if (b) b->size = 0;
}

int brs_buffer_append(BrsBuffer *b, const void *data, size_t n)
{
    if (!b) return -1;
    if (n == 0) return 0;
    if (!data) return -1;
    if (n > BRS_BUFFER_MAX - b->size) return -1;   /* overflow size+n */
    if (brs_buffer_reserve(b, b->size + n) != 0) return -1;
    memcpy(b->data + b->size, data, n);
    b->size += n;
    return 0;
}

int brs_buffer_append_str(BrsBuffer *b, const char *s)
{
    if (!s) return -1;
    return brs_buffer_append(b, s, strlen(s));
}

int brs_buffer_append_u8(BrsBuffer *b, uint8_t v)
{
    return brs_buffer_append(b, &v, 1);
}

int brs_buffer_append_u16_le(BrsBuffer *b, uint16_t v)
{
    uint8_t t[2];
    for (int i = 0; i < 2; ++i)
        t[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    return brs_buffer_append(b, t, sizeof t);
}

int brs_buffer_append_u32_le(BrsBuffer *b, uint32_t v)
{
    uint8_t t[4];
    for (int i = 0; i < 4; ++i)
        t[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    return brs_buffer_append(b, t, sizeof t);
}

int brs_buffer_append_u64_le(BrsBuffer *b, uint64_t v)
{
    uint8_t t[8];
    for (int i = 0; i < 8; ++i)
        t[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    return brs_buffer_append(b, t, sizeof t);
}
