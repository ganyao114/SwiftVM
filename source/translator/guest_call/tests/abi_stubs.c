//
// Guest-side ABI stubs, compiled by clang for x86-64 Linux.
//
// WHY THESE EXIST.  The host-side classification in abi_sysv.h is a reading of
// the psABI document; these stubs are the *compiler's* reading of the same
// document, compiled to real x86-64 machine code.  If the two disagree about
// which register an argument lives in, the stub computes the wrong number and
// the test fails.  That is a stronger check than any assertion the host could
// make about its own tables.
//
// Freestanding: no libc, no globals that need relocating except the entry
// table.  Build with tests/build_abi_stubs.sh.
//
#include <stdarg.h>

#include "abi_stubs_index.h"

typedef struct { int a; int b; } TwoInt;
typedef struct { double x; double y; } TwoDouble;
typedef struct { double d; long l; } DoubleLong;
typedef struct { long l; double d; } LongDouble;
typedef struct { float a; float b; } TwoFloat;
typedef struct { int a; float f; } IntFloat;
typedef struct { char a, b, c, d, e, f, g, h; } Chars8;
typedef struct { long a, b, c; } Big24;
typedef struct { long a; long b; } TwoLong;

// --- plain integer / floating-point argument passing ------------------------

long stub_int_sum9(long a, long b, long c, long d, long e, long f, long g, long h, long i) {
    return a * 1 + b * 3 + c * 7 + d * 11 + e * 13 + f * 17 + g * 19 + h * 23 + i * 29;
}

double stub_dbl_sum10(double a, double b, double c, double d, double e,
                      double f, double g, double h, double i, double j) {
    return a * 1 + b * 3 + c * 7 + d * 11 + e * 13 + f * 17 + g * 19 + h * 23 + i * 29 + j * 31;
}

// Integer and SSE argument counters advance independently.  Get that wrong and
// this is the case that notices; with fewer arguments a shared counter can
// accidentally produce the right answer.
double stub_mix8(long a, double x, long b, double y, long c, double z, long d, double w) {
    return (double)(a * 1 + b * 3 + c * 7 + d * 11) + x * 100 + y * 300 + z * 700 + w * 1100;
}

// --- small structs by value -------------------------------------------------

long stub_two_int(TwoInt s) { return (long)s.a * 1000 + s.b; }
double stub_two_double(TwoDouble s) { return s.x * 100 + s.y; }
double stub_double_long(DoubleLong s) { return s.d * 100 + (double)s.l; }
double stub_long_double(LongDouble s) { return (double)s.l * 100 + s.d; }
double stub_two_float(TwoFloat s) { return (double)s.a * 100 + (double)s.b; }
double stub_int_float(IntFloat s) { return (double)s.a * 100 + (double)s.f; }
long stub_chars8(Chars8 s) {
    return s.a + s.b * 2 + s.c * 4 + s.d * 8 + s.e * 16 + s.f * 32 + s.g * 64 + s.h * 128;
}

// --- MEMORY return values ---------------------------------------------------
// %rdi is the hidden return buffer, so a, b, c arrive in %rsi, %rdx, %rcx.

Big24 stub_make_big(long a, long b, long c) {
    Big24 r;
    r.a = a + 1;
    r.b = b + 2;
    r.c = c + 3;
    return r;
}

Big24 stub_big_ident(Big24 in, long k) {
    Big24 r;
    r.a = in.a + k;
    r.b = in.b + k * 2;
    r.c = in.c + k * 3;
    return r;
}

long stub_take_big(Big24 s, long k) { return s.a * 1 + s.b * 3 + s.c * 7 + k * 11; }

// --- register-returned structs ---------------------------------------------

TwoDouble stub_ret_two_double(double a, double b) {
    TwoDouble r;
    r.x = a * 2;
    r.y = b * 3;
    return r;
}

DoubleLong stub_ret_double_long(double a, long b) {
    DoubleLong r;
    r.d = a * 2;
    r.l = b * 3;
    return r;
}

// Two INTEGER eightbytes come back in %rax THEN %rdx -- the second return
// register is easy to get wrong and nothing else in this file exercises it.
TwoLong stub_ret_two_long(long a, long b) {
    TwoLong r;
    r.a = a * 2;
    r.b = b * 3;
    return r;
}

TwoInt stub_ret_two_int(int a, int b) {
    TwoInt r;
    r.a = a * 2;
    r.b = b * 3;
    return r;
}

// --- varargs ----------------------------------------------------------------
// The double variants read from the XMM register-save area, which the callee
// only fills when %al != 0.  A caller that forgets %al produces garbage here.

long stub_vararg_ints(int n, ...) {
    va_list ap;
    long sum = 0;
    long w = 1;
    va_start(ap, n);
    for (int i = 0; i < n; ++i) {
        sum += va_arg(ap, long) * w;
        w += 2;
    }
    va_end(ap);
    return sum;
}

double stub_vararg_doubles(int n, ...) {
    va_list ap;
    double sum = 0;
    double w = 1;
    va_start(ap, n);
    for (int i = 0; i < n; ++i) {
        sum += va_arg(ap, double) * w;
        w += 2;
    }
    va_end(ap);
    return sum;
}

double stub_vararg_mixed(int n, ...) {
    va_list ap;
    double sum = 0;
    va_start(ap, n);
    for (int i = 0; i < n; ++i) {
        long l = va_arg(ap, long);
        double d = va_arg(ap, double);
        sum += (double)l * 10 + d;
    }
    va_end(ap);
    return sum;
}

// --- spill cases ------------------------------------------------------------

// Eight doubles exhaust %xmm0-7, so the struct (SSE, SSE) has no vector
// registers left and the WHOLE argument goes on the stack -- psABI "if there
// are no registers available for any eightbyte ... assignments get reverted".
double stub_struct_by_spill(double a, double b, double c, double d, double e,
                            double f, double g, double h, TwoDouble s) {
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + s.x * 100 + s.y * 300;
}

// Six integers exhaust the INTEGER sequence; the float still gets %xmm0, which
// only works if the two counters are independent.
long stub_int_float_mix(long a, long b, long c, long d, long e, long f, float g) {
    return a + b * 3 + c * 7 + d * 11 + e * 13 + f * 17 + (long)(g * 1000);
}

// Seven integers put one long on the stack at offset 0; the __int128 that
// follows needs TWO integer registers (none are left) so it also goes to the
// stack -- at an offset respecting its OWN 16-byte alignment, i.e. 16, not 8.
// psABI: "pass the argument on the stack at an address respecting the
// argument's alignment (which might be more than its natural alignment)".
long stub_i128_after_stack(long a, long b, long c, long d, long e, long f, long g,
                           __int128 x) {
    return a + b * 3 + c * 7 + d * 11 + e * 13 + f * 17 + g * 19 + (long)x * 23 +
           (long)(x >> 64) * 29;
}

// --- failure modes ----------------------------------------------------------

long stub_crash(void) {
    *(volatile long*)0x11 = 1;  // unmapped guest address
    return 42;
}

// --- hand-written stubs where the exact machine state is the point ----------
__asm__(".text\n"
        ".globl stub_get_rsp\n"
        "stub_get_rsp:\n"
        "  movq %rsp, %rax\n"
        "  ret\n"

        ".globl stub_get_al\n"
        "stub_get_al:\n"
        "  movzbq %al, %rax\n"
        "  ret\n"

        ".globl stub_ud2\n"
        "stub_ud2:\n"
        "  ud2\n"

        // Unbounded recursion: the call stack runs off its bottom into
        // unmapped guest memory.  Written in asm because -O2 turns the C form
        // (`return f() + 1;`) into an accumulator LOOP that never touches the
        // stack, which tests nothing.
        ".globl stub_runaway\n"
        "stub_runaway:\n"
        "  pushq %rax\n"
        "  call stub_runaway\n"
        "  popq %rax\n"
        "  ret\n"

        // Returns to the caller with %rsp 16 bytes below where it must be.
        ".globl stub_unbalanced\n"
        "stub_unbalanced:\n"
        "  movq (%rsp), %rax\n"
        "  addq $8, %rsp\n"
        "  subq $16, %rsp\n"
        "  jmp *%rax\n"

        // Snapshots the complete argument register file into a fixed guest
        // address, so a test can assert *placement* and not just the result of
        // some arithmetic the compiler chose.
        ".globl stub_dump_args\n"
        "stub_dump_args:\n"
        "  movq %rdi, 0x30000000\n"
        "  movq %rsi, 0x30000008\n"
        "  movq %rdx, 0x30000010\n"
        "  movq %rcx, 0x30000018\n"
        "  movq %r8,  0x30000020\n"
        "  movq %r9,  0x30000028\n"
        "  movq %rax, 0x30000030\n"
        "  movq %rsp, 0x30000038\n"
        "  movups %xmm0, 0x30000040\n"
        "  movups %xmm1, 0x30000050\n"
        "  movups %xmm2, 0x30000060\n"
        "  movups %xmm3, 0x30000070\n"
        "  movups %xmm4, 0x30000080\n"
        "  movups %xmm5, 0x30000090\n"
        "  movups %xmm6, 0x300000A0\n"
        "  movups %xmm7, 0x300000B0\n"
        "  movq 8(%rsp), %rax\n  movq %rax, 0x300000C0\n"
        "  movq 16(%rsp), %rax\n movq %rax, 0x300000C8\n"
        "  movq 24(%rsp), %rax\n movq %rax, 0x300000D0\n"
        "  movq 32(%rsp), %rax\n movq %rax, 0x300000D8\n"
        "  movq 40(%rsp), %rax\n movq %rax, 0x300000E0\n"
        "  movq 48(%rsp), %rax\n movq %rax, 0x300000E8\n"
        "  movq 56(%rsp), %rax\n movq %rax, 0x300000F0\n"
        "  movq 64(%rsp), %rax\n movq %rax, 0x300000F8\n"
        "  ret\n");

long stub_get_rsp(void);
long stub_get_al(void);
long stub_ud2(void);
long stub_unbalanced(void);
long stub_runaway(void);
void stub_dump_args(void);

// The ELF entry point IS this table: e_entry points at slot 0, and the host
// reads entry + 8*index to find each stub.
__attribute__((used)) void* const _start[kStubCount] = {
        [kStubIntSum9] = (void*)stub_int_sum9,
        [kStubDblSum10] = (void*)stub_dbl_sum10,
        [kStubMix8] = (void*)stub_mix8,
        [kStubTwoInt] = (void*)stub_two_int,
        [kStubTwoDouble] = (void*)stub_two_double,
        [kStubDoubleLong] = (void*)stub_double_long,
        [kStubLongDouble] = (void*)stub_long_double,
        [kStubTwoFloat] = (void*)stub_two_float,
        [kStubIntFloat] = (void*)stub_int_float,
        [kStubChars8] = (void*)stub_chars8,
        [kStubMakeBig] = (void*)stub_make_big,
        [kStubBigIdent] = (void*)stub_big_ident,
        [kStubTakeBig] = (void*)stub_take_big,
        [kStubRetTwoDouble] = (void*)stub_ret_two_double,
        [kStubRetDoubleLong] = (void*)stub_ret_double_long,
        [kStubRetTwoInt] = (void*)stub_ret_two_int,
        [kStubVarargInts] = (void*)stub_vararg_ints,
        [kStubVarargDoubles] = (void*)stub_vararg_doubles,
        [kStubVarargMixed] = (void*)stub_vararg_mixed,
        [kStubGetRsp] = (void*)stub_get_rsp,
        [kStubGetAl] = (void*)stub_get_al,
        [kStubDumpArgs] = (void*)stub_dump_args,
        [kStubCrash] = (void*)stub_crash,
        [kStubUd2] = (void*)stub_ud2,
        [kStubUnbalanced] = (void*)stub_unbalanced,
        [kStubStructBySpill] = (void*)stub_struct_by_spill,
        [kStubIntFloatMix] = (void*)stub_int_float_mix,
        [kStubI128AfterStack] = (void*)stub_i128_after_stack,
        [kStubRetTwoLong] = (void*)stub_ret_two_long,
        [kStubRunaway] = (void*)stub_runaway,
};
