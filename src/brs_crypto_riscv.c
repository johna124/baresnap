#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "brs_crypto.h"
#include "monocypher.h"

/* Funciones auxiliares para empaquetar/desempaquetar el KVV (Layout fijo de 72 bytes) */
static int brs_kvv_pack_local(uint8_t *dst, const uint8_t *nonce, const uint8_t *ct, const uint8_t *mac) {
    memcpy(dst, nonce, BRS_KVV_NONCE_LEN);
    memcpy(dst + BRS_KVV_NONCE_LEN, ct, BRS_KVV_CT_LEN);
    memcpy(dst + BRS_KVV_NONCE_LEN + BRS_KVV_CT_LEN, mac, BRS_KVV_MAC_LEN);
    return 0;
}

static int brs_kvv_unpack_local(const uint8_t *src, uint8_t *nonce, uint8_t *ct, uint8_t *mac) {
    memcpy(nonce, src, BRS_KVV_NONCE_LEN);
    memcpy(ct, src + BRS_KVV_NONCE_LEN, BRS_KVV_CT_LEN);
    memcpy(mac, src + BRS_KVV_NONCE_LEN + BRS_KVV_CT_LEN, BRS_KVV_MAC_LEN);
    return 0;
}

/* ============================================================================
 * brs_init_crypto: Edición especial para RISC-V e i386 (Anti-Ruido)
 * ==========================================================================*/
int brs_init_crypto(BrsRepoConfig *cfg, const char *passphrase)
{
    if (!cfg || !passphrase) return -1;

    BrsSecureKey key;
    uint8_t local_salt[BRS_CRYPTO_SALT_LEN];

    /* 1. Generar entropía limpia directamente en la pila alineada */
    if (brs_random_bytes(local_salt, BRS_CRYPTO_SALT_LEN) != 0) {
        return -1;
    }

    /* 2. Extraer parámetros de KDF de forma segura (Bypass a la desalineación de structs) */
    uint32_t local_blocks;
    uint32_t local_passes;
    memcpy(&local_blocks, &cfg->kdf_nb_blocks, sizeof(local_blocks));
    memcpy(&local_passes, &cfg->kdf_nb_passes, sizeof(local_passes));

    /* Si no están inicializados, usar los valores por defecto del ecosistema */
    if (local_blocks == 0) local_blocks = BRS_KDF_DEFAULT_NB_BLOCKS;
    if (local_passes == 0) local_passes = BRS_KDF_DEFAULT_NB_PASSES;

    /* 3. Derivar la clave utilizando Argon2id */
    if (brs_argon2_derive(&key, passphrase, strlen(passphrase),
                          local_salt, local_blocks, local_passes) != 0) {
        return -1;
    }

    /* 4. Cifrado del bloque KVV sobre buffers locales de la pila */
    uint8_t nonce[BRS_KVV_NONCE_LEN];
    if (brs_random_bytes(nonce, BRS_KVV_NONCE_LEN) != 0) {
        brs_secure_key_wipe(&key);
        return -1;
    }

    uint8_t zeros[BRS_KVV_CT_LEN] = {0};
    uint8_t ct[BRS_KVV_CT_LEN];
    uint8_t mac[BRS_KVV_MAC_LEN];

    crypto_aead_lock(ct, mac, key.bytes, nonce, NULL, 0, zeros, sizeof(zeros));
    brs_secure_key_wipe(&key);

    /* 5. Volcado plano final al struct del disco (Cero ruido, cero padding) */
    uint8_t local_kvv[BRS_KVV_SIZE];
    brs_kvv_pack_local(local_kvv, nonce, ct, mac);

    memcpy(cfg->crypto_salt, local_salt, BRS_CRYPTO_SALT_LEN);
    memcpy(cfg->kvv, local_kvv, BRS_KVV_SIZE);
    memcpy(&cfg->kdf_nb_blocks, &local_blocks, sizeof(local_blocks));
    memcpy(&cfg->kdf_nb_passes, &local_passes, sizeof(local_passes));

    uint32_t encrypted_flag = 1;
    memcpy(&cfg->encrypted, &encrypted_flag, sizeof(encrypted_flag));

    return 0;
}

/* ============================================================================
 * brs_derive_key: Edición especial para RISC-V e i386 (Anti-Ruido)
 * ==========================================================================*/
int brs_derive_key(const BrsRepoConfig *cfg, const char *passphrase,
                   BrsSecureKey *out_key)
{
    if (!cfg || !passphrase || !out_key) return -1;

    uint32_t is_encrypted;
    memcpy(&is_encrypted, &cfg->encrypted, sizeof(is_encrypted));
    if (!is_encrypted) return 0;

    /* Copia defensiva de Salt y constantes para evitar traps del silicio */
    uint8_t local_salt[BRS_CRYPTO_SALT_LEN];
    memcpy(local_salt, cfg->crypto_salt, BRS_CRYPTO_SALT_LEN);

    uint32_t local_blocks;
    uint32_t local_passes;
    memcpy(&local_blocks, &cfg->kdf_nb_blocks, sizeof(local_blocks));
    memcpy(&local_passes, &cfg->kdf_nb_passes, sizeof(local_passes));

    if (brs_argon2_derive(out_key, passphrase, strlen(passphrase),
                          local_salt, local_blocks, local_passes) != 0) {
        return -1;
    }

    /* Extraer KVV de forma plana */
    uint8_t local_kvv[BRS_KVV_SIZE];
    memcpy(local_kvv, cfg->kvv, BRS_KVV_SIZE);

    uint8_t nonce[BRS_KVV_NONCE_LEN];
    uint8_t ct[BRS_KVV_CT_LEN];
    uint8_t mac[BRS_KVV_MAC_LEN];

    if (brs_kvv_unpack_local(local_kvv, nonce, ct, mac) != 0) {
        brs_secure_key_wipe(out_key);
        return -1;
    }

    uint8_t decrypted[BRS_KVV_CT_LEN];
    /* Llamada a Monocypher usando la memoria de la pila perfectamente alineada */
    int rc = crypto_aead_unlock(decrypted, mac, out_key->bytes, nonce,
                                NULL, 0, ct, sizeof(ct));
    
    crypto_wipe(nonce, sizeof(nonce));
    if (rc != 0) return -1;

    int ok = 1;
    for (size_t i = 0; i < sizeof(decrypted); ++i) {
        if (decrypted[i] != 0) ok = 0;
    }
    crypto_wipe(decrypted, sizeof(decrypted));
    return ok ? 0 : -1;
}

