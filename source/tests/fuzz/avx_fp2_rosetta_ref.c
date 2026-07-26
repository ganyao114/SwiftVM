// ===========================================================================
// Second-wave VEX reference generator -- runs real x86-64 AVX under Rosetta.
// ===========================================================================
//
// Covers the scalar moves, scalar conversions, shuffles, lane moves and the
// VEX.128 twins listed in avx_fp2_ops.inc.  Same oracle and same traps as
// avx_fp_rosetta_ref.c:
//
//   * Rosetta does NOT advertise AVX through CPUID unless the process starts
//     with ROSETTA_ADVERTISE_AVX=1.  Execution works either way, so support is
//     decided by EXECUTING an instruction and catching SIGILL, never by
//     reading CPUID.
//   * Rosetta is an emulator with its own defects (VPSLLVQ's shift count was
//     measured truncated to 32 bits, and four other oracle defects turned up
//     over this work), so a Rosetta result is evidence, not proof.  Anything
//     surprising must be cross-checked against the Intel SDM before it is
//     believed -- see the notes next to the pairs below.
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// ----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/avxfp2ref avx_fp2_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxfp2ref > avx_fp2_rosetta_ref.inc
//
// Add --dump-encodings to print each stub's bytes to stderr for auditing
// against a disassembler.
//
// WHAT A ROW CONTAINS, AND WHY IT CONTAINS THE ENCODING
// ----------------------------------------------------
// Every row carries the LITERAL BYTES of the instruction sequence under test.
// avx_fp2_test.cpp replays those bytes rather than re-encoding from the shared
// table, so the two sides cannot possibly assemble different instructions --
// the failure mode the first wave had to guard against with a hand-audited
// encoder on each side.  The table (avx_fp2_ops.inc) still says WHAT must be
// covered; it no longer has to say HOW.
//
// Each stub is
//
//     vmovdqu ymm0, [rdi+0x60]        ; poison the destination
//     vmovdqu ymm1, [rdi+0x00]        ; A
//     vmovdqu ymm2, [rdi+0x20]        ; B
//     <the recorded byte sequence>
//     <capture tail, per form>
//     vzeroupper
//     ret
//
// and the three forms differ only in where the answer is read from:
//
//   form 0  ymm0, read back with a 256-bit store the recorded bytes EXCLUDE
//           (so the test can read the register out of ThreadContext64 instead
//           and a broken vmovdqu cannot mask a broken handler).  Reading all
//           32 bytes of a poisoned ymm0 is what MEASURES contract C3.
//   form 1  the 32-byte capture slot at [rdi+0x40].  The recorded bytes
//           include whatever writes it: a store instruction, or the five SETcc
//           of a flag capture.
//   form 2  rax, appended to the slot by a `mov [rdi+0x40], rax` the recorded
//           bytes EXCLUDE.  The test reads rax from the context.
//
// NOTHING HERE IS HAND-COMPUTED.  Every value is the literal bytes Rosetta
// wrote.  An instruction Rosetta refuses becomes a SKIP comment, never a value
// filled in from the manual.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define DATA_A 0x00
#define DATA_B 0x20
#define DATA_O 0x40
#define DATA_P 0x60
#define DATA_SIZE 0x80

// Must agree with the poison avx_fp2_test.cpp writes into ThreadContext64.
#define FP2_POISON(i) ((u8)(0xA5u ^ (unsigned)(i)))

// ---------------------------------------------------------------------------
// Input vectors.
//
// Both 128-bit lanes differ on both sides of every pair, so an implementation
// that derived the upper half from the lower half's operands (the way contract
// C1's two-halves split is most easily broken) cannot pass.  Beyond that each
// pair aims at a specific failure:
//
//   bytes    distinct byte values -- vpshufb, vpminub/vpmaxub, the byte
//            shifts, vpextrb/vpinsrb.  Every byte is unique within its lane so
//            a wrong index is visible as a wrong VALUE, not just wrong data.
//   ctrl     B is a plausible shuffle/permute control: in-range indices out of
//            order, plus bytes with bit 7 SET, which vpshufb must turn into
//            zero rather than into an index.
//   f32nan   NaN payloads, signalling NaNs, infinities, signed zero.
//   f32cvt   values that round differently under nearest-even than under
//            truncation, the exact +/-2^31 endpoints, NaN and huge -- the
//            inputs that must yield x86's 0x80000000 "integer indefinite"
//            rather than AArch64's saturating FCVT result.
//   f64cvt   the same for doubles, plus values that overflow and underflow f32
//            (the only place vcvtsd2ss's rounding is observable).
//   i32bits  INT_MIN / INT_MAX / values needing mantissa rounding -- the
//            source for vcvtdq2pd, and unsigned-vs-signed discriminating
//            operands for vpminud / vpmaxud / vpcmpgtq (0xFFFFFFFF is the
//            LARGEST unsigned and the SMALLEST-but-one signed).
//   signbits sign bits in a non-degenerate pattern, different per half, so
//            vpmovmskb's `lo | hi << 16` combine is distinguishable from
//            `hi | lo << 16` and from either half alone.
//   random   whatever the structured cases are blind to.
//
// The last three exist only for vptest, whose answer is a property of the
// whole register rather than of a lane, so all four (ZF, CF) outcomes need
// four different PAIRS:
//   ptdisjoint  A & B == 0, B & ~A != 0   -> ZF=1 CF=0
//   ptsubset    A & B != 0, B & ~A == 0   -> ZF=0 CF=1
//   ptbzero     B == 0                    -> ZF=1 CF=1
// and `random` supplies the overlapping-but-not-subset case, ZF=0 CF=0.
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    u8 a[32];
    u8 b[32];
} Pair;

static Pair g_pairs[24];
static int g_npairs;

static void put32(u8* p, int i, u32 v) { memcpy(p + i * 4, &v, 4); }
static void put64(u8* p, int i, u64 v) { memcpy(p + i * 8, &v, 8); }
static u32 f32bits(float f) {
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}
static u64 f64bits(double d) {
    u64 v;
    memcpy(&v, &d, 8);
    return v;
}

#define QNAN_A32 0x7FC00111u
#define QNAN_B32 0x7FC00222u
#define SNAN_A32 0x7F800333u
#define INF32 0x7F800000u
#define NINF32 0xFF800000u
#define NZERO32 0x80000000u
#define DEN_MIN32 0x00000001u

#define QNAN_A64 0x7FF8000000000111ull
#define SNAN_A64 0x7FF0000000000333ull
#define INF64 0x7FF0000000000000ull
#define NINF64 0xFFF0000000000000ull
#define NZERO64 0x8000000000000000ull
#define DEN_MIN64 0x0000000000000001ull

static Pair* new_pair(const char* name) {
    Pair* p = &g_pairs[g_npairs++];
    memset(p, 0, sizeof(*p));
    p->name = name;
    return p;
}

static void build_pairs(void) {
    {
        Pair* p = new_pair("bytes");
        for (int i = 0; i < 32; i++) {
            p->a[i] = (u8)(0x10 + i);      // 0x10..0x2F, all distinct
            p->b[i] = (u8)(0xF0 - i * 3);  // distinct, wraps through 0x00
        }
    }
    {
        // B is a shuffle control.  Bytes 3, 9, 20 and 27 have bit 7 set, which
        // VPSHUFB must read as "write zero" rather than as an index; the rest
        // are in-range and out of order, and the two 128-bit lanes use
        // different permutations so a lane-crossing implementation is caught.
        Pair* p = new_pair("ctrl");
        static const u8 lo[16] = {0x0F, 0x00, 0x07, 0x83, 0x02, 0x0C, 0x01, 0x0A,
                                  0x05, 0x9F, 0x0E, 0x03, 0x08, 0x0D, 0x04, 0x0B};
        static const u8 hi[16] = {0x06, 0x0B, 0x00, 0x0F, 0xC1, 0x09, 0x02, 0x0D,
                                  0x04, 0x08, 0x0E, 0xFF, 0x03, 0x0A, 0x01, 0x07};
        for (int i = 0; i < 32; i++) {
            p->a[i] = (u8)(0xA0 ^ (i * 7 + 1));
        }
        memcpy(p->b, lo, 16);
        memcpy(p->b + 16, hi, 16);
    }
    {
        Pair* p = new_pair("f32nan");
        const u32 a[8] = {QNAN_A32,     SNAN_A32,      NZERO32, f32bits(1.5f),
                          f32bits(-0.0f), f32bits(-3.25f), INF32,   NINF32};
        const u32 b[8] = {f32bits(1.0f), QNAN_B32,       INF32,   f32bits(4.0f),
                          f32bits(-5.5f), DEN_MIN32,      NINF32,  f32bits(0.0f)};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("f32cvt");
        const u32 a[8] = {f32bits(0.5f),   f32bits(1.5f),          f32bits(2.5f),
                          f32bits(-2.5f),  f32bits(2147483648.0f), f32bits(-2147483648.0f),
                          QNAN_A32,        f32bits(1e30f)};
        const u32 b[8] = {f32bits(-0.5f),  f32bits(-1.5f),         f32bits(3.5f),
                          f32bits(-3.5f),  f32bits(2147483520.0f), f32bits(-3e30f),
                          INF32,           NINF32};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("f64cvt");
        const u64 a[4] = {f64bits(1e300), f64bits(-1e300), f64bits(1e-300), f64bits(2.5)};
        const u64 b[4] = {f64bits(-0.5), QNAN_A64, SNAN_A64, f64bits(-2.5)};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("f64edge");
        const u64 a[4] = {NZERO64, INF64, NINF64, DEN_MIN64};
        const u64 b[4] = {f64bits(2147483648.0), f64bits(-2147483648.0), f64bits(0.5),
                          f64bits(-1e18)};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("i32bits");
        const u32 a[8] = {0x00000000u, 0x00000001u, 0xFFFFFFFFu, 0x7FFFFFFFu,
                          0x80000000u, 0x01000001u, 0xFF000001u, 0x075BCD15u};
        const u32 b[8] = {0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x0000FFFFu,
                          0xFFFF0000u, 0xAAAAAAAAu, 0x55555555u, 0x12345678u};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("signbits");
        static const u8 a[32] = {0x80, 0x00, 0x7F, 0xFF, 0x01, 0x80, 0x00, 0x88,
                                 0xFF, 0x00, 0x80, 0x00, 0x00, 0xC0, 0x7F, 0x80,
                                 0x00, 0xFF, 0x00, 0x80, 0x90, 0x00, 0xF0, 0x01,
                                 0x80, 0x80, 0x00, 0x00, 0x7F, 0xFF, 0x00, 0x81};
        memcpy(p->a, a, 32);
        for (int i = 0; i < 32; i++) {
            p->b[i] = (u8)(a[31 - i] ^ 0x80);
        }
    }
    {
        Pair* p = new_pair("random");
        u32 s = 0xBADC0DE1u;
        for (int i = 0; i < 32; i++) {
            s = s * 1664525u + 1013904223u;
            p->a[i] = (u8)(s >> 23);
        }
        for (int i = 0; i < 32; i++) {
            s = s * 1664525u + 1013904223u;
            p->b[i] = (u8)(s >> 23);
        }
    }
    {
        // Disjoint: every set bit of B is clear in A and vice versa.
        Pair* p = new_pair("ptdisjoint");
        for (int i = 0; i < 32; i++) {
            p->a[i] = (u8)0x0F;
            p->b[i] = (u8)0xF0;
        }
    }
    {
        // B is a strict non-empty subset of A.
        Pair* p = new_pair("ptsubset");
        for (int i = 0; i < 32; i++) {
            p->a[i] = (u8)(0xFF);
            p->b[i] = (u8)(i == 5 ? 0x10 : 0x00);
        }
        p->b[17] = 0x04;
    }
    {
        // B is all zeros: both (B AND A) and (B AND NOT A) are zero.
        Pair* p = new_pair("ptbzero");
        for (int i = 0; i < 32; i++) {
            p->a[i] = (u8)(0x5A ^ i);
            p->b[i] = 0x00;
        }
    }
}

// ---------------------------------------------------------------------------
// Encoder.  All fields un-inverted; always the 3-byte C4 form so the recorded
// bytes are uniform and easy to disassemble.
// ---------------------------------------------------------------------------
typedef struct {
    u8 b[128];
    int n;
    int mark;  // where the recorded (replayed) portion begins
} Code;

static void emit(Code* c, u8 x) { c->b[c->n++] = x; }

static void vex3(Code* c, int pp, int mmmmm, int vvvv, int l, int r, int x, int bb, int w) {
    emit(c, 0xC4);
    emit(c, (u8)((((~r) & 1) << 7) | (((~x) & 1) << 6) | (((~bb) & 1) << 5) | (mmmmm & 0x1F)));
    emit(c, (u8)(((w & 1) << 7) | (((~vvvv) & 0xF) << 3) | ((l & 1) << 2) | (pp & 3)));
}

// ModRM for [rdi + disp8]; rdi is register 7, which needs no SIB.
static void modrm_mem(Code* c, int reg, int disp8) {
    emit(c, (u8)(0x40 | ((reg & 7) << 3) | 7));
    emit(c, (u8)disp8);
}
static void modrm_reg(Code* c, int reg, int rm) {
    emit(c, (u8)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

static void ld256(Code* c, int reg, int disp) {
    vex3(c, 2, 1, 0, 1, 0, 0, 0, 0);
    emit(c, 0x6F);
    modrm_mem(c, reg, disp);
}
static void st256(Code* c, int reg, int disp) {
    vex3(c, 2, 1, 0, 1, 0, 0, 0, 0);
    emit(c, 0x7F);
    modrm_mem(c, reg, disp);
}
static void prologue(Code* c) {
    ld256(c, 0, DATA_P);
    ld256(c, 1, DATA_A);
    ld256(c, 2, DATA_B);
    c->mark = c->n;  // everything from here is what the test replays
}
static void epilogue(Code* c) {
    emit(c, 0xC5);
    emit(c, 0xF8);
    emit(c, 0x77);  // vzeroupper
    emit(c, 0xC3);  // ret
}
// mov rsi, [rdi+disp]  /  mov esi, [rdi+disp]
static void load_gpr(Code* c, int wide, int disp) {
    if (wide) emit(c, 0x48);
    emit(c, 0x8B);
    modrm_mem(c, 6, disp);
}
// mov eax, 0x7FFFFFFF ; add eax, 1  ->  OF=1 SF=1 AF=1 CF=0 ZF=0 PF=0
static void seed_flags(Code* c) {
    emit(c, 0xB8);
    emit(c, 0xFF);
    emit(c, 0xFF);
    emit(c, 0xFF);
    emit(c, 0x7F);
    emit(c, 0x83);
    emit(c, 0xC0);
    emit(c, 0x01);
}
static void setcc_mem(Code* c, u8 op2, int disp) {
    emit(c, 0x0F);
    emit(c, op2);
    modrm_mem(c, 0, disp);
}
// mov [rdi+DATA_O], rax
static void store_rax(Code* c) {
    emit(c, 0x48);
    emit(c, 0x89);
    modrm_mem(c, 0, DATA_O);
}

// ---------------------------------------------------------------------------
// Execution and SIGILL trapping.
// ---------------------------------------------------------------------------
typedef void (*stub_fn)(u8* data);

static u8* g_page;
static jmp_buf g_jb;
static u8* g_data;
static int g_dump;
static int g_skipped;
static int g_rows;

static void on_sigill(int s) {
    (void)s;
    longjmp(g_jb, 1);
}

static int run_stub(const Code* c) {
    if (mprotect(g_page, 4096, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect rw");
        exit(2);
    }
    memcpy(g_page, c->b, (size_t)c->n);
    if (mprotect(g_page, 4096, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect rx");
        exit(2);
    }
    if (setjmp(g_jb) == 0) {
        ((stub_fn)(void*)g_page)(g_data);
        return 1;
    }
    return 0;
}

static void hexbytes(char* out, const u8* v, int n) {
    for (int i = 0; i < n; i++) {
        sprintf(out + i * 2, "%02x", v[i]);
    }
    out[n * 2] = 0;
}

// Finish `c` for the given form, run it for input pair `pi`, print the row.
static void row(const char* mnemonic, int width, int pi, int form, Code* c) {
    char enc[300];
    hexbytes(enc, c->b + c->mark, c->n - c->mark);
    // The capture tail is appended AFTER the encoding is recorded, so forms 0
    // and 2 do not make the test replay a store it does not need.
    if (form == 0) {
        st256(c, 0, DATA_O);
    } else if (form == 2) {
        store_rax(c);
    }
    epilogue(c);
    if (g_dump) {
        char all[300];
        hexbytes(all, c->b, c->n);
        fprintf(stderr, "%-16s L%-4d pair%-2d form%d  %s   (full %s)\n", mnemonic, width, pi, form,
                enc, all);
        return;
    }
    memcpy(g_data + DATA_A, g_pairs[pi].a, 32);
    memcpy(g_data + DATA_B, g_pairs[pi].b, 32);
    memset(g_data + DATA_O, 0xCC, 32);
    for (int i = 0; i < 32; i++) {
        g_data[DATA_P + i] = FP2_POISON(i);
    }
    if (!run_stub(c)) {
        printf("    // SKIP %s L=%d pair%d(%s) form%d: Rosetta refused this encoding\n", mnemonic,
               width, pi, g_pairs[pi].name, form);
        g_skipped++;
        return;
    }
    char out[80];
    hexbytes(out, g_data + DATA_O, 32);
    printf("    {\"%s\", %d, %d, %d, \"%s\", \"%s\"},\n", mnemonic, width, pi, form, enc, out);
    g_rows++;
}

// ---------------------------------------------------------------------------
// Per-shape emission.
// ---------------------------------------------------------------------------
enum { REG_FORM = 0, MEM_FORM = 1 };

// dst = ymm0, VEX.vvvv = ymm1, r/m = ymm2 or [rdi+B].
static void gen_bin(const char* name, int map, int pp, int w, int op, int aux, int l, int mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, pp, map, 1, l, 0, 0, 0, w);
    emit(&c, (u8)op);
    if (mem) {
        modrm_mem(&c, 0, DATA_B);
    } else {
        modrm_reg(&c, 0, 2);
    }
    if (aux >= 0) emit(&c, (u8)aux);
    row(name, l ? 256 : 128, -1, 0, &c);
}

// dst = ymm0, no vvvv, r/m = ymm1 or [rdi+A].
static void gen_un(const char* name, int map, int pp, int w, int op, int aux, int l, int mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, pp, map, 0, l, 0, 0, 0, w);
    emit(&c, (u8)op);
    if (mem) {
        modrm_mem(&c, 0, DATA_A);
    } else {
        modrm_reg(&c, 0, 1);
    }
    if (aux >= 0) emit(&c, (u8)aux);
    row(name, l ? 256 : 128, -1, 0, &c);
}

static int g_pair;  // the pair `row` should use; set by the driver loop

int main(int argc, char** argv) {
    g_dump = (argc > 1 && strcmp(argv[1], "--dump-encodings") == 0);
    signal(SIGILL, on_sigill);
    signal(SIGSEGV, on_sigill);
    signal(SIGBUS, on_sigill);
    signal(SIGTRAP, on_sigill);
    signal(SIGFPE, on_sigill);
    build_pairs();

    g_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_page == MAP_FAILED) {
        perror("mmap code");
        return 2;
    }
    u8* raw = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) {
        perror("mmap data");
        return 2;
    }
    g_data = raw;

    // --- capability gate: prove 256-bit execution before emitting anything --
    {
        Code c;
        c.n = 0;
        prologue(&c);
        vex3(&c, 0, 1, 1, 1, 0, 0, 0, 0);  // vaddps ymm0, ymm1, ymm2
        emit(&c, 0x58);
        modrm_reg(&c, 0, 2);
        st256(&c, 0, DATA_O);
        epilogue(&c);
        memcpy(g_data + DATA_A, g_pairs[0].a, 32);
        memcpy(g_data + DATA_B, g_pairs[0].b, 32);
        if (!run_stub(&c)) {
            fprintf(stderr,
                    "FATAL: VEX.256 vaddps raised SIGILL under this runtime.\n"
                    "Rosetta on this machine cannot serve as an AVX oracle;\n"
                    "no reference data was generated.\n");
            return 1;
        }
    }

    u32 cpuid1[4] = {0}, cpuid7[4] = {0};
    __asm__ volatile("cpuid"
                     : "=a"(cpuid1[0]), "=b"(cpuid1[1]), "=c"(cpuid1[2]), "=d"(cpuid1[3])
                     : "a"(1), "c"(0));
    __asm__ volatile("cpuid"
                     : "=a"(cpuid7[0]), "=b"(cpuid7[1]), "=c"(cpuid7[2]), "=d"(cpuid7[3])
                     : "a"(7), "c"(0));

    if (!g_dump) {
        printf("// GENERATED FILE -- DO NOT EDIT BY HAND.\n");
        printf("//\n");
        printf("// Reference values for the SECOND-WAVE VEX family (scalar moves, scalar\n");
        printf("// conversions, shuffles, lane moves, the VEX.128 twins), produced by\n");
        printf("// ACTUALLY EXECUTING each encoding on x86-64 through Rosetta 2.  Nothing\n");
        printf("// here is hand-computed; an encoding Rosetta refused appears as a SKIP\n");
        printf("// comment instead of a value.\n");
        printf("//\n");
        printf("// Regenerate with the recipe in avx_fp2_rosetta_ref.c:\n");
        printf("//   clang -arch x86_64 -O1 -o /tmp/avxfp2ref avx_fp2_rosetta_ref.c\n");
        printf("//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxfp2ref > "
               "avx_fp2_rosetta_ref.inc\n");
        printf("//\n");
        printf("// Generating runtime: CPUID.1.ECX=%08x EDX=%08x, CPUID.7.0.EBX=%08x\n", cpuid1[2],
               cpuid1[3], cpuid7[1]);
        printf("//   (AVX bit=%d AVX2 bit=%d OSXSAVE bit=%d -- Rosetta hides these unless\n",
               (cpuid1[2] >> 28) & 1, (cpuid7[1] >> 5) & 1, (cpuid1[2] >> 27) & 1);
        printf("//    ROSETTA_ADVERTISE_AVX=1 is set; execution works regardless, which is\n");
        printf("//    why the gate above is a live vaddps and not a CPUID read.)\n");
        printf("\n");
        printf("// Input vectors: name, A (32 bytes), B (32 bytes).\n");
        printf("static const AvxFp2Input kAvxFp2Inputs[] = {\n");
        for (int i = 0; i < g_npairs; i++) {
            char ha[80], hb[80];
            hexbytes(ha, g_pairs[i].a, 32);
            hexbytes(hb, g_pairs[i].b, 32);
            printf("    {\"%s\", \"%s\", \"%s\"},\n", g_pairs[i].name, ha, hb);
        }
        printf("};\n\n");
        printf("// Results: mnemonic, VEX width (128/256), input-pair index, FORM, the\n");
        printf("// literal instruction bytes executed, and the 32 bytes of the answer.\n");
        printf("//   form 0: the answer is ymm0 (poisoned beforehand, so the zeroed upper\n");
        printf("//           half of a VEX.128 result is MEASURED here, not assumed).\n");
        printf("//   form 1: the answer is the 32-byte capture slot; the recorded bytes\n");
        printf("//           include whatever wrote it (a store, or five SETcc).\n");
        printf("//   form 2: the answer is rax, stored as eight little-endian bytes.\n");
        printf("// ymm1 held A and ymm2 held B; rdi pointed at the data block, whose\n");
        printf("// layout is A=+0x00 B=+0x20 capture=+0x40 poison=+0x60.\n");
        printf("static const AvxFp2Ref kAvxFp2Refs[] = {\n");
    }

    for (g_pair = 0; g_pair < g_npairs; g_pair++) {
        const int pi = g_pair;
#define ROW(name, width, form, code)     \
    do {                                 \
        Code c;                          \
        c.n = 0;                         \
        prologue(&c);                    \
        code;                            \
        row(name, width, pi, form, &c);  \
    } while (0)

#define SVM_FP2(name, shape, map, pp, w, opcode, aux) gen_##shape(#name, map, pp, w, opcode, aux, pi);
#define gen_S_BIN(nm, map, pp, w, op, aux, pi)                                 \
    for (int l = 0; l < 2; l++)                                                \
        for (int mem = 0; mem < 2; mem++) {                                    \
            Code c;                                                            \
            c.n = 0;                                                           \
            prologue(&c);                                                      \
            vex3(&c, pp, map, 1, l, 0, 0, 0, w);                               \
            emit(&c, (u8)(op));                                                \
            if (mem)                                                           \
                modrm_mem(&c, 0, DATA_B);                                      \
            else                                                               \
                modrm_reg(&c, 0, 2);                                           \
            if ((aux) >= 0) emit(&c, (u8)(aux));                               \
            row(nm, l ? 256 : 128, pi, 0, &c);                                 \
        }
#define gen_S_BIN256(nm, map, pp, w, op, aux, pi)                              \
    for (int mem = 0; mem < 2; mem++) {                                        \
        Code c;                                                                \
        c.n = 0;                                                               \
        prologue(&c);                                                          \
        vex3(&c, pp, map, 1, 1, 0, 0, 0, w);                                   \
        emit(&c, (u8)(op));                                                    \
        if (mem)                                                               \
            modrm_mem(&c, 0, DATA_B);                                          \
        else                                                                   \
            modrm_reg(&c, 0, 2);                                               \
        if ((aux) >= 0) emit(&c, (u8)(aux));                                   \
        row(nm, 256, pi, 0, &c);                                               \
    }
#define gen_S_BINR(nm, map, pp, w, op, aux, pi)     \
    {                                               \
        Code c;                                     \
        c.n = 0;                                    \
        prologue(&c);                               \
        vex3(&c, pp, map, 1, 0, 0, 0, 0, w);        \
        emit(&c, (u8)(op));                         \
        modrm_reg(&c, 0, 2);                        \
        if ((aux) >= 0) emit(&c, (u8)(aux));        \
        row(nm, 128, pi, 0, &c);                    \
    }
#define gen_S_UN(nm, map, pp, w, op, aux, pi)                                  \
    for (int l = 0; l < 2; l++)                                                \
        for (int mem = 0; mem < 2; mem++) {                                    \
            Code c;                                                            \
            c.n = 0;                                                           \
            prologue(&c);                                                      \
            vex3(&c, pp, map, 0, l, 0, 0, 0, w);                               \
            emit(&c, (u8)(op));                                                \
            if (mem)                                                           \
                modrm_mem(&c, 0, DATA_A);                                      \
            else                                                               \
                modrm_reg(&c, 0, 1);                                           \
            if ((aux) >= 0) emit(&c, (u8)(aux));                               \
            row(nm, l ? 256 : 128, pi, 0, &c);                                 \
        }
#define gen_S_UNM(nm, map, pp, w, op, aux, pi)      \
    {                                               \
        Code c;                                     \
        c.n = 0;                                    \
        prologue(&c);                               \
        vex3(&c, pp, map, 0, 1, 0, 0, 0, w);        \
        emit(&c, (u8)(op));                         \
        modrm_mem(&c, 0, DATA_A);                   \
        if ((aux) >= 0) emit(&c, (u8)(aux));        \
        row(nm, 256, pi, 0, &c);                    \
    }
// vextractf128: ModRM.reg is the SOURCE (ymm1) and r/m the DESTINATION -- so
// the register form writes xmm0 (read back as form 0) and the memory form
// writes the capture slot (form 1).
#define gen_S_REV(nm, map, pp, w, op, aux, pi)      \
    for (int mem = 0; mem < 2; mem++) {             \
        Code c;                                     \
        c.n = 0;                                    \
        prologue(&c);                               \
        vex3(&c, pp, map, 0, 1, 0, 0, 0, w);        \
        emit(&c, (u8)(op));                         \
        if (mem)                                    \
            modrm_mem(&c, 1, DATA_O);               \
        else                                        \
            modrm_reg(&c, 1, 0);                    \
        emit(&c, (u8)(aux));                        \
        row(nm, 256, pi, mem ? 1 : 0, &c);          \
    }
// vinsertf128 ymm0, ymm1, xmm2/[rdi+B], imm8.
#define gen_S_INS128(nm, map, pp, w, op, aux, pi)   \
    for (int mem = 0; mem < 2; mem++) {             \
        Code c;                                     \
        c.n = 0;                                    \
        prologue(&c);                               \
        vex3(&c, pp, map, 1, 1, 0, 0, 0, w);        \
        emit(&c, (u8)(op));                         \
        if (mem)                                    \
            modrm_mem(&c, 0, DATA_B);               \
        else                                        \
            modrm_reg(&c, 0, 2);                    \
        emit(&c, (u8)(aux));                        \
        row(nm, 256, pi, 0, &c);                    \
    }
// vmovss / vmovsd, all four forms.  `aux` is the lane width, used only for the
// row's name suffix; the encoding is the same for both.
#define gen_S_MOVS(nm, map, pp, w, op, aux, pi)                        \
    {                                                                  \
        /* 10 /r reg-reg: dst = ymm0, src1 = ymm1, src2 = ymm2 */      \
        Code c;                                                        \
        c.n = 0;                                                       \
        prologue(&c);                                                  \
        vex3(&c, pp, map, 1, 0, 0, 0, 0, w);                           \
        emit(&c, 0x10);                                                \
        modrm_reg(&c, 0, 2);                                           \
        row(nm ".rr10", 128, pi, 0, &c);                               \
    }                                                                  \
    {                                                                  \
        /* 11 /r reg-reg: DESTINATION is r/m, source is ModRM.reg */   \
        Code c;                                                        \
        c.n = 0;                                                       \
        prologue(&c);                                                  \
        vex3(&c, pp, map, 1, 0, 0, 0, 0, w);                           \
        emit(&c, 0x11);                                                \
        modrm_reg(&c, 2, 0);                                           \
        row(nm ".rr11", 128, pi, 0, &c);                               \
    }                                                                  \
    {                                                                  \
        /* 10 /r load: no merge source at all, bits 255:lane zeroed */ \
        Code c;                                                        \
        c.n = 0;                                                       \
        prologue(&c);                                                  \
        vex3(&c, pp, map, 0, 0, 0, 0, 0, w);                           \
        emit(&c, 0x10);                                                \
        modrm_mem(&c, 0, DATA_A + 4);                                  \
        row(nm ".load", 128, pi, 0, &c);                               \
    }                                                                  \
    {                                                                  \
        /* 11 /r store: no register written */                         \
        Code c;                                                        \
        c.n = 0;                                                       \
        prologue(&c);                                                  \
        vex3(&c, pp, map, 0, 0, 0, 0, 0, w);                           \
        emit(&c, 0x11);                                                \
        modrm_mem(&c, 1, DATA_O + 8);                                  \
        row(nm ".store", 128, pi, 1, &c);                              \
    }
// vmovlps / vmovhps / vmovlpd / vmovhpd: 64-bit load merged into one half, and
// the matching 64-bit store.  `op` is 0x12 (low) or 0x16 (high); the store
// opcode is op + 1.
#define gen_S_MOVLH(nm, map, pp, w, op, aux, pi)  \
    {                                             \
        Code c;                                   \
        c.n = 0;                                  \
        prologue(&c);                             \
        vex3(&c, pp, map, 1, 0, 0, 0, 0, w);      \
        emit(&c, (u8)(op));                       \
        modrm_mem(&c, 0, DATA_B + 8);             \
        row(nm ".load", 128, pi, 0, &c);          \
    }                                             \
    {                                             \
        Code c;                                   \
        c.n = 0;                                  \
        prologue(&c);                             \
        vex3(&c, pp, map, 0, 0, 0, 0, 0, w);      \
        emit(&c, (u8)((op) + 1));                 \
        modrm_mem(&c, 1, DATA_O + 8);             \
        row(nm ".store", 128, pi, 1, &c);         \
    }
// vcvtsi2ss / vcvtsi2sd.  The integer comes from memory through rsi so the
// recorded bytes are self-contained (the test replays the load too).
#define gen_S_SI2F(nm, map, pp, w, op, aux, pi)          \
    for (int k = 0; k < 4; k++) {                        \
        Code c;                                          \
        c.n = 0;                                         \
        prologue(&c);                                    \
        load_gpr(&c, 1, DATA_A + k * 8);                 \
        vex3(&c, pp, map, 1, 0, 0, 0, 0, w);             \
        emit(&c, (u8)(op));                              \
        modrm_reg(&c, 0, 6); /* rsi / esi */             \
        row(nm, 128, pi, 0, &c);                         \
    }                                                    \
    {                                                    \
        Code c;                                          \
        c.n = 0;                                         \
        prologue(&c);                                    \
        vex3(&c, pp, map, 1, 0, 0, 0, 0, w);             \
        emit(&c, (u8)(op));                              \
        modrm_mem(&c, 0, DATA_A + 8);                    \
        row(nm ".m", 128, pi, 0, &c);                    \
    }
// vcvt*2si: destination is rax / eax, source is xmm1 or [rdi+A+k*8].
#define gen_S_F2SI(nm, map, pp, w, op, aux, pi)          \
    {                                                    \
        Code c;                                          \
        c.n = 0;                                         \
        prologue(&c);                                    \
        emit(&c, 0x48);                                  \
        emit(&c, 0xC7);                                  \
        modrm_reg(&c, 0, 0);                             \
        emit(&c, 0xFF);                                  \
        emit(&c, 0xFF);                                  \
        emit(&c, 0xFF);                                  \
        emit(&c, 0xFF); /* mov rax, -1 */                \
        vex3(&c, pp, map, 0, 0, 0, 0, 0, w);             \
        emit(&c, (u8)(op));                              \
        modrm_reg(&c, 0, 1);                             \
        row(nm, 128, pi, 2, &c);                         \
    }                                                    \
    for (int k = 0; k < 3; k++) {                        \
        Code c;                                          \
        c.n = 0;                                         \
        prologue(&c);                                    \
        emit(&c, 0x48);                                  \
        emit(&c, 0xC7);                                  \
        modrm_reg(&c, 0, 0);                             \
        emit(&c, 0xFF);                                  \
        emit(&c, 0xFF);                                  \
        emit(&c, 0xFF);                                  \
        emit(&c, 0xFF);                                  \
        vex3(&c, pp, map, 0, 0, 0, 0, 0, w);             \
        emit(&c, (u8)(op));                              \
        modrm_mem(&c, 0, DATA_A + k * 8);                \
        row(nm ".m", 128, pi, 2, &c);                    \
    }
// vpmovmskb eax, xmm1/ymm1.  ModRM.reg is the GPR and r/m the vector, so the
// two register NUMBERS differ -- the case a decoder confusing reg with rm gets
// wrong.  rax is pre-set to -1 so the 32-bit destination's zero-extension is
// visible.
#define gen_S_MSK(nm, map, pp, w, op, aux, pi)           \
    for (int l = 0; l < 2; l++) {                        \
        Code c;                                          \
        c.n = 0;                                         \
        prologue(&c);                                    \
        emit(&c, 0x48);                                  \
        emit(&c, 0xC7);                                  \
        modrm_reg(&c, 0, 0);                             \
        emit(&c, 0xFF);                                  \
        emit(&c, 0xFF);                                  \
        emit(&c, 0xFF);                                  \
        emit(&c, 0xFF);                                  \
        vex3(&c, pp, map, 0, l, 0, 0, 0, w);             \
        emit(&c, (u8)(op));                              \
        modrm_reg(&c, 0, 1);                             \
        row(nm, l ? 256 : 128, pi, 2, &c);               \
    }
// vpsrldq / vpslldq: NDD, so the destination is VEX.vvvv (ymm0) and the source
// is ModRM.rm (ymm1).  Counts cover both ends and past the end.
#define gen_S_NDD(nm, map, pp, w, op, aux, pi)                          \
    {                                                                   \
        static const int counts[] = {0, 1, 7, 8, 15, 16, 17};           \
        for (int l = 0; l < 2; l++)                                     \
            for (unsigned ci = 0; ci < sizeof(counts) / sizeof(int); ci++) { \
                Code c;                                                 \
                c.n = 0;                                                \
                prologue(&c);                                           \
                vex3(&c, pp, map, 0, l, 0, 0, 0, w);                    \
                emit(&c, (u8)(op));                                     \
                modrm_reg(&c, (aux), 1);                                \
                emit(&c, (u8)counts[ci]);                               \
                row(nm, l ? 256 : 128, pi, 0, &c);                      \
            }                                                           \
    }
// vptest: EFLAGS captured with SETcc, at the same offsets and in the same
// order the test expects (CF, PF, ZF, OF, SF).  The flags are SEEDED first so
// the architectural CLEARING of OF/SF/PF is observable rather than a no-op.
#define gen_S_TEST(nm, map, pp, w, op, aux, pi)          \
    for (int l = 0; l < 2; l++)                          \
        for (int mem = 0; mem < 2; mem++) {              \
            Code c;                                      \
            c.n = 0;                                     \
            prologue(&c);                                \
            seed_flags(&c);                              \
            vex3(&c, pp, map, 0, l, 0, 0, 0, w);         \
            emit(&c, (u8)(op));                          \
            if (mem)                                     \
                modrm_mem(&c, 1, DATA_B);                \
            else                                         \
                modrm_reg(&c, 1, 2);                     \
            setcc_mem(&c, 0x92, DATA_O + 0);             \
            setcc_mem(&c, 0x9A, DATA_O + 1);             \
            setcc_mem(&c, 0x94, DATA_O + 2);             \
            setcc_mem(&c, 0x90, DATA_O + 3);             \
            setcc_mem(&c, 0x98, DATA_O + 4);             \
            row(nm, l ? 256 : 128, pi, 1, &c);           \
        }
// vpextr*: `aux` is the element width.  Both the GPR destination (form 2) and
// the memory destination (form 1) are exercised, over several element indexes.
#define gen_S_EXTR(nm, map, pp, w, op, aux, pi)                          \
    {                                                                    \
        const int count = 128 / (aux);                                   \
        for (int idx = 0; idx < count; idx += (count > 4 ? count / 4 : 1)) { \
            {                                                            \
                Code c;                                                  \
                c.n = 0;                                                 \
                prologue(&c);                                            \
                emit(&c, 0x48);                                          \
                emit(&c, 0xC7);                                          \
                modrm_reg(&c, 0, 0);                                     \
                emit(&c, 0xFF);                                          \
                emit(&c, 0xFF);                                          \
                emit(&c, 0xFF);                                          \
                emit(&c, 0xFF);                                          \
                vex3(&c, pp, map, 0, 0, 0, 0, 0, w);                     \
                emit(&c, (u8)(op));                                      \
                /* vpextrw (0F C5) has reg = GPR, rm = vector; the 0F3A   \
                   forms have reg = vector, rm = GPR. */                  \
                if ((map) == 1)                                          \
                    modrm_reg(&c, 0, 1);                                 \
                else                                                     \
                    modrm_reg(&c, 1, 0);                                 \
                emit(&c, (u8)idx);                                       \
                row(nm, 128, pi, 2, &c);                                 \
            }                                                            \
            if ((map) != 1) {                                            \
                Code c;                                                  \
                c.n = 0;                                                 \
                prologue(&c);                                            \
                vex3(&c, pp, map, 0, 0, 0, 0, 0, w);                     \
                emit(&c, (u8)(op));                                      \
                modrm_mem(&c, 1, DATA_O + 4);                            \
                emit(&c, (u8)idx);                                       \
                row(nm ".m", 128, pi, 1, &c);                            \
            }                                                            \
        }                                                                \
    }
// vpinsr*: source is rsi (loaded from A) or memory.
#define gen_S_INSR(nm, map, pp, w, op, aux, pi)                          \
    {                                                                    \
        const int count = 128 / (aux);                                   \
        for (int idx = 0; idx < count; idx += (count > 4 ? count / 4 : 1)) { \
            {                                                            \
                Code c;                                                  \
                c.n = 0;                                                 \
                prologue(&c);                                            \
                load_gpr(&c, 1, DATA_B + 8);                             \
                vex3(&c, pp, map, 1, 0, 0, 0, 0, w);                     \
                emit(&c, (u8)(op));                                      \
                modrm_reg(&c, 0, 6);                                     \
                emit(&c, (u8)idx);                                       \
                row(nm, 128, pi, 0, &c);                                 \
            }                                                            \
            {                                                            \
                Code c;                                                  \
                c.n = 0;                                                 \
                prologue(&c);                                            \
                vex3(&c, pp, map, 1, 0, 0, 0, 0, w);                     \
                emit(&c, (u8)(op));                                      \
                modrm_mem(&c, 0, DATA_B + 4);                            \
                emit(&c, (u8)idx);                                       \
                row(nm ".m", 128, pi, 0, &c);                            \
            }                                                            \
        }                                                                \
    }

#include "avx_fp2_ops.inc"

#undef gen_S_BIN
#undef gen_S_BIN256
#undef gen_S_BINR
#undef gen_S_UN
#undef gen_S_UNM
#undef gen_S_REV
#undef gen_S_INS128
#undef gen_S_MOVS
#undef gen_S_MOVLH
#undef gen_S_SI2F
#undef gen_S_F2SI
#undef gen_S_MSK
#undef gen_S_NDD
#undef gen_S_TEST
#undef gen_S_EXTR
#undef gen_S_INSR
#undef ROW
    }

    if (!g_dump) {
        printf("};\n");
        fprintf(stderr, "%d rows emitted, %d skipped\n", g_rows, g_skipped);
        if (g_skipped) {
            fprintf(stderr, "warning: %d combinations were SKIPped by Rosetta\n", g_skipped);
        }
    }
    (void)gen_bin;
    (void)gen_un;
    return 0;
}
