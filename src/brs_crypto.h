/* brs_crypto.h — cifrado BareSnap sobre Monocypher (C puro) */
#ifndef BRS_CRYPTO_H
#define BRS_CRYPTO_H

#include <stddef.h>
#include "brs_types.h"
#include "brs_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Passphrase: primero BARESNAP_PASSPHRASE, luego /dev/tty sin eco.
 * FIX init.cpp: lectura acotada a out_size-1 y fd cerrado en TODAS
 * las rutas de salida. Devuelve 0 ok, -1 error. */

/* BrsCipherAlgo definido en brs_types.h */

int brs_read_passphrase(char *out, size_t out_size, const char *prompt);

/* Argon2 (Monocypher). Work area = nb_blocks * 1024 bytes. */
int brs_argon2_derive(BrsSecureKey *out_key,
                      const char *pass, size_t pass_len,
                      const uint8_t salt[BRS_CRYPTO_SALT_LEN],
                      uint32_t nb_blocks, uint32_t nb_passes);

/* Genera salt aleatorio + KVV en cfg. Respeta kdf_nb_blocks/passes si
 * ya son != 0, si no usa los defaults.
 * FIX: sin fallback determinista del salt; KVV via brs_kvv_pack (72 B). */
int brs_init_crypto(BrsRepoConfig *cfg, const char *passphrase);

/* Deriva clave y valida contra el KVV. 0 ok, -1 passphrase incorrecta. */
int brs_derive_key(const BrsRepoConfig *cfg, const char *passphrase,
                   BrsSecureKey *out_key);

/* AEAD XChaCha20-Poly1305. Layout: nonce(24) | ciphertext | mac(16).
 * El C++ tenia encrypt_chunk/encrypt_buffer duplicados: aqui UNA funcion. */
int brs_encrypt_buffer(const BrsSecureKey *key, BrsCipherAlgo cipher,
                       const uint8_t *plain, size_t plain_len,
                       BrsBuffer *out);
int brs_decrypt_buffer(const BrsSecureKey *key, BrsCipherAlgo cipher,
                       const uint8_t *enc, size_t enc_len,
                       BrsBuffer *out);

#ifdef __cplusplus
}
#endif

#endif /* BRS_CRYPTO_H */