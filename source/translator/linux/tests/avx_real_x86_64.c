//
// avx_real_x86_64.c -- SwiftVM guest side of the real-AVX-program test.
//
// Freestanding: its own _start, raw Linux syscalls, no libc at all.  That is
// not minimalism for its own sake -- a static glibc would route memcpy/strlen
// through an ifunc that selects an AVX2 variant only when the CPU also has
// BMI2, which SwiftVM does not implement, so the AVX2 paths under test would
// never actually run.  Here every AVX2 instruction is unconditional.
//
// Build (Linux):
//   gcc -static -nostdlib -fno-pic -fno-stack-protector -mavx2 -O2 \
//       -DSVM_GUEST_FREESTANDING -o avx_real_x86_64 avx_real_x86_64.c
// Build (macOS cross, no ELF linker on the host -- see mklinuxelf.py):
//   see build_avx_real_tests.sh
//
// Run:  SVM_AVX=1 ./svm_translator_linux avx_real_x86_64 [kernel-index]
//
#define SVM_GUEST_FREESTANDING 1

typedef unsigned long ulong_t;

static long sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

void svm_write(const char *buf, ulong_t len) {
    ulong_t done = 0;
    while (done < len) {
        long r = sys3(1 /* write */, 1, (long)(buf + done), (long)(len - done));
        if (r <= 0) break;
        done += (ulong_t)r;
    }
}

void svm_exit(int code) {
    sys3(231 /* exit_group */, code, 0, 0);
    for (;;) sys3(60 /* exit */, code, 0, 0);
}

// -nostdlib still needs these: the optimizer turns array fills and struct
// copies into calls.  `volatile` on the cursor keeps clang from recognising
// the loop as the very idiom it is implementing and emitting a self-call.
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

void *memmove(void *dst, const void *src, ulong_t n) {
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

#include "avx_real_kernels.h"

// argv[1], when present, selects a single kernel by index so that a fatal
// decode gap can be bisected without editing the program.
int guest_main(long argc, char **argv) {
    int only = -1;
    if (argc > 1 && argv[1]) {
        int v = 0, any = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) {
            v = v * 10 + (*p - '0');
            any = 1;
        }
        if (any) only = v;
    }
    svm_exit(svm_avx_run(only));
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
