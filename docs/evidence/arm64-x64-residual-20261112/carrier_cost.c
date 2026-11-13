// Host cost of the flag and floating-point carriers used by the x64 backend.
// Each kernel is one loop iteration of a generated-code shape; see carrier_cost.S.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define K(name) void name(uint64_t);
K(k_base) K(k_pushf_save) K(k_pushf_popf) K(k_lahf_save) K(k_lahf_sahf) K(k_setcc_test)
K(k_recompare) K(k_fp_plain) K(k_fp_nan) K(k_fp_post) K(k_fp_admit) K(k_fp_region)
K(k_vec_admit) K(k_vec_post)

static double run(void (*kernel)(uint64_t), uint64_t iterations) {
    double best = 1e30;
    for (int i = 0; i < 7; ++i) {
        struct timespec begin, end;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        kernel(iterations);
        clock_gettime(CLOCK_MONOTONIC, &end);
        const double ns = (end.tv_sec - begin.tv_sec) * 1e9 + (end.tv_nsec - begin.tv_nsec);
        if (ns < best) best = ns;
    }
    return best / iterations;
}

int main(void) {
    const uint64_t iterations = 200000000ull;
    static const struct {
        const char* name;
        void (*kernel)(uint64_t);
    } table[] = {
            {"int: add; sub; jne (base, 1 cycle)", k_base},
            {"int: + pushfq/pop/mov save", k_pushf_save},
            {"int: + pushfq save + push/popfq restore", k_pushf_popf},
            {"int: + lahf/seto/mov save", k_lahf_save},
            {"int: + lahf save + add al,7f/sahf restore", k_lahf_sahf},
            {"int: + setne ... test/jne", k_setcc_test},
            {"int: + re-test result", k_recompare},
            {"fp : addsd only", k_fp_plain},
            {"fp : addsd + ucomisd/jp", k_fp_nan},
            {"fp : addsd + NaN & MinNormal post-check", k_fp_post},
            {"fp : 2-input admission + addsd", k_fp_admit},
            {"fp : MXCSR region + admission + addsd", k_fp_region},
            {"vec: 4-lane admission + addpd", k_vec_admit},
            {"vec: addpd + SSE4.1 post-check", k_vec_post},
    };
    // Reach the steady clock frequency before the first measured kernel.
    for (int i = 0; i < 3; ++i) run(k_base, iterations);
    const double base = run(k_base, iterations);
    for (unsigned i = 0; i < sizeof table / sizeof *table; ++i) {
        const double ns = run(table[i].kernel, iterations);
        printf("%-44s %6.3f ns/iter  %5.1f cycles\n", table[i].name, ns, ns / base);
    }
    return 0;
}
