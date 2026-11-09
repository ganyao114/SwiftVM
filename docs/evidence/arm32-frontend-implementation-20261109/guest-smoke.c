typedef unsigned u32;
typedef unsigned long long u64;
typedef u32 Vec __attribute__((vector_size(16)));
static volatile u32 sink;
static long write_stdout(const char* data, u32 size) {
    register long r0 __asm__("r0") = 1;
    register const char* r1 __asm__("r1") = data;
    register u32 r2 __asm__("r2") = size;
    register long r7 __asm__("r7") = 4;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory", "cc");
    return r0;
}
__attribute__((noinline)) static u32 integer_work(u32 x, u32 y) {
    for (u32 i = 0; i < 400; ++i) {
        x ^= y + (x << (i & 31));
        x = (x >> 7) | (x << 25);
        y = y * 1664525u + 1013904223u;
    }
    return x ^ y;
}
__attribute__((noinline)) static u32 vector_work(u32 seed) {
    Vec a = {seed, seed + 1, seed + 2, seed + 3};
    Vec b = {13, 17, 19, 23};
    for (u32 i = 0; i < 40; ++i) {
        a = (a + b) ^ (a >> 3);
        b = __builtin_shufflevector(b, b, 1, 2, 3, 0) + a;
    }
    return a[0] ^ a[1] ^ a[2] ^ a[3] ^ b[0];
}
__attribute__((noinline)) static u32 fp_work(void) {
    volatile double input = 1.5;
    double x = input;
    float y = 0.25f;
    for (u32 i = 0; i < 80; ++i) {
        x = x * 1.00001 + 0.125;
        y = y * 0.99999f + 0.0625f;
    }
    return (u32)(x * 1024.0) ^ (u32)(y * 1024.0f);
}
int guest_main(void) {
    u32 checksum = integer_work(1234567u, 3456789u) ^ vector_work(0x10203040) ^ fp_work();
    sink = checksum;
    char output[9];
    const char digits[] = "0123456789abcdef";
    for (u32 i = 0; i < 8; ++i) output[i] = digits[(checksum >> (28 - 4 * i)) & 15];
    output[8] = '\n';
    return write_stdout(output, sizeof(output)) == sizeof(output) ? 0 : 1;
}
