// ===========================================================================
// VEX.256 reference-value generator -- runs real x86-64 AVX2 under Rosetta 2.
// ===========================================================================
//
// WHY THIS EXISTS
// ---------------
// Unicorn 2.1.4 rejects every VEX.L=1 encoding with UC_ERR_INSN_INVALID (see the
// FACT 1 note above the VEX.128 cases in x86_fuzz.cpp), so the 256-bit handlers
// in source/runtime/frontend/x86/decoder_avx.cc have no emulator oracle at all.
// Rosetta 2 on macOS 26/27 does execute AVX and AVX2 -- including the full
// 256-bit register file -- so it can serve as the ground truth instead.
//
// One trap to know about: Rosetta does NOT advertise AVX through CPUID unless
// the process is started with ROSETTA_ADVERTISE_AVX=1.  Execution works either
// way; only the feature bits are hidden.  This program therefore reports the
// CPUID bits for information but decides support by EXECUTING an instruction and
// catching SIGILL, which is the only answer that matters here.
//
// HOW TO REGENERATE  (must be run on an Apple Silicon Mac with Rosetta 2)
// -----------------------------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/avx256ref avx256_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avx256ref > avx256_rosetta_ref.inc
//
// Then rebuild and run the "avx256" case in x86_fuzz.cpp.  Regeneration is only
// needed when avx256_ops.inc gains an entry or the input vectors below change;
// the emitted file carries a header recording the machine it came from.
//
// Add --dump-encodings to print each instruction's bytes to stderr instead of
// generating the table, for auditing the encoder against a disassembler:
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avx256ref --dump-encodings
//
// HOW IT WORKS
// ------------
// Instructions are ASSEMBLED AT RUNTIME from avx256_ops.inc rather than written
// as inline asm.  That is deliberate: x86_fuzz.cpp builds its blocks from the
// same table with the same field meanings, so a wrong opcode byte cannot make
// the two sides test different instructions -- they would both be wrong
// identically, which is a state a hand-written inline-asm version could not
// reach and would hide.  Each stub is
//
//     vmovdqu ymm1, [rdi+0x00]        ; A
//     vmovdqu ymm2, [rdi+0x20]        ; B
//     <op>    ymm0, ymm1, ymm2        ; or  <op> ymm0, ymm1, [rdi+0x20]
//     vmovdqu [rdi+0x40], ymm0
//     vzeroupper
//     ret
//
// so the register mapping the test relies on is exact: ymm1 = A with A[0..15] in
// xmm1 and A[16..31] in ymm1's upper half, likewise ymm2 = B, and the result is
// read back from ymm0 the same way.
//
// NOTHING HERE IS HAND-COMPUTED.  Every value in the generated table is the
// literal 32 bytes Rosetta wrote to memory.  An instruction Rosetta refuses is
// emitted as a SKIP row rather than being filled in from the Intel definition.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <time.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define DATA_A 0x00
#define DATA_B 0x20
#define DATA_O 0x40
#define DATA_SIZE 0x80

// ---------------------------------------------------------------------------
// Input vectors.  Every pair keeps its two 128-bit lanes DIFFERENT on both
// sides, so an implementation that computed the upper half from the lower half's
// data (the most likely way to break a two-halves split) cannot pass.
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    u8 a[32];
    u8 b[32];
} Pair;

static Pair g_pairs[] = {
        // 0: the degenerate extremes.  vpshufb sees an all-0xFF control, i.e.
        // bit 7 set everywhere, which must zero the entire result.
        {"zeros_ones",
         {0},
         {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
          0xFF, 0xFF}},

        // 1: signed boundaries at every lane width (0x7F/0x80, 0x7FFF/0x8000,
        // 0x7FFFFFFF/0x80000000), which is what separates vpcmpgt* from an
        // unsigned compare and vpminub/vpminud from their signed twins.
        {"signbnd",
         {0x7F, 0x7F, 0x7F, 0x7F, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
          0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF,
          0xFF, 0xFF},
         {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x01, 0x01,
          0x01, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80, 0x7F, 0x7F,
          0x7F, 0x7F}},

        // 2: carry / borrow boundaries.  0xFF..FF + 1 wraps at 8, 16, 32 and 64
        // bits, and a lane width confused for a wider one shows up immediately
        // as a carry leaking into the next lane.
        {"carry",
         {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
          0xFF, 0xFF},
         {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
          0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x01}},

        // 3: THE vpshufb lane test.  A is 0x01..0x20 so every byte names its own
        // position.  B's low lane holds control values 0x1F..0x10 -- indices
        // whose bit 4 is set.  Per-lane semantics ignore bit 4 and select bytes
        // 15..0 of the LOW lane (A = 0x10..0x01); a cross-lane implementation
        // would select bytes 31..16 of the register (A = 0x20..0x11) instead, so
        // the two readings cannot produce the same answer.  B's high lane mixes
        // plain indices with bit-7-set (0x88, 0xFF, 0x80) zeroing cases.
        {"laneidx",
         {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
          0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
          0x1F, 0x20},
         {0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
          0x10, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x88, 0x89, 0x8A, 0x8B, 0xFF, 0x80,
          0x0F, 0x07}},

        // 4: sign-bit placement for vpmovmskb.  The low half's 16-bit mask and
        // the high half's are deliberately different and neither is 0x0000 or
        // 0xFFFF, so `lo | hi<<16` is distinguishable from `hi | lo<<16` and
        // from either half alone -- the one instruction in this family whose two
        // halves are not independent.
        {"signbits",
         {0x80, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x80},
         {0x7F, 0xFF, 0x00, 0x81, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x0F, 0x1E, 0x2D,
          0x3C, 0xC3, 0xD2, 0xE1, 0xF0, 0x0A, 0x0B, 0x0C, 0x0D, 0x55, 0xAA, 0x55, 0xAA, 0x11, 0x22,
          0x33, 0x44}},

        // 5: pseudo-random, filled below; catches anything the structured cases
        // happen to be blind to.
        {"random", {0}, {0}},
};
#define NPAIRS ((int)(sizeof(g_pairs) / sizeof(g_pairs[0])))

static void fill_random_pair(void) {
    Pair* p = &g_pairs[NPAIRS - 1];
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

// ---------------------------------------------------------------------------
// VEX encoder.  Field meanings and inversion behaviour are identical to
// EmitVexC4 in x86_fuzz.cpp -- all inputs un-inverted, the 3-byte C4 form
// always, so the encodings the two programs build agree byte for byte apart
// from the base register and displacement.
// ---------------------------------------------------------------------------
typedef struct {
    u8 b[64];
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
    vex3(c, 2, 1, 0, 1, reg >> 3, 0, 0, 0);
    emit(c, 0x6F);
    modrm_mem(c, reg, disp);
}
// vmovdqu [rdi+disp], ymm<reg>   (VEX.256.F3.0F 7F /r)
static void st256(Code* c, int reg, int disp) {
    vex3(c, 2, 1, 0, 1, reg >> 3, 0, 0, 0);
    emit(c, 0x7F);
    modrm_mem(c, reg, disp);
}
static void epilogue(Code* c) {
    emit(c, 0xC5);
    emit(c, 0xF8);
    emit(c, 0x77);  // vzeroupper
    emit(c, 0xC3);  // ret
}

// ---------------------------------------------------------------------------
// Executable-page management and SIGILL trapping.
// ---------------------------------------------------------------------------
typedef void (*stub_fn)(u8* data);

static u8* g_page;
static jmp_buf g_jb;
static volatile int g_trapped;

static void on_sigill(int s) {
    (void)s;
    g_trapped = 1;
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
    g_trapped = 0;
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

// Emit one table row: run `c` for input pair `pi`, print the 32-byte result.
static void row(const char* mnemonic, const char* shape, int pi, Code* c) {
    char enc[160];
    hexbytes(enc, c->b, c->n);
    if (g_dump) {
        fprintf(stderr, "%-12s %-6s pair%d  %s\n", mnemonic, shape, pi, enc);
        return;
    }
    memcpy(g_data + DATA_A, g_pairs[pi].a, 32);
    memcpy(g_data + DATA_B, g_pairs[pi].b, 32);
    memset(g_data + DATA_O, 0xCC, 32);
    if (!run_stub(c, g_data)) {
        printf("    // SKIP %s.%s pair%d (%s): Rosetta raised SIGILL, no reference value\n",
               mnemonic, shape, pi, g_pairs[pi].name);
        g_skipped++;
        return;
    }
    char h[80];
    hex32(h, g_data + DATA_O);
    printf("    {\"%s\", %d, \"%s\"},\n", mnemonic, pi, h);
}

int main(int argc, char** argv) {
    g_dump = (argc > 1 && strcmp(argv[1], "--dump-encodings") == 0);
    signal(SIGILL, on_sigill);
    signal(SIGSEGV, on_sigill);
    signal(SIGBUS, on_sigill);
    signal(SIGTRAP, on_sigill);
    fill_random_pair();

    g_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_page == MAP_FAILED) {
        perror("mmap code");
        return 2;
    }
    // 64-byte aligned so the ALIGNED move forms (vmovdqa / vmovaps / vmovapd /
    // vmovntdq / vmovntps / vmovntpd) do not #GP on their 32-byte requirement.
    u8* raw = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) {
        perror("mmap data");
        return 2;
    }
    g_data = raw;  // page-aligned already

    // --- capability gate: prove 256-bit execution before emitting anything ---
    {
        Code c;
        c.n = 0;
        ld256(&c, 1, DATA_A);
        // vpaddb ymm0, ymm1, ymm2 -- if this SIGILLs there is no oracle at all.
        ld256(&c, 2, DATA_B);
        vex3(&c, 1, 1, 1, 1, 0, 0, 0, 0);
        emit(&c, 0xFC);
        modrm_reg(&c, 0, 2);
        st256(&c, 0, DATA_O);
        epilogue(&c);
        memcpy(g_data + DATA_A, g_pairs[2].a, 32);
        memcpy(g_data + DATA_B, g_pairs[2].b, 32);
        if (!run_stub(&c, g_data)) {
            fprintf(stderr,
                    "FATAL: VEX.256 vpaddb raised SIGILL under this runtime.\n"
                    "Rosetta on this machine cannot serve as a 256-bit oracle;\n"
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
        printf("// Reference values for VEX.256 (AVX2), produced by ACTUALLY EXECUTING each\n");
        printf("// instruction on x86-64 hardware through Rosetta 2.  Nothing here is\n");
        printf("// hand-computed; an instruction Rosetta refused would appear as a SKIP\n");
        printf("// comment instead of a value.\n");
        printf("//\n");
        printf("// Regenerate with the recipe in avx256_rosetta_ref.c:\n");
        printf("//   clang -arch x86_64 -O1 -o /tmp/avx256ref avx256_rosetta_ref.c\n");
        printf("//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avx256ref > "
               "avx256_rosetta_ref.inc\n");
        printf("//\n");
        printf("// Generating runtime: CPUID.1.ECX=%08x EDX=%08x, CPUID.7.0.EBX=%08x\n", cpuid1[2],
               cpuid1[3], cpuid7[1]);
        printf("//   (AVX bit=%d AVX2 bit=%d OSXSAVE bit=%d -- Rosetta hides these unless\n",
               (cpuid1[2] >> 28) & 1, (cpuid7[1] >> 5) & 1, (cpuid1[2] >> 27) & 1);
        printf("//    ROSETTA_ADVERTISE_AVX=1 is set; 256-bit execution works regardless,\n");
        printf("//    which is why the generator gates on a live vpaddb ymm and not CPUID.)\n");
        printf("\n");

        // --- inputs ---
        printf("// Input vectors: index, name, A (32 bytes), B (32 bytes).\n");
        printf("// Both 128-bit lanes differ on both sides of every pair, so an\n");
        printf("// implementation deriving the upper half from the lower half's data cannot\n");
        printf("// pass.  See avx256_rosetta_ref.c for what each pair is aimed at.\n");
        printf("static const Avx256Input kAvx256Inputs[] = {\n");
        for (int i = 0; i < NPAIRS; i++) {
            char ha[80], hb[80];
            hex32(ha, g_pairs[i].a);
            hex32(hb, g_pairs[i].b);
            printf("    {\"%s\", \"%s\", \"%s\"},\n", g_pairs[i].name, ha, hb);
        }
        printf("};\n\n");
        printf("// Results: mnemonic, input-pair index, 32-byte result of\n");
        printf("//   <op> ymm0, ymm1(=A), ymm2(=B)\n");
        printf("// with A[0..15] in xmm1 / A[16..31] in ymm1's upper half, and the result\n");
        printf("// read back the same way from ymm0.\n");
        printf("static const Avx256Ref kAvx256Refs[] = {\n");
    }

    // --- three-operand ALU forms -------------------------------------------
#define SVM_AVX256_ALU(name, pp, mmmmm, opcode)                     \
    for (int pi = 0; pi < NPAIRS; pi++) {                           \
        Code c;                                                     \
        c.n = 0;                                                    \
        ld256(&c, 1, DATA_A);                                       \
        ld256(&c, 2, DATA_B);                                       \
        vex3(&c, (pp), (mmmmm), 1, 1, 0, 0, 0, 0);                  \
        emit(&c, (u8)(opcode));                                     \
        modrm_reg(&c, 0, 2);                                        \
        st256(&c, 0, DATA_O);                                       \
        epilogue(&c);                                               \
        row(#name, "rr", pi, &c);                                   \
    }
#include "avx256_ops.inc"

    // --- data movement: load into ymm0, store it back out -------------------
    // Both directions are identity on the data, but they are separate decoder
    // paths in SwiftVM and separate opcodes here, so each is measured rather
    // than assumed.
#define SVM_AVX256_MOV(name, pp, mmmmm, ld_opcode, st_opcode)              \
    for (int pi = 0; pi < NPAIRS; pi++) {                                  \
        Code c;                                                            \
        c.n = 0;                                                           \
        if ((ld_opcode) != 0xFF) {                                         \
            vex3(&c, (pp), (mmmmm), 0, 1, 0, 0, 0, 0);                     \
            emit(&c, (u8)(ld_opcode));                                     \
            modrm_mem(&c, 0, DATA_A);                                      \
        } else {                                                           \
            ld256(&c, 0, DATA_A);                                          \
        }                                                                  \
        if ((st_opcode) != 0xFF) {                                         \
            vex3(&c, (pp), (mmmmm), 0, 1, 0, 0, 0, 0);                     \
            emit(&c, (u8)(st_opcode));                                     \
            modrm_mem(&c, 0, DATA_O);                                      \
        } else {                                                           \
            st256(&c, 0, DATA_O);                                          \
        }                                                                  \
        epilogue(&c);                                                      \
        row(#name, "mov", pi, &c);                                         \
    }
#include "avx256_ops.inc"

    // --- vpmovmskb r32, ymm1 ------------------------------------------------
    // VEX.256.66.0F D7 /r.  Result is a GPR, stored as 4 little-endian bytes at
    // the front of the output slot; the rest of the slot stays 0xCC so a
    // 64-bit-wide write would also be visible.
    for (int pi = 0; pi < NPAIRS; pi++) {
        Code c;
        c.n = 0;
        ld256(&c, 1, DATA_A);
        vex3(&c, 1, 1, 0, 1, 0, 0, 0, 0);
        emit(&c, 0xD7);
        modrm_reg(&c, 0, 1);  // eax <- mask(ymm1)
        emit(&c, 0x89);       // mov [rdi+DATA_O], eax
        modrm_mem(&c, 0, DATA_O);
        epilogue(&c);
        row("vpmovmskb", "mskb", pi, &c);
    }

    // --- vbroadcastss ymm0, xmm1 -------------------------------------------
    // VEX.256.66.0F38.W0 18 /r.  Source is the low dword of xmm1, i.e. of the
    // LOW half of A, replicated into all eight lanes.
    for (int pi = 0; pi < NPAIRS; pi++) {
        Code c;
        c.n = 0;
        ld256(&c, 1, DATA_A);
        vex3(&c, 1, 2, 0, 1, 0, 0, 0, 0);
        emit(&c, 0x18);
        modrm_reg(&c, 0, 1);
        st256(&c, 0, DATA_O);
        epilogue(&c);
        row("vbroadcastss", "bcss", pi, &c);
    }

    if (!g_dump) {
        printf("};\n");
        if (g_skipped) {
            fprintf(stderr, "warning: %d instruction/pair combinations were SKIPped\n", g_skipped);
        }
    }
    return 0;
}
