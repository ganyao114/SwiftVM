#include <stdint.h>
#include <stdio.h>
#include <time.h>
int main(void) {
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    double d = 1.0;
    for (uint64_t i = 0; i < 150000000ull; ++i) { d = d * 1.0000001 + 1e-9; }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const long long ns = (t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec);
    printf("elapsed_ns=%lld d=%a\n", ns, d);
    return 0;
}
