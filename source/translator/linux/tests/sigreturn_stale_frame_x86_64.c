#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static __thread volatile uint64_t tls_value = UINT64_C(0x13579bdf2468ace0);
static __thread volatile uint64_t tls_expected = UINT64_C(0x13579bdf2468ace0);
static __thread volatile sig_atomic_t tls_hits;
static volatile sig_atomic_t tls_bad;

extern long stack_depth_syscall(int deep);

__asm__(".text\n"
        ".globl stack_depth_syscall\n"
        ".type stack_depth_syscall,@function\n"
        "stack_depth_syscall:\n"
        "test %edi,%edi\n"
        "jz 1f\n"
        "sub $32,%rsp\n"
        "1:\n"
        "mov $39,%eax\n"
        "syscall\n"
        "test %edi,%edi\n"
        "jz 2f\n"
        "add $32,%rsp\n"
        "2:\n"
        "ret\n"
        ".size stack_depth_syscall,.-stack_depth_syscall\n");

static void handle_alarm(int signal) {
    (void)signal;
    if (tls_value != tls_expected) {
        tls_bad = 1;
    }
    const uint64_t next =
            (tls_value ^ UINT64_C(0x9e3779b97f4a7c15)) + (uint64_t)tls_hits + 1;
    tls_value = next;
    tls_expected = next;
    ++tls_hits;
    alarm(1);
}

int main(int argc, char **argv) {
    const sig_atomic_t target_hits =
            argc > 1 && argv[1][0] == '1' && argv[1][1] == '\0' ? 1 : 4;
    struct sigaction action = {};
    action.sa_handler = handle_alarm;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) != 0) {
        perror("sigaction");
        return 2;
    }

    alarm(1);
    uint64_t seed = UINT64_C(0x0123456789abcdef);
    unsigned iteration = 0;
    while (tls_hits < target_hits && !tls_bad) {
        seed ^= (uint64_t)stack_depth_syscall(iteration++ & 1);
    }

    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &blocked, NULL) != 0) {
        perror("sigprocmask");
        return 3;
    }
    alarm(0);

    if (tls_bad || tls_hits < target_hits || tls_value != tls_expected) {
        fprintf(stderr,
                "FAIL hits=%d bad=%d tls=%#llx expected=%#llx seed=%#llx\n",
                tls_hits,
                tls_bad,
                (unsigned long long)tls_value,
                (unsigned long long)tls_expected,
                (unsigned long long)seed);
        return 1;
    }
    printf("PASS sigreturn TLS hits=%d\n", tls_hits);
    return 0;
}
