// Standalone crypto-NI KAT: no libcrypto dependency, so it can be built as a
// static x86 guest.  AES vectors are NIST SP 800-38A / SP 800-38D; GHASH is
// intentionally bit-serial here so the GCM tag oracle is independent of the
// PCLMUL lowering tested below.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wmmintrin.h>

static void aes128(const uint8_t key_bytes[16], const uint8_t in[16], uint8_t out[16]) {
    __m128i key = _mm_loadu_si128((const __m128i *)key_bytes);
    __m128i state = _mm_xor_si128(_mm_loadu_si128((const __m128i *)in), key);
#define EXPAND(rcon) do { \
    __m128i assist = _mm_shuffle_epi32(_mm_aeskeygenassist_si128(key, rcon), 0xff); \
    __m128i t = _mm_slli_si128(key, 4); key = _mm_xor_si128(key, t); \
    t = _mm_slli_si128(t, 4); key = _mm_xor_si128(key, t); \
    t = _mm_slli_si128(t, 4); key = _mm_xor_si128(key, t); \
    key = _mm_xor_si128(key, assist); \
} while (0)
#define ROUND(rcon) do { EXPAND(rcon); state = _mm_aesenc_si128(state, key); } while (0)
    ROUND(0x01); ROUND(0x02); ROUND(0x04); ROUND(0x08); ROUND(0x10);
    ROUND(0x20); ROUND(0x40); ROUND(0x80); ROUND(0x1b);
    EXPAND(0x36);
    state = _mm_aesenclast_si128(state, key);
#undef ROUND
#undef EXPAND
    _mm_storeu_si128((__m128i *)out, state);
}

static int equal(const uint8_t *a, const uint8_t *b, size_t n) { return memcmp(a, b, n) == 0; }

static void inc32(uint8_t counter[16]) {
    for (int i = 15; i >= 12; --i) if (++counter[i]) break;
}

static void ghash_mul(uint8_t out[16], const uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16] = {}, v[16];
    memcpy(v, h, 16);
    for (int bit = 0; bit < 128; ++bit) {
        if ((x[bit / 8] >> (7 - (bit & 7))) & 1)
            for (int i = 0; i < 16; ++i) z[i] ^= v[i];
        const unsigned lsb = v[15] & 1;
        for (int i = 15; i > 0; --i) v[i] = (uint8_t)((v[i] >> 1) | (v[i - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    memcpy(out, z, 16);
}

static void ghash_update(uint8_t y[16], const uint8_t x[16], const uint8_t h[16]) {
    uint8_t mixed[16];
    for (int i = 0; i < 16; ++i) mixed[i] = y[i] ^ x[i];
    ghash_mul(y, mixed, h);
}

static int aes_kats(void) {
    static const uint8_t ecb_key[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    static const uint8_t ecb_pt[16] = {0,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    static const uint8_t ecb_ct[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    static const uint8_t cbc_key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
    static const uint8_t cbc_iv[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    static const uint8_t cbc_pt[48] = {0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef};
    static const uint8_t cbc_ct[48] = {0x76,0x49,0xab,0xac,0x81,0x19,0xb2,0x46,0xce,0xe9,0x8e,0x9b,0x12,0xe9,0x19,0x7d,0x50,0x86,0xcb,0x9b,0x50,0x72,0x19,0xee,0x95,0xdb,0x11,0x3a,0x91,0x76,0x78,0xb2,0x73,0xbe,0xd6,0xb8,0xe3,0xc1,0x74,0x3b,0x71,0x16,0xe6,0x9e,0x22,0x22,0x95,0x16};
    uint8_t out[16], chain[16], block[16];
    aes128(ecb_key, ecb_pt, out);
    if (!equal(out, ecb_ct, 16)) return 0;
    memcpy(chain, cbc_iv, 16);
    for (int b = 0; b < 3; ++b) {
        for (int i = 0; i < 16; ++i) block[i] = cbc_pt[b * 16 + i] ^ chain[i];
        aes128(cbc_key, block, chain);
        if (!equal(chain, cbc_ct + b * 16, 16)) return 0;
    }
    return 1;
}

static int gcm_kat(void) {
    static const uint8_t expected_ct[16] = {0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78};
    static const uint8_t expected_tag[16] = {0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf};
    uint8_t zero[16] = {}, h[16], j0[16] = {}, ctr[16] = {}, ct[16], y[16] = {}, len[16] = {}, e[16], tag[16];
    j0[15] = 1; memcpy(ctr, j0, 16); inc32(ctr);
    aes128(zero, zero, h); aes128(zero, ctr, ct);
    if (!equal(ct, expected_ct, 16)) return 0;
    ghash_update(y, ct, h);
    len[15] = 128; ghash_update(y, len, h);
    aes128(zero, j0, e);
    for (int i = 0; i < 16; ++i) tag[i] = e[i] ^ y[i];
    return equal(tag, expected_tag, 16);
}

static __uint128_t ref_clmul(uint64_t a, uint64_t b) {
    __uint128_t out = 0;
    for (int i = 0; i < 64; ++i) if ((b >> i) & 1) out ^= (__uint128_t)a << i;
    return out;
}

static __m128i pclmul(__m128i a, __m128i b, int control) {
    switch (control) {
        case 0x00: return _mm_clmulepi64_si128(a, b, 0x00);
        case 0x01: return _mm_clmulepi64_si128(a, b, 0x01);
        case 0x10: return _mm_clmulepi64_si128(a, b, 0x10);
        default: return _mm_clmulepi64_si128(a, b, 0x11);
    }
}

static int pclmul_kat(void) {
    uint64_t seed = 0x9e3779b97f4a7c15ULL;
    static const int controls[] = {0, 1, 0x10, 0x11};
    for (int n = 0; n < 32; ++n) {
        uint64_t lane[4];
        for (int i = 0; i < 4; ++i) { seed = seed * 6364136223846793005ULL + 1; lane[i] = seed; }
        const __m128i a = _mm_set_epi64x((long long)lane[1], (long long)lane[0]);
        const __m128i b = _mm_set_epi64x((long long)lane[3], (long long)lane[2]);
        for (unsigned c = 0; c < sizeof(controls) / sizeof(controls[0]); ++c) {
            const int control = controls[c];
            const __uint128_t want = ref_clmul(lane[(control & 1) ? 1 : 0], lane[2 + ((control & 0x10) ? 1 : 0)]);
            __uint128_t got;
            _mm_storeu_si128((__m128i *)&got, pclmul(a, b, control));
            if (got != want) {
                printf("pclmul mismatch n=%d control=%02x\n", n, control);
                return 0;
            }
        }
    }
    return 1;
}

int main(void) {
    const int aes = aes_kats();
    const int gcm = gcm_kat();
    const int pclmul_ok = pclmul_kat();
    printf("aes=%s gcm_tag=%s pclmul=%s\n", aes ? "ok" : "fail", gcm ? "ok" : "fail", pclmul_ok ? "ok" : "fail");
    return aes && gcm && pclmul_ok ? 0 : 1;
}
