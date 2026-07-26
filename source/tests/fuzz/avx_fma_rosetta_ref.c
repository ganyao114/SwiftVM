// ===========================================================================
// FMA3 reference generator -- runs real x86-64 FMA under Rosetta.
// ===========================================================================
//
// Produces avx_fma_rosetta_ref.inc: for every mnemonic in avx_fma_ops.inc, at
// VEX.L = 0 and 1 and with the r/m operand both in a register and in memory,
// the literal 32 bytes real x86-64 left in ymm0.
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// ----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/avxfmaref avx_fma_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxfmaref > avx_fma_rosetta_ref.inc
//
// Add --dump-encodings to print each stub's bytes to stderr for auditing
// against a disassembler.
//
// ORACLE CAVEATS, THE SAME ONES THE REST OF THIS DIRECTORY CARRIES
// ---------------------------------------------------------------
//   * Rosetta does NOT advertise AVX (let alone FMA) through CPUID unless the
//     process starts with ROSETTA_ADVERTISE_AVX=1.  Execution works either
//     way, so capability is decided by EXECUTING an instruction and catching
//     SIGILL -- never by reading CPUID.  The gate below is a live vfmadd231pd,
//     not a feature bit.
//   * Rosetta is an emulator with measured defects of its own (VPSLLVQ's shift
//     count truncated to 32 bits, XSAVE writing extra bytes, a non-
//     deterministic vptest PF, ...), so agreement is evidence and not proof.
//     Anything surprising is cross-read against the Intel SDM before it is
//     believed, and where the two disagree the SDM wins and the divergence is
//     recorded rather than papered over.
//   * Rosetta runs on AArch64, whose native FMA is also fused -- so it is a
//     GOOD oracle for the rounding question (it cannot accidentally agree with
//     an unfused implementation) but a WEAK one for NaN payload selection,
//     where an emulator is most tempted to let the host's rule show through.
//     The nan32/nan64 pairs exist to expose that; see the notes in the test.
//
// WHAT EACH ROW CONTAINS, AND WHY IT CONTAINS THE ENCODING
// -------------------------------------------------------
// Every row carries the LITERAL BYTES of the instruction executed, and
// avx_fma_test.cpp replays those rather than re-encoding from the shared
// table.  If both sides built the instruction from avx_fma_ops.inc, a wrong
// opcode or a wrong VEX.W in the table would make both sides test the same
// wrong instruction and the differential would pass vacuously.  This matters
// more here than anywhere else in the directory: the 60 mnemonics differ only
// in one opcode byte and one VEX bit, and several PAIRS of them (132 vs 231
// under a symmetric input, say) produce identical results on careless data.
//
// THE STUB
// --------
//     vmovdqu ymm0, [rdi+0x40]        ; C -- the destination, which is ALSO
//                                     ;      operand 1 of every FMA
//     vmovdqu ymm1, [rdi+0x00]        ; A -- VEX.vvvv, operand 2
//     vmovdqu ymm2, [rdi+0x20]        ; B -- ModRM.r/m, operand 3
//     <the recorded byte sequence>
//     vmovdqu [rdi+0x60], ymm0        ; NOT recorded; the test reads the
//     vzeroupper                      ; register out of ThreadContext64
//     ret
//
// Three DIFFERENT registers hold three DIFFERENT values, which is what makes
// the row sensitive to the 132/213/231 numbering at all.  C also serves as the
// contract-C3 witness: it is nonzero in bytes 16..31 of every pair, so a
// VEX.128 row's reference carries sixteen zero bytes that HARDWARE wrote.
//
// NOTHING HERE IS HAND-COMPUTED.  An instruction Rosetta refuses becomes a
// SKIP comment, never a value filled in from the manual.

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
#define DATA_C 0x40
#define DATA_O 0x60
#define DATA_SIZE 0x80

// ---------------------------------------------------------------------------
// Input triples.
//
// Three vectors per case: A -> ymm1 (VEX.vvvv, operand 2), B -> ymm2 (r/m,
// operand 3), C -> ymm0 (the destination, operand 1).
//
// The three arithmetic roles rotate with the mnemonic:
//     132   dst = op1 * op3 + op2  =  C * B + A
//     213   dst = op2 * op1 + op3  =  A * C + B
//     231   dst = op2 * op3 + op1  =  A * B + C
// so an input built for one order is usually dull for the other two.  The
// fusion cases below solve that with a SYMMETRIC construction that cancels
// under all three orders at once (see the comment there); the remaining cases
// use per-lane values that differ between the two 128-bit halves, so a handler
// that derived the upper half from the lower is caught.
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    u8 a[32];
    u8 b[32];
    u8 c[32];
} Triple;

static Triple g_cases[16];
static int g_ncases;

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

static Triple* new_case(const char* name) {
    Triple* t = &g_cases[g_ncases++];
    memset(t, 0, sizeof(*t));
    t->name = name;
    return t;
}

// Bits 16..31 of C must never be all zero: they are what a VEX.128 row's
// zeroed upper half is measured against (contract C3).  Asserted, because a
// case that quietly ended up with a zero upper C would silently stop testing
// it.
static void check_c3_witness(void) {
    for (int i = 0; i < g_ncases; i++) {
        int nz = 0;
        for (int j = 16; j < 32; j++) {
            if (g_cases[i].c[j] != 0) nz = 1;
        }
        if (!nz) {
            fprintf(stderr, "FATAL: case %s has a zero upper half in C; contract C3 would be\n"
                            "untested for every VEX.128 row.\n", g_cases[i].name);
            exit(2);
        }
    }
}

#define QNAN_A32 0x7FC00111u
#define QNAN_B32 0xFFC00222u  // negative payload: the sign must survive
#define SNAN_C32 0x7F800333u
#define INF32 0x7F800000u
#define NINF32 0xFF800000u
#define NZERO32 0x80000000u
#define DEN32 0x00000001u

#define QNAN_A64 0x7FF8000000000111ull
#define QNAN_B64 0xFFF8000000000222ull
#define SNAN_C64 0x7FF0000000000333ull
#define INF64 0x7FF0000000000000ull
#define NINF64 0xFFF0000000000000ull
#define NZERO64 0x8000000000000000ull
#define DEN64 0x0000000000000001ull

static void build_cases(void) {
    {
        // Small exact integers, all distinct, so the three operand orders give
        // three DIFFERENT answers for every mnemonic.  With (op1,op2,op3) =
        // (C,A,B) = (5,2,3): 132 -> 5*3+2 = 17, 213 -> 2*5+3 = 13,
        // 231 -> 2*3+5 = 11.  This is the case that pins the numbering; the
        // rest exercise arithmetic.
        Triple* t = new_case("order32");
        for (int i = 0; i < 8; i++) {
            put32(t->a, i, f32bits(2.0f + (float)i));
            put32(t->b, i, f32bits(3.0f + (float)(i * 2)));
            put32(t->c, i, f32bits(5.0f - (float)(i * 3)));
        }
    }
    {
        Triple* t = new_case("order64");
        for (int i = 0; i < 4; i++) {
            put64(t->a, i, f64bits(2.0 + i));
            put64(t->b, i, f64bits(3.0 + i * 2));
            put64(t->c, i, f64bits(5.0 - i * 3));
        }
    }
    // -----------------------------------------------------------------------
    // THE FUSION DETECTORS
    // -----------------------------------------------------------------------
    // What must be caught is a product that is ROUNDED before the addition.
    // For that, two things have to hold at once: the exact product must have
    // bits below the destination format's precision, and the addition must
    // CANCEL the leading bits so the surviving remainder becomes the answer
    // rather than a rounding error far below it.
    //
    // The obvious construction -- pick a product, pick an addend that cancels
    // it -- runs into the fact that a case is read by 60 different mnemonics
    // and each of them assigns the three operands to the multiply and the add
    // differently.  An addend that cancels for vfmadd231 does not cancel for
    // vfmadd132 (a different operand is the addend) nor for vfmsub231 (the
    // addend is negated).
    //
    // Both problems have the same answer.  Choose
    //
    //     A = 1 + 2^-j     B = 1 + 2^-k     C = -(1 + 2^-m)
    //
    // with j + k, j + m and k + m all greater than the mantissa width.  Then
    // EVERY pairwise product has a remainder below the precision, and every
    // order cancels, because each of A*B + C, A*C + B and C*B + A is
    // (something near 1) minus (something near 1).  The sign of C then selects
    // between the two halves of the sign flags: with C negative the plain
    // "+ addend" mnemonics cancel (vfmadd, vfnmsub), with C positive the
    // subtracting ones do (vfmsub, vfnmadd), and the addsub pair needs one of
    // each on adjacent lanes.  So the sign alternates lane by lane, and the
    // `b` variant starts the alternation on the other phase -- which is what
    // gives the SCALAR forms, which only ever see lane 0, both polarities.
    //
    // Two of the exponent triples make the product an EXACT TIE (j + k is one
    // more than the mantissa width, so the discarded remainder is precisely
    // half an ulp and round-to-nearest-even drops it); the others leave a
    // quarter ulp.  Both are lost by a pre-rounding implementation.
    for (int variant = 0; variant < 2; variant++) {
        Triple* t = new_case(variant ? "fuse32b" : "fuse32a");
        // (j, k, m); every pairwise sum exceeds 23, and the first two make A*B
        // an exact tie.
        static const int e[8][3] = {{12, 12, 13}, {11, 13, 12}, {12, 13, 14}, {13, 14, 12},
                                    {14, 12, 13}, {12, 14, 11}, {13, 12, 14}, {11, 14, 13}};
        for (int i = 0; i < 8; i++) {
            const u32 one = 0x3F800000u;
            const u32 a = one | (1u << (23 - e[i][0]));
            const u32 b = one | (1u << (23 - e[i][1]));
            u32 c = one | (1u << (23 - e[i][2]));
            if (((i + variant) % 2) == 0) c |= 0x80000000u;
            put32(t->a, i, a);
            put32(t->b, i, b);
            put32(t->c, i, c);
        }
    }
    for (int variant = 0; variant < 2; variant++) {
        Triple* t = new_case(variant ? "fuse64b" : "fuse64a");
        // Every pairwise sum exceeds 52; (26, 27) makes A*B an exact tie.
        static const int e[4][3] = {{26, 27, 28}, {27, 26, 28}, {28, 26, 27}, {27, 28, 26}};
        for (int i = 0; i < 4; i++) {
            const u64 one = 0x3FF0000000000000ull;
            const u64 a = one | (1ull << (52 - e[i][0]));
            const u64 b = one | (1ull << (52 - e[i][1]));
            u64 c = one | (1ull << (52 - e[i][2]));
            if (((i + variant) % 2) == 0) c |= 0x8000000000000000ull;
            put64(t->a, i, a);
            put64(t->b, i, b);
            put64(t->c, i, c);
        }
    }
    {
        // NaN and infinity.  Covers, per lane: a quiet NaN in each of the three
        // operand positions (which is what makes the propagation PRIORITY
        // observable), a signalling NaN that must come back quieted, a negative
        // NaN whose sign must survive, and Inf*0 / (Inf + -Inf), the two
        // invalid operations an FMA can raise with no NaN input at all -- x86
        // answers those with the QNaN indefinite, whose sign bit is SET, and
        // AArch64's default NaN's is clear, so the lanes discriminate.
        Triple* t = new_case("nan32");
        const u32 a[8] = {QNAN_A32,     f32bits(2.0f),  f32bits(3.0f), SNAN_C32,
                          QNAN_B32,     INF32,          f32bits(0.0f), f32bits(1.5f)};
        const u32 b[8] = {f32bits(2.0f), QNAN_A32,      f32bits(4.0f), f32bits(2.0f),
                          f32bits(2.0f), f32bits(0.0f), INF32,         INF32};
        const u32 c[8] = {f32bits(3.0f), f32bits(5.0f), QNAN_A32,      f32bits(7.0f),
                          f32bits(1.0f), f32bits(1.0f), NINF32,        NINF32};
        for (int i = 0; i < 8; i++) {
            put32(t->a, i, a[i]);
            put32(t->b, i, b[i]);
            put32(t->c, i, c[i]);
        }
    }
    {
        // NaN PROPAGATION PRIORITY: two or three NaNs at once, with payloads
        // that identify which one came back.  Without this the priority rule is
        // untested -- every lane of `nan32` has a single NaN, so any rule
        // returns the same answer -- and the rule is exactly where an
        // implementation is free to invent one.  The last two lanes combine a
        // NaN source with an invalid operation (Inf*0, Inf added to -Inf) to
        // settle which of the two wins.
        Triple* t = new_case("nanprio32");
        const u32 qa = 0x7FC00111u, qb = 0xFFC00222u, qc = 0x7FC00333u;
        const u32 sa = 0x7F800444u, sb = 0x7F800555u;
        const u32 a[8] = {qa, qa, f32bits(2.0f), qa, sa, qa, INF32, qa};
        const u32 b[8] = {qb, f32bits(2.0f), qb, qb, qb, sb, f32bits(0.0f), INF32};
        const u32 c[8] = {f32bits(3.0f), qc, qc, qc, f32bits(3.0f), f32bits(3.0f), qc, NINF32};
        for (int i = 0; i < 8; i++) {
            put32(t->a, i, a[i]);
            put32(t->b, i, b[i]);
            put32(t->c, i, c[i]);
        }
    }
    {
        Triple* t = new_case("nanprio64");
        const u64 qa = 0x7FF8000000000111ull, qb = 0xFFF8000000000222ull,
                  qc = 0x7FF8000000000333ull;
        const u64 sa = 0x7FF0000000000444ull;
        const u64 a[4] = {qa, qa, f64bits(2.0), sa};
        const u64 b[4] = {qb, f64bits(2.0), qb, qb};
        const u64 c[4] = {f64bits(3.0), qc, qc, qc};
        for (int i = 0; i < 4; i++) {
            put64(t->a, i, a[i]);
            put64(t->b, i, b[i]);
            put64(t->c, i, c[i]);
        }
    }
    {
        Triple* t = new_case("nan64");
        const u64 a[4] = {QNAN_A64, f64bits(2.0), f64bits(3.0), INF64};
        const u64 b[4] = {f64bits(2.0), SNAN_C64, f64bits(4.0), f64bits(0.0)};
        const u64 c[4] = {f64bits(3.0), f64bits(5.0), QNAN_B64, f64bits(1.0)};
        for (int i = 0; i < 4; i++) {
            put64(t->a, i, a[i]);
            put64(t->b, i, b[i]);
            put64(t->c, i, c[i]);
        }
    }
    {
        // Denormals, signed zeros and the overflow/underflow boundaries.  The
        // sign of a zero result is a real observable: fma(+0, +0, -0) is +0 but
        // fma(-0, +0, -0) is -0, and the negating variants move that around.
        Triple* t = new_case("extremes32");
        const u32 a[8] = {DEN32,        NZERO32,      f32bits(0.0f), 0x7F7FFFFFu /* max */,
                          0x00800000u,  f32bits(-1.0f), 0x7F7FFFFFu,  DEN32};
        const u32 b[8] = {DEN32,        f32bits(0.0f), NZERO32,       0x7F7FFFFFu,
                          0x00800000u,  DEN32,        f32bits(2.0f), 0x4B000000u};
        const u32 c[8] = {NZERO32,      NZERO32,      NZERO32,       0xFF7FFFFFu /* -max */,
                          NZERO32,      DEN32,        0xFF7FFFFFu,   NZERO32};
        for (int i = 0; i < 8; i++) {
            put32(t->a, i, a[i]);
            put32(t->b, i, b[i]);
            put32(t->c, i, c[i]);
        }
    }
    {
        Triple* t = new_case("extremes64");
        const u64 a[4] = {DEN64, NZERO64, 0x7FEFFFFFFFFFFFFFull, 0x0010000000000000ull};
        const u64 b[4] = {DEN64, f64bits(0.0), 0x7FEFFFFFFFFFFFFFull, 0x0010000000000000ull};
        const u64 c[4] = {NZERO64, NZERO64, 0xFFEFFFFFFFFFFFFFull, NZERO64};
        for (int i = 0; i < 4; i++) {
            put64(t->a, i, a[i]);
            put64(t->b, i, b[i]);
            put64(t->c, i, c[i]);
        }
    }
    {
        // Whatever the structured cases are blind to.  A cheap LCG over the
        // exponent and mantissa fields rather than over raw bits, so the values
        // are ordinary finite floats across many binades instead of a soup of
        // NaNs; the structured cases above already own the special values.
        Triple* t = new_case("spread32");
        u32 s = 0x12345678u;
        for (int i = 0; i < 8; i++) {
            u32 v[3];
            for (int k = 0; k < 3; k++) {
                s = s * 1664525u + 1013904223u;
                const u32 exp = 100u + (s >> 27);          // 100..131 -> 2^-27..2^4
                const u32 man = s & 0x007FFFFFu;
                const u32 sign = (s >> 26) & 1u;
                v[k] = (sign << 31) | (exp << 23) | man;
            }
            put32(t->a, i, v[0]);
            put32(t->b, i, v[1]);
            put32(t->c, i, v[2]);
        }
    }
    {
        Triple* t = new_case("spread64");
        u64 s = 0x9E3779B97F4A7C15ull;
        for (int i = 0; i < 4; i++) {
            u64 v[3];
            for (int k = 0; k < 3; k++) {
                s = s * 6364136223846793005ull + 1442695040888963407ull;
                const u64 exp = 1000ull + ((s >> 50) & 0x3Full);
                const u64 man = s & 0x000FFFFFFFFFFFFFull;
                const u64 sign = (s >> 49) & 1ull;
                v[k] = (sign << 63) | (exp << 52) | man;
            }
            put64(t->a, i, v[0]);
            put64(t->b, i, v[1]);
            put64(t->c, i, v[2]);
        }
    }
    check_c3_witness();
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
    ld256(c, 0, DATA_C);  // ymm0 = C, the destination and operand 1
    ld256(c, 1, DATA_A);  // ymm1 = A, VEX.vvvv, operand 2
    ld256(c, 2, DATA_B);  // ymm2 = B, r/m, operand 3
    c->mark = c->n;       // everything from here is what the test replays
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

// Finish `c`, run it for input case `ci`, print the row.
static void row(const char* mnemonic, int width, int ci, Code* c) {
    char enc[300];
    hexbytes(enc, c->b + c->mark, c->n - c->mark);
    // The capture store is appended AFTER the encoding is recorded, so the
    // test does not have to replay a store it does not need.
    st256(c, 0, DATA_O);
    epilogue(c);
    if (g_dump) {
        char all[300];
        hexbytes(all, c->b, c->n);
        fprintf(stderr, "%-18s L%-4d case%-2d  %s   (full %s)\n", mnemonic, width, ci, enc, all);
        return;
    }
    memcpy(g_data + DATA_A, g_cases[ci].a, 32);
    memcpy(g_data + DATA_B, g_cases[ci].b, 32);
    memcpy(g_data + DATA_C, g_cases[ci].c, 32);
    memset(g_data + DATA_O, 0xCC, 32);
    if (!run_stub(c)) {
        printf("    // SKIP %s L=%d case%d(%s): Rosetta refused this encoding\n", mnemonic, width,
               ci, g_cases[ci].name);
        g_skipped++;
        return;
    }
    char out[80];
    hexbytes(out, g_data + DATA_O, 32);
    printf("    {\"%s\", %d, %d, \"%s\", \"%s\"},\n", mnemonic, width, ci, enc, out);
    g_rows++;
}

// dst = ymm0 (also operand 1), VEX.vvvv = ymm1, r/m = ymm2 or [rdi+B].
static void gen(const char* name, int opcode, int w, int l, int mem, int ci) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, 1 /* 66 */, 2 /* 0F38 */, 1 /* vvvv = ymm1 */, l, 0, 0, 0, w);
    emit(&c, (u8)opcode);
    if (mem) {
        modrm_mem(&c, 0, DATA_B);
    } else {
        modrm_reg(&c, 0, 2);
    }
    row(name, l ? 256 : 128, ci, &c);
}

int main(int argc, char** argv) {
    g_dump = (argc > 1 && strcmp(argv[1], "--dump-encodings") == 0);
    signal(SIGILL, on_sigill);
    signal(SIGSEGV, on_sigill);
    signal(SIGBUS, on_sigill);
    signal(SIGTRAP, on_sigill);
    signal(SIGFPE, on_sigill);
    build_cases();

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

    // --- capability gate: prove 256-bit FMA executes before emitting anything.
    // A live instruction, not a CPUID bit: Rosetta hides both AVX and FMA from
    // CPUID by default while executing them perfectly well.
    {
        Code c;
        c.n = 0;
        prologue(&c);
        vex3(&c, 1, 2, 1, 1, 0, 0, 0, 1);  // vfmadd231pd ymm0, ymm1, ymm2
        emit(&c, 0xB8);
        modrm_reg(&c, 0, 2);
        st256(&c, 0, DATA_O);
        epilogue(&c);
        memcpy(g_data + DATA_A, g_cases[0].a, 32);
        memcpy(g_data + DATA_B, g_cases[0].b, 32);
        memcpy(g_data + DATA_C, g_cases[0].c, 32);
        if (!run_stub(&c)) {
            fprintf(stderr,
                    "FATAL: VEX.256 vfmadd231pd raised SIGILL under this runtime.\n"
                    "Rosetta on this machine cannot serve as an FMA oracle;\n"
                    "no reference data was generated.\n");
            return 1;
        }
    }

    u32 cpuid1[4] = {0};
    __asm__ volatile("cpuid"
                     : "=a"(cpuid1[0]), "=b"(cpuid1[1]), "=c"(cpuid1[2]), "=d"(cpuid1[3])
                     : "a"(1), "c"(0));

    if (!g_dump) {
        printf("// GENERATED FILE -- DO NOT EDIT BY HAND.\n");
        printf("//\n");
        printf("// FMA3 reference values, produced by ACTUALLY EXECUTING each encoding on\n");
        printf("// x86-64 through Rosetta 2.  Nothing here is hand-computed; an encoding\n");
        printf("// Rosetta refused appears as a SKIP comment instead of a value.\n");
        printf("//\n");
        printf("// Regenerate with the recipe in avx_fma_rosetta_ref.c:\n");
        printf("//   clang -arch x86_64 -O1 -o /tmp/avxfmaref avx_fma_rosetta_ref.c\n");
        printf("//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxfmaref > "
               "avx_fma_rosetta_ref.inc\n");
        printf("//\n");
        printf("// Generating runtime: CPUID.1.ECX=%08x (AVX bit=%d FMA bit=%d OSXSAVE bit=%d\n",
               cpuid1[2], (cpuid1[2] >> 28) & 1, (cpuid1[2] >> 12) & 1, (cpuid1[2] >> 27) & 1);
        printf("//   -- Rosetta reports all three as ZERO unless the process starts with\n");
        printf("//   ROSETTA_ADVERTISE_AVX=1, while executing the instructions perfectly\n");
        printf("//   well either way; that is why the gate above is a live vfmadd231pd and\n");
        printf("//   not a CPUID read.)\n");
        printf("\n");
        printf("// Input triples: name, A (ymm1 / operand 2), B (ymm2 / operand 3),\n");
        printf("// C (ymm0 -- the destination, and operand 1).\n");
        printf("static const AvxFmaInput kAvxFmaInputs[] = {\n");
        for (int i = 0; i < g_ncases; i++) {
            char ha[80], hb[80], hc[80];
            hexbytes(ha, g_cases[i].a, 32);
            hexbytes(hb, g_cases[i].b, 32);
            hexbytes(hc, g_cases[i].c, 32);
            printf("    {\"%s\", \"%s\", \"%s\", \"%s\"},\n", g_cases[i].name, ha, hb, hc);
        }
        printf("};\n\n");
        printf("// Results: mnemonic, VEX width (128/256), input-case index, the literal\n");
        printf("// instruction bytes executed, and the 32 bytes ymm0 held afterwards.\n");
        printf("// ymm0 was loaded with C beforehand, so a VEX.128 row's sixteen zero bytes\n");
        printf("// above the result are contract C3 as the HARDWARE reported it.\n");
        printf("static const AvxFmaRef kAvxFmaRefs[] = {\n");
    }

    for (int ci = 0; ci < g_ncases; ci++) {
        // `shape` is unused here: a scalar form is VEX.LIG, so BOTH L values
        // are emitted for every mnemonic and the two shapes enumerate
        // identically.  It stays in the table because the TEST needs it to
        // assert what a scalar row's untouched lanes must contain.
#define SVM_FMA(name, opcode, w, shape)            \
    for (int l = 0; l < 2; l++)                    \
        for (int mem = 0; mem < 2; mem++) {        \
            gen(#name, (opcode), (w), l, mem, ci); \
        }
#include "avx_fma_ops.inc"
#undef SVM_FMA
    }

    if (!g_dump) {
        printf("};\n");
        printf("\n// %d rows, %d skipped, %d input cases.\n", g_rows, g_skipped, g_ncases);
    } else {
        fprintf(stderr, "\n(dump only; no reference data written)\n");
    }
    return 0;
}
