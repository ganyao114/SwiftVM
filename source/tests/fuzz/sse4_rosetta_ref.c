// ===========================================================================
// Legacy SSE3 / SSSE3 / SSE4.1 / SSE4.2 reference generator -- real x86-64
// under Rosetta.
// ===========================================================================
//
// Same oracle and the same traps as avx_misc_rosetta_ref.c:
//
//   * Rosetta does NOT advertise AVX through CPUID unless the process starts
//     with ROSETTA_ADVERTISE_AVX=1.  Execution works either way, so support is
//     decided by EXECUTING an instruction and catching SIGILL, never by
//     reading CPUID.  This file needs AVX only for its PROLOGUE and CAPTURE
//     (poisoning and reading back bits 255:128 of a YMM is the whole point of
//     the legacy-preservation contract), and for the VEX twin rows.
//   * Rosetta is an emulator with its own defects, so a Rosetta result is
//     evidence, not proof.  Anything surprising is cross-checked against the
//     Intel SDM before it is believed, and where the two disagree the SDM wins
//     and the disagreement is recorded in sse4_test.cpp.
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// ----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/sse4ref sse4_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/sse4ref > sse4_rosetta_ref.inc
//
// Add --dump-encodings to print each stub's bytes to stderr for auditing
// against a disassembler.
//
// EVERY ROW CARRIES THE LITERAL INSTRUCTION BYTES it executed and the test
// replays those bytes rather than re-encoding from the shared table, so the
// two sides cannot assemble different instructions.  Nothing in the output is
// hand-computed; an instruction Rosetta refuses becomes a SKIP comment.
//
// THE REGISTER ASSIGNMENT
// -----------------------
//   ymm0  the blend mask (K).  It is ALSO the implicit operand of
//         blendvps/blendvpd/pblendvb, which is why the destination cannot be
//         xmm0 the way it is in the AVX reference files.
//   ymm1  A
//   ymm2  B, the r/m source of almost every row
//   ymm3  D, the destination -- preloaded so that its LOW half is data and its
//         HIGH half is a poison pattern that no input contains.  A legacy row
//         must give that poison back unchanged; a VEX row must give zeros.
//   rax/rbx/rcx/rdx  preloaded with distinct constants, so an unexpected GPR
//         write is visible, and used as the destination of pextr*/extractps
//         and the source of pinsr*.
//
// The observation block is 128 bytes and is the SAME for every row, whatever
// the instruction writes:
//
//   +0x00  ymm3   (32 bytes)   the vector destination, both halves
//   +0x20  ymm0   (32 bytes)   the mask register: must be untouched
//   +0x40  rax    (8)
//   +0x48  rbx    (8)
//   +0x50  rcx    (8)
//   +0x58  rdx    (8)
//   +0x60  S      (16 bytes)   the memory store target of pextr*/extractps,
//                              prefilled 0xCC
//   +0x70  padding (16)
//
// so the test compares one fixed byte array and never has to know which class
// of destination the row under test used.
//
// ALL MEMORY OPERANDS USE disp32, never disp8.  avx_misc_rosetta_ref.c
// documents why: a disp8 of 0x80 is -128, and the first cut of that file
// silently read from 124 bytes BEFORE the data block.  A uniform disp32 makes
// that class of bug unrepresentable.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define DATA_A 0x000  /* 32 */
#define DATA_B 0x020  /* 32 */
#define DATA_D 0x040  /* 32: destination preload */
#define DATA_K 0x060  /* 32: blend mask -> ymm0 */
#define DATA_S 0x080  /* 16: memory store target */
#define DATA_N 0x090  /* 16: narrow memory source (== A[0:16]) */
#define DATA_M 0x0A0  /* 4 MXCSR words */
#define DATA_O 0x0C0  /* 128: observation */
#define DATA_SIZE 0x200

#define MXCSR_SLOT(rc) (DATA_M + 4 * (rc))

/* Must agree with sse4_test.cpp. */
#define DEST_POISON(i) ((u8)(0x5Au ^ (unsigned)(i)))
#define RAX_CONST 0xAAAAAAAAAAAAAAAAull
#define RBX_CONST 0xBBBBBBBBBBBBBBBBull
#define RCX_CONST 0x0123456789ABCDEFull
#define RDX_CONST 0xDDDDDDDDDDDDDDDDull

/* ------------------------------------------------------------------------ */
/* Input vectors                                                             */
/* ------------------------------------------------------------------------ */
typedef struct {
    const char* name;
    u8 a[32];
    u8 b[32];
    u8 k[32]; /* the blend mask */
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

#define QNAN_A64 0x7FF8000000000111ull
#define SNAN_A64 0x7FF0000000000333ull
#define INF64 0x7FF0000000000000ull
#define NINF64 0xFFF0000000000000ull
#define NZERO64 0x8000000000000000ull

static Pair* new_pair(const char* name) {
    Pair* p = &g_pairs[g_npairs++];
    memset(p, 0, sizeof(*p));
    p->name = name;
    /* Default mask: a mix of set and clear top bits at every granularity, so
       a blend that read the wrong bit or the wrong lane width shows up. */
    for (int i = 0; i < 32; i++) {
        p->k[i] = (u8)((i % 3 == 0) ? 0xFF : ((i % 3 == 1) ? 0x80 : 0x7F));
    }
    return p;
}

static void build_pairs(void) {
    {
        /* Byte/word/dword saturation and signedness edges: every one of
           0x00 / 0x01 / 0x7F / 0x80 / 0xFF appears, so signed vs unsigned
           min/max, PACKUSDW's clamp and PABS's INT_MIN case are all decided
           by the data rather than by luck. */
        Pair* p = new_pair("edge");
        static const u8 a[32] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x81, 0x7E,
                                 0x00, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x7F,
                                 0x80, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xFF, 0xFF,
                                 0x01, 0x00, 0x80, 0xFF, 0x00, 0x80, 0x00, 0x80};
        static const u8 b[32] = {0xFF, 0x80, 0x7F, 0x01, 0x00, 0x7F, 0x80, 0xFF,
                                 0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x80,
                                 0x00, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x7F,
                                 0x80, 0xFF, 0x01, 0x00, 0x80, 0x00, 0x80, 0x00};
        memcpy(p->a, a, 32);
        memcpy(p->b, b, 32);
    }
    {
        /* 0..31 against 31..0.  Every byte is distinct, so a wrong lane, a
           wrong shift amount or a swapped operand moves a value that names
           its own position -- which is what makes mpsadbw's window/needle
           offsets and pmovsx's element mapping readable straight off. */
        Pair* p = new_pair("seq");
        for (int i = 0; i < 32; i++) {
            p->a[i] = (u8)i;
            p->b[i] = (u8)(31 - i);
        }
        /* A mask whose per-lane top bits alternate at 8/16/32/64 granularity
           differently, so blendvps, blendvpd and pblendvb cannot agree by
           accident. */
        static const u8 k[32] = {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                                 0x00, 0x00, 0x00, 0xFF, 0x7F, 0x7F, 0x7F, 0x7F,
                                 0xFF, 0x00, 0x80, 0x00, 0x00, 0x80, 0x00, 0xFF,
                                 0x7F, 0xFF, 0x00, 0x80, 0x80, 0x00, 0xFF, 0x00};
        memcpy(p->k, k, 32);
    }
    {
        /* PMULHRSW's and PMULLD's overflow corners plus the 16-bit tie that
           separates a rounding multiply from a truncating one. */
        Pair* p = new_pair("mul16");
        static const u16 a[16] = {0x8000, 0x8000, 0x7FFF, 0x7FFF, 0x4000, 0xC000, 0x0001, 0xFFFF,
                                  0x0100, 0x2000, 0x5A82, 0xA57E, 0x0000, 0x8001, 0x3FFF, 0x7000};
        static const u16 b[16] = {0x8000, 0x7FFF, 0x7FFF, 0x8000, 0x4000, 0x4000, 0xFFFF, 0x0001,
                                  0x0100, 0x2000, 0x5A82, 0x5A82, 0x8000, 0x8001, 0x4001, 0x7000};
        for (int i = 0; i < 16; i++) {
            memcpy(p->a + i * 2, &a[i], 2);
            memcpy(p->b + i * 2, &b[i], 2);
        }
    }
    {
        /* f32: NaN payloads, a signalling NaN, infinities, signed zero, exact
           ties in every direction and a value past 2^23 where all four
           rounding modes are the identity. */
        Pair* p = new_pair("f32");
        const u32 a[8] = {f32bits(0.5f),  f32bits(1.5f), f32bits(2.5f), f32bits(-2.5f),
                          QNAN_A32,       SNAN_A32,      NZERO32,       f32bits(16777216.0f)};
        const u32 b[8] = {f32bits(-0.5f), f32bits(4.5f), INF32,         NINF32,
                          QNAN_B32,       f32bits(0.0f), f32bits(0.4f), f32bits(-1e30f)};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
        }
    }
    {
        Pair* p = new_pair("f64");
        const u64 a[4] = {f64bits(0.5), f64bits(-1.5), QNAN_A64, NZERO64};
        const u64 b[4] = {f64bits(2.5), f64bits(-0.25), SNAN_A64, INF64};
        for (int i = 0; i < 4; i++) {
            put64(p->a, i, a[i]);
            put64(p->b, i, b[i]);
        }
        put64(p->a, 2, QNAN_A64);
        put64(p->b, 3, NINF64);
    }
    {
        /* Clean powers of two: which lanes a dpps imm8 multiplied and which
           received the sum are both readable straight off the result. */
        Pair* p = new_pair("dot");
        const u32 a[8] = {f32bits(1.0f),  f32bits(2.0f),  f32bits(4.0f),  f32bits(8.0f),
                          f32bits(16.0f), f32bits(32.0f), f32bits(64.0f), f32bits(128.0f)};
        const u32 b[8] = {f32bits(256.0f),   f32bits(512.0f),  f32bits(1024.0f),
                          f32bits(2048.0f),  f32bits(4096.0f), f32bits(8192.0f),
                          f32bits(16384.0f), f32bits(32768.0f)};
        for (int i = 0; i < 8; i++) {
            put32(p->a, i, a[i]);
            put32(p->b, i, b[i]);
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
        for (int i = 0; i < 32; i++) {
            s = s * 1664525u + 1013904223u;
            p->k[i] = (u8)(s >> 23);
        }
    }
    {
        /* ptest's discriminating pair: A AND B is zero but A ANDN B is not,
           so ZF and CF disagree.  Without a row like this both flags could be
           computed from the same expression and still pass. */
        Pair* p = new_pair("test");
        memset(p->a, 0, 32);
        memset(p->b, 0, 32);
        p->a[0] = 0x0F;
        p->a[17] = 0xF0;
        p->b[1] = 0xF0;
        p->b[16] = 0x0F;
    }
    {
        /* ptest's OTHER corner: SRC's bits are a strict SUBSET of DEST's, so
           CF = 1 and ZF = 0.  Without it every row in the file has CF = 0 and
           an implementation that hard-coded CF would pass. */
        Pair* p = new_pair("subset");
        memset(p->a, 0xFF, 32);
        memset(p->b, 0, 32);
        p->b[0] = 0x0F;
        p->b[7] = 0xF0;
        p->b[15] = 0x01;
    }
    {
        /* SRC = 0: ZF and CF are both 1.  The third of the four flag corners;
           the fourth (ZF = 0, CF = 0) is what almost every other pair gives. */
        Pair* p = new_pair("srczero");
        memset(p->a, 0x5A, 32);
        memset(p->b, 0, 32);
        for (int i = 16; i < 32; i++) {
            p->b[i] = (u8)(i * 7);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Encoder                                                                   */
/* ------------------------------------------------------------------------ */
// 512 is not decoration: the prologue is 76 bytes and the capture epilogue 78,
// so a 160-byte buffer left exactly 6 bytes for the instruction and every
// memory-operand form (disp32) or ldmxcsr-bracketed form overflowed into `n`
// and `mark`.  The symptom was 864 spurious "Rosetta refused this encoding"
// SKIPs -- silent under-coverage, not a crash.
typedef struct {
    u8 b[512];
    int n;
    int mark;
} Code;

static void emit(Code* c, u8 x) { c->b[c->n++] = x; }
static void emit32(Code* c, u32 v) {
    emit(c, (u8)v);
    emit(c, (u8)(v >> 8));
    emit(c, (u8)(v >> 16));
    emit(c, (u8)(v >> 24));
}

/* ModRM for [rdi + disp32]; rdi is register 7, which needs no SIB. */
static void modrm_mem(Code* c, int reg, int disp) {
    emit(c, (u8)(0x80 | ((reg & 7) << 3) | 7));
    emit32(c, (u32)disp);
}
static void modrm_reg(Code* c, int reg, int rm) {
    emit(c, (u8)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

static void vex3(Code* c, int pp, int mmmmm, int vvvv, int l, int w) {
    emit(c, 0xC4);
    emit(c, (u8)((1 << 7) | (1 << 6) | (1 << 5) | (mmmmm & 0x1F)));
    emit(c, (u8)(((w & 1) << 7) | (((~vvvv) & 0xF) << 3) | ((l & 1) << 2) | (pp & 3)));
}

static void ld256(Code* c, int reg, int disp) {
    vex3(c, 2, 1, 0, 1, 0); /* VEX.256.F3.0F 6F -- vmovdqu */
    emit(c, 0x6F);
    modrm_mem(c, reg, disp);
}
static void st256(Code* c, int reg, int disp) {
    vex3(c, 2, 1, 0, 1, 0);
    emit(c, 0x7F);
    modrm_mem(c, reg, disp);
}
/* mov r64, imm64 (REX.W B8+rd io) for rax/rbx/rcx/rdx. */
static void mov_imm64(Code* c, int reg, u64 v) {
    emit(c, 0x48);
    emit(c, (u8)(0xB8 + (reg & 7)));
    for (int i = 0; i < 8; i++) {
        emit(c, (u8)(v >> (i * 8)));
    }
}
/* mov [rdi+disp32], r64 */
static void st_gpr(Code* c, int reg, int disp) {
    emit(c, 0x48);
    emit(c, 0x89);
    modrm_mem(c, reg, disp);
}
/* mov r64, [rdi+disp32] */
static void ld_gpr(Code* c, int reg, int disp) {
    emit(c, 0x48);
    emit(c, 0x8B);
    modrm_mem(c, reg, disp);
}
static void ldmxcsr(Code* c, int disp) {
    emit(c, 0x0F);
    emit(c, 0xAE);
    emit(c, (u8)(0x80 | (2 << 3) | 7)); /* mod=10, reg=2 (ldmxcsr), rm=rdi */
    emit32(c, (u32)disp);
}

static void prologue(Code* c) {
    ld256(c, 0, DATA_K);
    ld256(c, 1, DATA_A);
    ld256(c, 2, DATA_B);
    ld256(c, 3, DATA_D);
    mov_imm64(c, 0, RAX_CONST);
    mov_imm64(c, 3, RBX_CONST);
    mov_imm64(c, 1, RCX_CONST);
    mov_imm64(c, 2, RDX_CONST);
    c->mark = c->n; /* everything from here is what the test replays */
}

static void epilogue(Code* c) {
    st256(c, 3, DATA_O + 0x00);
    st256(c, 0, DATA_O + 0x20);
    st_gpr(c, 0, DATA_O + 0x40); /* rax */
    st_gpr(c, 3, DATA_O + 0x48); /* rbx */
    st_gpr(c, 1, DATA_O + 0x50); /* rcx */
    st_gpr(c, 2, DATA_O + 0x58); /* rdx */
    /* rax is already captured, so it may be reused to copy S. */
    ld_gpr(c, 0, DATA_S + 0);
    st_gpr(c, 0, DATA_O + 0x60);
    ld_gpr(c, 0, DATA_S + 8);
    st_gpr(c, 0, DATA_O + 0x68);
    emit(c, 0xC5);
    emit(c, 0xF8);
    emit(c, 0x77); /* vzeroupper */
    emit(c, 0xC3); /* ret */
}

/* ------------------------------------------------------------------------ */
/* Execution and SIGILL trapping                                             */
/* ------------------------------------------------------------------------ */
typedef void (*stub_fn)(u8* data);

static u8* g_page;
static jmp_buf g_jb;
static u8* g_data;
static int g_dump;
static int g_skipped;
static int g_rows;
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

/* Finish `c`, run it for the active input pair, print the row. */
static void row(const char* mnemonic, int imm, int rc, Code* c) {
    const int pi = g_pair;
    char enc[400];
    hexbytes(enc, c->b + c->mark, c->n - c->mark);
    epilogue(c);
    if (g_dump) {
        char all[400];
        hexbytes(all, c->b, c->n);
        fprintf(stderr, "%-12s pair%-2d imm%02x rc%d  %s   (full %s)\n", mnemonic, pi, imm, rc,
                enc, all);
        return;
    }
    memset(g_data, 0, DATA_SIZE);
    memcpy(g_data + DATA_A, g_pairs[pi].a, 32);
    memcpy(g_data + DATA_B, g_pairs[pi].b, 32);
    memcpy(g_data + DATA_K, g_pairs[pi].k, 32);
    /* Destination preload: data in the low half, poison in the high half. */
    memcpy(g_data + DATA_D, g_pairs[pi].a, 16);
    for (int i = 0; i < 16; i++) {
        g_data[DATA_D + 16 + i] = DEST_POISON(i);
    }
    memcpy(g_data + DATA_N, g_pairs[pi].a, 16);
    memset(g_data + DATA_S, 0xCC, 16);
    memset(g_data + DATA_O, 0x99, 128);
    for (int k = 0; k < 4; k++) {
        u32 word = 0x1F80u | ((u32)k << 13);
        memcpy(g_data + MXCSR_SLOT(k), &word, 4);
    }
    if (!run_stub(c)) {
        printf("    // SKIP %s pair%d(%s) imm=%02x rc=%d: Rosetta refused this encoding\n",
               mnemonic, pi, g_pairs[pi].name, imm, rc);
        g_skipped++;
        return;
    }
    char out[300];
    hexbytes(out, g_data + DATA_O, 128);
    printf("    {\"%s\", %d, %d, %d, \"%s\", \"%s\"},\n", mnemonic, pi, imm, rc, enc, out);
    g_rows++;
}

/* ------------------------------------------------------------------------ */
/* Per-shape emission                                                        */
/* ------------------------------------------------------------------------ */
/* map: 0 = two-byte 0F, 0x38 / 0x3A = three-byte.  pfx: 0x66 / 0xF2 / 0xF3.
   mem >= 0 selects a [rdi+mem] r/m operand, mem < 0 the register form.
   imm < 0 means no immediate. */
static void legacy_head(Code* c, int pfx, int rexw, int map, int op) {
    if (pfx) emit(c, (u8)pfx);
    if (rexw) emit(c, 0x48);
    emit(c, 0x0F);
    if (map) emit(c, (u8)map);
    emit(c, (u8)op);
}

/* dst = xmm3, r/m = xmm2 or [rdi+mem]. */
static void gen_legacy(const char* name, int pfx, int map, int op, int imm, int mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    legacy_head(&c, pfx, 0, map, op);
    if (mem >= 0) {
        modrm_mem(&c, 3, mem);
    } else {
        modrm_reg(&c, 3, 2);
    }
    if (imm >= 0) emit(&c, (u8)imm);
    row(name, imm < 0 ? 0 : imm, -1, &c);
}

/* The VEX twin of gen_legacy: dst = xmm3, VEX.vvvv = xmm3, r/m = xmm2/[mem].
   Same operands, so the low 128 bits must match the legacy row exactly. */
static void gen_vex(const char* name, int pp, int map, int op, int imm, int mem, int w) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, pp, map, 3, 0, w);
    emit(&c, (u8)op);
    if (mem >= 0) {
        modrm_mem(&c, 3, mem);
    } else {
        modrm_reg(&c, 3, 2);
    }
    if (imm >= 0) emit(&c, (u8)imm);
    row(name, imm < 0 ? 0 : imm, -1, &c);
}

/* A VEX form with NO vvvv operand (vroundps, vptest, vpmovsx*, vpabs*).
   The field is stored INVERTED, so "no operand" -- raw bits 1111b -- is
   written by asking for register 0.  Passing 15 here stores raw 0000b, which
   names xmm15 and is a #UD on these opcodes; that mistake cost 96 rows to a
   SKIP that looked like an oracle limitation. */
static void gen_vex_nov(const char* name, int pp, int map, int op, int imm, int mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, pp, map, 0, 0, 0);
    emit(&c, (u8)op);
    if (mem >= 0) {
        modrm_mem(&c, 3, mem);
    } else {
        modrm_reg(&c, 3, 2);
    }
    if (imm >= 0) emit(&c, (u8)imm);
    row(name, imm < 0 ? 0 : imm, -1, &c);
}

/* roundps/roundpd/roundss/roundsd, optionally bracketed by ldmxcsr. */
static void gen_round(const char* name, int op, int imm, int rc, int mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    if (rc >= 0) ldmxcsr(&c, MXCSR_SLOT(rc));
    legacy_head(&c, 0x66, 0, 0x3A, op);
    if (mem >= 0) {
        modrm_mem(&c, 3, mem);
    } else {
        modrm_reg(&c, 3, 2);
    }
    emit(&c, (u8)imm);
    if (rc >= 0) ldmxcsr(&c, MXCSR_SLOT(0));
    row(name, imm, rc, &c);
}

/* ptest, with the five condition flags materialized into al/ah/bl/cl/dl.
   The zeroing is INSIDE the recorded region so the flag bytes are unambiguous
   even if the instruction leaves a flag undefined. */
static void gen_ptest(const char* name, int vex, int mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    emit(&c, 0x31); /* xor eax, eax */
    emit(&c, 0xC0);
    emit(&c, 0x31); /* xor ebx, ebx */
    emit(&c, 0xDB);
    emit(&c, 0x31); /* xor ecx, ecx */
    emit(&c, 0xC9);
    emit(&c, 0x31); /* xor edx, edx */
    emit(&c, 0xD2);
    if (vex) {
        vex3(&c, 1, 2, 0, 0, 0);  /* vvvv unused: stored inverted, so 0 -> raw 1111b */
        emit(&c, 0x17);
    } else {
        legacy_head(&c, 0x66, 0, 0x38, 0x17);
    }
    if (mem >= 0) {
        modrm_mem(&c, 3, mem);
    } else {
        modrm_reg(&c, 3, 2);
    }
    emit(&c, 0x0F); /* setz al */
    emit(&c, 0x94);
    emit(&c, 0xC0);
    emit(&c, 0x0F); /* setp ah */
    emit(&c, 0x9A);
    emit(&c, 0xC4);
    emit(&c, 0x0F); /* setc bl */
    emit(&c, 0x92);
    emit(&c, 0xC3);
    emit(&c, 0x0F); /* sets cl */
    emit(&c, 0x98);
    emit(&c, 0xC1);
    emit(&c, 0x0F); /* seto dl */
    emit(&c, 0x90);
    emit(&c, 0xC2);
    row(name, 0, -1, &c);
}

/* blendvps / blendvpd / pblendvb -- the mask is the IMPLICIT xmm0. */
static void gen_blendv(const char* name, int op, int mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    legacy_head(&c, 0x66, 0, 0x38, op);
    if (mem >= 0) {
        modrm_mem(&c, 3, mem);
    } else {
        modrm_reg(&c, 3, 2);
    }
    row(name, 0, -1, &c);
}

/* The VEX twin: the mask register is encoded in the /is4 byte's high nibble,
   and it is set to xmm0 so the two rows compute the same thing. */
static void gen_vblendv(const char* name, int op, int mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, 1, 3, 3, 0, 0);
    emit(&c, (u8)op);
    if (mem >= 0) {
        modrm_mem(&c, 3, mem);
    } else {
        modrm_reg(&c, 3, 2);
    }
    emit(&c, 0x00); /* is4: mask = xmm0 */
    row(name, 0, -1, &c);
}

/* pextrb/pextrd/pextrq/extractps: destination eax/rax or [rdi+S]. */
static void gen_extract(const char* name, int op, int rexw, int imm, int to_mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    legacy_head(&c, 0x66, rexw, 0x3A, op);
    if (to_mem) {
        modrm_mem(&c, 3, DATA_S);
    } else {
        modrm_reg(&c, 3, 0); /* r/m = eax/rax */
    }
    emit(&c, (u8)imm);
    row(name, imm, -1, &c);
}

static void gen_vextract(const char* name, int op, int rexw, int imm, int to_mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, 1, 3, 0, 0, rexw);  /* vvvv unused */
    emit(&c, (u8)op);
    if (to_mem) {
        modrm_mem(&c, 3, DATA_S);
    } else {
        modrm_reg(&c, 3, 0);
    }
    emit(&c, (u8)imm);
    row(name, imm, -1, &c);
}

/* pinsrb/pinsrd/pinsrq: source ecx/rcx or [rdi+N]. */
static void gen_insert(const char* name, int op, int rexw, int imm, int from_mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    legacy_head(&c, 0x66, rexw, 0x3A, op);
    if (from_mem) {
        modrm_mem(&c, 3, DATA_N);
    } else {
        modrm_reg(&c, 3, 1); /* r/m = ecx/rcx */
    }
    emit(&c, (u8)imm);
    row(name, imm, -1, &c);
}

/* insertps: r/m is an xmm register or an m32. */
static void gen_insertps(const char* name, int vex, int imm, int mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    if (vex) {
        vex3(&c, 1, 3, 3, 0, 0);
        emit(&c, 0x21);
    } else {
        legacy_head(&c, 0x66, 0, 0x3A, 0x21);
    }
    if (mem >= 0) {
        modrm_mem(&c, 3, mem);
    } else {
        modrm_reg(&c, 3, 2);
    }
    emit(&c, (u8)imm);
    row(name, imm, -1, &c);
}

static const int kDotImms[] = {0x00, 0xFF, 0xF0, 0x0F, 0xF1, 0x1F, 0x71, 0x17,
                               0x31, 0x3F, 0x51, 0xA5, 0x81, 0x12, 0x48, 0x96};
#define N_DOT_IMMS ((int)(sizeof(kDotImms) / sizeof(kDotImms[0])))

/* (name, 0F38 opcode) for the plain two-operand SSSE3/SSE4.1 integer forms. */
typedef struct {
    const char* name;
    int op;
} Simple;

static const Simple k38_bin[] = {
        {"phaddw", 0x01},   {"phaddd", 0x02},  {"phaddsw", 0x03}, {"pmaddubsw", 0x04},
        {"phsubw", 0x05},   {"phsubd", 0x06},  {"phsubsw", 0x07}, {"psignb", 0x08},
        {"psignw", 0x09},   {"psignd", 0x0A},  {"pmulhrsw", 0x0B}, {"pmulld", 0x40},
        {"pcmpeqq", 0x29},  {"pcmpgtq", 0x37}, {"packusdw", 0x2B}, {"pminsb", 0x38},
        {"pminsd", 0x39},   {"pminuw", 0x3A},  {"pminud", 0x3B},   {"pmaxsb", 0x3C},
        {"pmaxsd", 0x3D},   {"pmaxuw", 0x3E},  {"pmaxud", 0x3F},
};
#define N_38_BIN ((int)(sizeof(k38_bin) / sizeof(k38_bin[0])))

static const Simple k38_un[] = {
        {"pabsb", 0x1C}, {"pabsw", 0x1D}, {"pabsd", 0x1E}, {"phminposuw", 0x41},
};
#define N_38_UN ((int)(sizeof(k38_un) / sizeof(k38_un[0])))

/* (name, opcode, source element bits, destination element bits). */
typedef struct {
    const char* name;
    int op;
    int src;
    int dst;
} Extend;

static const Extend k_extend[] = {
        {"pmovsxbw", 0x20, 8, 16},  {"pmovsxbd", 0x21, 8, 32},  {"pmovsxbq", 0x22, 8, 64},
        {"pmovsxwd", 0x23, 16, 32}, {"pmovsxwq", 0x24, 16, 64}, {"pmovsxdq", 0x25, 32, 64},
        {"pmovzxbw", 0x30, 8, 16},  {"pmovzxbd", 0x31, 8, 32},  {"pmovzxbq", 0x32, 8, 64},
        {"pmovzxwd", 0x33, 16, 32}, {"pmovzxwq", 0x34, 16, 64}, {"pmovzxdq", 0x35, 32, 64},
};
#define N_EXTEND ((int)(sizeof(k_extend) / sizeof(k_extend[0])))

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

    /* Capability gate: the prologue and the capture are AVX, so prove VEX.256
       executes before emitting anything.  Support is decided by EXECUTION --
       Rosetta's CPUID says AVX=0 unless ROSETTA_ADVERTISE_AVX=1. */
    {
        Code c;
        c.n = 0;
        prologue(&c);
        epilogue(&c);
        memset(g_data, 0, DATA_SIZE);
        if (!g_dump && !run_stub(&c)) {
            fprintf(stderr,
                    "this host cannot execute VEX.256 -- run under "
                    "`ROSETTA_ADVERTISE_AVX=1 arch -x86_64`\n");
            return 3;
        }
    }

    printf("// GENERATED by sse4_rosetta_ref.c under Rosetta 2 -- do not edit.\n");
    printf("// Regenerate: clang -arch x86_64 -O1 -o /tmp/sse4ref sse4_rosetta_ref.c &&\n");
    printf("//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/sse4ref > sse4_rosetta_ref.inc\n");
    printf("static const Sse4Input kSse4Inputs[] = {\n");
    for (int i = 0; i < g_npairs; i++) {
        char a[80], b[80], k[80];
        hexbytes(a, g_pairs[i].a, 32);
        hexbytes(b, g_pairs[i].b, 32);
        hexbytes(k, g_pairs[i].k, 32);
        printf("    {\"%s\", \"%s\", \"%s\", \"%s\"},\n", g_pairs[i].name, a, b, k);
    }
    printf("};\n\n");
    printf("static const Sse4Ref kSse4Refs[] = {\n");

    for (g_pair = 0; g_pair < g_npairs; g_pair++) {
        /* ---- rounding ------------------------------------------------- */
        for (int imm = 0; imm < 16; imm++) {
            gen_round("roundps", 0x08, imm, -1, -1);
            gen_round("roundpd", 0x09, imm, -1, -1);
            gen_round("roundss", 0x0A, imm, -1, -1);
            gen_round("roundsd", 0x0B, imm, -1, -1);
        }
        gen_round("roundps", 0x08, 3, -1, DATA_B);
        gen_round("roundpd", 0x09, 1, -1, DATA_B);
        gen_round("roundss", 0x0A, 2, -1, DATA_B);
        gen_round("roundsd", 0x0B, 0, -1, DATA_B);
        for (int rc = 0; rc < 4; rc++) {
            gen_round("roundps.mx", 0x08, 4, rc, -1);
            gen_round("roundpd.mx", 0x09, 4, rc, -1);
            gen_round("roundss.mx", 0x0A, 4, rc, -1);
            gen_round("roundsd.mx", 0x0B, 4, rc, -1);
            gen_round("roundps.mx", 0x08, 12, rc, -1);
        }
        for (int imm = 0; imm < 4; imm++) {
            gen_vex_nov("vroundps", 1, 3, 0x08, imm, -1);
            gen_vex_nov("vroundpd", 1, 3, 0x09, imm, -1);
        }

        /* ---- ptest ---------------------------------------------------- */
        gen_ptest("ptest", 0, -1);
        gen_ptest("ptest", 0, DATA_B);
        gen_ptest("vptest", 1, -1);

        /* ---- sign / zero extension ------------------------------------ */
        for (int i = 0; i < N_EXTEND; i++) {
            const Extend* e = &k_extend[i];
            gen_legacy(e->name, 0x66, 0x38, e->op, -1, -1);
            gen_legacy(e->name, 0x66, 0x38, e->op, -1, DATA_N);
        }
        gen_vex_nov("vpmovsxbw", 1, 2, 0x20, -1, -1);
        gen_vex_nov("vpmovzxdq", 1, 2, 0x35, -1, -1);

        /* ---- blends --------------------------------------------------- */
        gen_blendv("blendvps", 0x14, -1);
        gen_blendv("blendvps", 0x14, DATA_B);
        gen_blendv("blendvpd", 0x15, -1);
        gen_blendv("blendvpd", 0x15, DATA_B);
        gen_blendv("pblendvb", 0x10, -1);
        gen_blendv("pblendvb", 0x10, DATA_B);
        gen_vblendv("vblendvps", 0x4A, -1);
        gen_vblendv("vpblendvb", 0x4C, -1);
        for (int imm = 0; imm < 16; imm++) {
            gen_legacy("blendps", 0x66, 0x3A, 0x0C, imm, -1);
            gen_legacy("blendpd", 0x66, 0x3A, 0x0D, imm & 3, -1);
        }
        static const int kBlendwImms[] = {0x00, 0xFF, 0x0F, 0xF0, 0xAA, 0x55, 0x81, 0x3C};
        for (int i = 0; i < 8; i++) {
            gen_legacy("pblendw", 0x66, 0x3A, 0x0E, kBlendwImms[i], -1);
            gen_vex("vpblendw", 1, 3, 0x0E, kBlendwImms[i], -1, 0);
        }
        gen_legacy("blendps", 0x66, 0x3A, 0x0C, 0x5, DATA_B);
        gen_legacy("pblendw", 0x66, 0x3A, 0x0E, 0x5A, DATA_B);
        gen_vex("vblendps", 1, 3, 0x0C, 0x5, -1, 0);

        /* ---- the plain two-operand 0F38 integer family ------------------ */
        for (int i = 0; i < N_38_BIN; i++) {
            gen_legacy(k38_bin[i].name, 0x66, 0x38, k38_bin[i].op, -1, -1);
            gen_legacy(k38_bin[i].name, 0x66, 0x38, k38_bin[i].op, -1, DATA_B);
        }
        gen_vex("vpmulld", 1, 2, 0x40, -1, -1, 0);
        gen_vex("vpcmpeqq", 1, 2, 0x29, -1, -1, 0);
        gen_vex("vpcmpgtq", 1, 2, 0x37, -1, -1, 0);
        gen_vex("vpackusdw", 1, 2, 0x2B, -1, -1, 0);
        gen_vex("vphaddw", 1, 2, 0x01, -1, -1, 0);
        gen_vex("vpmaddubsw", 1, 2, 0x04, -1, -1, 0);
        gen_vex("vpminsb", 1, 2, 0x38, -1, -1, 0);
        gen_vex("vpmaxud", 1, 2, 0x3F, -1, -1, 0);

        /* ---- the unary 0F38 family -------------------------------------- */
        for (int i = 0; i < N_38_UN; i++) {
            gen_legacy(k38_un[i].name, 0x66, 0x38, k38_un[i].op, -1, -1);
            gen_legacy(k38_un[i].name, 0x66, 0x38, k38_un[i].op, -1, DATA_B);
        }
        gen_vex_nov("vpabsd", 1, 2, 0x1E, -1, -1);

        /* ---- insert / extract -------------------------------------------- */
        for (int imm = 0; imm < 16; imm++) {
            gen_insertps("insertps", 0, imm, -1);
        }
        static const int kIpsImms[] = {0x00, 0x39, 0x4E, 0xC5, 0xFF, 0x1A};
        for (int i = 0; i < 6; i++) {
            gen_insertps("insertps", 0, kIpsImms[i], -1);
            gen_insertps("insertps", 0, kIpsImms[i], DATA_N);
            gen_insertps("vinsertps", 1, kIpsImms[i], -1);
        }
        for (int imm = 0; imm < 4; imm++) {
            gen_extract("extractps", 0x17, 0, imm, 0);
            gen_extract("extractps", 0x17, 0, imm, 1);
            gen_extract("pextrd", 0x16, 0, imm, 0);
            gen_extract("pextrd", 0x16, 0, imm, 1);
            gen_insert("pinsrd", 0x22, 0, imm, 0);
            gen_insert("pinsrd", 0x22, 0, imm, 1);
        }
        for (int imm = 0; imm < 16; imm++) {
            gen_extract("pextrb", 0x14, 0, imm, 0);
            gen_insert("pinsrb", 0x20, 0, imm, 0);
        }
        gen_extract("pextrb", 0x14, 0, 5, 1);
        gen_insert("pinsrb", 0x20, 0, 5, 1);
        gen_vextract("vpextrb", 0x14, 0, 5, 0);
        for (int imm = 0; imm < 2; imm++) {
            gen_extract("pextrq", 0x16, 1, imm, 0);
            gen_extract("pextrq", 0x16, 1, imm, 1);
            gen_insert("pinsrq", 0x22, 1, imm, 0);
            gen_insert("pinsrq", 0x22, 1, imm, 1);
        }

        /* ---- SSE3 horizontal float and add/subtract --------------------- */
        gen_legacy("haddpd", 0x66, 0, 0x7C, -1, -1);
        gen_legacy("haddpd", 0x66, 0, 0x7C, -1, DATA_B);
        gen_legacy("hsubpd", 0x66, 0, 0x7D, -1, -1);
        gen_legacy("haddps", 0xF2, 0, 0x7C, -1, -1);
        gen_legacy("hsubps", 0xF2, 0, 0x7D, -1, -1);
        gen_legacy("addsubps", 0xF2, 0, 0xD0, -1, -1);
        gen_legacy("addsubps", 0xF2, 0, 0xD0, -1, DATA_B);
        gen_legacy("addsubpd", 0x66, 0, 0xD0, -1, -1);
        gen_legacy("addsubpd", 0x66, 0, 0xD0, -1, DATA_B);
        gen_vex("vhaddpd", 1, 1, 0x7C, -1, -1, 0);

        /* ---- dot product -------------------------------------------------- */
        for (int i = 0; i < N_DOT_IMMS; i++) {
            gen_legacy("dpps", 0x66, 0x3A, 0x40, kDotImms[i], -1);
            gen_legacy("dppd", 0x66, 0x3A, 0x41, kDotImms[i], -1);
        }
        gen_legacy("dpps", 0x66, 0x3A, 0x40, 0xF1, DATA_B);
        gen_vex("vdpps", 1, 3, 0x40, 0xF1, -1, 0);

        /* ---- odds and ends ------------------------------------------------ */
        for (int imm = 0; imm < 8; imm++) {
            gen_legacy("mpsadbw", 0x66, 0x3A, 0x42, imm, -1);
        }
        gen_legacy("mpsadbw", 0x66, 0x3A, 0x42, 3, DATA_B);
        gen_legacy("movntdqa", 0x66, 0x38, 0x2A, -1, DATA_B);
    }

    printf("};\n");
    fprintf(stderr, "%d rows, %d skipped\n", g_rows, g_skipped);
    return 0;
}
