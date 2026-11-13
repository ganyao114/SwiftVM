// Freestanding ARM64 comparison kernel; the harness resets writable memory.
#include <stddef.h>
#include <stdint.h>

void* memcpy(void* d, const void* s, size_t n) {
    unsigned char* a = d;
    const unsigned char* b = s;
    while (n--) *a++ = *b++;
    return d;
}

void* memset(void* d, int c, size_t n) {
    unsigned char* a = d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} Sha;

static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};

static void block(Sha* s, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5],
             g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

static void update(Sha* s, const void* data, size_t n) {
    const uint8_t* p = data;
    s->len += n;
    while (n--) {
        s->buf[s->used++] = *p++;
        if (s->used == 64) {
            block(s, s->buf);
            s->used = 0;
        }
    }
}

static void finish(Sha* s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->used != 56) update(s, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(s, len, 8);
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = s->h[i] >> 24;
        out[4 * i + 1] = s->h[i] >> 16;
        out[4 * i + 2] = s->h[i] >> 8;
        out[4 * i + 3] = s->h[i];
    }
}

static uint8_t data[300];
static uint32_t values[1024];
static double a[64], b[64], c[64];

struct {
    unsigned char sha[32];
    uint64_t sort;
    uint32_t mixed;
    uint32_t pad;
    double fp;
} benchmark_output;

__attribute__((noinline)) static uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    return x ^ (x >> 16);
}

static void compute(void) {
#ifndef SVM_BENCH_MATRIX_ONLY
    for (int i = 0; i < 300; ++i) data[i] = (uint8_t)(i * 37 + 11);
    Sha s = {{0x6a09e667,
              0xbb67ae85,
              0x3c6ef372,
              0xa54ff53a,
              0x510e527f,
              0x9b05688c,
              0x1f83d9ab,
              0x5be0cd19},
             0,
             {0},
             0};
    for (int i = 0; i < 1000; ++i) update(&s, data, sizeof data);
    finish(&s, benchmark_output.sha);
    for (unsigned i = 0; i < 1024; ++i) values[i] = mix(i + 17);
    for (unsigned i = 1; i < 1024; ++i) {
        unsigned x = values[i], j = i;
        while (j && values[j - 1] > x) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = x;
    }
    for (unsigned i = 0; i < 1024; ++i) benchmark_output.sort += (uint64_t)values[i] * (i + 1);
    uint32_t hash = 1;
    for (unsigned i = 0; i < 65536; ++i) hash = mix(hash + i);
    benchmark_output.mixed = hash;
#endif
    for (unsigned i = 0; i < 64; ++i) {
        a[i] = (double)(i + 1) * 0.03125;
        b[i] = (double)(65 - i) * 0.125;
    }
    for (unsigned repeat = 0; repeat < 500; ++repeat)
        for (unsigned i = 0; i < 8; ++i)
            for (unsigned j = 0; j < 8; ++j) {
                double sum = 0;
                for (unsigned k = 0; k < 8; ++k) sum += a[i * 8 + k] * b[k * 8 + j];
                c[i * 8 + j] = sum;
            }
    for (unsigned i = 0; i < 64; ++i) benchmark_output.fp += c[i];
}

void bench_entry(void) { compute(); }
