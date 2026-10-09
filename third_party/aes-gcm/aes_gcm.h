#ifndef AES_GCM_H
#define AES_GCM_H

#include <stdint.h>
#include <stddef.h>

#define AES256_KEY_SIZE   32
#define AES_GCM_IV_SIZE   12
#define AES_GCM_TAG_SIZE  16

/* Detección de features CPU (inicializado automáticamente) */
extern int g_has_aes_ni;
extern int g_has_pclmul;

int aes256_gcm_encrypt(const uint8_t *key, const uint8_t *iv,
                       const uint8_t *plain, size_t plain_len,
                       uint8_t *out, size_t *out_len);

int aes256_gcm_decrypt(const uint8_t *key, const uint8_t *iv,
                       const uint8_t *enc, size_t enc_len,
                       uint8_t *out, size_t *out_len);

#endif /* AES_GCM_H */
