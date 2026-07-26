// avx_hdbg_host.c -- native x86-64 oracle for the K0 fdot bisect harness.
// Run under `ROSETTA_ADVERTISE_AVX=1 arch -x86_64` on Apple Silicon.
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

#include "avx_hdbg_kernels.h"

int main(void) {
    svm_exit(svm_hdbg_run());
    return 0;
}
