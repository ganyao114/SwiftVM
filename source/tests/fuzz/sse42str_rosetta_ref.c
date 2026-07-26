// ===========================================================================
// SSE4.2 string-compare reference generator -- real x86-64 under Rosetta.
// ===========================================================================
//
// PCMPISTRI / PCMPISTRM / PCMPESTRI / PCMPESTRM and their VEX twins.  Same
// oracle and the same traps as sse4_rosetta_ref.c:
//
//   * Rosetta does NOT advertise AVX through CPUID unless the process starts
//     with ROSETTA_ADVERTISE_AVX=1.  Execution works either way, so support is
//     decided by EXECUTING an instruction and catching SIGILL, never by
//     reading CPUID.  AVX is needed here for the prologue/epilogue (poisoning
//     and reading back bits 255:128 of ymm0 is how the legacy-preservation
//     contract is measured) and for the VEX rows.
//   * Rosetta is an emulator with its own defects, so a Rosetta result is
//     evidence, not proof.  sse42str_test.cpp re-derives EVERY row from an
//     independent from-the-SDM model and requires the two to agree, so a
//     Rosetta defect shows up as a disagreement rather than being absorbed.
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// ----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/s42ref sse42str_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/s42ref > sse42str_rosetta_ref.inc
//
// Add --dump-encodings to print each stub's bytes to stderr for auditing
// against a disassembler.
//
// WHAT ONE ROW IS
// ---------------
// Each row carries the LITERAL instruction bytes hardware executed.  The test
// replays those bytes wrapped in a FIXED prefix and suffix that this file also
// prints (kSse42StrPrefix / kSse42StrSuffix), so neither side ever re-encodes
// anything:
//
//   prefix   push 0xAD7 ; popfq        -- every one of CF/PF/AF/ZF/SF/OF set
//                                         to 1, so a flag the handler forgets
//                                         to write is stuck at 1 and every row
//                                         whose true value is 0 catches it
//   <the instruction under test>
//   suffix   pushfq ; pop rsi ; and rsi, 0x8D5
//                                      -- the six architecturally defined
//                                         flags and nothing else (bit 1 and
//                                         IF are masked off: SwiftVM does not
//                                         model IF and real EFLAGS carries
//                                         host-dependent bits above them)
//
// THE REGISTER ASSIGNMENT
// -----------------------
//   ymm0  poisoned in BOTH halves.  It is the implicit destination of the
//         mask forms, and its high half is where contract C3 is measured: a
//         legacy pcmpistrm must leave the poison, a VEX vpcmpistrm must zero
//         it, and every index form must leave all 32 bytes alone.
//   ymm1  A, the first operand (ModRM.reg)
//   ymm2  B, the second operand (ModRM.rm) for the register form; the memory
//         form reads the same bytes from [rdi + DATA_B]
//   rax   the first operand's explicit length (or a poison constant)
//   rdx   the second operand's explicit length (or a poison constant)
//   rcx   poisoned; the index forms write ECX, so bits 63:32 must come back
//         ZERO and an implementation writing RCX is caught
//   rbx   poisoned and never written by anything here
//
// The observation block is 128 bytes and the same for every row:
//
//   +0x00  ymm0  (32)   the mask destination / the C3 witness
//   +0x20  rcx   (8)    the index destination
//   +0x28  rsi   (8)    EFLAGS & 0x8D5
//   +0x30  rax   (8)    must be unchanged
//   +0x38  rdx   (8)    must be unchanged
//   +0x40  ymm1  (32)   the first operand: must be unchanged
//   +0x60  ymm2  (32)   the second operand: must be unchanged
//
// ALL MEMORY OPERANDS USE disp32, never disp8 -- see avx_misc_rosetta_ref.c
// for the disp8 sign-extension trap that motivates the rule.

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
typedef int64_t s64;

#define DATA_A 0x000 /* 32 */
#define DATA_B 0x020 /* 32 */
#define DATA_K 0x040 /* 32: the ymm0 poison */
#define DATA_O 0x080 /* 128: observation */
#define DATA_SIZE 0x200

#define K_POISON_LO(i) ((u8)(0xC3u ^ (unsigned)(i)))
#define K_POISON_HI(i) ((u8)(0x5Au ^ (unsigned)(i)))
#define RCX_POISON 0x1122334455667788ull
#define RBX_POISON 0xBBBBBBBBBBBBBBBBull
#define RAX_POISON 0xAAAAAAAAAAAAAAAAull
#define RDX_POISON 0xDDDDDDDDDDDDDDDDull

#define FLAGS_PRESET 0xAD7u /* CF PF AF ZF SF OF all 1, plus bit1 and IF */
#define FLAGS_MASK 0x8D5u   /* CF PF AF ZF SF OF */

/* ------------------------------------------------------------------------ */
/* Input pairs                                                               */
/* ------------------------------------------------------------------------ */
typedef struct {
    const char* name;
    u8 a[32];
    u8 b[32];
} Pair;

static Pair g_pairs[24];
static int g_npairs;

static Pair* new_pair(const char* name) {
    Pair* p = &g_pairs[g_npairs++];
    memset(p, 0, sizeof(*p));
    p->name = name;
    /* Bytes 16..31 are NEVER architecturally read.  Filling them with data
       that would change every aggregation if it were read turns "the handler
       looked at 256 bits" into a wrong answer instead of a silent pass. */
    for (int i = 16; i < 32; i++) {
        p->a[i] = (u8)(0x11u + (unsigned)i);
        p->b[i] = (u8)(0x91u - (unsigned)i);
    }
    return p;
}

static void put_bytes(u8* dst, const char* s, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = (u8)s[i];
    }
}

static void put_word(u8* dst, int i, u16 v) {
    dst[i * 2] = (u8)v;
    dst[i * 2 + 1] = (u8)(v >> 8);
}

static void build_pairs(void) {
    Pair* p;

    /* Identical, no terminator anywhere: every element valid on both sides. */
    p = new_pair("same");
    put_bytes(p->a, "ABCDEFGHIJKLMNOP", 16);
    put_bytes(p->b, "ABCDEFGHIJKLMNOP", 16);

    /* One difference, at index 3 -- the classic strcmp answer. */
    p = new_pair("diff3");
    put_bytes(p->a, "ABCDEFGHIJKLMNOP", 16);
    put_bytes(p->b, "ABCxEFGHIJKLMNOP", 16);

    /* Terminators at DIFFERENT indices on the two sides (5 and 10), which is
       what separates len1 from len2 and therefore SF from ZF. */
    p = new_pair("nul5_10");
    put_bytes(p->a, "ABCDE\0GHIJKLMNO", 16);
    put_bytes(p->b, "ABCDEFGHIJ\0LMNO", 16);

    /* "equal any": a character SET in the first operand, text in the second. */
    p = new_pair("set");
    put_bytes(p->a, "aeiou\0\0\0\0\0\0\0\0\0\0", 16);
    put_bytes(p->b, "the quick brown ", 16);

    /* "ranges": three ranges a-z, A-Z, 0-9 in the first operand. */
    p = new_pair("range");
    put_bytes(p->a, "azAZ09\0\0\0\0\0\0\0\0\0", 16);
    put_bytes(p->b, "aQ7#z~ M9\0BbCc", 15);

    /* "equal ordered": a needle with several matches, one of them at 0. */
    p = new_pair("sub");
    put_bytes(p->a, "abc\0\0\0\0\0\0\0\0\0\0\0\0", 16);
    put_bytes(p->b, "abxabcxxabcxabc", 15);

    /* The needle matches only by running PAST the end of the haystack, which
       is the (valid, invalid) -> force-TRUE row of the override table and the
       only reason glibc's strstr works. */
    p = new_pair("tail");
    put_bytes(p->a, "OPQ\0\0\0\0\0\0\0\0\0\0\0\0", 16);
    put_bytes(p->b, "ABCDEFGHIJKLMNOP", 16);

    /* Everything invalid on both sides: len1 = len2 = 0. */
    new_pair("zeros");

    /* Signed vs unsigned: 0x80 and 0xFF are the largest UNSIGNED and the
       smallest/largest-negative SIGNED bytes, so the two data formats give
       different ranges answers. */
    p = new_pair("sign");
    {
        static const u8 a[16] = {0x80, 0x7F, 0xFF, 0x01, 0x00, 0x40, 0xC0, 0x20,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        static const u8 b[16] = {0x80, 0x7F, 0xFF, 0x01, 0x81, 0x7E, 0xFE, 0x02,
                                 0x40, 0xC0, 0x20, 0x60, 0xA0, 0x11, 0xEE, 0x33};
        memcpy(p->a, a, 16);
        memcpy(p->b, b, 16);
    }

    /* Word extremes, no zero word. */
    p = new_pair("words");
    for (int i = 0; i < 8; i++) {
        static const u16 av[8] = {0x8000, 0x7FFF, 0xFFFF, 0x0001,
                                  0x0100, 0x8001, 0x7F00, 0x00FF};
        static const u16 bv[8] = {0x8000, 0x7FFF, 0xFFFE, 0x0001,
                                  0x0101, 0x8002, 0x7F01, 0x00FE};
        put_word(p->a, i, av[i]);
        put_word(p->b, i, bv[i]);
    }

    /* A zero BYTE early and a zero WORD late, at different indices, so a
       handler that scans for a terminator at the wrong granularity gets a
       different length on every one of the four data formats. */
    p = new_pair("wnul");
    {
        static const u16 av[8] = {0x0102, 0x0300, 0x0405, 0x0607,
                                  0x0000, 0x0809, 0x0A0B, 0x0C0D};
        static const u16 bv[8] = {0x0102, 0x0304, 0x0500, 0x0607,
                                  0x0809, 0x0000, 0x0A0B, 0x0C0D};
        for (int i = 0; i < 8; i++) {
            put_word(p->a, i, av[i]);
            put_word(p->b, i, bv[i]);
        }
    }

    /* Every byte names its own position, in opposite directions. */
    p = new_pair("seq");
    for (int i = 0; i < 16; i++) {
        p->a[i] = (u8)(i + 1);
        p->b[i] = (u8)(16 - i);
    }
}

/* ------------------------------------------------------------------------ */
/* Explicit-length combinations                                              */
/* ------------------------------------------------------------------------ */
/* Written as full 64-bit patterns so the 32-bit (EAX/EDX) and the REX.W
   (RAX/RDX) forms of the same row are genuinely different inputs.  Combos
   12 and 13 exist only to separate them: with EAX = 3 but RAX = 0x100000003,
   a handler that reads the wrong width is off by 13. */
static const u64 g_lens[][2] = {
        {0, 0},
        {3, 5},
        {5, 3},
        {16, 16},
        {8, 8},
        {20, 20},
        {0xFFFFFFFFFFFFFFFDull, 0xFFFFFFFFFFFFFFFBull}, /* -3, -5 */
        {0xFFFFFFFFFFFFFFECull, 7},                     /* -20, 7 */
        {1, 16},
        {16, 1},
        {0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull}, /* -1, -1 */
        {0xFFFFFFFF80000000ull, 4},                     /* INT32_MIN, sign-extended */
        {0x0000000100000003ull, 0x0000000200000005ull}, /* 32 vs 64 bit discriminator */
        {0x8000000000000000ull, 4},                     /* INT64_MIN */
        {7, 0},
        {2, 9},
};
#define NLENS ((int)(sizeof(g_lens) / sizeof(g_lens[0])))

/* ------------------------------------------------------------------------ */
/* Encoder                                                                   */
/* ------------------------------------------------------------------------ */
typedef struct {
    u8 b[512];
    int n;
    int mark;    /* first replayed byte */
    int ins_at;  /* first byte of the instruction itself */
    int ins_len;
} Code;

static void emit(Code* c, u8 x) { c->b[c->n++] = x; }
static void emit32(Code* c, u32 v) {
    emit(c, (u8)v);
    emit(c, (u8)(v >> 8));
    emit(c, (u8)(v >> 16));
    emit(c, (u8)(v >> 24));
}
static void modrm_mem(Code* c, int reg, int disp) {
    emit(c, (u8)(0x80 | ((reg & 7) << 3) | 7)); /* [rdi + disp32] */
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
static void mov_imm64(Code* c, int reg, u64 v) {
    emit(c, 0x48);
    emit(c, (u8)(0xB8 + (reg & 7)));
    for (int i = 0; i < 8; i++) {
        emit(c, (u8)(v >> (i * 8)));
    }
}
static void st_gpr(Code* c, int reg, int disp) {
    emit(c, 0x48);
    emit(c, 0x89);
    modrm_mem(c, reg, disp);
}

/* Encoded register numbers for `mov r64, imm64` / `mov [mem], r64`. */
#define R_RAX 0
#define R_RCX 1
#define R_RDX 2
#define R_RBX 3
#define R_RSI 6

static void prologue(Code* c, u64 rax, u64 rdx) {
    ld256(c, 0, DATA_K);
    ld256(c, 1, DATA_A);
    ld256(c, 2, DATA_B);
    mov_imm64(c, R_RAX, rax);
    mov_imm64(c, R_RDX, rdx);
    mov_imm64(c, R_RCX, RCX_POISON);
    mov_imm64(c, R_RBX, RBX_POISON);
    c->mark = c->n;
    /* push 0xAD7 ; popfq */
    emit(c, 0x68);
    emit32(c, FLAGS_PRESET);
    emit(c, 0x9D);
}

static void capture_flags(Code* c) {
    emit(c, 0x9C); /* pushfq */
    emit(c, 0x5E); /* pop rsi */
    emit(c, 0x48); /* and rsi, imm32 */
    emit(c, 0x81);
    emit(c, 0xE6);
    emit32(c, FLAGS_MASK);
}

static void epilogue(Code* c) {
    st256(c, 0, DATA_O + 0x00);
    st_gpr(c, R_RCX, DATA_O + 0x20);
    st_gpr(c, R_RSI, DATA_O + 0x28);
    st_gpr(c, R_RAX, DATA_O + 0x30);
    st_gpr(c, R_RDX, DATA_O + 0x38);
    st256(c, 1, DATA_O + 0x40);
    st256(c, 2, DATA_O + 0x60);
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

/* Finish `c`, run it against pair `pi`, print the row. */
static void row(const char* mnemonic, int pi, int imm, int lens, int mem, Code* c) {
    char enc[200];
    hexbytes(enc, c->b + c->ins_at, c->ins_len);
    capture_flags(c);
    epilogue(c);
    if (g_dump) {
        char all[1100];
        hexbytes(all, c->b, c->n);
        fprintf(stderr, "%-12s pair%-2d imm%02x lens%-3d mem%d  %s  (full %s)\n", mnemonic, pi,
                imm, lens, mem, enc, all);
        return;
    }
    memset(g_data, 0, DATA_SIZE);
    memcpy(g_data + DATA_A, g_pairs[pi].a, 32);
    memcpy(g_data + DATA_B, g_pairs[pi].b, 32);
    for (int i = 0; i < 16; i++) {
        g_data[DATA_K + i] = K_POISON_LO(i);
        g_data[DATA_K + 16 + i] = K_POISON_HI(i);
    }
    memset(g_data + DATA_O, 0x99, 128);
    if (!run_stub(c)) {
        printf("    // SKIP %s pair%d(%s) imm=%02x lens=%d mem=%d: Rosetta refused this "
               "encoding\n",
               mnemonic, pi, g_pairs[pi].name, imm, lens, mem);
        g_skipped++;
        return;
    }
    char out[300];
    hexbytes(out, g_data + DATA_O, 128);
    printf("    {\"%s\", %d, %d, %d, %d, \"%s\", \"%s\"},\n", mnemonic, pi, imm, lens, mem, enc,
           out);
    g_rows++;
}

/* ------------------------------------------------------------------------ */
/* Per-shape emission                                                        */
/* ------------------------------------------------------------------------ */
/* op: 0x60..0x63.  mem >= 0 selects [rdi+mem] as the r/m operand.
   lens < 0 means the implicit form (rax/rdx get poison constants). */
static void gen_legacy(const char* name, int op, int pi, int imm, int lens, int mem, int wide) {
    Code c;
    c.n = 0;
    const u64 rax = lens < 0 ? RAX_POISON : g_lens[lens][0];
    const u64 rdx = lens < 0 ? RDX_POISON : g_lens[lens][1];
    prologue(&c, rax, rdx);
    c.ins_at = c.n;
    emit(&c, 0x66);
    if (wide) emit(&c, 0x48);
    emit(&c, 0x0F);
    emit(&c, 0x3A);
    emit(&c, (u8)op);
    if (mem >= 0) {
        modrm_mem(&c, 1, mem);
    } else {
        modrm_reg(&c, 1, 2);
    }
    emit(&c, (u8)imm);
    c.ins_len = c.n - c.ins_at;
    row(name, pi, imm, lens, mem >= 0 ? 1 : 0, &c);
}

static void gen_vex(const char* name, int op, int pi, int imm, int lens, int mem, int w) {
    Code c;
    c.n = 0;
    const u64 rax = lens < 0 ? RAX_POISON : g_lens[lens][0];
    const u64 rdx = lens < 0 ? RDX_POISON : g_lens[lens][1];
    prologue(&c, rax, rdx);
    c.ins_at = c.n;
    vex3(&c, 1, 3, 0, 0, w); /* VEX.128.66.0F3A, vvvv = 1111b */
    emit(&c, (u8)op);
    if (mem >= 0) {
        modrm_mem(&c, 1, mem);
    } else {
        modrm_reg(&c, 1, 2);
    }
    emit(&c, (u8)imm);
    c.ins_len = c.n - c.ins_at;
    row(name, pi, imm, lens, mem >= 0 ? 1 : 0, &c);
}

/* ------------------------------------------------------------------------ */
int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-encodings") == 0) g_dump = 1;
    }
    build_pairs();
    signal(SIGILL, on_sigill);
    g_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    g_data = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_page == MAP_FAILED || g_data == MAP_FAILED) {
        perror("mmap");
        return 2;
    }

    printf("// Generated by sse42str_rosetta_ref.c under Rosetta 2.  Do not edit.\n");
    printf("// Wrapper the test must replay around every row's `enc`:\n");
    {
        /* Print the fixed prefix/suffix as literal bytes so the test never
           re-encodes them either. */
        Code c;
        c.n = 0;
        emit(&c, 0x68);
        emit32(&c, FLAGS_PRESET);
        emit(&c, 0x9D);
        char pre[64];
        hexbytes(pre, c.b, c.n);
        c.n = 0;
        capture_flags(&c);
        char suf[64];
        hexbytes(suf, c.b, c.n);
        printf("static const char* const kSse42StrPrefix = \"%s\";\n", pre);
        printf("static const char* const kSse42StrSuffix = \"%s\";\n", suf);
    }
    printf("static const Sse42StrRef kSse42StrRefs[] = {\n");

    /* Spot imm8 values, chosen so that between them every aggregation, every
       polarity, both index directions, both mask forms and all four data
       formats appear at least once. */
    static const int kSpotImm[] = {0x00, 0x01, 0x0C, 0x1A, 0x14, 0x3D, 0x40, 0x4C, 0x55, 0x7B};
    static const int kLenImm[] = {0x00, 0x0D, 0x1A, 0x4C};

    /* ---- 1. the full imm8 sweep, implicit lengths, register form -------- */
    for (int pi = 0; pi < g_npairs; pi++) {
        for (int imm = 0; imm < 128; imm++) {
            gen_legacy("pcmpistri", 0x63, pi, imm, -1, -1, 0);
            gen_legacy("pcmpistrm", 0x62, pi, imm, -1, -1, 0);
        }
    }

    /* ---- 2. the memory form of the second operand ----------------------- */
    for (int pi = 0; pi < g_npairs; pi++) {
        for (int k = 0; k < 5; k++) {
            gen_legacy("pcmpistri", 0x63, pi, kSpotImm[k], -1, DATA_B, 0);
            gen_legacy("pcmpistrm", 0x62, pi, kSpotImm[k], -1, DATA_B, 0);
        }
    }

    /* ---- 3. imm8 bit 7 is reserved: hardware must ignore it ------------- */
    for (int pi = 0; pi < 4; pi++) {
        for (int k = 0; k < 6; k++) {
            gen_legacy("pcmpistri", 0x63, pi, kSpotImm[k] | 0x80, -1, -1, 0);
            gen_legacy("pcmpistrm", 0x62, pi, kSpotImm[k] | 0x80, -1, -1, 0);
        }
    }

    /* ---- 4. the full imm8 sweep, explicit lengths ------------------------ */
    for (int pi = 0; pi < 2; pi++) {
        for (int lens = 1; lens <= 6; lens += 5) { /* combos 1 and 6: positive and negative */
            for (int imm = 0; imm < 128; imm++) {
                gen_legacy("pcmpestri", 0x61, pi, imm, lens, -1, 0);
                gen_legacy("pcmpestrm", 0x60, pi, imm, lens, -1, 0);
            }
        }
    }

    /* ---- 5. every length combination, both widths ------------------------ */
    for (int pi = 0; pi < 3; pi++) {
        for (int lens = 0; lens < NLENS; lens++) {
            for (int k = 0; k < (int)(sizeof(kLenImm) / sizeof(kLenImm[0])); k++) {
                for (int wide = 0; wide < 2; wide++) {
                    gen_legacy("pcmpestri", 0x61, pi, kLenImm[k], lens, -1, wide);
                    gen_legacy("pcmpestrm", 0x60, pi, kLenImm[k], lens, -1, wide);
                }
            }
        }
    }

    /* ---- 6. the VEX twins ------------------------------------------------ */
    for (int pi = 0; pi < 2; pi++) {
        for (int imm = 0; imm < 128; imm++) {
            gen_vex("vpcmpistri", 0x63, pi, imm, -1, -1, 0);
            gen_vex("vpcmpistrm", 0x62, pi, imm, -1, -1, 0);
        }
    }
    for (int pi = 0; pi < 2; pi++) {
        for (int k = 0; k < (int)(sizeof(kLenImm) / sizeof(kLenImm[0])); k++) {
            for (int lens = 0; lens < NLENS; lens++) {
                for (int w = 0; w < 2; w++) {
                    gen_vex("vpcmpestri", 0x61, pi, kLenImm[k], lens, -1, w);
                    gen_vex("vpcmpestrm", 0x60, pi, kLenImm[k], lens, -1, w);
                }
            }
        }
    }
    /* The VEX memory form, so the VEX address path is exercised too. */
    for (int pi = 0; pi < 8; pi++) {
        for (int k = 0; k < 3; k++) {
            gen_vex("vpcmpistri", 0x63, pi, kSpotImm[k], -1, DATA_B, 0);
            gen_vex("vpcmpistrm", 0x62, pi, kSpotImm[k], -1, DATA_B, 0);
        }
    }

    printf("};\n");
    printf("// rows=%d skipped=%d pairs=%d\n", g_rows, g_skipped, g_npairs);

    printf("static const Sse42StrPair kSse42StrPairs[] = {\n");
    for (int i = 0; i < g_npairs; i++) {
        char a[80], b[80];
        hexbytes(a, g_pairs[i].a, 32);
        hexbytes(b, g_pairs[i].b, 32);
        printf("    {\"%s\", \"%s\", \"%s\"},\n", g_pairs[i].name, a, b);
    }
    printf("};\n");
    printf("static const unsigned long long kSse42StrLens[][2] = {\n");
    for (int i = 0; i < NLENS; i++) {
        printf("    {0x%016llxull, 0x%016llxull},\n", (unsigned long long)g_lens[i][0],
               (unsigned long long)g_lens[i][1]);
    }
    printf("};\n");

    fprintf(stderr, "rows=%d skipped=%d\n", g_rows, g_skipped);
    return 0;
}
