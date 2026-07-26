// ===========================================================================
// vround / vdpps / vdppd / vpermilpd-var reference generator -- real x86-64
// under Rosetta.
// ===========================================================================
//
// Same oracle and the same traps as avx_fp2_rosetta_ref.c:
//
//   * Rosetta does NOT advertise AVX through CPUID unless the process starts
//     with ROSETTA_ADVERTISE_AVX=1.  Execution works either way, so support is
//     decided by EXECUTING an instruction and catching SIGILL, never by
//     reading CPUID.
//   * Rosetta is an emulator with its own defects (VPSLLVQ's shift count
//     truncated to 32 bits, VPTEST's PF varying with unrelated details of the
//     surrounding program, and others found over this work), so a Rosetta
//     result is evidence, not proof.  Anything surprising is cross-checked
//     against the Intel SDM before it is believed, and where the two disagree
//     the SDM wins and the disagreement is recorded in avx_misc_test.cpp.
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// ----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/avxmiscref avx_misc_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxmiscref > avx_misc_rosetta_ref.inc
//
// Add --dump-encodings to print each stub's bytes to stderr for auditing
// against a disassembler.
//
// EVERY ROW CARRIES THE LITERAL INSTRUCTION BYTES it executed and the test
// replays those bytes rather than re-encoding from the shared table, so the
// two sides cannot assemble different instructions.  Nothing in the output is
// hand-computed; an instruction Rosetta refuses becomes a SKIP comment.
//
// THE MXCSR ROWS
// --------------
// vround's imm8 bit 2 means "use MXCSR.RC", so a reference for it has to name
// a rounding mode that is not in the instruction.  Those rows RECORD the
// ldmxcsr that selects it:
//
//     ldmxcsr [rdi+MXCSR_SLOT(rc)]    ; RC = rc, everything else default
//     <the vround under test>
//     ldmxcsr [rdi+MXCSR_SLOT(0)]     ; back to nearest-even
//
// all three inside the replayed portion, so the test drives the SwiftVM guest
// through exactly the same MXCSR transitions.  The restore matters: the test
// reuses one core across rows and a leaked RC would silently retune every
// later row.
//
// The stub layout is
//
//     vmovdqu ymm0, [rdi+0x60]        ; poison the destination
//     vmovdqu ymm1, [rdi+0x00]        ; A
//     vmovdqu ymm2, [rdi+0x20]        ; B
//     <the recorded byte sequence>
//     vmovdqu [rdi+0x40], ymm0        ; capture, EXCLUDED from the recording
//     vzeroupper
//     ret
//
// so the test can read ymm0 out of ThreadContext64 instead and a broken
// vmovdqu cannot mask a broken handler.  Reading all 32 bytes of a POISONED
// ymm0 is what measures contract C3 on every VEX.128 row.

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
#define DATA_M 0x80  /* four MXCSR words, one per rounding mode */
#define DATA_SIZE 0x100

#define MXCSR_SLOT(rc) (DATA_M + 4 * (rc))

// Must agree with the poison avx_misc_test.cpp writes into ThreadContext64.
#define MISC_POISON(i) ((u8)(0xA5u ^ (unsigned)(i)))

// ---------------------------------------------------------------------------
// Input vectors.  Both 128-bit lanes differ on both sides of every pair, so an
// implementation that derived the upper half from the lower half's operands
// cannot pass.
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
#define QNAN_B64 0x7FF8000000000222ull
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
        // THE PAIR THAT DECIDES round-half-even VS round-half-away.  Every
        // element is an exact tie, so the four modes give four different
        // answers and a std::round-style implementation fails here and only
        // here:  nearest-even 0,2,2,4,-0,-2,-2,-4  vs  half-away 1,2,3,4,...
        Pair* p = new_pair("mid32");
        const u32 a[8] = {f32bits(0.5f),  f32bits(1.5f),  f32bits(2.5f),  f32bits(3.5f),
                          f32bits(-0.5f), f32bits(-1.5f), f32bits(-2.5f), f32bits(-3.5f)};
        const u32 b[8] = {f32bits(4.5f),  f32bits(5.5f),  f32bits(-4.5f), f32bits(-5.5f),
                          f32bits(0.25f), f32bits(-0.25f), f32bits(0.75f), f32bits(-0.75f)};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("mid64");
        const u64 a[4] = {f64bits(0.5), f64bits(1.5), f64bits(-2.5), f64bits(-3.5)};
        const u64 b[4] = {f64bits(2.5), f64bits(-0.5), f64bits(4.5), f64bits(-1.5)};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
    }
    {
        // NaN payloads, signalling NaNs, infinities, signed zero, a denormal
        // and a value already past 2^23 (where every mode is the identity).
        Pair* p = new_pair("f32edge");
        const u32 a[8] = {QNAN_A32, SNAN_A32,   NZERO32,   DEN_MIN32,
                          INF32,    NINF32,     f32bits(16777216.0f), f32bits(-1e30f)};
        const u32 b[8] = {f32bits(0.0f),  QNAN_B32,  f32bits(-0.4f), f32bits(0.4f),
                          f32bits(-1.0f), INF32,     NINF32,         f32bits(8388609.0f)};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("f64edge");
        const u64 a[4] = {QNAN_A64, SNAN_A64, NZERO64, DEN_MIN64};
        const u64 b[4] = {INF64, NINF64, f64bits(-0.25), f64bits(4503599627370497.0)};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
    }
    {
        // vdpps: distinct, exactly-representable magnitudes so that WHICH
        // lanes were multiplied and WHICH received the sum are both readable
        // straight off the result, and every lane of the two 128-bit halves
        // differs so a lane-crossing sum is caught.
        Pair* p = new_pair("dot32");
        const u32 a[8] = {f32bits(1.0f),  f32bits(2.0f),  f32bits(4.0f),   f32bits(8.0f),
                          f32bits(16.0f), f32bits(32.0f), f32bits(64.0f),  f32bits(128.0f)};
        const u32 b[8] = {f32bits(256.0f),  f32bits(512.0f),   f32bits(1024.0f),
                          f32bits(2048.0f), f32bits(4096.0f),  f32bits(8192.0f),
                          f32bits(16384.0f), f32bits(32768.0f)};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        // vdpps' hard case: lane 0 is inf*0, which is a NaN if the lane is
        // SELECTED and must be exactly +0.0 if it is not -- the difference
        // between masking the operands and masking the product.  Lane 1 holds
        // NaNs so the addition tree's operand order is observable, and lanes
        // 2/3 are inf and -inf so the sum itself can be an indefinite.
        Pair* p = new_pair("dotnan");
        const u32 a[8] = {INF32,    QNAN_A32,      INF32,  NINF32,
                          NZERO32,  SNAN_A32,      INF32,  f32bits(1.0f)};
        const u32 b[8] = {f32bits(0.0f), QNAN_B32, f32bits(1.0f), f32bits(1.0f),
                          INF32,         f32bits(2.0f), f32bits(0.0f), NINF32};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("dot64");
        const u64 a[4] = {f64bits(1.0), f64bits(2.0), f64bits(4.0), f64bits(8.0)};
        const u64 b[4] = {f64bits(16.0), f64bits(32.0), f64bits(64.0), f64bits(128.0)};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
    }
    {
        // vpermilpd's variable control: the selector is BIT 1 of each qword.
        // Every other bit is deliberately noisy -- bit 0 set, high bits set,
        // the whole element large -- so an implementation reading bit 0, or
        // the whole element, or only the low byte, gives a different answer.
        // A is data with a distinct value per qword.
        Pair* p = new_pair("permctrl");
        const u64 a[4] = {0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull,
                          0x4444444444444444ull};
        const u64 b[4] = {0xFFFFFFFFFFFFFFFDull,  /* bit1 = 0, everything else 1 */
                          0x0000000000000002ull,  /* bit1 = 1 only */
                          0x8000000000000003ull,  /* bit1 = 1, bit0 = 1, sign set */
                          0x00000000FFFFFFFDull}; /* bit1 = 0, low dword nearly all 1 */
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
    }
    {
        // The mirror of permctrl, so no single control value dominates.
        Pair* p = new_pair("permctrl2");
        const u64 a[4] = {0xAAAAAAAAAAAAAAAAull, 0x5555555555555555ull, 0xDEADBEEFCAFEBABEull,
                          0x0123456789ABCDEFull};
        const u64 b[4] = {0x0000000000000002ull, 0xFFFFFFFFFFFFFFFDull, 0x00000000FFFFFFFFull,
                          0xFFFFFFFF00000002ull};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("random");
        u32 s = 0x5EED1234u;
        for (int i = 0; i < 32; i++) {
            s = s * 1664525u + 1013904223u;
            p->a[i] = (u8)(s >> 23);
        }
        for (int i = 0; i < 32; i++) {
            s = s * 1664525u + 1013904223u;
            p->b[i] = (u8)(s >> 23);
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
    int mark;
} Code;

static void emit(Code* c, u8 x) { c->b[c->n++] = x; }

static void vex3(Code* c, int pp, int mmmmm, int vvvv, int l, int w) {
    emit(c, 0xC4);
    emit(c, (u8)((1 << 7) | (1 << 6) | (1 << 5) | (mmmmm & 0x1F)));
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
    vex3(c, 2, 1, 0, 1, 0);
    emit(c, 0x6F);
    modrm_mem(c, reg, disp);
}
static void st256(Code* c, int reg, int disp) {
    vex3(c, 2, 1, 0, 1, 0);
    emit(c, 0x7F);
    modrm_mem(c, reg, disp);
}
// ldmxcsr [rdi+disp32] -- legacy encoding (0F AE /2), not VEX, so the test
// exercises the same path a real guest's fesetround takes.
//
// disp32 rather than the disp8 every other operand here uses: the MXCSR slots
// sit at 0x80 and up, and 0x80 as a SIGNED byte is -128.  The first cut used
// modrm_mem and silently loaded MXCSR from 124 bytes BEFORE the data block --
// which is not a crash, just a rounding mode that never changed, and the whole
// MXCSR half of this file quietly measured nothing.  Caught because Rosetta
// returned nearest-even for all four RC values while a standalone
// _mm_setcsr probe on the same host returned four different answers.
static void ldmxcsr(Code* c, int disp) {
    emit(c, 0x0F);
    emit(c, 0xAE);
    emit(c, (u8)(0x80 | (2 << 3) | 7));  // mod=10, reg=2, rm=rdi
    emit(c, (u8)(disp & 0xFF));
    emit(c, (u8)((disp >> 8) & 0xFF));
    emit(c, (u8)((disp >> 16) & 0xFF));
    emit(c, (u8)((disp >> 24) & 0xFF));
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
// The input pair every row emitted from here on refers to; set by the driver
// loop so the shape functions do not have to carry it.
static int g_pair;

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

// Finish `c`, run it for the active input pair, print the row.
static void row(const char* mnemonic, int width, int imm, int rc, Code* c) {
    const int pi = g_pair;
    char enc[300];
    hexbytes(enc, c->b + c->mark, c->n - c->mark);
    st256(c, 0, DATA_O);  // capture: appended AFTER the recording
    epilogue(c);
    if (g_dump) {
        char all[300];
        hexbytes(all, c->b, c->n);
        fprintf(stderr, "%-14s L%-4d pair%-2d imm%02x rc%d  %s   (full %s)\n", mnemonic, width, pi,
                imm, rc, enc, all);
        return;
    }
    memcpy(g_data + DATA_A, g_pairs[pi].a, 32);
    memcpy(g_data + DATA_B, g_pairs[pi].b, 32);
    memset(g_data + DATA_O, 0xCC, 32);
    for (int i = 0; i < 32; i++) {
        g_data[DATA_P + i] = MISC_POISON(i);
    }
    for (int k = 0; k < 4; k++) {
        u32 word = 0x1F80u | ((u32)k << 13);
        memcpy(g_data + MXCSR_SLOT(k), &word, 4);
    }
    if (!run_stub(c)) {
        printf("    // SKIP %s L=%d pair%d(%s) imm=%02x rc=%d: Rosetta refused this encoding\n",
               mnemonic, width, pi, g_pairs[pi].name, imm, rc);
        g_skipped++;
        return;
    }
    char out[80];
    hexbytes(out, g_data + DATA_O, 32);
    printf("    {\"%s\", %d, %d, %d, %d, \"%s\", \"%s\"},\n", mnemonic, width, pi, imm, rc, enc,
           out);
    g_rows++;
}

// ---------------------------------------------------------------------------
// Per-shape emission.  `mem` picks the memory operand form.
// ---------------------------------------------------------------------------

// vroundps / vroundpd: dst = ymm0, no vvvv, r/m = ymm1 or [rdi+A], imm8.
// rc >= 0 wraps the instruction in the ldmxcsr pair described in the header.
static void gen_round_p(const char* name, int op, int l, int mem, int imm, int rc) {
    Code c;
    c.n = 0;
    prologue(&c);
    if (rc >= 0) ldmxcsr(&c, MXCSR_SLOT(rc));
    vex3(&c, 1, 3, 0, l, 0);
    emit(&c, (u8)op);
    if (mem) {
        modrm_mem(&c, 0, DATA_A);
    } else {
        modrm_reg(&c, 0, 1);
    }
    emit(&c, (u8)imm);
    if (rc >= 0) ldmxcsr(&c, MXCSR_SLOT(0));
    row(name, l ? 256 : 128, imm, rc, &c);
}

// vroundss / vroundsd: dst = ymm0, VEX.vvvv = ymm1, r/m = ymm2 or [rdi+B].
static void gen_round_s(const char* name, int op, int mem, int imm, int rc) {
    Code c;
    c.n = 0;
    prologue(&c);
    if (rc >= 0) ldmxcsr(&c, MXCSR_SLOT(rc));
    vex3(&c, 1, 3, 1, 0, 0);
    emit(&c, (u8)op);
    if (mem) {
        modrm_mem(&c, 0, DATA_B);
    } else {
        modrm_reg(&c, 0, 2);
    }
    emit(&c, (u8)imm);
    if (rc >= 0) ldmxcsr(&c, MXCSR_SLOT(0));
    row(name, 128, imm, rc, &c);
}

// vdpps / vdppd / vpermilpd: dst = ymm0, VEX.vvvv = ymm1, r/m = ymm2 or [rdi+B].
static void gen_bin(const char* name, int map, int op, int l, int mem, int imm) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, 1, map, 1, l, 0);
    emit(&c, (u8)op);
    if (mem) {
        modrm_mem(&c, 0, DATA_B);
    } else {
        modrm_reg(&c, 0, 2);
    }
    if (imm >= 0) emit(&c, (u8)imm);
    row(name, l ? 256 : 128, imm < 0 ? 0 : imm, -1, &c);
}

// vdpps/vdppd imm8 values.  Chosen so the HIGH nibble (which lanes multiply)
// and the LOW nibble (which lanes receive the sum) differ in most rows: a
// stepped sweep would keep them equal and could not tell the two apart.
static const int kDotImms[] = {0x00, 0xFF, 0xF0, 0x0F, 0xF1, 0x1F, 0x71, 0x17,
                               0x31, 0x3F, 0x51, 0xA5, 0x81, 0x12, 0x48, 0x96};
#define N_DOT_IMMS ((int)(sizeof(kDotImms) / sizeof(kDotImms[0])))

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
        vex3(&c, 0, 1, 1, 1, 0);  // vaddps ymm0, ymm1, ymm2
        emit(&c, 0x58);
        modrm_reg(&c, 0, 2);
        st256(&c, 0, DATA_O);
        epilogue(&c);
        memcpy(g_data + DATA_A, g_pairs[0].a, 32);
        memcpy(g_data + DATA_B, g_pairs[0].b, 32);
        if (!g_dump && !run_stub(&c)) {
            fprintf(stderr,
                    "this host cannot execute VEX.256 -- run under "
                    "`ROSETTA_ADVERTISE_AVX=1 arch -x86_64`\n");
            return 3;
        }
    }

    printf("// GENERATED by avx_misc_rosetta_ref.c under Rosetta 2 -- DO NOT EDIT.\n");
    printf("// Every value below is the literal bytes real x86-64 wrote.\n");
    printf("constexpr AvxMiscInput kAvxMiscInputs[] = {\n");
    for (int i = 0; i < g_npairs; i++) {
        char ha[80], hb[80];
        hexbytes(ha, g_pairs[i].a, 32);
        hexbytes(hb, g_pairs[i].b, 32);
        printf("    {\"%s\", \"%s\", \"%s\"},\n", g_pairs[i].name, ha, hb);
    }
    printf("};\n\nconstexpr AvxMiscRef kAvxMiscRefs[] = {\n");

    for (g_pair = 0; g_pair < g_npairs; g_pair++) {
        for (int l = 0; l < 2; l++) {
            for (int mem = 0; mem < 2; mem++) {
                // vroundps / vroundpd: all sixteen imm8 values.  8..15 are the
                // precision-suppressing twins of 0..7 and must give identical
                // results; 4..7 and 12..15 all mean "follow MXCSR".
                for (int imm = 0; imm < 16; imm++) {
                    gen_round_p("vroundps", 0x08, l, mem, imm, -1);
                    gen_round_p("vroundpd", 0x09, l, mem, imm, -1);
                }
                // The MXCSR-following rows: imm8 = 4 (and its suppressing twin
                // 12) against each of the four RC values.
                for (int rc = 0; rc < 4; rc++) {
                    gen_round_p("vroundps.mx", 0x08, l, mem, 0x04, rc);
                    gen_round_p("vroundpd.mx", 0x09, l, mem, 0x04, rc);
                    gen_round_p("vroundps.mx", 0x08, l, mem, 0x0C, rc);
                    gen_round_p("vroundpd.mx", 0x09, l, mem, 0x0C, rc);
                }
                gen_bin("vpermilpd", 2, 0x0D, l, mem, -1);
                for (int i = 0; i < N_DOT_IMMS; i++) {
                    gen_bin("vdpps", 3, 0x40, l, mem, kDotImms[i]);
                }
                if (!l) {
                    for (int i = 0; i < N_DOT_IMMS; i++) {
                        gen_bin("vdppd", 3, 0x41, 0, mem, kDotImms[i]);
                    }
                }
            }
        }
        for (int mem = 0; mem < 2; mem++) {
            for (int imm = 0; imm < 16; imm++) {
                gen_round_s("vroundss", 0x0A, mem, imm, -1);
                gen_round_s("vroundsd", 0x0B, mem, imm, -1);
            }
            for (int rc = 0; rc < 4; rc++) {
                gen_round_s("vroundss.mx", 0x0A, mem, 0x04, rc);
                gen_round_s("vroundsd.mx", 0x0B, mem, 0x04, rc);
            }
        }
    }

    printf("};\n");
    if (!g_dump) {
        fprintf(stderr, "rows %d, skipped %d\n", g_rows, g_skipped);
    }
    return 0;
}


