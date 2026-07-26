//
// bench_suite_kernels.h -- the SwiftVM performance-baseline kernels.
//
// Included VERBATIM by both sides so the two cannot drift:
//   bench_suite_x86_64.c   freestanding guest, runs under svm_translator_linux
//   bench_suite_native.c   libc x86-64 build, run natively (Rosetta on Apple
//                          Silicon) to produce the golden checksums
//
// The includer must provide, before including this header:
//   typedef ... u64_t / u32_t          (64/32-bit unsigned)
//   void *memcpy(void*, const void*, ulong_t)
//   void bench_put_str(const char *)
//   void bench_put_hex64(u64_t)
//
// Each kernel is chosen to stress ONE lowering path so a profile can be
// attributed:
//
//   int     64-bit integer ALU + a long dependency chain, no memory traffic.
//   fp      SSE2 packed double/single arithmetic on a register-resident set.
//   mem     streaming + strided + dependent-pointer-chase over 4 MiB.
//   branch  data-dependent, deliberately unpredictable branches.
//   call    a 4-deep chain of non-inlinable calls (call/ret, RSB, function
//           mode compilation).
//
// The loop bodies are straight-line so that raising `scale` changes only the
// number of iterations executed, never the amount of code translated.  That
// is what makes the two-point (S, 2S) regression a valid translate/execute
// separator without instrumenting the translator.
//

// Each kernel is noinline: without it clang inlines all five into the driver,
// every run compiles all five, and the per-kernel SVM_PROF code-size counters
// stop meaning anything.
#define BENCH_NOINLINE __attribute__((noinline))

// Integer-dense.  Two interleaved xorshift/multiply chains: real ILP to find,
// but a genuine data dependency inside each.  No loads or stores in the body.
static BENCH_NOINLINE u64_t kernel_int(u64_t iters) {
    u64_t a = 0x123456789abcdefULL;
    u64_t b = 0xfedcba9876543210ULL;
    for (u64_t i = 0; i < iters; i++) {
        a ^= a << 13;
        a ^= a >> 7;
        a ^= a << 17;
        a += 0x9e3779b97f4a7c15ULL;
        b = b * 6364136223846793005ULL + 1442695040888963407ULL;
        b ^= a >> 32;
        a += (b >> 11) ^ (a << 5);
        a -= (u64_t)(i * 3);
    }
    return a ^ b;
}

// Float/vector-dense.  Plain C arrays of 4 doubles / 8 floats so clang emits
// packed SSE2 (addpd/mulpd/addps/mulps) with no intrinsics header; the working
// set stays in registers.
static BENCH_NOINLINE u64_t kernel_fp(u64_t iters) {
    double d[4] = {1.0009765625, 1.001953125, 1.0029296875, 1.00390625};
    double e[4] = {0.9990234375, 0.998046875, 0.9970703125, 0.99609375};
    float f[8] = {1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f, 8.5f};
    float g[8] = {0.5f, 0.25f, 0.125f, 0.0625f, 2.0f, 4.0f, 8.0f, 16.0f};
    for (u64_t i = 0; i < iters; i++) {
        for (int k = 0; k < 4; k++) {
            d[k] = d[k] * e[k] + 0.5;
            e[k] = e[k] * 0.9999999 + 1e-9;
            if (d[k] > 1e30) d[k] = 1.0;
        }
        for (int k = 0; k < 8; k++) {
            f[k] = f[k] * g[k] + 1.0f;
            g[k] = g[k] * 0.999999f;
            if (f[k] > 1e30f) f[k] = 1.0f;
        }
    }
    // Fold to an integer checksum through the bit patterns: exact, and it makes
    // any lowering difference visible instead of rounding it away.
    u64_t sum = 0;
    for (int k = 0; k < 4; k++) {
        u64_t bits;
        memcpy(&bits, &d[k], 8);
        sum = sum * 31 + bits;
        memcpy(&bits, &e[k], 8);
        sum = sum * 31 + bits;
    }
    for (int k = 0; k < 8; k++) {
        u32_t bits;
        memcpy(&bits, &f[k], 4);
        sum = sum * 31 + (u64_t)bits;
        memcpy(&bits, &g[k], 4);
        sum = sum * 31 + (u64_t)bits;
    }
    return sum;
}

// 4 MiB working set: bigger than a core's L1/L2 slice, so the guest really does
// go to memory instead of measuring the translator's ability to keep the array
// in registers.
//
// aligned(65536) is NOT cosmetic; it was added after measurement, twice.  The
// freestanding guest has a single RWX PT_LOAD, so .bss begins immediately after
// .text/.rodata/.data.  SwiftVM write-protects guest code at HOST page
// granularity, and on macOS/arm64 getpagesize() is 16 KiB -- so any guest data
// within 16 KiB of guest code makes every store to it a write-protect fault
// that invalidates and forces recompilation of the containing translations.
// Measured on this benchmark:
//   unaligned (.bss 1.8 KiB into the code page): 545 compilations, 136k guest
//       instructions retranslated, 402 ms of a 452 ms run inside the translator
//   aligned(4096)  -- still inside the 16 KiB host page: 457 compilations,
//       two PCs compiled 222 times each, 71 ms of a 90 ms run
//   aligned(65536) -- clear of the host page: see the baseline file
// This benchmark is supposed to measure memory traffic, not SMC.  The effect
// itself is a real translator finding and is written up in docs/perf-baseline.md.
#define BENCH_MEM_WORDS (512u * 1024u)
static u64_t bench_mem_arr[BENCH_MEM_WORDS] __attribute__((aligned(65536)));

// Memory-dense.  Three shapes so no single prefetch-friendly pattern dominates:
// sequential write, strided read-modify-write, and a dependent pointer chase
// (each load's address comes from the previous load's value).
static BENCH_NOINLINE u64_t kernel_mem(u64_t iters) {
    u64_t acc = 0;
    for (u64_t it = 0; it < iters; it++) {
        for (u32_t i = 0; i < BENCH_MEM_WORDS; i++) {
            bench_mem_arr[i] = (u64_t)i * 0x9e3779b97f4a7c15ULL + it;
        }
        for (u32_t i = 0; i < BENCH_MEM_WORDS; i += 7) {
            bench_mem_arr[i] ^= bench_mem_arr[(i * 13u) & (BENCH_MEM_WORDS - 1u)];
            acc += bench_mem_arr[i] >> 17;
        }
        u32_t idx = 1;
        for (u32_t n = 0; n < BENCH_MEM_WORDS / 4u; n++) {
            idx = (u32_t)(bench_mem_arr[idx] >> 13) & (BENCH_MEM_WORDS - 1u);
            acc ^= idx;
        }
    }
    return acc;
}

// Branch-dense.  A fresh pseudo-random predicate each iteration, so no branch
// predictor can help and every one becomes a real flag-compute + conditional in
// the generated code.  Six differently shaped comparisons (signed/unsigned,
// equality, range) exercise different flag subsets.
static BENCH_NOINLINE u64_t kernel_branch(u64_t iters) {
    u64_t s = 0x2545f4914f6cdd1dULL;
    u64_t acc = 0;
    for (u64_t i = 0; i < iters; i++) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        u32_t lo = (u32_t)s;
        if (lo & 1u) acc += 3; else acc ^= 5;
        if ((int)lo < 0) acc -= 7; else acc += 11;
        if ((lo & 0xffu) == 0x42u) acc ^= 0x1234;
        if (lo > 0x80000000u) acc += 13; else acc -= 17;
        if ((int)(lo >> 16) > (int)(lo & 0xffffu)) acc ^= 19; else acc += 23;
        if (((lo >> 8) & 7u) == 0u) acc = acc * 3 + 1;
        acc ^= (acc >> 5);
    }
    return acc ^ s;
}

// Call-dense.  noinline defeats inlining; the chain is four levels deep, so
// each iteration performs 4 calls and 4 returns.  Exercises function-mode
// compilation, the return stack buffer and the call/ret lowering rather than
// any ALU path.
static BENCH_NOINLINE u64_t call_leaf(u64_t x) { return x * 0x2545f4914f6cdd1dULL + 1; }
static BENCH_NOINLINE u64_t call_l3(u64_t x) { return call_leaf(x) ^ (x >> 3); }
static BENCH_NOINLINE u64_t call_l2(u64_t x) { return call_l3(x + 1) + (x << 1); }
static BENCH_NOINLINE u64_t call_l1(u64_t x) { return call_l2(x ^ 0x5bf03635ULL) - x; }

static BENCH_NOINLINE u64_t kernel_call(u64_t iters) {
    u64_t acc = 0;
    for (u64_t i = 0; i < iters; i++) {
        acc = call_l1(acc + i);
    }
    return acc;
}

// Iteration counts calibrated by measurement (not guessed) so every kernel
// lands near 450 ms under the translator at scale=1 on this machine: ~30x the
// ~15 ms translator startup floor, yet short enough to repeat 15 times per
// configuration.  Re-calibrate if the numbers drift far from each other; equal
// runtimes make the per-kernel medians directly comparable.
#define ITERS_INT    (20000000ULL)
#define ITERS_FP     (4000000ULL)
#define ITERS_MEM    (300ULL)
#define ITERS_BRANCH (12000000ULL)
#define ITERS_CALL   (16000000ULL)

static int bench_str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static u64_t bench_parse_u64(const char *p) {
    u64_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (u64_t)(*p - '0'); p++; }
    return v;
}

// Shared driver: argv[1] selects the kernel, argv[2] is the iteration
// multiplier.  Returns the process exit code.
static int bench_run(long argc, char **argv) {
    const char *which = (argc > 1 && argv[1]) ? argv[1] : "all";
    u64_t scale = (argc > 2 && argv[2]) ? bench_parse_u64(argv[2]) : 1;
    if (scale == 0) scale = 1;

    int all = bench_str_eq(which, "all");
    if (all || bench_str_eq(which, "int")) {
        bench_put_str("int    ");
        bench_put_hex64(kernel_int(ITERS_INT * scale));
    }
    if (all || bench_str_eq(which, "fp")) {
        bench_put_str("fp     ");
        bench_put_hex64(kernel_fp(ITERS_FP * scale));
    }
    if (all || bench_str_eq(which, "mem")) {
        bench_put_str("mem    ");
        bench_put_hex64(kernel_mem(ITERS_MEM * scale));
    }
    if (all || bench_str_eq(which, "branch")) {
        bench_put_str("branch ");
        bench_put_hex64(kernel_branch(ITERS_BRANCH * scale));
    }
    if (all || bench_str_eq(which, "call")) {
        bench_put_str("call   ");
        bench_put_hex64(kernel_call(ITERS_CALL * scale));
    }
    return 0;
}
