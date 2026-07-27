//
// avx_real_host.c -- native oracle for avx_real_x86_64.
//
// Same kernels, same compiler, same -mavx2 -O2; the only difference is that
// stdout goes through write(2) instead of a raw Linux syscall.  The golden
// file checked in next to the guest binary is the ACTUAL OUTPUT OF THIS
// PROGRAM running as x86-64 -- under Rosetta on Apple Silicon
// (`ROSETTA_ADVERTISE_AVX=1 arch -x86_64 ./avx_real_host_x86_64`) or natively
// on an x86-64 Linux box.  Nothing in the golden file is ever hand-computed.
//
// Note on Rosetta: its CPUID hides AVX unless ROSETTA_ADVERTISE_AVX=1, but
// this binary never asks.  It is compiled with unconditional AVX2, so if
// Rosetta could not execute AVX2 the program would die with SIGILL rather
// than quietly produce a scalar answer -- the capability check is the
// execution itself.
//
// Build:  clang -arch x86_64 -mavx2 -O2 -o avx_real_host_x86_64 avx_real_host.c
//   (on x86-64 Linux: gcc -static -mavx2 -O2 -o avx_real_host_x86_64 ...)
//
#include <unistd.h>

void svm_write(const char *buf, unsigned long len) {
    unsigned long done = 0;
    while (done < len) {
        long r = write(1, buf + done, len - done);
        if (r <= 0) break;
        done += (unsigned long)r;
    }
}

void svm_exit(int code) { _exit(code); }

#include "avx_real_kernels.h"

// Same argument rule as the guest: a decimal argument selects one kernel,
// anything else (e.g. "all") runs the whole program.  Kept character-for-
// character identical to guest_main() so the golden file and the guest run
// are answering the same question.
int main(int argc, char **argv) {
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
