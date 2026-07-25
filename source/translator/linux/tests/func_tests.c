#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__x86_64__)
#include <emmintrin.h>
#endif

#if defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

static NOINLINE uint64_t scramble(uint64_t x) {
    x ^= x >> 29;
    x *= UINT64_C(0x9e3779b185ebca87);
    x ^= x >> 31;
    return x;
}

static NOINLINE uint64_t loop_heavy(unsigned limit) {
    uint64_t sum = 0;
    unsigned primes = 0;
    for (unsigned n = 2; n <= limit; ++n) {
        int prime = 1;
        for (unsigned d = 2; d * d <= n; ++d) {
            if (n % d != 0) {
                continue;
            }
            prime = 0;
            break;
        }
        if (!prime) {
            continue;
        }
        sum += (uint64_t)n * (primes + 3u);
        if (++primes == 73) {
            break;
        }
    }

    for (unsigned outer = 1; outer < 18; ++outer) {
        for (unsigned inner = 0; inner < 31; ++inner) {
            if (((outer + inner) % 7u) == 0) {
                continue;
            }
            sum ^= (uint64_t)(outer * 131u + inner * 17u) << ((outer + inner) & 15u);
            if ((sum & UINT64_C(0x1ff)) == UINT64_C(0x155)) {
                break;
            }
        }
    }
    return scramble(sum ^ primes);
}

static NOINLINE uint64_t parse_fields(const char *text) {
    uint64_t total = 0;
    uint64_t current = 0;
    unsigned fields = 0;
    unsigned signs = 0;
    int negative = 0;

    for (;;) {
        const unsigned char ch = (unsigned char)*text++;
        if (ch >= '0' && ch <= '9') {
            current = current * 10u + (ch - '0');
            continue;
        }
        if (ch == '-' && current == 0) {
            negative = !negative;
            ++signs;
            continue;
        }
        if (ch == ',' || ch == ';' || ch == ':' || ch == '\0') {
            total ^= negative ? ~current : current;
            total = (total << 9) | (total >> (64 - 9));
            total += ++fields * UINT64_C(0x100000001b3);
            current = 0;
            negative = 0;
            if (ch == '\0') {
                break;
            }
            continue;
        }
        if (ch == '#') {
            while (*text && *text != ';') {
                ++text;
            }
            if (!*text) {
                break;
            }
            continue;
        }
        total ^= (uint64_t)ch << ((fields * 7u) & 31u);
    }
    return scramble(total ^ ((uint64_t)fields << 32) ^ signs);
}

static NOINLINE uint64_t switch_worker(int key, uint64_t value) {
    switch (key) {
        case 0:
            value += UINT64_C(0x11);
            /* fall through */
        case 1:
            value ^= value << 7;
            break;
        case 2:
            value = (value << 13) | (value >> (64 - 13));
            break;
        case 3:
            value += value >> 5;
            /* fall through */
        case 4:
            value *= UINT64_C(0x45d9f3b);
            break;
        case 5:
            value ^= UINT64_C(0xa5a5a5a55a5a5a5a);
            break;
        case 6:
            value -= value << 3;
            break;
        case 7:
            value = ~value + UINT64_C(0x1234);
            /* fall through */
        case 8:
            value ^= value >> 17;
            break;
        case 9:
            value += UINT64_C(0xfeedfacecafebeef);
            break;
        case 10:
            value = value * 33u + 17u;
            break;
        case 11:
            value ^= (value << 31) | (value >> 33);
            break;
        default:
            value = scramble(value ^ (uint64_t)(unsigned)key);
            break;
    }
    return value;
}

static NOINLINE uint64_t switch_stress(void) {
    uint64_t value = UINT64_C(0x0123456789abcdef);
    static const int keys[] = {0, 3, 7, 2, 11, 5, 8, 1, 10, 6, 9, 4, -3, 19};
    for (unsigned round = 0; round < 37; ++round) {
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            value = switch_worker(keys[(i + round) % (sizeof(keys) / sizeof(keys[0]))],
                                  value + round + i);
        }
    }
    return scramble(value);
}

static NOINLINE uint64_t fib(unsigned n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

static NOINLINE uint64_t factorial(unsigned n) {
    if (n < 2) {
        return 1;
    }
    return n * factorial(n - 1);
}

static NOINLINE uint64_t ackermann(unsigned m, unsigned n) {
    if (m == 0) {
        return n + 1;
    }
    if (n == 0) {
        return ackermann(m - 1, 1);
    }
    return ackermann(m - 1, (unsigned)ackermann(m, n - 1));
}

static NOINLINE uint64_t recursive_stress(void) {
    return scramble(fib(13) ^ (factorial(11) << 7) ^ (ackermann(2, 6) << 41));
}

static NOINLINE uint64_t mixed_flags(uint64_t seed) {
    uint64_t acc = seed;
    uint64_t carry_mix = 0;
    for (unsigned i = 0; i < 211; ++i) {
        uint64_t addend = UINT64_C(0xfedcba9876543210) + (uint64_t)i * UINT64_C(0x102030405);
        uint64_t next;
        const int carry = __builtin_add_overflow(acc, addend, &next);
        if (carry) {
            carry_mix += (next ^ i) + UINT64_C(0x9e3779b97f4a7c15);
            next ^= next >> ((i % 23u) + 1u);
        } else if ((next & 0x80000000u) != 0) {
            carry_mix ^= next + (carry_mix << 3);
        } else {
            carry_mix -= next ^ (carry_mix >> 5);
        }
        const unsigned rot = (i % 63u) + 1u;
        acc = (next << rot) | (next >> (64u - rot));
        acc ^= carry_mix;
    }
    return scramble(acc ^ carry_mix);
}

#if defined(__x86_64__)
static NOINLINE uint64_t sse_stress(uint64_t seed) {
    __m128i acc = _mm_set_epi64x((long long)(seed ^ UINT64_C(0x55aa55aa55aa55aa)),
                                 (long long)seed);
    __m128i step = _mm_set_epi32(0x10203040, 0x55667788, 0x31415926, 0x27182818);
    uint64_t out[2];
    for (unsigned i = 0; i < 193; ++i) {
        __m128i lane = _mm_set1_epi32((int)(i * 17u + 3u));
        acc = _mm_add_epi32(acc, step);
        acc = _mm_xor_si128(acc, lane);
        acc = _mm_shuffle_epi32(acc, _MM_SHUFFLE(2, 0, 3, 1));
        if ((i & 7u) == 3u) {
            acc = _mm_add_epi64(acc, _mm_srli_si128(acc, 8));
        }
    }
    _mm_storeu_si128((__m128i *)(void *)out, acc);
    return scramble(out[0] ^ (out[1] + seed));
}
#else
static NOINLINE uint64_t sse_stress(uint64_t seed) {
    return scramble(seed ^ UINT64_C(0x41524d36344e4f50));
}
#endif

static NOINLINE uint64_t helper_leaf(uint64_t x, unsigned shift) {
    x ^= UINT64_C(0xd6e8feb86659fd93) + shift;
    return (x << shift) | (x >> (64u - shift));
}

static NOINLINE uint64_t helper_mid(uint64_t x, unsigned round) {
    if ((round & 1u) != 0) {
        x += helper_leaf(x, (round % 31u) + 1u);
    } else {
        x ^= helper_leaf(x + round, ((round * 3u) % 31u) + 1u);
    }
    return switch_worker((int)(round % 14u) - 1, x);
}

static NOINLINE uint64_t called_helpers(uint64_t seed) {
    uint64_t value = seed;
    for (unsigned round = 1; round < 61; ++round) {
        value = helper_mid(value, round);
        if ((value & 15u) == round % 15u) {
            value ^= helper_leaf(value, (round % 29u) + 1u);
        }
    }
    return scramble(value);
}

int main(void) {
    const uint64_t loops = loop_heavy(1400) ^
                           parse_fields("17,-23,0042;991:5#ignored text;73,-8,123456");
    const uint64_t switches = switch_stress();
    const uint64_t recursive = recursive_stress();
    const uint64_t flags = mixed_flags(UINT64_C(0xc001d00d5eed1234));
    const uint64_t sse = sse_stress(flags ^ switches);
    const uint64_t helpers = called_helpers(loops ^ recursive);
    const uint64_t checksum =
            scramble(loops ^ switches ^ recursive ^ flags ^ sse ^ helpers);

    printf("loop=%016" PRIx64 "\n", loops);
    printf("switch=%016" PRIx64 "\n", switches);
    printf("recursive=%016" PRIx64 "\n", recursive);
    printf("flags=%016" PRIx64 "\n", flags);
    printf("sse=%016" PRIx64 "\n", sse);
    printf("helpers=%016" PRIx64 "\n", helpers);
    printf("checksum=%016" PRIx64 "\n", checksum);
    return (int)(checksum & 0x7f);
}
