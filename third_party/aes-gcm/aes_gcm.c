/* AES-256-GCM — implementación con aceleración AES-NI/PCLMULQDQ + fallback software
 * Basada en FIPS 197 (AES) y NIST SP 800-38D (GCM).
 * AES-NI: hardware acceleration para AES encrypt/decrypt
 * PCLMULQDQ: carry-less multiplication para GHASH
 */
#include "aes_gcm.h"

/* FIX BRS-AES: forzar camino software hasta validar el path hardware.
 * El path hardware actual necesita tests NIST y dispatch seguro. */
#ifndef BRS_AES_FORCE_SOFTWARE
#define BRS_AES_FORCE_SOFTWARE 1
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__x86_64__) && !defined(BRS_AES_FORCE_SOFTWARE)
#include <wmmintrin.h>  /* AES-NI */
#include <tmmintrin.h>  /* SSSE3 */
#include <smmintrin.h>  /* SSE4.1 */
#include <cpuid.h>
#endif

/* ============================================================
 * Detección de CPU features vía CPUID
 * ============================================================ */
int g_has_aes_ni = 0;
int g_has_pclmul = 0;

static void __attribute__((constructor)) detect_cpu_features(void)
{
    /* FIX: permitir forzar software con BARESNAP_AES_HW=0 */
    const char *env = getenv("BARESNAP_AES_HW");
    if (env && env[0] == '0') {
        g_has_aes_ni = 0;
        g_has_pclmul = 0;
        return;
    }

#ifdef BRS_AES_FORCE_SOFTWARE
    return;
#endif
#if defined(__x86_64__) && !defined(BRS_AES_FORCE_SOFTWARE)
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        g_has_aes_ni = (ecx >> 25) & 1;   /* bit 25 = AES-NI */
        g_has_pclmul = (ecx >> 1) & 1;    /* bit 1 = PCLMULQDQ */
    }
#endif
}

/* ============================================================
 * AES-256 core (software fallback)
 * ============================================================ */
static const uint8_t sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static const uint8_t rcon[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ (((x >> 7) & 1) * 0x1b)); }

static void sub_word(uint8_t *w) {
    w[0] = sbox[w[0]]; w[1] = sbox[w[1]];
    w[2] = sbox[w[2]]; w[3] = sbox[w[3]];
}

static void aes256_key_expansion_sw(const uint8_t key[32], uint8_t rk[240])
{
    memcpy(rk, key, 32);
    for (int i = 8; i < 60; i++) {
        uint8_t t[4];
        memcpy(t, rk + 4 * (i - 1), 4);
        if (i % 8 == 0) {
            uint8_t tmp = t[0];
            t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = tmp;
            sub_word(t);
            t[0] ^= rcon[i / 8];
        } else if (i % 8 == 4) {
            sub_word(t);
        }
        for (int j = 0; j < 4; j++)
            rk[4 * i + j] = rk[4 * (i - 8) + j] ^ t[j];
    }
}

static void mix_columns(uint8_t *s)
{
    for (int c = 0; c < 4; c++) {
        uint8_t *col = s + c * 4;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        uint8_t m = a0 ^ a1 ^ a2 ^ a3;
        col[0] ^= xtime(a0 ^ a1) ^ m;
        col[1] ^= xtime(a1 ^ a2) ^ m;
        col[2] ^= xtime(a2 ^ a3) ^ m;
        col[3] ^= xtime(a3 ^ a0) ^ m;
    }
}

static void shift_rows(uint8_t *s)
{
    uint8_t t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

static void aes_encrypt_block_sw(const uint8_t rk[240], const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int r = 1; r <= 13; r++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        shift_rows(s);
        mix_columns(s);
        for (int i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
    }
    for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
    shift_rows(s);
    for (int i = 0; i < 16; i++) s[i] ^= rk[14 * 16 + i];
    memcpy(out, s, 16);
}

/* ============================================================
 * AES-256 con AES-NI (hardware acceleration)
 * ============================================================ */
#if defined(__x86_64__) && !defined(BRS_AES_FORCE_SOFTWARE)

static void aes256_key_expansion_hw(const uint8_t key[32], __m128i rk[15])
{
    __m128i t1, t2, t3, t4;
    rk[0]  = _mm_loadu_si128((const __m128i*)key);
    rk[1]  = _mm_loadu_si128((const __m128i*)(key + 16));

    #define AES256_KEYGEN_ASSIST(x, rcon_val) \
        _mm_aeskeygenassist_si128(x, rcon_val)

    t1 = rk[0];
    t3 = rk[1];

    /* Round 1 (rcon=0x01) */
    t2 = AES256_KEYGEN_ASSIST(t3, 0x01);
    t4 = _mm_shuffle_epi32(t2, 0xff);
    t2 = _mm_xor_si128(t1, _mm_slli_si128(t1, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t1 = _mm_xor_si128(t2, t4);
    rk[2] = t1;

    t4 = AES256_KEYGEN_ASSIST(t1, 0x00);
    t4 = _mm_shuffle_epi32(t4, 0xaa);
    t2 = _mm_xor_si128(t3, _mm_slli_si128(t3, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t3 = _mm_xor_si128(t2, t4);
    rk[3] = t3;

    /* Round 2 (rcon=0x02) */
    t2 = AES256_KEYGEN_ASSIST(t3, 0x02);
    t4 = _mm_shuffle_epi32(t2, 0xff);
    t2 = _mm_xor_si128(t1, _mm_slli_si128(t1, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t1 = _mm_xor_si128(t2, t4);
    rk[4] = t1;

    t4 = AES256_KEYGEN_ASSIST(t1, 0x00);
    t4 = _mm_shuffle_epi32(t4, 0xaa);
    t2 = _mm_xor_si128(t3, _mm_slli_si128(t3, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t3 = _mm_xor_si128(t2, t4);
    rk[5] = t3;

    /* Round 3 (rcon=0x04) */
    t2 = AES256_KEYGEN_ASSIST(t3, 0x04);
    t4 = _mm_shuffle_epi32(t2, 0xff);
    t2 = _mm_xor_si128(t1, _mm_slli_si128(t1, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t1 = _mm_xor_si128(t2, t4);
    rk[6] = t1;

    t4 = AES256_KEYGEN_ASSIST(t1, 0x00);
    t4 = _mm_shuffle_epi32(t4, 0xaa);
    t2 = _mm_xor_si128(t3, _mm_slli_si128(t3, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t3 = _mm_xor_si128(t2, t4);
    rk[7] = t3;

    /* Round 4 (rcon=0x08) */
    t2 = AES256_KEYGEN_ASSIST(t3, 0x08);
    t4 = _mm_shuffle_epi32(t2, 0xff);
    t2 = _mm_xor_si128(t1, _mm_slli_si128(t1, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t1 = _mm_xor_si128(t2, t4);
    rk[8] = t1;

    t4 = AES256_KEYGEN_ASSIST(t1, 0x00);
    t4 = _mm_shuffle_epi32(t4, 0xaa);
    t2 = _mm_xor_si128(t3, _mm_slli_si128(t3, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t3 = _mm_xor_si128(t2, t4);
    rk[9] = t3;

    /* Round 5 (rcon=0x10) */
    t2 = AES256_KEYGEN_ASSIST(t3, 0x10);
    t4 = _mm_shuffle_epi32(t2, 0xff);
    t2 = _mm_xor_si128(t1, _mm_slli_si128(t1, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t1 = _mm_xor_si128(t2, t4);
    rk[10] = t1;

    t4 = AES256_KEYGEN_ASSIST(t1, 0x00);
    t4 = _mm_shuffle_epi32(t4, 0xaa);
    t2 = _mm_xor_si128(t3, _mm_slli_si128(t3, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t3 = _mm_xor_si128(t2, t4);
    rk[11] = t3;

    /* Round 6 (rcon=0x20) */
    t2 = AES256_KEYGEN_ASSIST(t3, 0x20);
    t4 = _mm_shuffle_epi32(t2, 0xff);
    t2 = _mm_xor_si128(t1, _mm_slli_si128(t1, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t1 = _mm_xor_si128(t2, t4);
    rk[12] = t1;

    t4 = AES256_KEYGEN_ASSIST(t1, 0x00);
    t4 = _mm_shuffle_epi32(t4, 0xaa);
    t2 = _mm_xor_si128(t3, _mm_slli_si128(t3, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t3 = _mm_xor_si128(t2, t4);
    rk[13] = t3;

    /* Round 7 (rcon=0x40) — final */
    t2 = AES256_KEYGEN_ASSIST(t3, 0x40);
    t4 = _mm_shuffle_epi32(t2, 0xff);
    t2 = _mm_xor_si128(t1, _mm_slli_si128(t1, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t2 = _mm_xor_si128(t2, _mm_slli_si128(t2, 4));
    t1 = _mm_xor_si128(t2, t4);
    rk[14] = t1;

    #undef AES256_KEYGEN_ASSIST
}

static void aes_encrypt_block_hw(const __m128i rk[15], const uint8_t in[16], uint8_t out[16])
{
    __m128i m = _mm_loadu_si128((const __m128i*)in);

    m = _mm_xor_si128(m, rk[0]);
    m = _mm_aesenc_si128(m, rk[1]);
    m = _mm_aesenc_si128(m, rk[2]);
    m = _mm_aesenc_si128(m, rk[3]);
    m = _mm_aesenc_si128(m, rk[4]);
    m = _mm_aesenc_si128(m, rk[5]);
    m = _mm_aesenc_si128(m, rk[6]);
    m = _mm_aesenc_si128(m, rk[7]);
    m = _mm_aesenc_si128(m, rk[8]);
    m = _mm_aesenc_si128(m, rk[9]);
    m = _mm_aesenc_si128(m, rk[10]);
    m = _mm_aesenc_si128(m, rk[11]);
    m = _mm_aesenc_si128(m, rk[12]);
    m = _mm_aesenc_si128(m, rk[13]);
    m = _mm_aesenclast_si128(m, rk[14]);

    _mm_storeu_si128((__m128i*)out, m);
}

/* ============================================================
 * GHASH con PCLMULQDQ (carry-less multiplication)
 * ============================================================ */

/* Reducción en GF(2^128) del producto de 256 bits */
static __m128i gcm_reduce(__m128i lo, __m128i hi)
{
    /* Reducción polinómica modular x^128 + x^7 + x^2 + x + 1 */
    __m128i tmp, t1, t2, t3;

    /* Primera reducción */
    tmp = _mm_clmulepi64_si128(hi, _mm_set_epi32(0, 0, 0, 0xc2000000), 0x10);
    t1 = _mm_srli_si128(tmp, 8);
    t2 = _mm_slli_si128(tmp, 8);
    hi = _mm_xor_si128(hi, t1);
    lo = _mm_xor_si128(lo, t2);

    /* Segunda reducción */
    tmp = _mm_clmulepi64_si128(hi, _mm_set_epi32(0, 0, 0, 0xc2000000), 0x10);
    t1 = _mm_srli_si128(tmp, 8);
    t2 = _mm_slli_si128(tmp, 8);
    hi = _mm_xor_si128(hi, t1);
    lo = _mm_xor_si128(lo, t2);

    return _mm_xor_si128(lo, hi);
}

/* Multiplicación en GF(2^128) usando PCLMULQDQ */
static void gf_mult_hw(const uint8_t X[16], const uint8_t Y[16], uint8_t Z[16])
{
    /* GCM usa bit-reflected polynomials: necesitamos byte-swap */
    __m128i a = _mm_loadu_si128((const __m128i*)X);
    __m128i b = _mm_loadu_si128((const __m128i*)Y);

    /* Byte swap para convertir a formato polinómico de GCM */
    __m128i mask = _mm_set_epi8(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
    a = _mm_shuffle_epi8(a, mask);
    b = _mm_shuffle_epi8(b, mask);

    /* Karatsuba multiplication en GF(2^128) */
    __m128i lo  = _mm_clmulepi64_si128(a, b, 0x00);
    __m128i hi  = _mm_clmulepi64_si128(a, b, 0x11);
    __m128i mid1 = _mm_clmulepi64_si128(a, b, 0x01);
    __m128i mid2 = _mm_clmulepi64_si128(a, b, 0x10);
    __m128i mid = _mm_xor_si128(mid1, mid2);

    __m128i mid_lo = _mm_slli_si128(mid, 8);
    __m128i mid_hi = _mm_srli_si128(mid, 8);

    lo = _mm_xor_si128(lo, mid_lo);
    hi = _mm_xor_si128(hi, mid_hi);

    __m128i result = gcm_reduce(lo, hi);

    /* Byte swap de vuelta */
    result = _mm_shuffle_epi8(result, mask);
    _mm_storeu_si128((__m128i*)Z, result);
}

static void ghash_hw(const uint8_t H[16], const uint8_t *data, size_t len, uint8_t tag[16])
{
    uint8_t block[16];
    size_t full = len / 16, rem = len % 16;

    for (size_t i = 0; i < full; i++) {
        for (int j = 0; j < 16; j++) tag[j] ^= data[i * 16 + j];
        uint8_t tmp[16];
        gf_mult_hw(tag, H, tmp);
        memcpy(tag, tmp, 16);
    }
    if (rem) {
        memset(block, 0, 16);
        memcpy(block, data + full * 16, rem);
        for (int j = 0; j < 16; j++) tag[j] ^= block[j];
        uint8_t tmp[16];
        gf_mult_hw(tag, H, tmp);
        memcpy(tag, tmp, 16);
    }
}

#endif /* __x86_64__ */

/* ============================================================
 * GF multiplication software fallback
 * ============================================================ */
static void gf_mult_sw(const uint8_t X[16], const uint8_t Y[16], uint8_t Z[16])
{
    uint8_t V[16];
    memcpy(V, Y, 16);
    memset(Z, 0, 16);
    for (int i = 0; i < 128; i++) {
        if (X[i / 8] & (0x80 >> (i % 8)))
            for (int j = 0; j < 16; j++) Z[j] ^= V[j];
        int lsb = V[15] & 1;
        for (int j = 15; j > 0; j--)
            V[j] = (uint8_t)((V[j] >> 1) | (V[j - 1] << 7));
        V[0] >>= 1;
        if (lsb) V[0] ^= 0xe1;
    }
}

static void ghash_sw(const uint8_t H[16], const uint8_t *data, size_t len, uint8_t tag[16])
{
    uint8_t blk[16], tmp[16];
    size_t full = len / 16, rem = len % 16;
    for (size_t i = 0; i < full; i++) {
        for (int j = 0; j < 16; j++) tag[j] ^= data[i * 16 + j];
        gf_mult_sw(tag, H, tmp);
        memcpy(tag, tmp, 16);
    }
    if (rem) {
        memset(blk, 0, 16);
        memcpy(blk, data + full * 16, rem);
        for (int j = 0; j < 16; j++) tag[j] ^= blk[j];
        gf_mult_sw(tag, H, tmp);
        memcpy(tag, tmp, 16);
    }
}

/* ============================================================
 * Dispatch: hardware o software
 * ============================================================ */
static void gf_mult_dispatch(const uint8_t X[16], const uint8_t Y[16], uint8_t Z[16])
{
#if defined(__x86_64__) && !defined(BRS_AES_FORCE_SOFTWARE)
    if (g_has_pclmul) {
        gf_mult_hw(X, Y, Z);
        return;
    }
#endif
    gf_mult_sw(X, Y, Z);
}

static void ghash_dispatch(const uint8_t H[16], const uint8_t *data, size_t len, uint8_t tag[16])
{
#if defined(__x86_64__) && !defined(BRS_AES_FORCE_SOFTWARE)
    if (g_has_pclmul) {
        ghash_hw(H, data, len, tag);
        return;
    }
#endif
    ghash_sw(H, data, len, tag);
}

/* ============================================================
 * AES encrypt block dispatch
 * ============================================================ */

/* Contexto de round keys: puede ser sw (240 bytes) o hw (15 x __m128i = 240 bytes) */
typedef struct {
    uint8_t sw_rk[240];
#if defined(__x86_64__) && !defined(BRS_AES_FORCE_SOFTWARE)
    __m128i hw_rk[15];
#endif
    int use_hw;
} AesRoundKeys;

static void aes256_key_expansion(const uint8_t key[32], AesRoundKeys *rk)
{
#if defined(__x86_64__) && !defined(BRS_AES_FORCE_SOFTWARE)
    if (g_has_aes_ni) {
        aes256_key_expansion_hw(key, rk->hw_rk);
        rk->use_hw = 1;
        /* También generar SW para GHASH H (que usa encrypt de zero block) */
        aes256_key_expansion_sw(key, rk->sw_rk);
        return;
    }
#endif
    aes256_key_expansion_sw(key, rk->sw_rk);
    rk->use_hw = 0;
}

static void aes_encrypt_block(const AesRoundKeys *rk, const uint8_t in[16], uint8_t out[16])
{
#if defined(__x86_64__) && !defined(BRS_AES_FORCE_SOFTWARE)
    if (rk->use_hw) {
        aes_encrypt_block_hw(rk->hw_rk, in, out);
        return;
    }
#endif
    aes_encrypt_block_sw(rk->sw_rk, in, out);
}

/* ============================================================
 * GCM core
 * ============================================================ */
static void gcm_compute_tag(const AesRoundKeys *rk, const uint8_t H[16],
                            const uint8_t J0[16],
                            const uint8_t *cipher, size_t cipher_len,
                            uint8_t tag[16])
{
    memset(tag, 0, 16);
    ghash_dispatch(H, cipher, cipher_len, tag);

    /* Length block: len(AAD)=0 || len(C) en bits, big-endian */
    uint8_t lb[16];
    memset(lb, 0, 16);
    uint64_t cb = (uint64_t)cipher_len * 8;
    lb[8]  = (uint8_t)(cb >> 56); lb[9]  = (uint8_t)(cb >> 48);
    lb[10] = (uint8_t)(cb >> 40); lb[11] = (uint8_t)(cb >> 32);
    lb[12] = (uint8_t)(cb >> 24); lb[13] = (uint8_t)(cb >> 16);
    lb[14] = (uint8_t)(cb >> 8);  lb[15] = (uint8_t)(cb);

    uint8_t tmp[16];
    for (int j = 0; j < 16; j++) tag[j] ^= lb[j];
    gf_mult_dispatch(tag, H, tmp);
    memcpy(tag, tmp, 16);

    uint8_t ej0[16];
    aes_encrypt_block(rk, J0, ej0);
    for (int j = 0; j < 16; j++) tag[j] ^= ej0[j];
}

static void inc32(uint8_t ctr[16])
{
    for (int k = 15; k >= 12; k--) { if (++ctr[k]) break; }
}

/* ============================================================
 * API pública
 * ============================================================ */

/* ============================================================
   AES_GCM_SELFTEST: vectores NIST integrados
   ============================================================ */
static int g_aes_selftest_done = 0;
static int g_aes_selftest_ok = 0;
static int g_aes_selftest_running = 0;

static int aes_gcm_selftest_run(void);

static int aes_gcm_selftest(void)
{
    if (g_aes_selftest_done)
        return g_aes_selftest_ok;

    /* Evita recursión cuando el self-test llama a aes256_gcm_encrypt() */
    if (g_aes_selftest_running)
        return 1;

    g_aes_selftest_running = 1;
    g_aes_selftest_ok = aes_gcm_selftest_run();
    g_aes_selftest_running = 0;
    g_aes_selftest_done = 1;

    if (!g_aes_selftest_ok) {
        fprintf(stderr,
                "aes_gcm: NIST self-test FAILED; AES-256-GCM disabled\n");
    }

    return g_aes_selftest_ok;
}

static int aes_gcm_selftest_run(void)
{
    static const uint8_t key[32] = {0};
    static const uint8_t iv[12] = {0};
    static const uint8_t pt16[16] = {0};

    /* AES-256-GCM empty plaintext: tag NIST */
    static const uint8_t exp_tag_empty[16] = {
        0x53, 0x0f, 0x8a, 0xfb, 0xc7, 0x45, 0x36, 0xb9,
        0xa9, 0x63, 0xb4, 0xf1, 0xc4, 0xcb, 0x73, 0x8b
    };

    /* AES-256-GCM 16 zero bytes */
    static const uint8_t exp_ct16[16] = {
        0xce, 0xa7, 0x40, 0x3d, 0x4d, 0x60, 0x6b, 0x6e,
        0x07, 0x4e, 0xc5, 0xd3, 0xba, 0xf3, 0x9d, 0x18
    };

    static const uint8_t exp_tag16[16] = {
        0xd0, 0xd1, 0xc8, 0xa7, 0x99, 0x99, 0x6b, 0xf0,
        0x26, 0x5b, 0x98, 0xb5, 0xd4, 0x8a, 0xb9, 0x19
    };

    uint8_t out[64];
    size_t out_len = 0;

    /* Empty plaintext */
    if (aes256_gcm_encrypt(key, iv, NULL, 0, out, &out_len) != 0)
        return 0;

    if (out_len != AES_GCM_TAG_SIZE)
        return 0;

    if (memcmp(out, exp_tag_empty, AES_GCM_TAG_SIZE) != 0)
        return 0;

    /* 16 zero bytes */
    if (aes256_gcm_encrypt(key, iv, pt16, sizeof pt16, out, &out_len) != 0)
        return 0;

    if (out_len != sizeof pt16 + AES_GCM_TAG_SIZE)
        return 0;

    if (memcmp(out, exp_ct16, sizeof exp_ct16) != 0)
        return 0;

    if (memcmp(out + sizeof exp_ct16, exp_tag16, AES_GCM_TAG_SIZE) != 0)
        return 0;

    /* Decrypt roundtrip */
    uint8_t dec[16];
    size_t dec_len = 0;

    if (aes256_gcm_decrypt(key, iv, out, out_len, dec, &dec_len) != 0)
        return 0;

    if (dec_len != sizeof pt16)
        return 0;

    if (memcmp(dec, pt16, sizeof pt16) != 0)
        return 0;

    /* Tag corrupto debe fallar */
    out[out_len - 1] ^= 0x01;

    if (aes256_gcm_decrypt(key, iv, out, out_len, dec, &dec_len) == 0)
        return 0;

    return 1;
}

#if defined(__GNUC__)
__attribute__((constructor))
static void aes_gcm_selftest_ctor(void)
{
    (void)aes_gcm_selftest();
}
#endif


int aes256_gcm_encrypt(const uint8_t *key, const uint8_t *iv,
                       const uint8_t *plain, size_t plain_len,
                       uint8_t *out, size_t *out_len)
{
    if (!g_aes_selftest_running && aes_gcm_selftest() != 1) return -1;

    if (!key || !iv || (!plain && plain_len > 0) || !out || !out_len)
        return -1;

    AesRoundKeys rk;
    aes256_key_expansion(key, &rk);

    /* H = E_K(0^128) */
    uint8_t H[16], zero[16] = {0};
    aes_encrypt_block(&rk, zero, H);

    /* J0 = IV || 0x00000001 */
    uint8_t J0[16];
    memset(J0, 0, 16);
    memcpy(J0, iv, 12);
    J0[15] = 0x01;

    /* CTR mode encryption */
    uint8_t ctr[16];
    memcpy(ctr, J0, 16);
    inc32(ctr);

    for (size_t i = 0; i < plain_len; i += 16) {
        uint8_t ks[16];
        aes_encrypt_block(&rk, ctr, ks);
        size_t bl = (plain_len - i < 16) ? (plain_len - i) : 16;
        for (size_t j = 0; j < bl; j++)
            out[i + j] = plain[i + j] ^ ks[j];
        inc32(ctr);
    }

    /* Compute tag */
    uint8_t tag[16];
    gcm_compute_tag(&rk, H, J0, out, plain_len, tag);
    memcpy(out + plain_len, tag, AES_GCM_TAG_SIZE);
    *out_len = plain_len + AES_GCM_TAG_SIZE;

    /* Limpiar round keys de memoria */
    memset(&rk, 0, sizeof(rk));
    return 0;
}

int aes256_gcm_decrypt(const uint8_t *key, const uint8_t *iv,
                       const uint8_t *enc, size_t enc_len,
                       uint8_t *out, size_t *out_len)
{
    if (!g_aes_selftest_running && aes_gcm_selftest() != 1) return -1;

    if (!key || !iv || !enc || enc_len < AES_GCM_TAG_SIZE || !out || !out_len)
        return -1;

    size_t clen = enc_len - AES_GCM_TAG_SIZE;
    const uint8_t *cipher = enc;
    const uint8_t *rtag = enc + clen;

    AesRoundKeys rk;
    aes256_key_expansion(key, &rk);

    uint8_t H[16], zero[16] = {0};
    aes_encrypt_block(&rk, zero, H);

    uint8_t J0[16];
    memset(J0, 0, 16);
    memcpy(J0, iv, 12);
    J0[15] = 0x01;

    /* Compute expected tag */
    uint8_t tag[16];
    gcm_compute_tag(&rk, H, J0, cipher, clen, tag);

    /* Constant-time comparison */
    uint8_t diff = 0;
    for (int i = 0; i < AES_GCM_TAG_SIZE; i++) diff |= tag[i] ^ rtag[i];

    if (diff) {
        memset(&rk, 0, sizeof(rk));
        return -1;
    }

    /* CTR mode decryption */
    uint8_t ctr[16];
    memcpy(ctr, J0, 16);
    inc32(ctr);
    for (size_t i = 0; i < clen; i += 16) {
        uint8_t ks[16];
        aes_encrypt_block(&rk, ctr, ks);
        size_t bl = (clen - i < 16) ? (clen - i) : 16;
        for (size_t j = 0; j < bl; j++)
            out[i + j] = cipher[i + j] ^ ks[j];
        inc32(ctr);
    }
    *out_len = clen;

    memset(&rk, 0, sizeof(rk));
    return 0;
}
