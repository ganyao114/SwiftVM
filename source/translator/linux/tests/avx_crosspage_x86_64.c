//
// avx_crosspage_x86_64.c -- SwiftVM guest side of the 32-byte cross-page fault
// experiment (see avx_crosspage_common.h for what it measures and why).
//
// SwiftVM delivers no signals to the guest, so the faulting access cannot be
// caught in-process.  It is instead run on a clone() thread: main.cpp only
// escalates a fatal ExitReason to the whole thread group when the *leader*
// hits it, so a worker thread dying of PageFatal leaves the leader alive with
// the shared address space intact -- exactly the observer this test needs.
// CLONE_CHILD_CLEARTID gives a reliable join, because the tid store and futex
// wake happen on the thread teardown path even after a fatal exit.
//
// Build: see build_avx_real_tests.sh
// Run:   SVM_AVX=1 ./svm_translator_linux avx_crosspage_x86_64
//
#define SVM_GUEST_FREESTANDING 1

typedef unsigned long ulong_t;

static long sys6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

void svm_write(const char *buf, ulong_t len) {
    ulong_t done = 0;
    while (done < len) {
        long r = sys6(1, 1, (long)(buf + done), (long)(len - done), 0, 0, 0);
        if (r <= 0) break;
        done += (ulong_t)r;
    }
}

void svm_exit(int code) {
    sys6(231, code, 0, 0, 0, 0, 0);
    for (;;) sys6(60, code, 0, 0, 0, 0, 0);
}

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

#define GUEST_PROT_RW 0x3
#define GUEST_MAP_PRIVATE_ANON 0x22

void *xp_mmap(ulong_t len) {
    long r = sys6(9 /* mmap */, 0, (long)len, GUEST_PROT_RW,
                  GUEST_MAP_PRIVATE_ANON, -1, 0);
    if (r < 0 && r > -4096) return 0;
    return (void *)r;
}

int xp_munmap(void *addr, ulong_t len) {
    return (int)sys6(11 /* munmap */, (long)addr, (long)len, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// clone() worker
// ---------------------------------------------------------------------------
// CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|CHILD_CLEARTID
#define XP_CLONE_FLAGS 0x250f00L

// rdi = flags, rsi = child stack top, rdx = ctid, rcx = child entry.
// Returns the child tid in the parent; the child never returns.
extern long xp_clone(long flags, void *stack_top, int *ctid, void (*entry)(void));
__asm__(".text\n"
        ".globl xp_clone\n"
        "xp_clone:\n"
        "  mov %rcx, %r9\n"       // entry survives the syscall (rcx does not)
        "  mov %rdx, %r10\n"      // ctid -> 4th syscall argument
        "  xor %edx, %edx\n"      // ptid = NULL
        "  xor %r8d, %r8d\n"      // tls  = NULL
        "  mov $56, %eax\n"       // SYS_clone
        "  syscall\n"
        "  test %eax, %eax\n"
        "  jnz 1f\n"
        "  xor %ebp, %ebp\n"      // child: stack_top is 16-aligned, call -> +8
        "  call *%r9\n"
        "  mov $60, %eax\n"       // SYS_exit (this thread only)
        "  xor %edi, %edi\n"
        "  syscall\n"
        "  hlt\n"
        "1:\n"
        "  ret\n");

static void (*xp_fn)(void *);
static void *xp_arg;
static volatile int xp_completed;
static volatile int xp_ctid;
// The worker stack is .bss, NOT mmap().  Deliberate: on this SwiftVM build a
// clone() child whose stack lives in an mmap()ed region kills the whole host
// process with an unhandled SIGBUS as soon as it exits, while the identical
// program with a .bss stack runs clean.  That bug is orthogonal to AVX and is
// reported separately; using .bss here keeps this test measuring what it is
// supposed to measure.
static char xp_stack[1 << 16] __attribute__((aligned(16)));

static void xp_child(void) {
    xp_fn(xp_arg);
    xp_completed = 1;
}

int xp_run_faulting(void (*fn)(void *), void *arg) {
    xp_fn = fn;
    xp_arg = arg;
    xp_completed = 0;
    xp_ctid = 1;
    void *top = (void *)(((unsigned long)(xp_stack + (1 << 16))) & ~15UL);
    long tid = xp_clone(XP_CLONE_FLAGS, top, (int *)&xp_ctid, xp_child);
    if (tid < 0) return -1;
    // CLONE_CHILD_CLEARTID zeroes xp_ctid on thread teardown, fatal or not.
    struct { long sec, nsec; } ts = {0, 2000000};
    for (int i = 0; i < 2000 && xp_ctid != 0; i++)
        sys6(35 /* nanosleep */, (long)&ts, 0, 0, 0, 0, 0);
    return !xp_completed;
}

#include "avx_crosspage_common.h"

int guest_main(long argc, char **argv) {
    int stage = 0;
    if (argc > 1 && argv[1]) {
        int v = 0, any = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) {
            v = v * 10 + (*p - '0');
            any = 1;
        }
        if (any) stage = v;
    }
    svm_exit(avx_crosspage_run(stage));
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
