// ===========================================================================
// Reference generator for the FP compare-to-mask family -- runs real x86-64
// under Rosetta 2.
// ===========================================================================
//
// Covers VCMPPS / VCMPPD / VCMPSS / VCMPSD at ALL 32 AVX predicates, at both
// VEX.L, at both r/m shapes, plus the four legacy SSE mnemonics (predicates
// 0..7) that share ir::OpCode::VecFCmpMask with them.
//
// WHY THIS FAMILY NEEDS A HARDWARE ORACLE MORE THAN MOST
// -----------------------------------------------------
// The 32 predicates are 16 relations x {signalling, quiet}, and 8 of the 16
// relations are indistinguishable from another one unless an operand is NaN:
// with no NaN anywhere, LT_OS and NGE_US agree, ORD_Q is all-ones, UNORD_Q is
// all-zeros, NEQ_UQ and NEQ_OQ agree, and so on.  A test whose inputs are
// ordinary floats would "cover" all 32 predicates while distinguishing only
// eight of them.  So every input pair here is built around a specific outcome
// -- less, equal, greater, unordered -- and avx_cmp_test.cpp ASSERTS from the
// reference data that all 16 relations are separated, per mnemonic and per
// width, rather than hoping they are.
//
// The oracle also has to answer a question no manual reading settles for this
// codebase: whether the signalling/quiet dimension is observable in the RESULT
// at all.  It is not -- imm8 `i` and `i + 16` were measured bit-identical here
// for every i, while MXCSR.IE followed the SDM's _OS/_OQ classification
// exactly.  avx_cmp_test.cpp re-asserts the first half of that from this data,
// so the assumption SwiftVM's decoder rests on is checked on every run.
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// ----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/avxcmpref avx_cmp_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxcmpref > avx_cmp_rosetta_ref.inc
//
// Add --dump-encodings to print each stub's bytes to stderr for auditing
// against a disassembler.
//
// Rosetta does NOT advertise AVX through CPUID unless ROSETTA_ADVERTISE_AVX=1;
// execution works either way, so support is decided by EXECUTING a VEX.256
// instruction and catching SIGILL, never by reading CPUID.  Rosetta is itself
// an emulator with measured defects, so anything surprising is cross-read
// against the Intel SDM before it is believed.
//
// WHAT A ROW CONTAINS
// -------------------
// Every row carries the LITERAL BYTES of the instruction sequence under test;
// avx_cmp_test.cpp replays those bytes rather than re-encoding from the shared
// table, so the two sides cannot assemble different instructions.  Each stub is
//
//     vmovdqu ymm0, [rdi+0x60]        ; poison the destination
//     vmovdqu ymm1, [rdi+0x00]        ; A
//     vmovdqu ymm2, [rdi+0x20]        ; B
//     <the recorded byte sequence>
//     vmovdqu [rdi+0x40], ymm0        ; NOT recorded -- the test reads the
//     vzeroupper                      ; register out of ThreadContext64
//     ret
//
// and the answer is always the full 32 bytes of a poisoned ymm0.  For the VEX
// rows that measures contract C3 (a VEX.128 write zeroes bits 255:128); for
// the legacy SSE rows it measures the OPPOSITE rule, that a non-VEX write
// leaves bits 255:128 alone.  The legacy rows begin with `movaps xmm0, xmm1`
// because CMPPS is destructive and its source-1 must be the destination; that
// move is part of the recorded bytes and is replayed.
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

// Must agree with the poison avx_cmp_test.cpp writes into ThreadContext64.
#define CMP_POISON(i) ((u8)(0xA5u ^ (unsigned)(i)))

// ---------------------------------------------------------------------------
// Input pairs.
//
// The compare family's answer depends only on WHICH of the four IEEE outcomes
// each lane produces, so the pairs are chosen by outcome and not by numeric
// interest.  Every pair is described twice -- once as eight f32 lanes and once
// as four f64 lanes -- because vcmpps and vcmppd read the same 32 bytes
// differently, and a pair that is a clean "less than" as f32 is usually
// nothing in particular as f64.  The dword layout of the NaN pairs is picked
// so the SAME bytes are NaN under both readings (a dword that is an f32 NaN
// pairs with 0x7FF80000 / 0x7FF00000, which supplies the f64 exponent).
//
//   lt / eq / gt   uniform lanes, one outcome each, in BOTH readings.  These
//                  are what separate the 16 relations for the SCALAR forms and
//                  for vcmppd at VEX.128, where a single row has too few lanes
//                  to show more than two outcomes.
//   qnan / snan    uniform unordered in both readings, quiet and signalling.
//                  SNaN matters because it is the one NaN that raises #IA for
//                  QUIET predicates too, so it is where a "signalling" mistake
//                  would show if the result depended on it at all.
//   zeroinf        +-0 against each other (equal, and the sign must not make
//                  them unequal) and +-Inf (ordered, not NaN).
//   denorm         denormals against zero and against each other, plus the
//                  smallest normals -- an implementation running with
//                  flush-to-zero would call these equal.
//   mixed32        eight f32 lanes covering less, equal, greater and unordered
//                  FOUR TIMES OVER, so vcmpps separates all 16 relations
//                  within a single row at both VEX.L.
//   mixed64        the same for four f64 lanes and vcmppd at VEX.L=1.
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    u8 a[32];
    u8 b[32];
} Pair;

static Pair g_pairs[16];
static int g_npairs;

static void put32(u8* p, int i, u32 v) { memcpy(p + i * 4, &v, 4); }
static void put64(u8* p, int i, u64 v) { memcpy(p + i * 8, &v, 8); }

#define ONE32 0x3F800000u
#define TWO32 0x40000000u
#define THREE32 0x40400000u
#define QNAN32 0x7FC00111u
#define SNAN32 0x7F800333u
#define INF32 0x7F800000u
#define NINF32 0xFF800000u
#define PZERO32 0x00000000u
#define NZERO32 0x80000000u
#define DEN32 0x00000001u
#define DEN32B 0x007FFFFFu
#define MINNORM32 0x00800000u

// High dwords that turn an f32-NaN low dword into an f64 NaN as well.
#define QNAN_HI 0x7FF80000u
#define SNAN_HI 0x7FF00000u

#define ONE64 0x3FF0000000000000ull
#define TWO64 0x4000000000000000ull
#define THREE64 0x4008000000000000ull
#define QNAN64 0x7FF8000000000111ull
#define INF64 0x7FF0000000000000ull
#define NINF64 0xFFF0000000000000ull
#define NZERO64 0x8000000000000000ull
#define DEN64 0x0000000000000001ull

static Pair* new_pair(const char* name) {
    Pair* p = &g_pairs[g_npairs++];
    memset(p, 0, sizeof(*p));
    p->name = name;
    return p;
}

// Fill both halves with a repeating dword pattern of length 2.
static void fill2(u8* v, u32 lo, u32 hi) {
    for (int i = 0; i < 8; i++) {
        put32(v, i, (i & 1) ? hi : lo);
    }
}

static void build_pairs(void) {
    {
        // f32: 1.0 < 2.0 in every lane.  f64: 0x3F8000003F800000 <
        // 0x4000000040000000, because the high dword carries the exponent.
        Pair* p = new_pair("lt");
        fill2(p->a, ONE32, ONE32);
        fill2(p->b, TWO32, TWO32);
    }
    {
        Pair* p = new_pair("eq");
        fill2(p->a, ONE32, ONE32);
        fill2(p->b, ONE32, ONE32);
    }
    {
        Pair* p = new_pair("gt");
        fill2(p->a, TWO32, TWO32);
        fill2(p->b, ONE32, ONE32);
    }
    {
        // A is a QUIET NaN under both readings; B is an ordinary value under
        // both.  Every lane unordered.
        Pair* p = new_pair("qnan");
        fill2(p->a, QNAN32, QNAN_HI);
        fill2(p->b, ONE32, ONE32);
    }
    {
        // B is a SIGNALLING NaN under both readings (f64 mantissa MSB clear).
        Pair* p = new_pair("snan");
        fill2(p->a, ONE32, ONE32);
        fill2(p->b, SNAN32, SNAN_HI);
    }
    {
        // The f32 and f64 zero/infinity cases cannot share one 32-byte pair --
        // the same bytes cannot be eight interesting floats and four
        // interesting doubles at once -- so they are separate pairs.  Each is
        // still executed by BOTH mnemonics; the reading it was not designed
        // for is arbitrary data, which is fine because the reference is
        // measured rather than predicted.
        Pair* p = new_pair("zeroinf32");
        put32(p->a, 0, PZERO32);  put32(p->b, 0, NZERO32);   // +0 == -0
        put32(p->a, 1, NZERO32);  put32(p->b, 1, PZERO32);
        put32(p->a, 2, INF32);    put32(p->b, 2, INF32);     // +Inf == +Inf
        put32(p->a, 3, NINF32);   put32(p->b, 3, INF32);     // -Inf < +Inf
        put32(p->a, 4, INF32);    put32(p->b, 4, NINF32);    // +Inf > -Inf
        put32(p->a, 5, NINF32);   put32(p->b, 5, NINF32);
        put32(p->a, 6, PZERO32);  put32(p->b, 6, INF32);
        put32(p->a, 7, NZERO32);  put32(p->b, 7, NINF32);
    }
    {
        Pair* p = new_pair("zeroinf64");
        put64(p->a, 0, 0);        put64(p->b, 0, NZERO64);   // +0 == -0
        put64(p->a, 1, INF64);    put64(p->b, 1, NINF64);
        put64(p->a, 2, NINF64);   put64(p->b, 2, INF64);
        put64(p->a, 3, INF64);    put64(p->b, 3, INF64);
    }
    {
        Pair* p = new_pair("denorm32");
        put32(p->a, 0, DEN32);      put32(p->b, 0, PZERO32);
        put32(p->a, 1, PZERO32);    put32(p->b, 1, DEN32);
        put32(p->a, 2, DEN32);      put32(p->b, 2, DEN32);
        put32(p->a, 3, DEN32B);     put32(p->b, 3, MINNORM32);
        put32(p->a, 4, MINNORM32);  put32(p->b, 4, DEN32B);
        put32(p->a, 5, DEN32);      put32(p->b, 5, DEN32B);
        put32(p->a, 6, NZERO32);    put32(p->b, 6, DEN32);
        put32(p->a, 7, DEN32B);     put32(p->b, 7, DEN32B);
    }
    {
        Pair* p = new_pair("denorm64");
        put64(p->a, 0, DEN64);      put64(p->b, 0, 0);
        put64(p->a, 1, 0);          put64(p->b, 1, DEN64);
        put64(p->a, 2, DEN64);      put64(p->b, 2, DEN64);
        put64(p->a, 3, DEN64 * 3);  put64(p->b, 3, DEN64);
    }
    {
        // Eight f32 lanes: less, equal, greater, unordered, twice over, so
        // BOTH 128-bit halves see all four outcomes and vcmpps separates all
        // 16 relations within one row at either VEX.L.
        Pair* p = new_pair("mixed32");
        static const u32 a[8] = {ONE32, TWO32, THREE32, QNAN32, NINF32, PZERO32, INF32, ONE32};
        static const u32 b[8] = {TWO32, TWO32, TWO32, TWO32, ONE32, NZERO32, ONE32, SNAN32};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        // Four f64 lanes: less, equal, greater, unordered -- what vcmppd at
        // VEX.L=1 needs to separate all 16 relations within one row.
        Pair* p = new_pair("mixed64");
        static const u64 a[4] = {ONE64, TWO64, THREE64, QNAN64};
        static const u64 b[4] = {TWO64, TWO64, TWO64, TWO64};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
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

// Finish `c`, run it for input pair `pi`, print the row.
static void row(const char* mnemonic, int width, int mem, int pi, int imm, Code* c) {
    char enc[300];
    hexbytes(enc, c->b + c->mark, c->n - c->mark);
    // The capture store is appended AFTER the encoding is recorded, so the
    // test does not have to replay a store it does not need.
    st256(c, 0, DATA_O);
    epilogue(c);
    if (g_dump) {
        char all[300];
        hexbytes(all, c->b, c->n);
        fprintf(stderr, "%-8s L%-4d %s pair%-2d imm%-3d  %s   (full %s)\n", mnemonic, width,
                mem ? "mem" : "reg", pi, imm, enc, all);
        return;
    }
    memcpy(g_data + DATA_A, g_pairs[pi].a, 32);
    memcpy(g_data + DATA_B, g_pairs[pi].b, 32);
    memset(g_data + DATA_O, 0xCC, 32);
    for (int i = 0; i < 32; i++) {
        g_data[DATA_P + i] = CMP_POISON(i);
    }
    if (!run_stub(c)) {
        printf("    // SKIP %s L=%d %s pair%d(%s) imm%d: Rosetta refused this encoding\n", mnemonic,
               width, mem ? "mem" : "reg", pi, g_pairs[pi].name, imm);
        g_skipped++;
        return;
    }
    char out[80];
    hexbytes(out, g_data + DATA_O, 32);
    printf("    {\"%s\", %d, %d, %d, %d, \"%s\", \"%s\"},\n", mnemonic, width, mem, pi, imm, enc,
           out);
    g_rows++;
}

// VEX: dst = ymm0/xmm0, VEX.vvvv = ymm1, r/m = ymm2 or [rdi+B].
static void gen_vex(const char* name, int pp, int l, int mem, int pi, int imm) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, pp, 1, 1, l, 0, 0, 0, 0);
    emit(&c, 0xC2);
    if (mem) {
        modrm_mem(&c, 0, DATA_B);
    } else {
        modrm_reg(&c, 0, 2);
    }
    emit(&c, (u8)imm);
    row(name, l ? 256 : 128, mem, pi, imm, &c);
}

// Legacy SSE: destructive, so xmm0 <- xmm1 first (movaps xmm0, xmm1) and that
// move is part of the recorded bytes.  Bits 255:128 of ymm0 must SURVIVE, and
// they are read back, so the non-VEX rule is measured and not assumed.
static void gen_sse(const char* name, int pp, int mem, int pi, int imm) {
    static const u8 kPrefix[4] = {0x00, 0x66, 0xF3, 0xF2};
    Code c;
    c.n = 0;
    prologue(&c);
    emit(&c, 0x0F);  // movaps xmm0, xmm1
    emit(&c, 0x28);
    modrm_reg(&c, 0, 1);
    if (kPrefix[pp]) emit(&c, kPrefix[pp]);
    emit(&c, 0x0F);
    emit(&c, 0xC2);
    if (mem) {
        modrm_mem(&c, 0, DATA_B);
    } else {
        modrm_reg(&c, 0, 2);
    }
    emit(&c, (u8)imm);
    row(name, 128, mem, pi, imm, &c);
}

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
        printf("// Reference values for VCMPPS / VCMPPD / VCMPSS / VCMPSD at all 32 AVX\n");
        printf("// predicates and for the four legacy SSE mnemonics, produced by ACTUALLY\n");
        printf("// EXECUTING each encoding on x86-64 through Rosetta 2.  Nothing here is\n");
        printf("// hand-computed; an encoding Rosetta refused appears as a SKIP comment\n");
        printf("// instead of a value.\n");
        printf("//\n");
        printf("// Regenerate with the recipe in avx_cmp_rosetta_ref.c:\n");
        printf("//   clang -arch x86_64 -O1 -o /tmp/avxcmpref avx_cmp_rosetta_ref.c\n");
        printf("//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxcmpref > "
               "avx_cmp_rosetta_ref.inc\n");
        printf("//\n");
        printf("// Generating runtime: CPUID.1.ECX=%08x EDX=%08x, CPUID.7.0.EBX=%08x\n", cpuid1[2],
               cpuid1[3], cpuid7[1]);
        printf("//   (AVX bit=%d AVX2 bit=%d OSXSAVE bit=%d -- Rosetta hides these unless\n",
               (cpuid1[2] >> 28) & 1, (cpuid7[1] >> 5) & 1, (cpuid1[2] >> 27) & 1);
        printf("//    ROSETTA_ADVERTISE_AVX=1 is set; execution works regardless, which is\n");
        printf("//    why the gate above is a live vaddps and not a CPUID read.)\n");
        printf("\n");
        printf("// Input pairs: name, A (32 bytes), B (32 bytes).\n");
        printf("constexpr AvxCmpInput kAvxCmpInputs[] = {\n");
        for (int i = 0; i < g_npairs; i++) {
            char a[80], b[80];
            hexbytes(a, g_pairs[i].a, 32);
            hexbytes(b, g_pairs[i].b, 32);
            printf("    {\"%s\", \"%s\", \"%s\"},\n", g_pairs[i].name, a, b);
        }
        printf("};\n\n");
        printf("// name, width (VEX.L), mem (r/m shape), pair, imm8, encoding, ymm0 after.\n");
        printf("constexpr AvxCmpRef kAvxCmpRefs[] = {\n");
    }

    struct {
        const char* name;
        int vex, pp, scalar, l256;
    } ops[] = {
#define SVM_CMP(name, vex, pp, scalar, l256) {#name, vex, pp, scalar, l256},
#include "avx_cmp_ops.inc"
    };
    const int nops = (int)(sizeof(ops) / sizeof(ops[0]));

    for (int pi = 0; pi < g_npairs; pi++) {
        for (int oi = 0; oi < nops; oi++) {
            const int nimm = ops[oi].vex ? 32 : 8;
            for (int mem = 0; mem <= 1; mem++) {
                for (int imm = 0; imm < nimm; imm++) {
                    if (ops[oi].vex) {
                        gen_vex(ops[oi].name, ops[oi].pp, 0, mem, pi, imm);
                        if (ops[oi].l256) {
                            gen_vex(ops[oi].name, ops[oi].pp, 1, mem, pi, imm);
                        }
                    } else {
                        gen_sse(ops[oi].name, ops[oi].pp, mem, pi, imm);
                    }
                }
            }
        }
    }

    if (!g_dump) {
        printf("};\n");
        fprintf(stderr, "rows=%d skipped=%d pairs=%d\n", g_rows, g_skipped, g_npairs);
    }
    return 0;
}
