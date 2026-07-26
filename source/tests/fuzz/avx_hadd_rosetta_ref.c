// ===========================================================================
// VEX horizontal / pairwise reference generator -- real x86-64 under Rosetta.
// ===========================================================================
//
// Covers vhaddps/pd, vhsubps/pd, vphaddw/d, vphaddsw, vphsubw/d, vphsubsw and
// vpmaddubsw, each at BOTH VEX.L values and in BOTH operand shapes.
//
// SAME ORACLE, SAME TRAPS AS THE EARLIER WAVES
// --------------------------------------------
//   * Rosetta does NOT advertise AVX through CPUID unless the process starts
//     with ROSETTA_ADVERTISE_AVX=1.  Execution works either way, so capability
//     is decided below by EXECUTING a 256-bit instruction and catching SIGILL,
//     never by reading CPUID.
//   * Rosetta is an emulator with its own measured defects (VPSLLVQ's shift
//     count truncated to 32 bits, XSAVE writing extra bytes, vptest's PF
//     non-deterministic, and more), so a Rosetta result is evidence and not
//     proof.  avx_hadd_test.cpp therefore re-derives every row it can from the
//     Intel SDM's per-lane definition and asserts the RECORDED BYTES match --
//     an independent check on the oracle, not just on the implementation.
//     Where the two disagree the SDM wins and the disagreement is recorded.
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// ----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/avxhaddref avx_hadd_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxhaddref > avx_hadd_rosetta_ref.inc
//
// Add --dump-encodings to print each stub's bytes to stderr for auditing
// against a disassembler.
//
// WHAT A ROW CONTAINS, AND WHY IT CONTAINS THE ENCODING
// ----------------------------------------------------
// Every row carries the LITERAL BYTES of the instruction under test.
// avx_hadd_test.cpp replays those bytes rather than re-encoding from the shared
// table, so the two sides cannot assemble different instructions.
//
// Each stub is
//
//     vmovdqu ymm0, [rdi+0x60]        ; poison the destination
//     vmovdqu ymm1, [rdi+0x00]        ; A  (becomes VEX.vvvv, i.e. SRC1)
//     vmovdqu ymm2, [rdi+0x20]        ; B  (becomes ModRM.r/m, i.e. SRC2)
//     <the recorded byte sequence>    ; the one instruction under test
//     vmovdqu [rdi+0x40], ymm0        ; NOT recorded -- see below
//     vzeroupper
//     ret
//
// The read-back store is excluded from the recorded bytes so the test can take
// ymm0 straight out of ThreadContext64: a broken vmovdqu on the SwiftVM side
// then cannot mask a broken handler.  Reading all 32 bytes of a POISONED ymm0
// is what MEASURES contract C3 (a VEX.128 write zeroes bits 255:128) on real
// hardware for every 128-bit row, rather than assuming it.
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
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;

#define DATA_A 0x00
#define DATA_B 0x20
#define DATA_O 0x40
#define DATA_P 0x60
#define DATA_SIZE 0x80

// Must agree with Poison(0) in avx_hadd_test.cpp.
#define HADD_POISON(i) ((u8)(0xA5u ^ (unsigned)(i)))

// ---------------------------------------------------------------------------
// Input vectors.
//
// The failure modes this family actually has are (a) the wrong PAIRING of
// elements, (b) the wrong INTERLEAVE of the two sources in the result, (c) the
// wrong operand order inside a pair, and (d) at 256 bits, deriving the upper
// lane from the lower lane's operands.  Every pair below therefore differs
// between its two 128-bit lanes AND between A and B, so none of those four can
// produce a matching answer by luck.
//
//   ramp32/ramp64  distinct powers of two.  Sums and differences of distinct
//                  powers of two are EXACT in binary floating point, so these
//                  rows are the ones avx_hadd_test.cpp can re-derive from the
//                  SDM with no rounding ambiguity -- they are the cross-check
//                  on the oracle itself.
//   f32nan/f64nan  a different NaN payload in each element of a pair, so the
//                  recorded bytes SAY which element wins.  x86 gives operand 1
//                  of an add priority, and the two elements of a horizontal
//                  pair are operands 1 and 2 -- but the SDM's pseudocode lists
//                  them in one order for HADDPS and the other for HADDPD, and
//                  never states an intra-pair rule.  Only measurement settles
//                  it.  Also carries SNaN (must be quieted), inf + -inf and
//                  inf - inf (must give the x86 "real indefinite" -QNaN, not
//                  AArch64's default NaN).
//   f32round       sums that are NOT exact: a tie that must round to even, and
//                  a value plus half an ulp.  Distinguishes a correctly rounded
//                  add from a double-rounded or truncated one.
//   w16ramp        distinct 16-bit values, no overflow: separates vphaddw from
//                  vphaddsw only by their operand plumbing.
//   w16sat         pairs that overflow signed 16 bits in BOTH directions.  The
//                  saturating opcodes must clamp and the non-saturating ones
//                  must WRAP, on the very same bytes -- so a handler that
//                  saturated vphaddw, or wrapped vphaddsw, fails here and
//                  nowhere else.
//   d32ramp        distinct 32-bit values.
//   d32wrap        32-bit pairs that overflow.  vphaddd/vphsubd have no
//                  saturating form at all, so these must wrap modulo 2^32.
//   ubs            vpmaddubsw's whole difficulty in one vector: 0xFF x 0x80
//                  twice (the exact -32768 clamp), 0xFF x 0x7F twice (the exact
//                  +32767 clamp), and bytes 0x80/0xFF in BOTH operands so that
//                  reading A's byte as signed, or B's as unsigned, changes the
//                  answer.
//   bytes/random   distinct and unstructured bytes; what the structured cases
//                  are blind to.
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    u8 a[32];
    u8 b[32];
} Pair;

static Pair g_pairs[16];
static int g_npairs;

static void put16(u8* p, int i, u16 v) { memcpy(p + i * 2, &v, 2); }
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

#define INF32 0x7F800000u
#define NINF32 0xFF800000u
#define INF64 0x7FF0000000000000ull
#define NINF64 0xFFF0000000000000ull

static Pair* new_pair(const char* name) {
    Pair* p = &g_pairs[g_npairs++];
    memset(p, 0, sizeof(*p));
    p->name = name;
    return p;
}

static void build_pairs(void) {
    {
        // Powers of two: every one of the eight sums and eight differences is
        // a distinct exact value, so any mis-pairing or mis-interleave shows up
        // as a different NUMBER rather than as a rearrangement of equal ones.
        Pair* p = new_pair("ramp32");
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, f32bits((float)(1 << i)));
            put32(p->b, i, f32bits((float)(1 << (i + 8))));
        }
    }
    {
        Pair* p = new_pair("ramp64");
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, f64bits((double)(1 << i)));
            put64(p->b, i, f64bits((double)(1 << (i + 4))));
        }
    }
    {
        Pair* p = new_pair("f32nan");
        // Pairs, low element first:
        //   A: (QNaN .111, QNaN .222)  both NaN, different payloads
        //      (SNaN .333, 1.0)        SNaN even only -> quieted
        //      (2.0, QNaN .444)        NaN odd only
        //      (QNaN .555, SNaN .666)  QNaN even vs SNaN odd: position, not
        //                              signalling-ness, must decide
        //   B: (+inf, -inf)            hadd -> indefinite; hsub -> +inf
        //      (+inf, +inf)            hadd -> +inf;        hsub -> indefinite
        //      (SNaN .777, QNaN .888)  SNaN even vs QNaN odd, the mirror case
        //      (-0.0, 0.0)             hadd -> +0; hsub -> -0
        const u32 a[8] = {0x7FC00111u, 0x7FC00222u, 0x7F800333u, f32bits(1.0f),
                          f32bits(2.0f), 0x7FC00444u, 0x7FC00555u, 0x7F800666u};
        const u32 b[8] = {INF32, NINF32, INF32, INF32,
                          0x7F800777u, 0x7FC00888u, 0x80000000u, 0x00000000u};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("f64nan");
        const u64 a[4] = {0x7FF8000000000111ull, 0x7FF8000000000222ull, 0x7FF0000000000333ull,
                          f64bits(1.0)};
        const u64 b[4] = {INF64, NINF64, 0x8000000000000000ull, 0x7FF8000000000444ull};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
    }
    {
        // Rounding: 1 + 2^-24 is exactly halfway and must round to even (1.0);
        // (1 + 2^-23) + 2^-24 is halfway the other way and must round UP.
        // Denormal + denormal stays denormal; huge + huge overflows to inf.
        Pair* p = new_pair("f32round");
        const u32 a[8] = {f32bits(1.0f),          0x33800000u,  // 1.0, 2^-24
                          0x3F800001u,            0x33800000u,  // 1+2^-23, 2^-24
                          0x00000001u,            0x00000002u,  // denormals
                          f32bits(3.4e38f),       f32bits(3.4e38f)};
        const u32 b[8] = {f32bits(-1.0f),         0x33800000u,
                          f32bits(16777216.0f),   f32bits(1.0f),   // 2^24 + 1 -> 2^24
                          f32bits(-3.4e38f),      f32bits(-3.4e38f),
                          f32bits(0.1f),          f32bits(0.2f)};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("w16ramp");
        for (int i = 0; i < 16; i++) {
            put16(p->a, i, (u16)(1 << (i & 14)));
            put16(p->b, i, (u16)(-(i * 37 + 1)));
        }
    }
    {
        // Every pair overflows signed 16 bits, positively or negatively, for
        // BOTH the add and the subtract.
        Pair* p = new_pair("w16sat");
        const u16 a[16] = {0x7FFF, 0x7FFF, 0x8000, 0x8000, 0x7FFF, 0x8000, 0x0001, 0xFFFF,
                           0x4000, 0x4000, 0xC000, 0xC000, 0x7FFF, 0x0001, 0x8000, 0xFFFF};
        const u16 b[16] = {0x8000, 0x8000, 0x7FFF, 0x7FFF, 0x0000, 0xFFFF, 0x8000, 0x7FFF,
                           0x7FFF, 0x0001, 0x8000, 0xFFFF, 0x7FFF, 0x8000, 0x8000, 0x0001};
        for (int i = 0; i < 16; i++) {
            put16(p->a, i, a[i]);
            put16(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("d32ramp");
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, (u32)(1u << (i * 3)));
            put32(p->b, i, (u32)(-(i + 1) * 1000));
        }
    }
    {
        // No saturating 32-bit horizontal form exists, so these must WRAP.
        Pair* p = new_pair("d32wrap");
        const u32 a[8] = {0x7FFFFFFFu, 0x00000001u, 0x80000000u, 0xFFFFFFFFu,
                          0x7FFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x80000000u};
        const u32 b[8] = {0x80000000u, 0x80000000u, 0xFFFFFFFFu, 0x00000001u,
                          0xFFFFFFFFu, 0x00000001u, 0x00000001u, 0xFFFFFFFFu};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        // vpmaddubsw.  A's bytes are UNSIGNED and B's are SIGNED; 0x80 and 0xFF
        // appear in both so a swapped signedness cannot agree.
        Pair* p = new_pair("ubs");
        static const u8 a[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x80, 0x01, 0x01,
                                 0x00, 0xFF, 0x7F, 0x80, 0x02, 0x03, 0xFE, 0xFD,
                                 0x80, 0x01, 0xFF, 0x00, 0x40, 0x40, 0x10, 0x20,
                                 0xAA, 0x55, 0xC3, 0x3C, 0xFF, 0x80, 0x7F, 0x01};
        static const u8 b[32] = {0x80, 0x80, 0x7F, 0x7F, 0x7F, 0x7F, 0x80, 0x80,
                                 0x80, 0x00, 0x80, 0x7F, 0xFF, 0xFE, 0x02, 0x03,
                                 0x01, 0x80, 0x7F, 0xFF, 0x80, 0x80, 0x7F, 0x7F,
                                 0x33, 0xCC, 0x7F, 0x80, 0x7F, 0x7F, 0x80, 0x80};
        memcpy(p->a, a, 32);
        memcpy(p->b, b, 32);
    }
    {
        Pair* p = new_pair("bytes");
        for (int i = 0; i < 32; i++) {
            p->a[i] = (u8)(0x10 + i);      // 0x10..0x2F, all distinct
            p->b[i] = (u8)(0xF0 - i * 3);  // distinct, wraps through 0x00
        }
    }
    {
        Pair* p = new_pair("random");
        u32 s = 0x5EED1234u;
        for (int i = 0; i < 32; i++) {
            s = s * 1664525u + 1013904223u;
            p->a[i] = (u8)(s >> 21);
        }
        for (int i = 0; i < 32; i++) {
            s = s * 1664525u + 1013904223u;
            p->b[i] = (u8)(s >> 21);
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
static void row(const char* mnemonic, int width, int mem, int pi, Code* c) {
    char enc[300];
    hexbytes(enc, c->b + c->mark, c->n - c->mark);
    // The read-back store is appended AFTER the encoding is recorded, so the
    // test does not replay a store it does not need.
    st256(c, 0, DATA_O);
    epilogue(c);
    if (g_dump) {
        char all[300];
        hexbytes(all, c->b, c->n);
        fprintf(stderr, "%-14s L%-4d %-4s pair%-2d  %s   (full %s)\n", mnemonic, width,
                mem ? "mem" : "reg", pi, enc, all);
        return;
    }
    memcpy(g_data + DATA_A, g_pairs[pi].a, 32);
    memcpy(g_data + DATA_B, g_pairs[pi].b, 32);
    memset(g_data + DATA_O, 0xCC, 32);
    for (int i = 0; i < 32; i++) {
        g_data[DATA_P + i] = HADD_POISON(i);
    }
    if (!run_stub(c)) {
        printf("    // SKIP %s L=%d %s pair%d(%s): Rosetta refused this encoding\n", mnemonic,
               width, mem ? "mem" : "reg", pi, g_pairs[pi].name);
        g_skipped++;
        return;
    }
    char out[80];
    hexbytes(out, g_data + DATA_O, 32);
    printf("    {\"%s\", %d, %d, %d, \"%s\", \"%s\"},\n", mnemonic, width, mem, pi, enc, out);
    g_rows++;
}

// dst = ymm0, VEX.vvvv = ymm1 (SRC1 = A), r/m = ymm2 or [rdi+B] (SRC2 = B).
static void gen_bin(const char* name, int map, int pp, int op, int l, int mem, int pi) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, pp, map, 1, l, 0, 0, 0, 0);
    emit(&c, (u8)op);
    if (mem) {
        modrm_mem(&c, 0, DATA_B);
    } else {
        modrm_reg(&c, 0, 2);
    }
    row(name, l ? 256 : 128, mem, pi, &c);
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
    // A LIVE instruction, not a CPUID read: Rosetta reports AVX=0 by default
    // while executing AVX perfectly well, so CPUID is the one thing that must
    // not be consulted here.
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
        printf("// Reference values for the VEX HORIZONTAL / PAIRWISE family (vhadd*,\n");
        printf("// vhsub*, vphadd*, vphsub*, vpmaddubsw), produced by ACTUALLY EXECUTING\n");
        printf("// each encoding on x86-64 through Rosetta 2.  Nothing here is\n");
        printf("// hand-computed; an encoding Rosetta refused appears as a SKIP comment\n");
        printf("// instead of a value.\n");
        printf("//\n");
        printf("// Regenerate with the recipe in avx_hadd_rosetta_ref.c:\n");
        printf("//   clang -arch x86_64 -O1 -o /tmp/avxhaddref avx_hadd_rosetta_ref.c\n");
        printf("//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxhaddref > "
               "avx_hadd_rosetta_ref.inc\n");
        printf("//\n");
        printf("// Generating runtime: CPUID.1.ECX=%08x EDX=%08x, CPUID.7.0.EBX=%08x\n", cpuid1[2],
               cpuid1[3], cpuid7[1]);
        printf("//   (AVX bit=%d AVX2 bit=%d OSXSAVE bit=%d -- Rosetta hides these unless\n",
               (cpuid1[2] >> 28) & 1, (cpuid7[1] >> 5) & 1, (cpuid1[2] >> 27) & 1);
        printf("//    ROSETTA_ADVERTISE_AVX=1 is set; execution works regardless, which is\n");
        printf("//    why the gate above is a live vaddps and not a CPUID read.)\n");
        printf("\n");
        printf("// Input vectors: name, A (32 bytes), B (32 bytes).\n");
        printf("static const AvxHaddInput kAvxHaddInputs[] = {\n");
        for (int i = 0; i < g_npairs; i++) {
            char ha[80], hb[80];
            hexbytes(ha, g_pairs[i].a, 32);
            hexbytes(hb, g_pairs[i].b, 32);
            printf("    {\"%s\", \"%s\", \"%s\"},\n", g_pairs[i].name, ha, hb);
        }
        printf("};\n\n");
        printf("// name, VEX.L width, memory-operand flag, input pair, encoding, ymm0.\n");
        printf("static const AvxHaddRef kAvxHaddRefs[] = {\n");
    }

    for (int pi = 0; pi < g_npairs; pi++) {
#define SVM_HADD(name, map, pp, opcode, lanes, kind)      \
    for (int l = 0; l < 2; l++)                           \
        for (int mem = 0; mem < 2; mem++)                 \
            gen_bin(#name, map, pp, opcode, l, mem, pi);
#include "avx_hadd_ops.inc"
#undef SVM_HADD
    }

    if (!g_dump) {
        printf("};\n");
        fprintf(stderr, "rows=%d skipped=%d pairs=%d\n", g_rows, g_skipped, g_npairs);
    }
    return 0;
}
