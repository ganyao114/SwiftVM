// Standalone Rosetta precision probe for the opt-in ARM64 x87 reduced path.
//
// Build/run on an Apple Silicon host:
//   clang -O2 -arch x86_64 -msse2 source/tests/x87_precision_probe.c \
//       -o /private/tmp/x87_precision_probe
//   arch -x86_64 /private/tmp/x87_precision_probe

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct __attribute__((packed)) {
    uint64_t significand;
    uint16_t sign_exp;
} Ext80;

static uint64_t rng_state = UINT64_C(0x9e3779b97f4a7c15);

static uint64_t Next(void) {
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * UINT64_C(2685821657736338717);
}

static Ext80 Exact(char op, double a, double b, uint16_t* status) {
    Ext80 out;
    switch (op) {
        case '+':
            __asm__ volatile("fnclex\n\t"
                             "fldl %2\n\t"
                             "fldl %3\n\t"
                             "faddp\n\t"
                             "fstpt %0\n\t"
                             "fnstsw %1"
                             : "=m"(out), "=m"(*status)
                             : "m"(a), "m"(b)
                             : "st");
            break;
        case '-':
            __asm__ volatile("fnclex\n\t"
                             "fldl %2\n\t"
                             "fldl %3\n\t"
                             "fsubrp\n\t"
                             "fstpt %0\n\t"
                             "fnstsw %1"
                             : "=m"(out), "=m"(*status)
                             : "m"(a), "m"(b)
                             : "st");
            break;
        case '*':
            __asm__ volatile("fnclex\n\t"
                             "fldl %2\n\t"
                             "fldl %3\n\t"
                             "fmulp\n\t"
                             "fstpt %0\n\t"
                             "fnstsw %1"
                             : "=m"(out), "=m"(*status)
                             : "m"(a), "m"(b)
                             : "st");
            break;
        case 's':
            __asm__ volatile("fnclex\n\t"
                             "fldl %2\n\t"
                             "fsqrt\n\t"
                             "fstpt %0\n\t"
                             "fnstsw %1"
                             : "=m"(out), "=m"(*status)
                             : "m"(a)
                             : "st");
            break;
        default:
            __asm__ volatile("fnclex\n\t"
                             "fldl %2\n\t"
                             "fldl %3\n\t"
                             "fdivrp\n\t"
                             "fstpt %0\n\t"
                             "fnstsw %1"
                             : "=m"(out), "=m"(*status)
                             : "m"(a), "m"(b)
                             : "st");
            break;
    }
    return out;
}

static Ext80 Reduced(char op, double a, double b, uint16_t* status) {
    uint32_t mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr &= ~UINT32_C(0x3f);
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    volatile double result;
    switch (op) {
        case '+': result = a + b; break;
        case '-': result = a - b; break;
        case '*': result = a * b; break;
        case 's': result = sqrt(a); break;
        default: result = a / b; break;
    }
    Ext80 out;
    __asm__ volatile("fldl %1\n\t"
                     "fstpt %0"
                     : "=m"(out)
                     : "m"(result)
                     : "st");
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    *status = (uint16_t)(mxcsr & 0x3f);
    return out;
}

static double RandomNormal(void) {
    // A broad, finite binary64 pool. Keeping exponents away from the endpoints
    // makes the count about double rounding rather than overflow/underflow.
    const uint64_t sign = Next() & UINT64_C(0x8000000000000000);
    const uint64_t exponent = (uint64_t)(64 + (Next() % (1983 - 64))) << 52;
    const uint64_t fraction = Next() & UINT64_C(0x000fffffffffffff);
    const uint64_t bits = sign | exponent | fraction;
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

int main(void) {
    static const char ops[] = {'+', '-', '*', '/', 's'};
    for (size_t op_index = 0; op_index < sizeof(ops); ++op_index) {
        const char op = ops[op_index];
        uint64_t mismatches = 0;
        uint64_t same_exponent = 0;
        uint64_t max_significand_delta = 0;
        uint64_t flag_mismatches = 0;
        uint64_t flag_bit_mismatches[6] = {};
        unsigned printed = 0;
        for (uint64_t i = 0; i < UINT64_C(1000000); ++i) {
            double a = RandomNormal();
            if (op == 's') a = fabs(a);
            const double b = RandomNormal();
            uint16_t exact_status, reduced_status;
            const Ext80 exact = Exact(op, a, b, &exact_status);
            const Ext80 reduced = Reduced(op, a, b, &reduced_status);
            if ((exact_status & 0x3f) != reduced_status) {
                ++flag_mismatches;
                for (unsigned bit = 0; bit < 6; ++bit) {
                    if (((exact_status ^ reduced_status) >> bit) & 1) {
                        ++flag_bit_mismatches[bit];
                    }
                }
            }
            if (exact.sign_exp == reduced.sign_exp &&
                exact.significand == reduced.significand) {
                continue;
            }
            ++mismatches;
            if (exact.sign_exp == reduced.sign_exp) {
                ++same_exponent;
                const uint64_t delta = exact.significand > reduced.significand
                                               ? exact.significand - reduced.significand
                                               : reduced.significand - exact.significand;
                if (delta > max_significand_delta) max_significand_delta = delta;
            }
            if (printed++ < 5) {
                uint64_t a_bits, b_bits;
                memcpy(&a_bits, &a, 8);
                memcpy(&b_bits, &b, 8);
                printf("%c a=%016" PRIx64 " b=%016" PRIx64
                       " exact=%04x:%016" PRIx64
                       " reduced=%04x:%016" PRIx64 "\n",
                       op,
                       a_bits,
                       b_bits,
                       exact.sign_exp,
                       exact.significand,
                       reduced.sign_exp,
                       reduced.significand);
            }
        }
        printf("%c mismatches=%" PRIu64 "/1000000 same_exp=%" PRIu64
               " max_ext80_significand_delta=%" PRIu64
               " flag_mismatches=%" PRIu64
               " [IE=%" PRIu64 " DE=%" PRIu64 " ZE=%" PRIu64
               " OE=%" PRIu64 " UE=%" PRIu64 " PE=%" PRIu64 "]\n",
               op,
               mismatches,
               same_exponent,
               max_significand_delta,
               flag_mismatches,
               flag_bit_mismatches[0],
               flag_bit_mismatches[1],
               flag_bit_mismatches[2],
               flag_bit_mismatches[3],
               flag_bit_mismatches[4],
               flag_bit_mismatches[5]);
    }
    return 0;
}
