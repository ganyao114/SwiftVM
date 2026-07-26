// ===========================================================================
// Widening-multiply reference generator -- runs real x86-64 under Rosetta.
// ===========================================================================
//
// Covers vpmuludq / vpmuldq at both VEX widths and both VEX.W values, plus the
// legacy SSE pmuludq / pmuldq.  Same oracle and same traps as
// avx_fp2_rosetta_ref.c:
//
//   * Rosetta does NOT advertise AVX through CPUID unless the process starts
//     with ROSETTA_ADVERTISE_AVX=1.  Execution works either way, so support is
//     decided by EXECUTING an instruction and catching SIGILL, never by
//     reading CPUID.  The CPUID words are printed into the generated header as
//     provenance only.
//   * Rosetta is an emulator with its own measured defects, so a Rosetta result
//     is evidence, not proof.  The one thing about this family that could go
//     wrong silently -- WHICH source lanes are multiplied -- is therefore also
//     asserted independently against the Intel SDM in avx_mul_test.cpp, which
//     checks that the recorded lane 1 is a[2]*b[2] and not a[1]*b[1].
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// ----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/avxmulref avx_mul_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxmulref > avx_mul_rosetta_ref.inc
//
// Add --dump-encodings to print each stub's bytes to stderr for auditing
// against a disassembler.
//
// Each stub is
//
//     vmovdqu ymm0, [rdi+0x60]        ; poison the destination
//     vmovdqu ymm1, [rdi+0x00]        ; A
//     vmovdqu ymm2, [rdi+0x20]        ; B
//     <the recorded byte sequence>
//     <a 256-bit store of the answer register, NOT recorded>
//     vzeroupper
//     ret
//
// The capture store is excluded from the recorded bytes so avx_mul_test.cpp can
// read the register out of ThreadContext64 instead -- a broken vmovdqu cannot
// then mask a broken handler.  form 0 means the answer is ymm0 (the VEX rows,
// whose destination was poisoned, so all 32 bytes MEASURE contract C3); form 1
// means ymm1 (the legacy SSE rows, whose upper half must come back holding A's
// upper half untouched).
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

// Must agree with the poison avx_mul_test.cpp writes into ThreadContext64.
#define MUL_POISON(i) ((u8)(0xA5u ^ (unsigned)(i)))

// ---------------------------------------------------------------------------
// Input vectors.
//
// EVERY 32-bit lane of every operand is distinct, and each pair additionally
// satisfies a[1]*b[1] != a[2]*b[2] so that a lowering which widened lanes 0
// and 1 (AArch64's UMULL) instead of lanes 0 and 2 (what x86 specifies) is
// visible in the SECOND result lane.  Beyond that each pair aims at something:
//
//   lanes     every lane a different small value -- the plain "did the right
//             lane reach the right place" case, readable by eye in a failure.
//   signdisc  operands where the signed and unsigned products differ in the
//             HIGH dword: 0xFFFFFFFF*2 is 0x1FFFFFFFE unsigned and
//             0xFFFFFFFFFFFFFFFE signed.  Getting is_signed backwards changes
//             every one of these lanes.
//   extremes  0, 1, INT_MIN, INT_MAX, UINT_MAX in both operands.  Note that
//             0x80000000 * 0x80000000 is 0x4000000000000000 BOTH ways, so this
//             pair alone could not discriminate signedness -- which is exactly
//             why signdisc exists.
//   carry     0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE00000001 unsigned: the full
//             64-bit product, so a 32-bit-truncating implementation fails.
//   random    whatever the structured cases are blind to.
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    u8 a[32];
    u8 b[32];
} Pair;

static Pair g_pairs[16];
static int g_npairs;

static void put32(u8* p, int i, u32 v) { memcpy(p + i * 4, &v, 4); }

static Pair* new_pair(const char* name) {
    Pair* p = &g_pairs[g_npairs++];
    memset(p, 0, sizeof(*p));
    p->name = name;
    return p;
}

static void fill32(Pair* p, const u32 a[8], const u32 b[8]) {
    for (int i = 0; i < 8; i++) {
        put32(p->a, i, a[i]);
        put32(p->b, i, b[i]);
    }
}

static void build_pairs(void) {
    {
        Pair* p = new_pair("lanes");
        const u32 a[8] = {0x00000003u, 0x11111111u, 0x00000005u, 0x22222222u,
                          0x00000007u, 0x33333333u, 0x0000000Bu, 0x44444444u};
        const u32 b[8] = {0x00000101u, 0x55555555u, 0x00010001u, 0x66666666u,
                          0x01000001u, 0x77777777u, 0x10000001u, 0x88888888u};
        fill32(p, a, b);
    }
    {
        Pair* p = new_pair("signdisc");
        const u32 a[8] = {0xFFFFFFFFu, 0x00000001u, 0x80000000u, 0x7FFFFFFFu,
                          0xFFFFFFFEu, 0x12345678u, 0x80000001u, 0xCAFEBABEu};
        const u32 b[8] = {0x00000002u, 0xFFFFFFFFu, 0x00000003u, 0x00000002u,
                          0x7FFFFFFFu, 0x9ABCDEF0u, 0xFFFFFFFDu, 0x00000005u};
        fill32(p, a, b);
    }
    {
        Pair* p = new_pair("extremes");
        const u32 a[8] = {0x00000000u, 0x00000001u, 0x7FFFFFFFu, 0x80000000u,
                          0xFFFFFFFFu, 0x00000002u, 0x80000001u, 0x7FFFFFFEu};
        const u32 b[8] = {0xFFFFFFFFu, 0x80000000u, 0x80000000u, 0x7FFFFFFFu,
                          0x00000001u, 0xFFFFFFFEu, 0x00000000u, 0x00000003u};
        fill32(p, a, b);
    }
    {
        Pair* p = new_pair("carry");
        const u32 a[8] = {0xFFFFFFFFu, 0x00000001u, 0xFFFFFFFEu, 0x00000002u,
                          0xFFFF0000u, 0x00000004u, 0x0000FFFFu, 0x00000008u};
        const u32 b[8] = {0xFFFFFFFFu, 0x00000003u, 0xFFFFFFFDu, 0x00000005u,
                          0x0000FFFFu, 0x00000007u, 0xFFFF0000u, 0x00000009u};
        fill32(p, a, b);
    }
    {
        Pair* p = new_pair("random");
        u32 s = 0x5EED1234u;
        u32 a[8], b[8];
        for (int i = 0; i < 8; i++) {
            s = s * 1664525u + 1013904223u;
            a[i] = s ^ (s >> 11);
            s = s * 1664525u + 1013904223u;
            b[i] = s ^ (s >> 13);
        }
        fill32(p, a, b);
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

static void vex3(Code* c, int pp, int mmmmm, int vvvv, int l, int w) {
    emit(c, 0xC4);
    emit(c, (u8)((1 << 7) | (1 << 6) | (1 << 5) | (mmmmm & 0x1F)));  // R/X/B all un-set
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
static void prologue(Code* c) {
    c->n = 0;
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

// Finish `c`, run it for input pair `pi`, print the row.  `form` names the
// answer register: 0 = ymm0 (VEX), 1 = ymm1 (legacy SSE).
static void row(const char* mnemonic, int width, int pi, int form, Code* c) {
    char enc[300];
    hexbytes(enc, c->b + c->mark, c->n - c->mark);
    st256(c, form, DATA_O);  // appended AFTER recording; never replayed
    epilogue(c);
    if (g_dump) {
        char all[300];
        hexbytes(all, c->b, c->n);
        fprintf(stderr, "%-10s L%-4d pair%-2d form%d  %s   (full %s)\n", mnemonic, width, pi, form,
                enc, all);
        return;
    }
    memcpy(g_data + DATA_A, g_pairs[pi].a, 32);
    memcpy(g_data + DATA_B, g_pairs[pi].b, 32);
    memset(g_data + DATA_O, 0xCC, 32);
    for (int i = 0; i < 32; i++) {
        g_data[DATA_P + i] = MUL_POISON(i);
    }
    if (!run_stub(c)) {
        printf("    // SKIP %s L=%d pair%d(%s): Rosetta refused this encoding\n", mnemonic, width,
               pi, g_pairs[pi].name);
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

// VEX.NDS: dst = ymm0, VEX.vvvv = ymm1, r/m = ymm2 or [rdi+B].
static void gen_vexbin(const char* name, int map, int pp, int op, int pi, int l, int mem, int w) {
    Code c;
    prologue(&c);
    vex3(&c, pp, map, 1, l, w);
    emit(&c, (u8)op);
    if (mem) {
        modrm_mem(&c, 0, DATA_B);
    } else {
        modrm_reg(&c, 0, 2);
    }
    row(name, l ? 256 : 128, pi, 0, &c);
}

// Legacy SSE: 66 [0F 38] op /r with dst = xmm1 (already holding A).
static void gen_ssebin(const char* name, int map, int op, int pi, int mem) {
    Code c;
    prologue(&c);
    emit(&c, 0x66);
    emit(&c, 0x0F);
    if (map == 2) emit(&c, 0x38);
    emit(&c, (u8)op);
    if (mem) {
        modrm_mem(&c, 1, DATA_B);
    } else {
        modrm_reg(&c, 1, 2);
    }
    row(name, 128, pi, 1, &c);
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
        prologue(&c);
        vex3(&c, 0, 1, 1, 1, 0);  // vaddps ymm0, ymm1, ymm2
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
        printf("// Reference values for the WIDENING MULTIPLY family (vpmuludq, vpmuldq and\n");
        printf("// their legacy SSE twins), produced by ACTUALLY EXECUTING each encoding on\n");
        printf("// x86-64 through Rosetta 2.  Nothing here is hand-computed; an encoding\n");
        printf("// Rosetta refused appears as a SKIP comment instead of a value.\n");
        printf("//\n");
        printf("// Regenerate with the recipe in avx_mul_rosetta_ref.c:\n");
        printf("//   clang -arch x86_64 -O1 -o /tmp/avxmulref avx_mul_rosetta_ref.c\n");
        printf("//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxmulref > "
               "avx_mul_rosetta_ref.inc\n");
        printf("//\n");
        printf("// Generating runtime: CPUID.1.ECX=%08x EDX=%08x, CPUID.7.0.EBX=%08x\n", cpuid1[2],
               cpuid1[3], cpuid7[1]);
        printf("//   (AVX bit=%d AVX2 bit=%d OSXSAVE bit=%d -- Rosetta hides these unless\n",
               (cpuid1[2] >> 28) & 1, (cpuid7[1] >> 5) & 1, (cpuid1[2] >> 27) & 1);
        printf("//    ROSETTA_ADVERTISE_AVX=1 is set; execution works regardless, which is\n");
        printf("//    why the gate above is a live vaddps and not a CPUID read.)\n");
        printf("\n");
        printf("// Input vectors: name, A (32 bytes), B (32 bytes).\n");
        printf("static const AvxMulInput kAvxMulInputs[] = {\n");
        for (int i = 0; i < g_npairs; i++) {
            char ha[80], hb[80];
            hexbytes(ha, g_pairs[i].a, 32);
            hexbytes(hb, g_pairs[i].b, 32);
            printf("    {\"%s\", \"%s\", \"%s\"},\n", g_pairs[i].name, ha, hb);
        }
        printf("};\n\n");
        printf("// name, VEX.L width, input pair, answer register (0 = ymm0, 1 = ymm1),\n");
        printf("// the literal instruction bytes, and the 32 bytes read back.\n");
        printf("static const AvxMulRef kAvxMulRefs[] = {\n");
    }

    for (int pi = 0; pi < g_npairs; pi++) {
#define SVM_MUL(nm, shape, map, pp, op) gen_##shape(#nm, map, pp, op, pi)
        // S_VEXBIN: both widths, both operand shapes at W=0, plus the
        // register form at W=1 to pin that these opcodes really are WIG.
#define gen_S_VEXBIN(nm, map, pp, op, pi)                    \
    {                                                        \
        for (int l = 0; l < 2; l++) {                        \
            gen_vexbin(nm, map, pp, op, pi, l, 0, 0);        \
            gen_vexbin(nm ".m", map, pp, op, pi, l, 1, 0);   \
            gen_vexbin(nm ".w1", map, pp, op, pi, l, 0, 1);  \
        }                                                    \
    }
#define gen_S_SSEBIN(nm, map, pp, op, pi)    \
    {                                        \
        (void)(pp);                          \
        gen_ssebin(nm, map, op, pi, 0);      \
        gen_ssebin(nm ".m", map, op, pi, 1); \
    }
#include "avx_mul_ops.inc"
#undef gen_S_VEXBIN
#undef gen_S_SSEBIN
#undef SVM_MUL
    }

    if (!g_dump) {
        printf("};\n");
        fprintf(stderr, "%d rows emitted, %d skipped\n", g_rows, g_skipped);
        if (g_skipped) {
            fprintf(stderr, "warning: %d combinations were SKIPped by Rosetta\n", g_skipped);
        }
    }
    return 0;
}
