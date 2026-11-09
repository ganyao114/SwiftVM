typedef unsigned u32;
typedef unsigned long long u64;
static volatile u32 word = 7;
static volatile u64 wide = 0x123456789abcdef0ULL;
static long service(u32 number, u32 a, u32 b, u32 c) {
    register long r0 __asm__("r0") = a;
    register u32 r1 __asm__("r1") = b;
    register u32 r2 __asm__("r2") = c;
    register u32 r7 __asm__("r7") = number;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory", "cc");
    return r0;
}
int guest_main(void) {
    typedef int (*CAS)(u32,u32,volatile u32*);
    typedef int (*CAS64)(const u64*,const u64*,volatile u64*);
    typedef u32 (*TLS)(void);
    typedef void (*Barrier)(void);
    if (*(const u32*)0xffff0ffc < 5) return 2;
    if (service(0xf0005,0x12345678,0,0) != 0) return 3;
    if (((TLS)0xffff0fe0)() != 0x12345678) return 4;
    ((Barrier)0xffff0fa0)();
    if (((CAS)0xffff0fc0)(7,9,&word) || word != 9) return 5;
    if (!((CAS)0xffff0fc0)(7,11,&word) || word != 9) return 6;
    const u64 old = wide, next = 0xfedcba9876543210ULL;
    if (((CAS64)0xffff0f60)(&old,&next,&wide) || wide != next) return 7;
    if (!((CAS64)0xffff0f60)(&old,&old,&wide) || wide != next) return 8;
    const char output[] = "arm32 kuser ok\n";
    return service(4,1,(u32)output,sizeof(output)-1) == sizeof(output)-1 ? 0 : 9;
}
