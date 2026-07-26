//
// bench_suite_x86_64.c -- SwiftVM performance-baseline guest.
//
// Freestanding (own _start, raw Linux syscalls, no libc), same recipe as
// avx_real_x86_64.c.  Freestanding is deliberate and it matters for a
// benchmark: a static-glibc guest spends most of a short run inside libc
// startup, so a libc-based microbenchmark would measure the translator's cost
// of compiling glibc rather than the cost of running the kernel under test.
// Here the only code that ever executes is _start plus the selected kernel.
//
// The kernels themselves live in bench_suite_kernels.h and are shared verbatim
// with bench_suite_native.c, which produces the golden checksums.
//
// Usage:
//   svm_translator_linux bench_suite_x86_64 <kernel> [scale]
//     <kernel>  int | fp | mem | branch | call | all
//     [scale]   iteration multiplier, decimal, default 1
//
// `scale` is what makes the translation-vs-execution split measurable WITHOUT
// instrumenting the translator: run the same kernel at scale S and 2S; the
// slope is pure execution and the intercept is startup + translation.
//
// Build: bash build_bench_tests.sh
//
#define SVM_GUEST_FREESTANDING 1

typedef unsigned long ulong_t;
typedef unsigned long long u64_t;
typedef unsigned int u32_t;

static long sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

static void svm_write(const char *buf, ulong_t len) {
    ulong_t done = 0;
    while (done < len) {
        long r = sys3(1 /* write */, 1, (long)(buf + done), (long)(len - done));
        if (r <= 0) break;
        done += (ulong_t)r;
    }
}

static void svm_exit(int code) {
    sys3(231 /* exit_group */, code, 0, 0);
    for (;;) sys3(60 /* exit */, code, 0, 0);
}

// -nostdlib still needs these: the optimizer turns array fills and struct
// copies into calls.  volatile on the cursor keeps clang from recognising the
// loop as the very idiom it is implementing and emitting a self-call.
void *memset(void *dst, int c, ulong_t n) {
    volatile unsigned char *p = (volatile unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, ulong_t n) {
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void bench_put_str(const char *s) {
    ulong_t n = 0;
    while (s[n]) n++;
    svm_write(s, n);
}

static void bench_put_hex64(u64_t v) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned d = (unsigned)((v >> (60 - 4 * i)) & 0xf);
        buf[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    buf[18] = '\n';
    svm_write(buf, 19);
}

#include "bench_suite_kernels.h"

int guest_main(long argc, char **argv) {
    svm_exit(bench_run(argc, argv));
    return 0;
}

__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "  xor %rbp, %rbp\n"
        "  mov (%rsp), %rdi\n"   // argc
        "  lea 8(%rsp), %rsi\n"  // argv
        "  and $-16, %rsp\n"     // call pushes 8 -> ABI-correct at entry
        "  call guest_main\n"
        "  hlt\n");
