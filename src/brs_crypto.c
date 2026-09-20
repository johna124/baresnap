/* brs_crypto.c — Argon2 + XChaCha20-Poly1305 + KVV (Monocypher) */
#include "brs_crypto.h"

#include "brs_util.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "monocypher.h"
#include "aes_gcm.h"

#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif

/* XChaCha20-Poly1305: nonce 24, mac 16 (coinciden con las dimensiones
 * del KVV, pero semanticamente son constantes del AEAD). */
#define BRS_AEAD_NONCE_LEN 24
#define BRS_AEAD_MAC_LEN   16

/* ============================================================================
 * Lectura de passphrase acotada
 * FIX: limite estricto de tamano + fd cerrado en todas las rutas.
 * ==========================================================================*/
static int read_line_tty(int fd, char *out, size_t out_size)
{
    size_t len = 0;
    int overflow = 0;
    char ch;
    for (;;) {
        ssize_t n = read(fd, &ch, 1);
        if (n <= 0) break;
        if (ch == '\n' || ch == '\r') break;
        if (len + 1 < out_size) {
            out[len++] = ch;
        } else {
            overflow = 1;   /* seguir drenando la linea sin almacenar */
        }
    }
    out[len] = '\0';
    return overflow ? -1 : 0;
}

int brs_read_passphrase(char *out, size_t out_size, const char *prompt)
{
    if (!out || out_size < 2) return -1;
    out[0] = '\0';

    const char *env = getenv("BARESNAP_PASSPHRASE");
    if (env && env[0] != '\0') {
        size_t n = strlen(env);
        if (n >= out_size) {
            fprintf(stderr, "error: BARESNAP_PASSPHRASE too long (max %zu)\n",
                    out_size - 1);
            return -1;
        }
        memcpy(out, env, n + 1);
        return 0;
    }

    int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr,
                "error: cannot open /dev/tty and BARESNAP_PASSPHRASE not set\n");
        return -1;
    }

    struct termios old_term, new_term;
    int term_set = 0;
    if (tcgetattr(fd, &old_term) == 0) {
        new_term = old_term;
        new_term.c_lflag &= ~((tcflag_t)ECHO);
        if (tcsetattr(fd, TCSANOW, &new_term) == 0) term_set = 1;
    }

    if (prompt) (void)!write(fd, prompt, strlen(prompt));
    int rc = read_line_tty(fd, out, out_size);
    (void)!write(fd, "\n", 1);

    if (term_set) tcsetattr(fd, TCSANOW, &old_term);
    close(fd);   /* FIX: siempre cerrado, tambien si tcgetattr fallo */

    if (rc != 0) {
        fprintf(stderr, "error: passphrase too long (max %zu)\n", out_size - 1);
        brs_secure_wipe(out, out_size);
        return -1;
    }
    if (out[0] == '\0') {
        fprintf(stderr, "error: empty passphrase\n");
        return -1;
    }
    return 0;
}

/* ============================================================================
 * Argon2
 * ==========================================================================*/
int brs_argon2_derive(BrsSecureKey *out_key,
                      const char *pass, size_t pass_len,
                      const uint8_t salt[BRS_CRYPTO_SALT_LEN],
                      uint32_t nb_blocks, uint32_t nb_passes)
{
    if (!out_key || !pass || !salt) return -1;
    if (nb_blocks == 0 || nb_passes == 0) return -1;
    if (pass_len > UINT32_MAX) return -1;
    if ((uint64_t)nb_blocks * 1024ULL > (uint64_t)SIZE_MAX) return -1;

    size_t work_size = (size_t)nb_blocks * 1024;
    void *work = malloc(work_size);
    if (!work) return -1;

    crypto_argon2_config config;
    config.nb_blocks = nb_blocks;
    config.nb_passes = nb_passes;
    config.nb_lanes  = 1;

    crypto_argon2_inputs inputs;
    inputs.pass      = (const uint8_t *)pass;
    inputs.pass_size = (uint32_t)pass_len;
    inputs.salt      = salt;
    inputs.salt_size = BRS_CRYPTO_SALT_LEN;

    crypto_argon2_extras extras = crypto_argon2_no_extras;

    crypto_argon2(out_key->bytes, BRS_KEY_LEN, work, config, inputs, extras);

    /* El work area contiene estado intermedio de Argon2: se limpia. */
    brs_secure_wipe(work, work_size);
    free(work);
    return 0;
}

/* ============================================================================
 * init_crypto: salt + KVV para un repo nuevo
 * FIX: salt con RNG real (falla sin entropia, nunca determinista).
 * FIX: empaquetado KVV unico via brs_kvv_pack (72 bytes exactos).
 * ==========================================================================*/
int brs_init_crypto(BrsRepoConfig *cfg, const char *passphrase)
{
    if (!cfg || !passphrase || passphrase[0] == '\0') return -1;

    if (brs_random_bytes(cfg->crypto_salt, BRS_CRYPTO_SALT_LEN) != 0) {
        fprintf(stderr, "error: no entropy available for crypto salt\n");
        return -1;
    }

    if (getenv("BRS_TEST_FAST_KDF")) {
        cfg->kdf_nb_blocks = 8;
        cfg->kdf_nb_passes = 1;
    } else {
        if (cfg->kdf_nb_blocks == 0) cfg->kdf_nb_blocks = BRS_KDF_DEFAULT_NB_BLOCKS;
        if (cfg->kdf_nb_passes == 0) cfg->kdf_nb_passes = BRS_KDF_DEFAULT_NB_PASSES;
    } /* FIX: BRS_TEST_FAST_KDF */
    cfg->encrypted = 1;
    cfg->flags |= BRS_FLAG_ENCRYPTED;

    BrsSecureKey key;
    if (brs_argon2_derive(&key, passphrase, strlen(passphrase),
                          cfg->crypto_salt,
                          cfg->kdf_nb_blocks, cfg->kdf_nb_passes) != 0) {
        return -1;
    }

    uint8_t nonce[BRS_AEAD_NONCE_LEN];
    if (brs_random_bytes(nonce, sizeof nonce) != 0) {
        brs_secure_key_wipe(&key);
        fprintf(stderr, "error: no entropy available for KVV nonce\n");
        return -1;
    }

    uint8_t zeros[BRS_KVV_CT_LEN];
    uint8_t ct[BRS_KVV_CT_LEN];
    uint8_t mac[BRS_KVV_MAC_LEN];
    memset(zeros, 0, sizeof zeros);

    crypto_aead_lock(ct, mac, key.bytes, nonce,
                     NULL, 0, zeros, sizeof zeros);
    brs_secure_key_wipe(&key);

    return brs_kvv_pack(cfg->kvv, nonce, ct, mac);
}

/* ============================================================================
 * derive_key: derivar y validar contra el KVV
 * ==========================================================================*/
int brs_derive_key(const BrsRepoConfig *cfg, const char *passphrase,
                   BrsSecureKey *out_key)
{
    if (!cfg || !passphrase || !out_key) return -1;
    if (!cfg->encrypted) return 0;

    if (brs_argon2_derive(out_key, passphrase, strlen(passphrase),
                          cfg->crypto_salt,
                          cfg->kdf_nb_blocks, cfg->kdf_nb_passes) != 0) {
        return -1;
    }

    uint8_t nonce[BRS_KVV_NONCE_LEN];
    uint8_t ct[BRS_KVV_CT_LEN];
    uint8_t mac[BRS_KVV_MAC_LEN];
    if (brs_kvv_unpack(cfg->kvv, nonce, ct, mac) != 0) {
        brs_secure_key_wipe(out_key);
        return -1;
    }

    uint8_t decrypted[BRS_KVV_CT_LEN];
    int rc = crypto_aead_unlock(decrypted, mac, out_key->bytes, nonce,
                                NULL, 0, ct, sizeof ct);
    brs_secure_wipe(nonce, sizeof nonce);
    if (rc != 0) return -1;

    int ok = 1;
    for (size_t i = 0; i < sizeof decrypted; ++i) {
        if (decrypted[i] != 0) ok = 0;
    }
    brs_secure_wipe(decrypted, sizeof decrypted);
    return ok ? 0 : -1;
}

/* ============================================================================
 * AEAD buffers/chunks (una sola implementacion para ambos)
 * ==========================================================================*/
static int encrypt_chacha20(const BrsSecureKey *key,
const uint8_t *plain, size_t plain_len,
BrsBuffer *out)
{
    if (!key || !out) return -1;
    if (plain_len > 0 && !plain) return -1;

    uint8_t nonce[BRS_AEAD_NONCE_LEN];
    if (brs_random_bytes(nonce, sizeof nonce) != 0) return -1;

    size_t total = BRS_AEAD_NONCE_LEN + plain_len + BRS_AEAD_MAC_LEN;
    if (total > BRS_BUFFER_MAX) return -1;
    if (brs_buffer_resize(out, total) != 0) return -1;

    memcpy(out->data, nonce, BRS_AEAD_NONCE_LEN);
    crypto_aead_lock(out->data + BRS_AEAD_NONCE_LEN,
                     out->data + BRS_AEAD_NONCE_LEN + plain_len,
                     key->bytes, nonce,
                     NULL, 0, plain, plain_len);
    return 0;
}

static int decrypt_chacha20(const BrsSecureKey *key,
const uint8_t *enc, size_t enc_len,
BrsBuffer *out)
{
    if (!key || !enc || !out) return -1;
    if (enc_len < BRS_AEAD_NONCE_LEN + BRS_AEAD_MAC_LEN) return -1;

    const uint8_t *nonce = enc;
    size_t ct_len = enc_len - BRS_AEAD_NONCE_LEN - BRS_AEAD_MAC_LEN;
    const uint8_t *ct  = enc + BRS_AEAD_NONCE_LEN;
    const uint8_t *mac = enc + BRS_AEAD_NONCE_LEN + ct_len;

    if (brs_buffer_resize(out, ct_len) != 0) return -1;

    int rc = crypto_aead_unlock(out->data, mac, key->bytes, nonce,
                                NULL, 0, ct, ct_len);
    if (rc != 0) {
        brs_buffer_wipe_free(out);
        brs_buffer_init(out);
        return -1;
    }
    return 0;
}

/* ============================================================================
AEAD: AES-256-GCM
==========================================================================*/
    static int encrypt_aes256gcm(const BrsSecureKey *key,
                             const uint8_t *plain, size_t plain_len,
                             BrsBuffer *out)
{
    if (!key || !out) return -1;
    if (plain_len > 0 && !plain) return -1;

    uint8_t iv[AES_GCM_IV_SIZE];
    if (brs_random_bytes(iv, sizeof iv) != 0) return -1;

    size_t total = AES_GCM_IV_SIZE + plain_len + AES_GCM_TAG_SIZE;
    if (total > BRS_BUFFER_MAX) return -1;

    if (brs_buffer_resize(out, total) != 0) return -1;
    memcpy(out->data, iv, AES_GCM_IV_SIZE);

    const uint8_t *in = plain_len ? plain : (const uint8_t *)"";
    size_t enc_len = 0;

    if (aes256_gcm_encrypt(key->bytes, iv, in, plain_len,
                           out->data + AES_GCM_IV_SIZE, &enc_len) != 0) {
        brs_buffer_wipe_free(out);
        brs_buffer_init(out);
        return -1;
    }

    if (AES_GCM_IV_SIZE + enc_len > total) {
        brs_buffer_wipe_free(out);
        brs_buffer_init(out);
        return -1;
    }

    out->size = AES_GCM_IV_SIZE + enc_len;
    return 0;
}

static int decrypt_aes256gcm(const BrsSecureKey *key,
                             const uint8_t *enc, size_t enc_len,
                             BrsBuffer *out)
{
    if (!key || !enc || !out) return -1;
    if (enc_len < AES_GCM_IV_SIZE + AES_GCM_TAG_SIZE) return -1;

    const uint8_t *iv = enc;
    const uint8_t *payload = enc + AES_GCM_IV_SIZE;
    size_t payload_len = enc_len - AES_GCM_IV_SIZE;

    if (payload_len < AES_GCM_TAG_SIZE) return -1;

    if (brs_buffer_resize(out, payload_len) != 0) return -1;

    size_t dec_len = 0;
    if (aes256_gcm_decrypt(key->bytes, iv, payload, payload_len,
                           out->data, &dec_len) != 0) {
        brs_buffer_wipe_free(out);
        brs_buffer_init(out);
        return -1;
    }

    if (dec_len > payload_len - AES_GCM_TAG_SIZE) {
        brs_buffer_wipe_free(out);
        brs_buffer_init(out);
        return -1;
    }

    if (brs_buffer_resize(out, dec_len) != 0) {
        brs_buffer_wipe_free(out);
        brs_buffer_init(out);
        return -1;
    }

    return 0;
}

/* ============================================================================
API pública: dispatch por cipher
==========================================================================*/
int brs_encrypt_buffer(const BrsSecureKey *key, BrsCipherAlgo cipher,
const uint8_t *plain, size_t plain_len,
BrsBuffer *out)
{
switch (cipher) {
case BRS_CIPHER_AES256_GCM:
    return encrypt_aes256gcm(key, plain, plain_len, out);
case BRS_CIPHER_CHACHA20_POLY1305:
default:
    return encrypt_chacha20(key, plain, plain_len, out);
}
}

int brs_decrypt_buffer(const BrsSecureKey *key, BrsCipherAlgo cipher,
const uint8_t *enc, size_t enc_len,
BrsBuffer *out)
{
switch (cipher) {
case BRS_CIPHER_AES256_GCM:
    return decrypt_aes256gcm(key, enc, enc_len, out);
case BRS_CIPHER_CHACHA20_POLY1305:
default:
    return decrypt_chacha20(key, enc, enc_len, out);
}
}
