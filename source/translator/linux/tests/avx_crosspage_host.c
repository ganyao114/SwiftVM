//
// avx_crosspage_host.c -- native oracle for avx_crosspage_x86_64.
//
// Same experiment, same intrinsics; the difference is only how the observer
// survives the fault.  Here a SIGSEGV/SIGBUS handler siglongjmp()s out, which
// leaves the mapping exactly as the faulting instruction left it -- the signal
// is delivered after the access has either committed or not, so reading the
// page back in the handler's continuation is a faithful measurement.
//
// The golden file is the ACTUAL OUTPUT of this program running as x86-64:
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 ./avx_crosspage_host_x86_64
// or natively on an x86-64 Linux box.  Never hand-computed.
//
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
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

void *xp_mmap(unsigned long len) {
    void *p = mmap(0, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}

int xp_munmap(void *addr, unsigned long len) { return munmap(addr, len); }

static sigjmp_buf xp_jmp;

static void xp_handler(int sig) {
    (void)sig;
    siglongjmp(xp_jmp, 1);
}

int xp_run_faulting(void (*fn)(void *), void *arg) {
    struct sigaction sa, old_segv, old_bus;
    sa.sa_handler = xp_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    int faulted = 1;
    if (sigsetjmp(xp_jmp, 1) == 0) {
        fn(arg);
        faulted = 0;
    }
    sigaction(SIGSEGV, &old_segv, 0);
    sigaction(SIGBUS, &old_bus, 0);
    return faulted;
}

#include "avx_crosspage_common.h"

int main(int argc, char **argv) {
    int stage = 0;
    if (argc > 1) {
        stage = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
            stage = stage * 10 + (*p - '0');
    }
    svm_exit(avx_crosspage_run(stage));
    return 0;
}
