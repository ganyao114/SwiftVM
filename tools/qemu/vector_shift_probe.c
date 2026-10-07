#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern void ProbeShift(void*, uint64_t, void*);
extern void ProbeImmediate(void*, uint64_t, void*);

int main(void) {
    uint64_t input[2] = {UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000)};
    uint64_t output[2] = {0};
    const uint64_t expected = UINT64_C(0xff00000000000000);
    ProbeShift(input, 0, output);
    printf("vx %016" PRIx64 " %016" PRIx64 "\n", output[0], output[1]);
    int failed = output[0] != expected || output[1] != expected;
    ProbeImmediate(input, 0, output);
    printf("vi %016" PRIx64 " %016" PRIx64 "\n", output[0], output[1]);
    failed |= output[0] != expected || output[1] != expected;
    return failed;
}
