#include <stdint.h>
#include <stdio.h>
#include <time.h>
int main(void) {
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t x = 0x243f6a8885a308d3ull;
    for (uint64_t i = 0; i < 150000000ull; ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        x ^= x >> 33;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const long long ns = (t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec);
    printf("elapsed_ns=%lld x=%016llx\n", ns, (unsigned long long)x);
    return 0;
}
