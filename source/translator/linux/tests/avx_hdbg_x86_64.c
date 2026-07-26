// avx_hdbg_x86_64.c -- SwiftVM guest shim for the K0 fdot bisect harness.
// Same freestanding recipe as avx_real_x86_64.c.  Build: build_avx_hdbg.sh
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
        long r = sys3(1, 1, (long)(buf + done), (long)(len - done));
        if (r <= 0) break;
        done += (ulong_t)r;
    }
}

void svm_exit(int code) {
    sys3(231, code, 0, 0);
    for (;;) sys3(60, code, 0, 0);
}

#include "avx_hdbg_kernels.h"

int guest_main(long argc, char **argv) {
    (void)argc;
    (void)argv;
    svm_exit(svm_hdbg_run());
    return 0;
}

__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "  xor %rbp, %rbp\n"
        "  mov (%rsp), %rdi\n"
        "  lea 8(%rsp), %rsi\n"
        "  and $-16, %rsp\n"
        "  call guest_main\n"
        "  hlt\n");
