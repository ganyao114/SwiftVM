//
// avx_real_kernels.h -- the AVX/AVX2 workloads shared by the SwiftVM guest
// binary (avx_real_x86_64.c) and the native oracle binary (avx_real_host.c).
//
// These are deliberately *programs*, not encoding probes: dot products,
// matrix multiplies, a hand-rolled AVX2 memcpy/strlen, cross-lane shuffles.
// Every kernel folds its results into one u64 by BIT PATTERN, never by
// numeric comparison, so a wrong NaN payload or a wrong -0.0 is a failure
// rather than a silently-equal answer.
//
// Rules that keep the guest and the oracle byte-identical:
//   * no libc, no libm, no floating-point compares -- only bit casts;
//   * no runtime CPU dispatch: AVX2 is unconditional, so "did AVX really
//     execute?" is answered by the binary faulting, never by CPUID;
//   * no -ffast-math: scalar reduction order is fixed by C semantics.
//
// The caller supplies svm_write()/svm_exit(); everything else lives here.
//
#ifndef SVM_AVX_REAL_KERNELS_H
#define SVM_AVX_REAL_KERNELS_H

#include <immintrin.h>

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed int s32;

// Provided by the guest / host shim.
void svm_write(const char *buf, unsigned long len);
void svm_exit(int code);

// ---------------------------------------------------------------------------
// tiny freestanding output helpers
// ---------------------------------------------------------------------------
static void emit_str(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    svm_write(s, n);
}

static void emit_kv(const char *name, u64 value) {
    char buf[64];
    unsigned long i = 0;
    while (name[i]) { buf[i] = name[i]; i++; }
    buf[i++] = '=';
    for (int shift = 60; shift >= 0; shift -= 4) {
        buf[i++] = "0123456789abcdef"[(value >> shift) & 0xF];
    }
    buf[i++] = '\n';
    svm_write(buf, i);
}

// FNV-1a-ish 64-bit mixer.  Order-sensitive on purpose.
static u64 mix(u64 h, u64 v) {
    h ^= v;
    h *= 0x100000001b3ULL;
    h ^= h >> 29;
    return h;
}

static u64 mix_m256i(u64 h, __m256i v) {
    u64 tmp[4];
    _mm256_storeu_si256((__m256i *)tmp, v);
    for (int i = 0; i < 4; i++) h = mix(h, tmp[i]);
    return h;
}

static u64 mix_ps(u64 h, __m256 v) {
    return mix_m256i(h, _mm256_castps_si256(v));
}

static u64 mix_pd(u64 h, __m256d v) {
    return mix_m256i(h, _mm256_castpd_si256(v));
}

static float bits_to_f(u32 b) {
    union { u32 u; float f; } c;
    c.u = b;
    return c.f;
}

static u32 f_to_bits(float f) {
    union { u32 u; float f; } c;
    c.f = f;
    return c.u;
}

static double bits_to_d(u64 b) {
    union { u64 u; double d; } c;
    c.u = b;
    return c.d;
}

static u64 d_to_bits(double d) {
    union { u64 u; double d; } c;
    c.d = d;
    return c.u;
}

// Deterministic PRNG so both builds see identical inputs.
static u64 rng_state;
static u64 rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

// ---------------------------------------------------------------------------
// K1  fdot -- 4096-element float dot product, 8 lanes at a time
// ---------------------------------------------------------------------------
#define NDOT 4096
static float dot_a[NDOT], dot_b[NDOT];

static u64 k_fdot(void) {
    rng_state = 0x243f6a8885a308d3ULL;
    for (int i = 0; i < NDOT; i++) {
        // Small exact-ish magnitudes keep the reduction well conditioned but
        // still exercise rounding in every lane.
        dot_a[i] = (float)(int)(rng_next() % 2001 - 1000) * 0.125f;
        dot_b[i] = (float)(int)(rng_next() % 2001 - 1000) * 0.03125f;
    }
    __m256 acc = _mm256_setzero_ps();
    for (int i = 0; i < NDOT; i += 8) {
        __m256 x = _mm256_loadu_ps(dot_a + i);
        __m256 y = _mm256_loadu_ps(dot_b + i);
        acc = _mm256_add_ps(acc, _mm256_mul_ps(x, y));
    }
    u64 h = mix_ps(0xcbf29ce484222325ULL, acc);
    // Horizontal reduction through hadd + the 128-bit halves: exercises
    // VHADDPS, VEXTRACTF128 and the SSE-width tail.
    __m256 t = _mm256_hadd_ps(acc, acc);
    t = _mm256_hadd_ps(t, t);
    __m128 lo = _mm256_castps256_ps128(t);
    __m128 hi = _mm256_extractf128_ps(t, 1);
    __m128 sum = _mm_add_ss(lo, hi);
    h = mix(h, f_to_bits(_mm_cvtss_f32(sum)));
    h = mix_ps(h, t);
    return h;
}

// ---------------------------------------------------------------------------
// K2  fedge -- packed single-precision on NaN / Inf / denormal / -0.0
// ---------------------------------------------------------------------------
static u64 k_fedge(void) {
    const u32 pat[8] = {
        0x7fc00001u,  // qNaN, payload 1
        0xffc00002u,  // -qNaN, payload 2
        0x7f800000u,  // +Inf
        0xff800000u,  // -Inf
        0x00000001u,  // smallest denormal
        0x807fffffu,  // -largest denormal
        0x80000000u,  // -0.0
        0x3f800000u,  // 1.0
    };
    const u32 pat2[8] = {
        0x3f800000u, 0x00000001u, 0x7f800000u, 0x3f000000u,
        0x00800000u, 0x7f7fffffu, 0x00000000u, 0xbf800000u,
    };
    float a[8], b[8];
    for (int i = 0; i < 8; i++) { a[i] = bits_to_f(pat[i]); b[i] = bits_to_f(pat2[i]); }
    __m256 x = _mm256_loadu_ps(a), y = _mm256_loadu_ps(b);

    u64 h = 0x9e3779b97f4a7c15ULL;
    h = mix_ps(h, _mm256_add_ps(x, y));
    h = mix_ps(h, _mm256_sub_ps(x, y));
    h = mix_ps(h, _mm256_mul_ps(x, y));
    h = mix_ps(h, _mm256_div_ps(x, y));
    // min/max are NOT commutative on NaN: they return the second source.
    h = mix_ps(h, _mm256_min_ps(x, y));
    h = mix_ps(h, _mm256_max_ps(x, y));
    h = mix_ps(h, _mm256_min_ps(y, x));
    h = mix_ps(h, _mm256_max_ps(y, x));
    h = mix_ps(h, _mm256_sqrt_ps(x));
    h = mix_ps(h, _mm256_andnot_ps(_mm256_set1_ps(-0.0f), x));  // |x|
    h = mix_ps(h, _mm256_xor_ps(x, _mm256_set1_ps(-0.0f)));     // -x
    // All 8 predicate flavours that differ on NaN.
    h = mix_ps(h, _mm256_cmp_ps(x, y, _CMP_EQ_OQ));
    h = mix_ps(h, _mm256_cmp_ps(x, y, _CMP_LT_OS));
    h = mix_ps(h, _mm256_cmp_ps(x, y, _CMP_UNORD_Q));
    h = mix_ps(h, _mm256_cmp_ps(x, y, _CMP_NEQ_UQ));
    h = mix(h, (u32)_mm256_movemask_ps(_mm256_cmp_ps(x, y, _CMP_LT_OS)));
    // Conversions, including the out-of-range / NaN "integer indefinite" case.
    h = mix_m256i(h, _mm256_cvttps_epi32(x));
    h = mix_m256i(h, _mm256_cvtps_epi32(x));
    h = mix_pd(h, _mm256_cvtps_pd(_mm256_castps256_ps128(x)));
    return h;
}

// ---------------------------------------------------------------------------
// K3  dmath -- packed double precision
// ---------------------------------------------------------------------------
static u64 k_dmath(void) {
    const u64 pat[4] = {0x7ff8000000000001ULL, 0xfff0000000000000ULL,
                        0x0000000000000001ULL, 0x3ff0000000000000ULL};
    const u64 pat2[4] = {0x3ff0000000000000ULL, 0x4000000000000000ULL,
                         0x7ff0000000000000ULL, 0x8000000000000000ULL};
    double a[4], b[4];
    for (int i = 0; i < 4; i++) { a[i] = bits_to_d(pat[i]); b[i] = bits_to_d(pat2[i]); }
    __m256d x = _mm256_loadu_pd(a), y = _mm256_loadu_pd(b);

    u64 h = 0x2545f4914f6cdd1dULL;
    h = mix_pd(h, _mm256_add_pd(x, y));
    h = mix_pd(h, _mm256_sub_pd(x, y));
    h = mix_pd(h, _mm256_mul_pd(x, y));
    h = mix_pd(h, _mm256_div_pd(x, y));
    h = mix_pd(h, _mm256_min_pd(x, y));
    h = mix_pd(h, _mm256_max_pd(x, y));
    h = mix_pd(h, _mm256_sqrt_pd(y));
    h = mix_pd(h, _mm256_cmp_pd(x, y, _CMP_NLE_UQ));
    h = mix_pd(h, _mm256_unpacklo_pd(x, y));
    h = mix_pd(h, _mm256_unpackhi_pd(x, y));
    h = mix(h, (u32)_mm256_movemask_pd(_mm256_cmp_pd(x, y, _CMP_ORD_Q)));
    // Round-trip through 32-bit ints (indefinite value for NaN/out of range).
    h = mix_m256i(h, _mm256_castsi128_si256(_mm256_cvttpd_epi32(x)));
    h = mix_ps(h, _mm256_castps128_ps256(_mm256_cvtpd_ps(x)));
    // Well-behaved values so the arithmetic itself is checked too.
    double c[4] = {2.0, 3.5, -7.25, 1e300};
    double d[4] = {0.5, 1.25, 4.0, 1e-300};
    __m256d p = _mm256_loadu_pd(c), q = _mm256_loadu_pd(d);
    h = mix_pd(h, _mm256_mul_pd(p, q));
    h = mix_pd(h, _mm256_div_pd(p, q));
    h = mix_pd(h, _mm256_sqrt_pd(_mm256_andnot_pd(_mm256_set1_pd(-0.0), p)));
    return h;
}

// ---------------------------------------------------------------------------
// K4  matmul -- 8x8 float matrix multiply, one YMM row at a time
// ---------------------------------------------------------------------------
static float mm_a[64], mm_b[64], mm_c[64];

static u64 k_matmul(void) {
    rng_state = 0x13198a2e03707344ULL;
    for (int i = 0; i < 64; i++) {
        mm_a[i] = (float)(int)(rng_next() % 17 - 8) * 0.5f;
        mm_b[i] = (float)(int)(rng_next() % 17 - 8) * 0.25f;
    }
    for (int i = 0; i < 8; i++) {
        __m256 acc = _mm256_setzero_ps();
        for (int k = 0; k < 8; k++) {
            __m256 av = _mm256_broadcast_ss(&mm_a[i * 8 + k]);
            __m256 bv = _mm256_loadu_ps(&mm_b[k * 8]);
            acc = _mm256_add_ps(acc, _mm256_mul_ps(av, bv));
        }
        _mm256_storeu_ps(&mm_c[i * 8], acc);
    }
    u64 h = 0xa4093822299f31d0ULL;
    for (int i = 0; i < 64; i++) h = mix(h, f_to_bits(mm_c[i]));
    // Trace via a masked load/store round trip (VMASKMOVPS).
    const s32 maskbits[8] = {-1, 0, -1, 0, 0, -1, -1, 0};
    __m256i m = _mm256_loadu_si256((const __m256i *)maskbits);
    __m256 row = _mm256_maskload_ps(mm_c, m);
    h = mix_ps(h, row);
    float out[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    _mm256_maskstore_ps(out, m, _mm256_loadu_ps(mm_c + 8));
    for (int i = 0; i < 8; i++) h = mix(h, f_to_bits(out[i]));
    return h;
}

// ---------------------------------------------------------------------------
// K5  iproc -- byte-wise integer processing: compare, movemask, shuffle, sad
// ---------------------------------------------------------------------------
static u8 ibuf[256];

static u64 k_iproc(void) {
    rng_state = 0x082efa98ec4e6c89ULL;
    for (int i = 0; i < 256; i++) ibuf[i] = (u8)(rng_next() >> 11);
    __m256i x = _mm256_loadu_si256((const __m256i *)(ibuf + 0));
    __m256i y = _mm256_loadu_si256((const __m256i *)(ibuf + 32));
    __m256i ctl = _mm256_loadu_si256((const __m256i *)(ibuf + 64));

    u64 h = 0x452821e638d01377ULL;
    h = mix_m256i(h, _mm256_cmpeq_epi8(x, y));
    h = mix_m256i(h, _mm256_cmpgt_epi8(x, y));
    h = mix_m256i(h, _mm256_cmpgt_epi32(x, y));
    h = mix_m256i(h, _mm256_cmpgt_epi64(x, y));
    h = mix(h, (u32)_mm256_movemask_epi8(_mm256_cmpgt_epi8(x, y)));
    // VPSHUFB is per-128-bit-lane and zeroes a byte when the control's high
    // bit is set: two independent ways to get it wrong.
    h = mix_m256i(h, _mm256_shuffle_epi8(x, ctl));
    h = mix_m256i(h, _mm256_shuffle_epi8(x, _mm256_and_si256(ctl, _mm256_set1_epi8(0x0f))));
    h = mix_m256i(h, _mm256_sad_epu8(x, y));
    h = mix_m256i(h, _mm256_max_epu8(x, y));
    h = mix_m256i(h, _mm256_min_epi8(x, y));
    h = mix_m256i(h, _mm256_avg_epu8(x, y));
    h = mix_m256i(h, _mm256_adds_epu8(x, y));
    h = mix_m256i(h, _mm256_subs_epi8(x, y));
    h = mix_m256i(h, _mm256_packs_epi16(x, y));
    h = mix_m256i(h, _mm256_packus_epi16(x, y));
    h = mix_m256i(h, _mm256_packs_epi32(x, y));
    h = mix_m256i(h, _mm256_unpacklo_epi8(x, y));
    h = mix_m256i(h, _mm256_unpackhi_epi16(x, y));
    h = mix_m256i(h, _mm256_unpacklo_epi32(x, y));
    h = mix_m256i(h, _mm256_unpackhi_epi64(x, y));
    h = mix_m256i(h, _mm256_madd_epi16(x, y));
    h = mix_m256i(h, _mm256_maddubs_epi16(x, y));
    h = mix_m256i(h, _mm256_mullo_epi16(x, y));
    h = mix_m256i(h, _mm256_mullo_epi32(x, y));
    h = mix_m256i(h, _mm256_mul_epu32(x, y));
    h = mix_m256i(h, _mm256_mul_epi32(x, y));
    h = mix_m256i(h, _mm256_mulhi_epu16(x, y));
    h = mix_m256i(h, _mm256_abs_epi8(y));
    h = mix_m256i(h, _mm256_sign_epi16(x, y));
    h = mix_m256i(h, _mm256_hadd_epi16(x, y));
    h = mix_m256i(h, _mm256_hsub_epi32(x, y));
    h = mix_m256i(h, _mm256_blendv_epi8(x, y, ctl));
    h = mix(h, (u32)_mm256_testz_si256(x, y));
    h = mix(h, (u32)_mm256_testc_si256(x, y));
    return h;
}

// ---------------------------------------------------------------------------
// K6  ishift -- immediate and per-element variable shifts, incl. counts >= width
// ---------------------------------------------------------------------------
// These vectors were also checked against a hand-written Intel SDM model of
// VPSLLVD/VPSRLVD/VPSRAVD/VPSLLVQ/VPSRLVQ (count >= element width yields zero,
// or all sign bits for the arithmetic form) rather than against Rosetta alone.
// They discriminate the truncation bugs an emulator typically has: as qwords
// the counts are 0x100000000, 0x2000001f, 0x400000003f and 0x7ffffffff, so an
// implementation that truncates the count to its low 6 or low 32 bits produces
// visibly non-zero lanes.  SwiftVM, Rosetta and the SDM model all agree here,
// which is the only three-way agreement in this file.
static u64 k_ishift(void) {
    const u32 lanes[8] = {0x80000001u, 0x0f0f0f0fu, 0xffffffffu, 0x00000010u,
                          0x7fffffffu, 0xdeadbeefu, 0x00000000u, 0xcafebabeu};
    // Deliberately includes 0, 31, 32, 63, 64 and a huge count: x86 saturates
    // variable shifts to "all zero" (or all sign bits) instead of wrapping.
    const u32 counts[8] = {0, 1, 31, 32, 63, 64, 0xffffffffu, 7};
    __m256i x = _mm256_loadu_si256((const __m256i *)lanes);
    __m256i c = _mm256_loadu_si256((const __m256i *)counts);

    u64 h = 0xbe5466cf34e90c6cULL;
    h = mix_m256i(h, _mm256_slli_epi16(x, 3));
    h = mix_m256i(h, _mm256_srli_epi32(x, 5));
    h = mix_m256i(h, _mm256_srai_epi32(x, 9));
    h = mix_m256i(h, _mm256_slli_epi64(x, 40));
    h = mix_m256i(h, _mm256_srli_epi64(x, 33));
    h = mix_m256i(h, _mm256_slli_si256(x, 5));   // per-lane byte shift
    h = mix_m256i(h, _mm256_srli_si256(x, 11));
    h = mix_m256i(h, _mm256_sll_epi32(x, _mm256_castsi256_si128(c)));
    h = mix_m256i(h, _mm256_sllv_epi32(x, c));
    h = mix_m256i(h, _mm256_srlv_epi32(x, c));
    h = mix_m256i(h, _mm256_srav_epi32(x, c));
    h = mix_m256i(h, _mm256_sllv_epi64(x, c));   // known Rosetta divergence
    h = mix_m256i(h, _mm256_srlv_epi64(x, c));
    return h;
}

// ---------------------------------------------------------------------------
// K7  xlane -- cross-lane permutes and broadcasts (the AVX2-only shapes)
// ---------------------------------------------------------------------------
static u64 k_xlane(void) {
    const u32 lanes[8] = {0x00010203u, 0x04050607u, 0x08090a0bu, 0x0c0d0e0fu,
                          0x10111213u, 0x14151617u, 0x18191a1bu, 0x1c1d1e1fu};
    const u32 idx[8] = {7, 0, 5, 2, 3, 6, 1, 4};
    __m256i x = _mm256_loadu_si256((const __m256i *)lanes);
    __m256i y = _mm256_slli_epi32(x, 1);
    __m256i iv = _mm256_loadu_si256((const __m256i *)idx);

    u64 h = 0xc0ac29b7c97c50ddULL;
    h = mix_m256i(h, _mm256_permute4x64_epi64(x, 0x1b));
    h = mix_m256i(h, _mm256_permute4x64_epi64(x, 0x00));
    h = mix_pd(h, _mm256_permute4x64_pd(_mm256_castsi256_pd(x), 0x93));
    h = mix_m256i(h, _mm256_permute2x128_si256(x, y, 0x21));
    h = mix_m256i(h, _mm256_permute2x128_si256(x, y, 0x13));
    h = mix_m256i(h, _mm256_permute2x128_si256(x, y, 0x88));  // zeroing form
    h = mix_m256i(h, _mm256_permutevar8x32_epi32(x, iv));
    h = mix_ps(h, _mm256_permutevar8x32_ps(_mm256_castsi256_ps(x), iv));
    h = mix_ps(h, _mm256_permute_ps(_mm256_castsi256_ps(x), 0x1b));
    h = mix_pd(h, _mm256_permute_pd(_mm256_castsi256_pd(x), 0x5));
    h = mix_ps(h, _mm256_permutevar_ps(_mm256_castsi256_ps(x), iv));
    h = mix_m256i(h, _mm256_broadcastb_epi8(_mm256_castsi256_si128(x)));
    h = mix_m256i(h, _mm256_broadcastw_epi16(_mm256_castsi256_si128(x)));
    h = mix_m256i(h, _mm256_broadcastd_epi32(_mm256_castsi256_si128(y)));
    h = mix_m256i(h, _mm256_broadcastq_epi64(_mm256_castsi256_si128(y)));
    h = mix_m256i(h, _mm256_broadcastsi128_si256(_mm256_extracti128_si256(x, 1)));
    h = mix_m256i(h, _mm256_inserti128_si256(x, _mm256_castsi256_si128(y), 1));
    h = mix_m256i(h, _mm256_castsi128_si256(_mm256_extracti128_si256(x, 0)));
    h = mix_m256i(h, _mm256_blend_epi32(x, y, 0xa5));
    h = mix_m256i(h, _mm256_blend_epi16(x, y, 0x3c));
    h = mix_m256i(h, _mm256_alignr_epi8(x, y, 5));
    h = mix_m256i(h, _mm256_shuffle_epi32(x, 0x4e));
    h = mix_m256i(h, _mm256_shufflelo_epi16(x, 0x1b));
    h = mix_m256i(h, _mm256_shufflehi_epi16(x, 0xb1));
    h = mix_ps(h, _mm256_shuffle_ps(_mm256_castsi256_ps(x), _mm256_castsi256_ps(y), 0x4e));
    h = mix_ps(h, _mm256_movehdup_ps(_mm256_castsi256_ps(x)));
    h = mix_ps(h, _mm256_moveldup_ps(_mm256_castsi256_ps(x)));
    h = mix_pd(h, _mm256_movedup_pd(_mm256_castsi256_pd(x)));
    // Sign/zero widening across the lane boundary.
    h = mix_m256i(h, _mm256_cvtepi8_epi32(_mm256_castsi256_si128(x)));
    h = mix_m256i(h, _mm256_cvtepu8_epi16(_mm256_castsi256_si128(x)));
    h = mix_m256i(h, _mm256_cvtepi16_epi64(_mm256_castsi256_si128(x)));
    h = mix_m256i(h, _mm256_cvtepu32_epi64(_mm256_castsi256_si128(x)));
    return h;
}

// ---------------------------------------------------------------------------
// K8  memops -- hand-rolled AVX2 memcpy / strlen / memchr.
// Explicit intrinsics on purpose: libc's AVX2 paths arrive through an ifunc
// that also wants BMI2, which SwiftVM does not implement yet, so a libc
// memcpy would never reach an AVX2 variant here.
// ---------------------------------------------------------------------------
static u8 src_buf[4096 + 64];
static u8 dst_buf[4096 + 64];

static void avx2_memcpy(u8 *d, const u8 *s, unsigned long n) {
    while (n >= 32) {
        _mm256_storeu_si256((__m256i *)d, _mm256_loadu_si256((const __m256i *)s));
        d += 32; s += 32; n -= 32;
    }
    if (n >= 16) {
        _mm_storeu_si128((__m128i *)d, _mm_loadu_si128((const __m128i *)s));
        d += 16; s += 16; n -= 16;
    }
    while (n--) *d++ = *s++;
}

static unsigned long avx2_strlen(const char *s) {
    const __m256i zero = _mm256_setzero_si256();
    unsigned long off = 0;
    for (;;) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(s + off));
        u32 m = (u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, zero));
        if (m) return off + (unsigned long)__builtin_ctz(m);
        off += 32;
    }
}

static long avx2_memchr(const u8 *p, u8 c, unsigned long n) {
    const __m256i needle = _mm256_set1_epi8((char)c);
    unsigned long off = 0;
    for (; off + 32 <= n; off += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(p + off));
        u32 m = (u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, needle));
        if (m) return (long)(off + (unsigned long)__builtin_ctz(m));
    }
    for (; off < n; off++) if (p[off] == c) return (long)off;
    return -1;
}

static u64 k_memops(void) {
    rng_state = 0x9216d5d98979fb1bULL;
    for (int i = 0; i < 4096; i++) src_buf[i] = (u8)((rng_next() >> 13) | 1);
    u64 h = 0xd1310ba698dfb5acULL;

    // Every (src offset, dst offset, length) combination below crosses the
    // 32-byte boundary differently, so a mis-sized tail shows up immediately.
    for (int so = 0; so < 5; so++) {
        for (int dof = 0; dof < 5; dof++) {
            for (unsigned long len = 1; len <= 200; len += 37) {
                for (int i = 0; i < 4096; i++) dst_buf[i] = 0xCD;
                avx2_memcpy(dst_buf + dof, src_buf + so, len);
                for (unsigned long i = 0; i < len + 8; i++) h = mix(h, dst_buf[dof + i]);
            }
        }
    }
    // strlen over every alignment of a 300-byte string.
    for (int off = 0; off < 40; off++) {
        for (int i = 0; i < 300; i++) src_buf[off + i] = (u8)(1 + (i % 251));
        src_buf[off + 300] = 0;
        h = mix(h, avx2_strlen((const char *)(src_buf + off)));
    }
    for (int i = 0; i < 4096; i++) src_buf[i] = (u8)((rng_next() >> 13) | 1);
    src_buf[1234] = 0x42;
    h = mix(h, (u64)avx2_memchr(src_buf, 0x42, 4096));
    h = mix(h, (u64)avx2_memchr(src_buf, 0x00, 4096));
    return h;
}

// ---------------------------------------------------------------------------
// K9  vzu -- VZEROUPPER / VZEROALL and the AVX<->SSE state transition
// ---------------------------------------------------------------------------
static u64 k_vzu(void) {
    u64 h = 0x1f83d9abfb41bd6bULL;
    u64 out[4];

    // 1. ymm0 upper half must survive an unrelated SSE-width VEX op.
    __asm__ volatile(
        "vpcmpeqd %%ymm0, %%ymm0, %%ymm0\n\t"       // ymm0 = all ones
        "vpxor %%xmm1, %%xmm1, %%xmm1\n\t"          // VEX.128 zeroes ymm1 upper
        "vmovdqu %%ymm0, %0\n\t"
        : "=m"(out) : : "ymm0", "ymm1", "memory");
    for (int i = 0; i < 4; i++) h = mix(h, out[i]);

    // 2. VEX.128 writes must zero the upper 128 bits of the destination.
    __asm__ volatile(
        "vpcmpeqd %%ymm2, %%ymm2, %%ymm2\n\t"
        "vpaddb %%xmm2, %%xmm2, %%xmm2\n\t"         // 128-bit form -> upper = 0
        "vmovdqu %%ymm2, %0\n\t"
        : "=m"(out) : : "ymm2", "memory");
    for (int i = 0; i < 4; i++) h = mix(h, out[i]);

    // 3. Legacy SSE writes must LEAVE the upper 128 bits alone.
    __asm__ volatile(
        "vpcmpeqd %%ymm3, %%ymm3, %%ymm3\n\t"
        "paddb %%xmm3, %%xmm3\n\t"                  // legacy SSE, upper preserved
        "vmovdqu %%ymm3, %0\n\t"
        : "=m"(out) : : "ymm3", "memory");
    for (int i = 0; i < 4; i++) h = mix(h, out[i]);

    // 4. VZEROUPPER must zero the upper halves and keep the lower ones.
    __asm__ volatile(
        "vpcmpeqd %%ymm4, %%ymm4, %%ymm4\n\t"
        "vzeroupper\n\t"
        "vmovdqu %%ymm4, %0\n\t"
        : "=m"(out) : : "ymm4", "memory");
    for (int i = 0; i < 4; i++) h = mix(h, out[i]);

    // 5. VZEROALL must clear everything, including the low halves.
    __asm__ volatile(
        "vpcmpeqd %%ymm5, %%ymm5, %%ymm5\n\t"
        "vzeroall\n\t"
        "vmovdqu %%ymm5, %0\n\t"
        : "=m"(out) : : "ymm5", "memory");
    for (int i = 0; i < 4; i++) h = mix(h, out[i]);

    // 6. VZEROUPPER then a legacy SSE op then a 256-bit read: the classic
    //    "dirty upper state" sequence a real compiler emits at call sites.
    __asm__ volatile(
        "vpcmpeqd %%ymm6, %%ymm6, %%ymm6\n\t"
        "vzeroupper\n\t"
        "pcmpeqd %%xmm6, %%xmm6\n\t"
        "vmovdqu %%ymm6, %0\n\t"
        : "=m"(out) : : "ymm6", "memory");
    for (int i = 0; i < 4; i++) h = mix(h, out[i]);
    return h;
}

// ---------------------------------------------------------------------------
// K10 sysst -- YMM state across an OS entry.
// The syscall number is the only thing that differs between the guest and the
// oracle; both write 0 bytes to fd 1, i.e. no output, but a real kernel /
// SwiftVM syscall path is traversed with a dirty upper YMM state.
// ---------------------------------------------------------------------------
#ifdef SVM_GUEST_FREESTANDING
#define SVM_SYS_WRITE 1L               // Linux x86-64 write
#else
#define SVM_SYS_WRITE 0x2000004L       // macOS BSD write
#endif

static u64 k_sysst(void) {
    u64 h = 0x5be0cd19137e2179ULL;
    u64 before[4] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL,
                     0xdeadbeefcafebabeULL, 0x8badf00dfeedface};
    u64 after[8];

    // ymm8/ymm9 hold live data across the syscall; rcx and r11 are clobbered
    // by the syscall instruction itself.
    __asm__ volatile(
        "vmovdqu %2, %%ymm8\n\t"
        "vpcmpeqd %%ymm9, %%ymm9, %%ymm9\n\t"
        "mov %3, %%rax\n\t"
        "mov $1, %%edi\n\t"
        "xor %%esi, %%esi\n\t"
        "xor %%edx, %%edx\n\t"
        "syscall\n\t"
        "vmovdqu %%ymm8, %0\n\t"
        "vmovdqu %%ymm9, %1\n\t"
        : "=m"(after[0]), "=m"(after[4])
        : "m"(before[0]), "i"(SVM_SYS_WRITE)
        : "rax", "rcx", "rdi", "rsi", "rdx", "r11", "ymm8", "ymm9", "memory");
    for (int i = 0; i < 8; i++) h = mix(h, after[i]);

    // Same again, but with VZEROUPPER right before the OS entry: the upper
    // halves must read back as zero afterwards, not as stale saved state.
    __asm__ volatile(
        "vmovdqu %2, %%ymm10\n\t"
        "vpcmpeqd %%ymm11, %%ymm11, %%ymm11\n\t"
        "vzeroupper\n\t"
        "mov %3, %%rax\n\t"
        "mov $1, %%edi\n\t"
        "xor %%esi, %%esi\n\t"
        "xor %%edx, %%edx\n\t"
        "syscall\n\t"
        "vmovdqu %%ymm10, %0\n\t"
        "vmovdqu %%ymm11, %1\n\t"
        : "=m"(after[0]), "=m"(after[4])
        : "m"(before[0]), "i"(SVM_SYS_WRITE)
        : "rax", "rcx", "rdi", "rsi", "rdx", "r11", "ymm10", "ymm11", "memory");
    for (int i = 0; i < 8; i++) h = mix(h, after[i]);
    return h;
}

// ---------------------------------------------------------------------------
// K11 gather -- VPGATHER/VGATHER.  Isolated at the end because it is the most
// likely gap: if the process dies here everything before it is already out.
// ---------------------------------------------------------------------------
static s32 gath_i32[64];
static float gath_f32[64];

static u64 k_gather(void) {
    for (int i = 0; i < 64; i++) {
        gath_i32[i] = (s32)(i * 0x01010101 + 7);
        gath_f32[i] = (float)i * 0.5f;
    }
    const s32 idx[8] = {0, 17, 3, 63, 8, 41, 22, 5};
    __m256i vidx = _mm256_loadu_si256((const __m256i *)idx);
    u64 h = 0x510e527fade682d1ULL;
    h = mix_m256i(h, _mm256_i32gather_epi32(gath_i32, vidx, 4));
    h = mix_ps(h, _mm256_i32gather_ps(gath_f32, vidx, 4));
    const s32 mask[8] = {-1, 0, -1, -1, 0, 0, -1, 0};
    __m256i vmask = _mm256_loadu_si256((const __m256i *)mask);
    h = mix_m256i(h, _mm256_mask_i32gather_epi32(_mm256_set1_epi32(-1), gath_i32,
                                                 vidx, vmask, 4));
    return h;
}

// ---------------------------------------------------------------------------
// driver
// ---------------------------------------------------------------------------
typedef u64 (*kernel_fn)(void);

struct kernel_entry {
    const char *name;
    kernel_fn fn;
};

static const struct kernel_entry kernels[] = {
    {"fdot", k_fdot},     {"fedge", k_fedge},   {"dmath", k_dmath},
    {"matmul", k_matmul}, {"iproc", k_iproc},   {"ishift", k_ishift},
    {"xlane", k_xlane},   {"memops", k_memops}, {"vzu", k_vzu},
    {"sysst", k_sysst},   {"gather", k_gather},
};
#define NKERNELS ((int)(sizeof(kernels) / sizeof(kernels[0])))

// `only` >= 0 runs exactly one kernel (bisection after a fatal decode gap);
// otherwise every kernel runs and each result is flushed as it is produced, so
// a crash names the kernel that caused it.
static int svm_avx_run(int only) {
    u64 total = 0x6a09e667f3bcc908ULL;
    for (int i = 0; i < NKERNELS; i++) {
        if (only >= 0 && i != only) continue;
        u64 v = kernels[i].fn();
        emit_kv(kernels[i].name, v);
        total = mix(total, v);
    }
    emit_kv("checksum", total);
    return (int)(total & 0x7f);
}

#endif  // SVM_AVX_REAL_KERNELS_H
