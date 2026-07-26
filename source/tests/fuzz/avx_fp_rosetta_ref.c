// ===========================================================================
// VEX floating-point reference generator -- runs real x86-64 AVX under Rosetta.
// ===========================================================================
//
// WHY THIS EXISTS
// ---------------
// Unicorn 2.1.4 rejects every VEX.L=1 encoding with UC_ERR_INSN_INVALID, so the
// 256-bit handlers in source/runtime/frontend/x86/decoder_avx_fp.cc have no
// emulator oracle.  Worse, for the FLOAT family even the 128-bit forms need a
// hardware oracle: the parts that are easy to get wrong are NaN propagation,
// min/max operand order, signed zero and denormals, and those are precisely the
// places where a hand-written model would encode the same misunderstanding as
// the implementation it is meant to check.  Rosetta 2 on macOS 26/27 executes
// AVX including the full 256-bit register file, so it is used as ground truth.
//
// One trap: Rosetta does NOT advertise AVX through CPUID unless the process is
// started with ROSETTA_ADVERTISE_AVX=1.  Execution works either way; only the
// feature bits are hidden.  This program reports the CPUID bits for information
// but decides support by EXECUTING an instruction and catching SIGILL.
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// ----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/avxfpref avx_fp_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxfpref > avx_fp_rosetta_ref.inc
//
// Add --dump-encodings to print each stub's bytes to stderr instead of the
// table, for auditing the encoder against a disassembler.
//
// HOW IT WORKS
// ------------
// Instructions are ASSEMBLED AT RUNTIME from avx_fp_ops.inc rather than written
// as inline asm, because avx_fp_test.cpp builds its blocks from the same table
// with the same field meanings.  A wrong opcode byte therefore cannot make the
// two sides test different instructions -- a state a hand-written inline-asm
// version could reach and would hide.  Each stub is
//
//     vmovdqu ymm0, [rdi+0x60]        ; poison the destination
//     vmovdqu ymm1, [rdi+0x00]        ; A
//     vmovdqu ymm2, [rdi+0x20]        ; B
//     <op>    ymm0, ymm1, ymm2        ; or the L=0 / 2-operand variant
//     vmovdqu [rdi+0x40], ymm0        ; read back ALL 32 bytes
//     vzeroupper
//     ret
//
// Reading back 32 bytes even for a VEX.128 operation is deliberate: it captures
// the architectural zeroing of bits 255:128 (contract C3) as measured data.
//
// NOTHING HERE IS HAND-COMPUTED.  Every value in the generated table is the
// literal bytes Rosetta wrote to memory.  An instruction Rosetta refuses is
// emitted as a SKIP comment rather than being filled in from the Intel manual.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>

// The destination-poison macro lives in the shared table (so the generator and
// the SwiftVM test cannot disagree about it).  Including the table here with no
// entry macro defined expands to nothing but leaves SVM_AVXFP_POISON behind.
#include "avx_fp_ops.inc"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define DATA_A 0x00
#define DATA_B 0x20
#define DATA_O 0x40
#define DATA_P 0x60
#define DATA_SIZE 0x80

// ---------------------------------------------------------------------------
// Input vectors.
//
// Every pair keeps its two 128-bit lanes DIFFERENT on both sides, so an
// implementation that computed the upper half from the lower half's data (the
// most likely way to break the two-halves split required by contract C1) cannot
// pass.  Beyond that, the set is built around the four things this family gets
// wrong in practice:
//
//   NaN         -- payloads DIFFER between A and B, so the reference records
//                  WHICH operand's NaN propagated, not merely that one did.
//                  Signalling NaNs are included: x86 quiets them, so the result
//                  has bit 22 (f32) / bit 51 (f64) set that the input lacked.
//   NaN ORDER   -- f32nanA / f32nanB and f64nanA / f64nanB are MIRRORS of each
//                  other.  min/max must give different answers on the two, and
//                  an implementation using ARM's FMIN/FMAX gives the same.
//   signed zero -- +0/-0 in both orders.  min(+0,-0) = -0 but min(-0,+0) = +0
//                  on x86, and 0 == -0 compares equal, so only the raw bits
//                  distinguish them.
//   denormal    -- inputs, and products/quotients that land in the denormal
//                  range or underflow through it.
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

// f32 special bit patterns.  The NaN payloads are distinct per operand so the
// reference shows which one survived.
#define QNAN_A32 0x7FC00111u
#define QNAN_B32 0x7FC00222u
#define SNAN_A32 0x7F800333u  // signalling: quiet bit clear, payload non-zero
#define SNAN_B32 0x7F800444u
#define NQNAN_A32 0xFFC00555u
#define INF32 0x7F800000u
#define NINF32 0xFF800000u
#define PZERO32 0x00000000u
#define NZERO32 0x80000000u
#define DEN_MIN32 0x00000001u
#define DEN_MAX32 0x007FFFFFu
#define NORM_MIN32 0x00800000u
#define FMAX32 0x7F7FFFFFu

#define QNAN_A64 0x7FF8000000000111ull
#define QNAN_B64 0x7FF8000000000222ull
#define SNAN_A64 0x7FF0000000000333ull
#define SNAN_B64 0x7FF0000000000444ull
#define NQNAN_A64 0xFFF8000000000555ull
#define INF64 0x7FF0000000000000ull
#define NINF64 0xFFF0000000000000ull
#define PZERO64 0x0000000000000000ull
#define NZERO64 0x8000000000000000ull
#define DEN_MIN64 0x0000000000000001ull
#define DEN_MAX64 0x000FFFFFFFFFFFFFull
#define NORM_MIN64 0x0010000000000000ull

static Pair* new_pair(const char* name) {
    Pair* p = &g_pairs[g_npairs++];
    memset(p, 0, sizeof(*p));
    p->name = name;
    return p;
}

static void build_pairs(void) {
    // ---- 0: f32 NaN / special values on the A side -----------------------
    // Lane 7 has a NaN on BOTH sides, so "both operands NaN" (x86 takes
    // operand 1 for arithmetic, operand 2 for min/max) is covered here too.
    {
        Pair* p = new_pair("f32nanA");
        const u32 a[8] = {QNAN_A32, SNAN_A32, NQNAN_A32, f32bits(1.5f),
                          QNAN_A32, f32bits(-3.25f), f32bits(0.0f), SNAN_A32};
        const u32 b[8] = {f32bits(1.0f), f32bits(2.0f), f32bits(3.0f), f32bits(4.0f),
                          f32bits(-5.5f), f32bits(6.25f), f32bits(7.0f), QNAN_B32};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    // ---- 1: the MIRROR of pair 0 -----------------------------------------
    // min/max and subtract/divide are not commutative, so this pair must
    // produce different results from pair 0 for those; an implementation using
    // ARM's NaN rules produces the SAME result for both, which is the failure
    // this mirror exists to expose.
    {
        Pair* p = new_pair("f32nanB");
        memcpy(p->a, g_pairs[0].b, 32);
        memcpy(p->b, g_pairs[0].a, 32);
    }
    // ---- 2: signed zero and infinity, both orders ------------------------
    // Inf-Inf, Inf*0, 0/0 and Inf/Inf each produce the default QNaN, which is a
    // NaN the SOURCES did not contain -- the one case where x86 does not
    // propagate an operand.
    {
        Pair* p = new_pair("f32zeroinf");
        const u32 a[8] = {PZERO32, NZERO32, INF32, NINF32,
                          INF32, PZERO32, f32bits(1.0f), NINF32};
        const u32 b[8] = {NZERO32, PZERO32, NINF32, INF32,
                          INF32, INF32, PZERO32, PZERO32};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    // ---- 3: denormals -----------------------------------------------------
    // Products and quotients here land ON the denormal boundary, so a host that
    // flushed denormals to zero (FTZ/DAZ) would differ visibly.
    {
        Pair* p = new_pair("f32denorm");
        const u32 a[8] = {DEN_MIN32, NORM_MIN32, DEN_MAX32, DEN_MIN32,
                          FMAX32, f32bits(1.0f), (DEN_MIN32 | 0x80000000u), NORM_MIN32};
        const u32 b[8] = {f32bits(2.0f), f32bits(0.5f), f32bits(2.0f), f32bits(0.5f),
                          f32bits(2.0f), DEN_MIN32, NORM_MIN32, DEN_MAX32};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    // ---- 4: conversion boundaries ----------------------------------------
    // For vcvtps2dq / vcvttps2dq: values that round differently under
    // round-to-nearest-even than under truncation (0.5, 1.5, 2.5, -2.5), the
    // exact +/-2^31 endpoints (where +2^31 is OUT of range and -2^31 is IN),
    // and NaN -- all of which must produce the x86 "integer indefinite"
    // 0x80000000 rather than ARM's saturating FCVT result.
    {
        Pair* p = new_pair("f32cvt");
        const u32 a[8] = {f32bits(0.5f), f32bits(1.5f), f32bits(2.5f), f32bits(-2.5f),
                          f32bits(2147483648.0f), f32bits(-2147483648.0f), QNAN_A32,
                          f32bits(1e30f)};
        const u32 b[8] = {f32bits(-0.5f), f32bits(-1.5f), f32bits(3.5f), f32bits(-3.5f),
                          f32bits(2147483520.0f), f32bits(-3e30f), INF32, NINF32};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    // ---- 5: integer bit patterns -----------------------------------------
    // The source for vcvtdq2ps (INT_MIN / INT_MAX and values needing rounding
    // to fit 24 mantissa bits), and the operands for the bitwise ops, where
    // A OP B != B OP A for AND-NOT.  As floats these are also denormals and
    // NaNs, which is harmless: the reference is whatever hardware produced.
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
    // ---- 6 / 7: f64 NaN and its mirror -----------------------------------
    {
        Pair* p = new_pair("f64nanA");
        const u64 a[4] = {QNAN_A64, SNAN_A64, NQNAN_A64, f64bits(1.5)};
        const u64 b[4] = {f64bits(1.0), f64bits(2.0), f64bits(-3.0), QNAN_B64};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("f64nanB");
        memcpy(p->a, g_pairs[6].b, 32);
        memcpy(p->b, g_pairs[6].a, 32);
    }
    // ---- 8: f64 signed zero / infinity / denormal ------------------------
    {
        Pair* p = new_pair("f64zeroinfden");
        const u64 a[4] = {PZERO64, NZERO64, INF64, DEN_MIN64};
        const u64 b[4] = {NZERO64, INF64, INF64, f64bits(0.5)};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
    }
    // ---- 9: f64 conversion range -----------------------------------------
    // 1e300 overflows f32 (vcvtpd2ps -> +Inf) and 1e-300 underflows it (-> 0),
    // which is the only place vcvtpd2ps's rounding is observable.
    {
        Pair* p = new_pair("f64cvt");
        const u64 a[4] = {f64bits(1e300), f64bits(-1e300), f64bits(1e-300), f64bits(2.5)};
        const u64 b[4] = {f64bits(-0.5), NORM_MIN64, DEN_MAX64, f64bits(-2.5)};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
    }
    // ---- 10: sign-bit placement for vmovmskps / vmovmskpd ----------------
    // The low half's mask and the high half's are deliberately different and
    // neither is all-zeros or all-ones, so `lo | hi<<n` is distinguishable from
    // `hi | lo<<n` and from either half alone.
    {
        Pair* p = new_pair("signbits");
        const u32 a[8] = {0x80000000u, 0x00000000u, 0x80000000u, 0x00000000u,
                          0x00000000u, 0x80000000u, 0x80000000u, 0x80000000u};
        const u32 b[8] = {0x00000000u, 0x80000000u, 0x00000000u, 0x80000000u,
                          0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    // ---- 11: pseudo-random ------------------------------------------------
    // Catches whatever the structured cases happen to be blind to.
    {
        Pair* p = new_pair("random");
        u32 s = 0xC0FFEE11u;
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
// VEX encoder.  Field meanings and inversion behaviour match EmitVexC4 in
// avx_fp_test.cpp exactly: all inputs un-inverted, always the 3-byte C4 form,
// so the two programs build byte-identical encodings apart from the base
// register and displacement.
// ---------------------------------------------------------------------------
typedef struct {
    u8 b[96];
    int n;
} Code;

static void emit(Code* c, u8 x) { c->b[c->n++] = x; }

static void vex3(Code* c, int pp, int mmmmm, int vvvv, int l, int r, int x, int bb, int w) {
    emit(c, 0xC4);
    emit(c, (u8)((((~r) & 1) << 7) | (((~x) & 1) << 6) | (((~bb) & 1) << 5) | (mmmmm & 0x1F)));
    emit(c, (u8)(((w & 1) << 7) | (((~vvvv) & 0xF) << 3) | ((l & 1) << 2) | (pp & 3)));
}

// ModRM for [rdi + disp8].  rdi is register 7, which needs no SIB byte.
static void modrm_mem(Code* c, int reg, int disp8) {
    emit(c, (u8)(0x40 | ((reg & 7) << 3) | 7));
    emit(c, (u8)disp8);
}
static void modrm_reg(Code* c, int reg, int rm) { emit(c, (u8)(0xC0 | ((reg & 7) << 3) | (rm & 7))); }

// vmovdqu ymm<reg>, [rdi+disp]   (VEX.256.F3.0F 6F /r)
static void ld256(Code* c, int reg, int disp) {
    vex3(c, 2, 1, 0, 1, 0, 0, 0, 0);
    emit(c, 0x6F);
    modrm_mem(c, reg, disp);
}
// vmovdqu [rdi+disp], ymm<reg>   (VEX.256.F3.0F 7F /r)
static void st256(Code* c, int reg, int disp) {
    vex3(c, 2, 1, 0, 1, 0, 0, 0, 0);
    emit(c, 0x7F);
    modrm_mem(c, reg, disp);
}
static void epilogue(Code* c) {
    emit(c, 0xC5);
    emit(c, 0xF8);
    emit(c, 0x77);  // vzeroupper
    emit(c, 0xC3);  // ret
}
// Load the poison into ymm0 and the operands into ymm1 / ymm2.
static void prologue(Code* c) {
    ld256(c, 0, DATA_P);
    ld256(c, 1, DATA_A);
    ld256(c, 2, DATA_B);
}

// ---------------------------------------------------------------------------
// Executable-page management and SIGILL trapping.
// ---------------------------------------------------------------------------
typedef void (*stub_fn)(u8* data);

static u8* g_page;
static jmp_buf g_jb;

static void on_sigill(int s) {
    (void)s;
    longjmp(g_jb, 1);
}

// Returns 1 on success, 0 if the instruction raised SIGILL/SIGSEGV/SIGBUS.
static int run_stub(const Code* c, u8* data) {
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
        ((stub_fn)(void*)g_page)(data);
        return 1;
    }
    return 0;
}

static void hex32(char* out, const u8* v) {
    for (int i = 0; i < 32; i++) {
        sprintf(out + i * 2, "%02x", v[i]);
    }
    out[64] = 0;
}
static void hexbytes(char* out, const u8* v, int n) {
    for (int i = 0; i < n; i++) {
        sprintf(out + i * 2, "%02x", v[i]);
    }
    out[n * 2] = 0;
}

static u8* g_data;
static int g_dump;
static int g_skipped;
static int g_rows;

// Emit one table row: run `c` for input pair `pi` and print the 32-byte output
// slot.  `width` is 128 or 256 (the VEX.L the stub used), `imm` is the compare
// predicate or -1.
static void row(const char* mnemonic, int width, int pi, int imm, Code* c) {
    if (g_dump) {
        char enc[256];
        hexbytes(enc, c->b, c->n);
        fprintf(stderr, "%-12s L%-4d pair%-2d imm%-3d %s\n", mnemonic, width, pi, imm, enc);
        return;
    }
    memcpy(g_data + DATA_A, g_pairs[pi].a, 32);
    memcpy(g_data + DATA_B, g_pairs[pi].b, 32);
    memset(g_data + DATA_O, 0xCC, 32);
    for (int i = 0; i < 32; i++) {
        g_data[DATA_P + i] = SVM_AVXFP_POISON(i);
    }
    if (!run_stub(c, g_data)) {
        printf("    // SKIP %s L=%d pair%d(%s) imm=%d: Rosetta raised SIGILL, no reference\n",
               mnemonic, width, pi, g_pairs[pi].name, imm);
        g_skipped++;
        return;
    }
    char h[80];
    hex32(h, g_data + DATA_O);
    printf("    {\"%s\", %d, %d, %d, \"%s\"},\n", mnemonic, width, pi, imm, h);
    g_rows++;
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
    g_data = raw;  // page aligned

    // --- capability gate: prove 256-bit FP execution before emitting -------
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
        if (!run_stub(&c, g_data)) {
            fprintf(stderr,
                    "FATAL: VEX.256 vaddps raised SIGILL under this runtime.\n"
                    "Rosetta on this machine cannot serve as an AVX float oracle;\n"
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
        printf("// Reference values for the VEX floating-point family, produced by ACTUALLY\n");
        printf("// EXECUTING each instruction on x86-64 hardware through Rosetta 2.  Nothing\n");
        printf("// here is hand-computed; an instruction Rosetta refused appears as a SKIP\n");
        printf("// comment instead of a value.\n");
        printf("//\n");
        printf("// Regenerate with the recipe in avx_fp_rosetta_ref.c:\n");
        printf("//   clang -arch x86_64 -O1 -o /tmp/avxfpref avx_fp_rosetta_ref.c\n");
        printf("//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxfpref > "
               "avx_fp_rosetta_ref.inc\n");
        printf("//\n");
        printf("// Generating runtime: CPUID.1.ECX=%08x EDX=%08x, CPUID.7.0.EBX=%08x\n", cpuid1[2],
               cpuid1[3], cpuid7[1]);
        printf("//   (AVX bit=%d AVX2 bit=%d OSXSAVE bit=%d -- Rosetta hides these unless\n",
               (cpuid1[2] >> 28) & 1, (cpuid7[1] >> 5) & 1, (cpuid1[2] >> 27) & 1);
        printf("//    ROSETTA_ADVERTISE_AVX=1 is set; execution works regardless, which is\n");
        printf("//    why the generator gates on a live vaddps ymm and not on CPUID.)\n");
        printf("\n");

        printf("// Input vectors: index, name, A (32 bytes), B (32 bytes).\n");
        printf("// See avx_fp_rosetta_ref.c for what each pair is aimed at; the short\n");
        printf("// version is NaN payloads, NaN operand ORDER (mirrored pairs), signed\n");
        printf("// zero, infinity, denormals and conversion range limits.\n");
        printf("static const AvxFpInput kAvxFpInputs[] = {\n");
        for (int i = 0; i < g_npairs; i++) {
            char ha[80], hb[80];
            hex32(ha, g_pairs[i].a);
            hex32(hb, g_pairs[i].b);
            printf("    {\"%s\", \"%s\", \"%s\"},\n", g_pairs[i].name, ha, hb);
        }
        printf("};\n\n");
        printf("// Results: mnemonic, VEX width (128/256), input-pair index, imm8 predicate\n");
        printf("// (-1 when the form has none), and the 32 bytes read back from ymm0 after\n");
        printf("//   <op> ymm0, ymm1(=A), ymm2(=B)\n");
        printf("// with A[0..15] in xmm1 / A[16..31] in ymm1's upper half.  ymm0 held the\n");
        printf("// poison 0xA5^index beforehand, so a 128-bit form's zeroed upper half is\n");
        printf("// measured here, not assumed.\n");
        printf("//\n");
        printf("// vucomis*/vcomis* rows are NOT a vector: they hold CF,PF,ZF,OF,SF as five\n");
        printf("// 0/1 bytes followed by 0xCC filler.  vmovmsk* rows hold the 32-bit GPR\n");
        printf("// result as four little-endian bytes, likewise followed by filler.\n");
        printf("static const AvxFpRef kAvxFpRefs[] = {\n");
    }

    const int widths[2] = {0, 1};  // VEX.L

    // --- three-operand packed ----------------------------------------------
#define SVM_AVXFP_PACKED(name, pp, opcode, lane)                \
    for (int wi = 0; wi < 2; wi++) {                            \
        for (int pi = 0; pi < g_npairs; pi++) {                 \
            Code c;                                             \
            c.n = 0;                                            \
            prologue(&c);                                       \
            vex3(&c, (pp), 1, 1, widths[wi], 0, 0, 0, 0);       \
            emit(&c, (u8)(opcode));                             \
            modrm_reg(&c, 0, 2);                                \
            st256(&c, 0, DATA_O);                               \
            epilogue(&c);                                       \
            row(#name, widths[wi] ? 256 : 128, pi, -1, &c);     \
        }                                                       \
    }
#include "avx_fp_ops.inc"

    // --- three-operand scalar (VEX.LIG; emitted with L = 0) ----------------
#define SVM_AVXFP_SCALAR(name, pp, opcode, lane)  \
    for (int pi = 0; pi < g_npairs; pi++) {       \
        Code c;                                   \
        c.n = 0;                                  \
        prologue(&c);                             \
        vex3(&c, (pp), 1, 1, 0, 0, 0, 0, 0);      \
        emit(&c, (u8)(opcode));                   \
        modrm_reg(&c, 0, 2);                      \
        st256(&c, 0, DATA_O);                     \
        epilogue(&c);                             \
        row(#name, 128, pi, -1, &c);              \
    }
#include "avx_fp_ops.inc"

    // --- two-operand, source width = destination width ---------------------
    // VEX.vvvv is passed as 0, which the encoder inverts to 1111b -- the
    // architectural "no second source" marker.
#define SVM_AVXFP_UNARY(name, pp, opcode, lane)                       \
    for (int wi = 0; wi < 2; wi++) {                            \
        for (int pi = 0; pi < g_npairs; pi++) {                 \
            Code c;                                             \
            c.n = 0;                                            \
            prologue(&c);                                       \
            vex3(&c, (pp), 1, 0, widths[wi], 0, 0, 0, 0);       \
            emit(&c, (u8)(opcode));                             \
            modrm_reg(&c, 0, 1);                                \
            st256(&c, 0, DATA_O);                               \
            epilogue(&c);                                       \
            row(#name, widths[wi] ? 256 : 128, pi, -1, &c);     \
        }                                                       \
    }
#include "avx_fp_ops.inc"

    // --- vcvtps2pd: source is half the destination width -------------------
#define SVM_AVXFP_WIDEN(name, pp, opcode)                       \
    for (int wi = 0; wi < 2; wi++) {                            \
        for (int pi = 0; pi < g_npairs; pi++) {                 \
            Code c;                                             \
            c.n = 0;                                            \
            prologue(&c);                                       \
            vex3(&c, (pp), 1, 0, widths[wi], 0, 0, 0, 0);       \
            emit(&c, (u8)(opcode));                             \
            modrm_reg(&c, 0, 1);                                \
            st256(&c, 0, DATA_O);                               \
            epilogue(&c);                                       \
            row(#name, widths[wi] ? 256 : 128, pi, -1, &c);     \
        }                                                       \
    }
#include "avx_fp_ops.inc"

    // --- vcvtpd2ps: destination is half the source width -------------------
#define SVM_AVXFP_NARROW(name, pp, opcode)                      \
    for (int wi = 0; wi < 2; wi++) {                            \
        for (int pi = 0; pi < g_npairs; pi++) {                 \
            Code c;                                             \
            c.n = 0;                                            \
            prologue(&c);                                       \
            vex3(&c, (pp), 1, 0, widths[wi], 0, 0, 0, 0);       \
            emit(&c, (u8)(opcode));                             \
            modrm_reg(&c, 0, 1);                                \
            st256(&c, 0, DATA_O);                               \
            epilogue(&c);                                       \
            row(#name, widths[wi] ? 256 : 128, pi, -1, &c);     \
        }                                                       \
    }
#include "avx_fp_ops.inc"

    // --- vcmpps/pd/ss/sd over predicates 0..7 ------------------------------
#define SVM_AVXFP_CMP(name, pp, opcode, lane, scalar)                    \
    for (int wi = 0; wi < ((scalar) ? 1 : 2); wi++) {                    \
        for (int imm = 0; imm < 8; imm++) {                              \
            for (int pi = 0; pi < g_npairs; pi++) {                      \
                Code c;                                                  \
                c.n = 0;                                                 \
                prologue(&c);                                            \
                vex3(&c, (pp), 1, 1, (scalar) ? 0 : widths[wi],          \
                     0, 0, 0, 0);                                        \
                emit(&c, (u8)(opcode));                                  \
                modrm_reg(&c, 0, 2);                                     \
                emit(&c, (u8)imm);                                       \
                st256(&c, 0, DATA_O);                                    \
                epilogue(&c);                                            \
                row(#name, ((scalar) || !widths[wi]) ? 128 : 256, pi,    \
                    imm, &c);                                            \
            }                                                            \
        }                                                                \
    }
#include "avx_fp_ops.inc"

    // --- vucomis*/vcomis*: EFLAGS captured with SETcc ----------------------
    // The flags are SEEDED first (mov eax,0x7FFFFFFF; add eax,1 leaves
    // OF=1 SF=1 AF=1 CF=0 ZF=0 PF=0), so the architectural CLEARING of OF and
    // SF by the compare is observable rather than a no-op.
#define SVM_AVXFP_COMIS(name, pp, opcode, lane)      \
    for (int pi = 0; pi < g_npairs; pi++) {          \
        Code c;                                      \
        c.n = 0;                                     \
        prologue(&c);                                \
        emit(&c, 0xB8);                              \
        emit(&c, 0xFF);                              \
        emit(&c, 0xFF);                              \
        emit(&c, 0xFF);                              \
        emit(&c, 0x7F); /* mov eax, 0x7FFFFFFF */    \
        emit(&c, 0x83);                              \
        emit(&c, 0xC0);                              \
        emit(&c, 0x01); /* add eax, 1 */             \
        vex3(&c, (pp), 1, 0, 0, 0, 0, 0, 0);         \
        emit(&c, (u8)(opcode));                      \
        modrm_reg(&c, 1, 2); /* xmm1 vs xmm2 */      \
        emit(&c, 0x0F);                              \
        emit(&c, 0x92);                              \
        modrm_mem(&c, 0, DATA_O + 0); /* setb  CF */ \
        emit(&c, 0x0F);                              \
        emit(&c, 0x9A);                              \
        modrm_mem(&c, 0, DATA_O + 1); /* setp  PF */ \
        emit(&c, 0x0F);                              \
        emit(&c, 0x94);                              \
        modrm_mem(&c, 0, DATA_O + 2); /* sete  ZF */ \
        emit(&c, 0x0F);                              \
        emit(&c, 0x90);                              \
        modrm_mem(&c, 0, DATA_O + 3); /* seto  OF */ \
        emit(&c, 0x0F);                              \
        emit(&c, 0x98);                              \
        modrm_mem(&c, 0, DATA_O + 4); /* sets  SF */ \
        epilogue(&c);                                \
        row(#name, 128, pi, -1, &c);                 \
    }
#include "avx_fp_ops.inc"

    // --- vmovmskps / vmovmskpd ---------------------------------------------
#define SVM_AVXFP_MOVMSK(name, pp, opcode, lane)                \
    for (int wi = 0; wi < 2; wi++) {                            \
        for (int pi = 0; pi < g_npairs; pi++) {                 \
            Code c;                                             \
            c.n = 0;                                            \
            prologue(&c);                                       \
            vex3(&c, (pp), 1, 0, widths[wi], 0, 0, 0, 0);       \
            emit(&c, (u8)(opcode));                             \
            modrm_reg(&c, 0, 1); /* eax <- mask(ymm1) */        \
            emit(&c, 0x89);                                     \
            modrm_mem(&c, 0, DATA_O); /* mov [rdi+O], eax */    \
            epilogue(&c);                                       \
            row(#name, widths[wi] ? 256 : 128, pi, -1, &c);     \
        }                                                       \
    }
#include "avx_fp_ops.inc"

    if (!g_dump) {
        printf("};\n");
        fprintf(stderr, "%d rows emitted, %d skipped\n", g_rows, g_skipped);
        if (g_skipped) {
            fprintf(stderr, "warning: %d combinations were SKIPped by Rosetta\n", g_skipped);
        }
    }
    (void)widths;
    return 0;
}
