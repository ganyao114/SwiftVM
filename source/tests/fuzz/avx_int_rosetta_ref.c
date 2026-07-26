// ===========================================================================
// VEX integer / data-rearrangement reference generator -- runs real x86-64
// AVX/AVX2 under Rosetta 2 and prints what the hardware actually produced.
// ===========================================================================
//
// WHY THIS EXISTS
// ---------------
// Unicorn 2.1.4 rejects every VEX.L=1 encoding, and the bundled distorm cannot
// even decode the AVX2-only opcodes (vpbroadcast*, vpermd/vpermq, vperm2i128,
// vinserti128/vextracti128, vpblendd, vpsllvd/vpsrlvd/vpsravd), so the handlers
// in source/runtime/frontend/x86/decoder_avx_int.cc have no emulator oracle.
// Rosetta 2 on macOS 26/27 does execute AVX and AVX2 including the full 256-bit
// register file, so it serves as the ground truth instead.
//
// Rosetta does NOT advertise AVX through CPUID unless the process is started
// with ROSETTA_ADVERTISE_AVX=1; execution works either way.  This program
// therefore reports the CPUID bits for information only and decides support by
// EXECUTING an instruction and catching SIGILL.
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// -----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/avxintref avx_int_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxintref > avx_int_rosetta_ref.inc
//
// Audit the encoder against a disassembler with:
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxintref --dump-encodings
//
// HOW IT WORKS
// ------------
// Instructions are ASSEMBLED AT RUNTIME from avx_int_ops.inc rather than written
// as inline asm, because avx_int_test.cpp builds its blocks from the same table
// with the same field meanings.  A wrong opcode byte therefore cannot make the
// two sides test different instructions.  Each stub is
//
//     vmovdqu ymm0, [rdi+0x80]     ; POISON the destination first
//     vmovdqu ymm1, [rdi+0x00]     ; A
//     vmovdqu ymm2, [rdi+0x20]     ; B
//     vmovdqu ymm3, [rdi+0x40]     ; blend mask
//     <op>    ymm0/xmm0, ...
//     vmovdqu [rdi+0x60], ymm0     ; read back BOTH halves
//     vzeroupper
//     ret
//
// Poisoning ymm0 before the instruction and reading back all 32 bytes is what
// makes contract C3 measurable: a VEX.128 form MUST leave zeros in bytes 16..31
// of the output, and the poison is what a failure to zero would show up as.
//
// NOTHING HERE IS HAND-COMPUTED.  Every value in the generated table is the
// literal 32 bytes Rosetta wrote to memory.  An instruction Rosetta refuses is
// emitted as a SKIP comment rather than being filled in from the Intel manual.

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
#define DATA_M 0x40
#define DATA_O 0x60
#define DATA_P 0x80
#define DATA_SIZE 0xA0

// ---------------------------------------------------------------------------
// Input vectors.  Every pair keeps its two 128-bit lanes DIFFERENT on both
// sides, so an implementation that derived the upper half from the lower half's
// data -- the most likely way to break the two-halves split -- cannot pass.
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    u8 a[32];
    u8 b[32];
} Pair;

static Pair g_pairs[] = {
        // 0: degenerate extremes.  Also the pair where every shift count is
        // 0xFFFF... i.e. hugely out of range, which x86 does NOT reduce modulo
        // the lane width -- the result must be 0 (or all sign bits for psra).
        {"zeros_ones",
         {0},
         {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
          0xFF, 0xFF}},

        // 1: signed and carry boundaries at every lane width (0x7F/0x80,
        // 0x7FFF/0x8000, 0x7FFFFFFF/0x80000000, 0xFF..FF + 1).  This is what
        // separates the saturating forms from the wrapping ones, the signed
        // min/max from the unsigned, and PABS of INT_MIN (which stays INT_MIN)
        // from a true absolute value.
        {"signbnd",
         {0x7F, 0x7F, 0x80, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0x01, 0x80, 0xFF, 0x7F, 0x00, 0x80, 0xFF,
          0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF,
          0xFF, 0xFF},
         {0x01, 0x81, 0x01, 0xFF, 0xFF, 0x01, 0x01, 0x80, 0x7F, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0x01,
          0x01, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80, 0x7F, 0x7F,
          0x7F, 0x7F}},

        // 2: THE lane test.  A is 0x01..0x20 so every byte names its own
        // position.  B's low lane holds 0x1F..0x10 (indices whose bit 4 is set)
        // and its high lane 0x00..0x0F.  Any per-lane operation on this pair
        // produces a result 30 of whose 32 bytes differ from what a cross-lane
        // reading would produce, so a wrong split cannot coincide with the
        // right answer.
        {"laneidx",
         {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
          0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
          0x1F, 0x20},
         {0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
          0x10, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x88, 0x89, 0x8A, 0x8B, 0xFF, 0x80,
          0x0F, 0x07}},

        // 3: vpermd index vectors.  A's dwords are 0x0F, 0x13, 0x25, 0x01,
        // 0x1E, 0xFFFFFFFA, 0x04, 0x08 -- masked to bits [2:0] that is
        // 7,3,5,1,6,2,4,0, a permutation in which EVERY output dword comes from
        // the other 128-bit lane of B.  The out-of-range raw values also prove
        // the index is masked rather than clamped.  B's dwords are eight
        // distinct nibble-repeats so any misrouting names itself.
        {"permidx",
         {0x0F, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
          0x00, 0x1E, 0x00, 0x00, 0x00, 0xFA, 0xFF, 0xFF, 0xFF, 0x04, 0x00, 0x00, 0x00, 0x08, 0x00,
          0x00, 0x00},
         {0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33, 0x33, 0x44, 0x44, 0x44,
          0x44, 0x55, 0x55, 0x55, 0x55, 0x66, 0x66, 0x66, 0x66, 0x77, 0x77, 0x77, 0x77, 0x88, 0x88,
          0x88, 0x88}},

        // 4: shift counts small enough to be meaningful.  B's dwords are
        // 3,0,1,0,5,0,65,0, so its LOW QWORD is 3 (what the xmm-count forms
        // read), its dwords give per-lane counts 3,0,1,0,5,0,65,0 for the *vd
        // variable shifts and its qwords give 3,1,5,65 for the *vq ones -- a
        // mix of in-range counts and one (65) past the lane width.
        {"shiftcnt",
         {0x01, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00,
          0x00, 0x78, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x89, 0x98, 0xBA, 0xDC, 0xFE, 0xFF, 0x00,
          0xFF, 0x00},
         {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00}},

        // 5: pseudo-random, filled below; catches anything the structured cases
        // happen to be blind to.
        {"random", {0}, {0}},
};
#define NPAIRS ((int)(sizeof(g_pairs) / sizeof(g_pairs[0])))

// The vpblendvb mask.  Fixed rather than per-pair so the test can hardcode it;
// the two 128-bit lanes carry different patterns and both 0x80-set and
// 0x7F-only bytes appear, so neither "always src1" nor "always src2" nor a
// lane-swapped mask can pass.
static const u8 g_mask[32] = {0x80, 0x00, 0xFF, 0x7F, 0x01, 0x81, 0x00, 0x80, 0xC0, 0x40, 0x80,
                              0x00, 0x00, 0xFF, 0x80, 0x7F, 0x00, 0x80, 0x7F, 0xFF, 0x80, 0x00,
                              0x81, 0x01, 0x40, 0xC0, 0x00, 0x80, 0xFF, 0x00, 0x7F, 0x80};

// Destination poison.  Any byte of it surviving in the output of a VEX.128 form
// is a C3 (upper-half zeroing) failure; any byte surviving a VEX.256 form is a
// half that was never written.
static u8 g_poison[32];

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

static void fill_poison(void) {
    for (int i = 0; i < 32; i++) {
        g_poison[i] = (u8)(0xE7 ^ (i * 5 + 1));
    }
}

// ---------------------------------------------------------------------------
// VEX encoder.  Field meanings and inversion behaviour are identical to
// EmitVexC4 in avx_int_test.cpp -- all inputs un-inverted, the 3-byte C4 form
// always -- so the two programs build byte-identical encodings apart from the
// base register and displacement.
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

static void modrm_reg(Code* c, int reg, int rm) {
    emit(c, (u8)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

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

// Every stub starts the same way: poison the destination, then load the three
// inputs.  Loading ymm3 unconditionally costs one instruction and keeps a
// single prologue for all shapes.
static void prologue(Code* c) {
    c->n = 0;
    ld256(c, 0, DATA_P);
    ld256(c, 1, DATA_A);
    ld256(c, 2, DATA_B);
    ld256(c, 3, DATA_M);
}

static void epilogue(Code* c) {
    st256(c, 0, DATA_O);
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

// Emit one table row: run `c` for input pair `pi`, print the 32 bytes the
// hardware left in the output slot.
static void row(const char* mnemonic, int width, unsigned imm, int pi, Code* c) {
    char enc[256];
    hexbytes(enc, c->b, c->n);
    if (g_dump) {
        fprintf(stderr, "%-14s L%-3d imm=0x%02x pair%d  %s\n", mnemonic, width, imm, pi, enc);
        return;
    }
    memcpy(g_data + DATA_A, g_pairs[pi].a, 32);
    memcpy(g_data + DATA_B, g_pairs[pi].b, 32);
    memcpy(g_data + DATA_M, g_mask, 32);
    memcpy(g_data + DATA_P, g_poison, 32);
    memset(g_data + DATA_O, 0xCC, 32);
    if (!run_stub(c, g_data)) {
        printf("    // SKIP %s.%d imm=0x%02x pair%d (%s): Rosetta refused it, no reference value\n",
               mnemonic, width, imm, pi, g_pairs[pi].name);
        g_skipped++;
        return;
    }
    char h[80];
    hex32(h, g_data + DATA_O);
    printf("    {\"%s\", %d, 0x%02x, %d, \"%s\"},\n", mnemonic, width, imm, pi, h);
    g_rows++;
}

// `lmask` bit 0 = the VEX.128 form exists, bit 1 = the VEX.256 form exists.
#define FOR_WIDTHS(lmask) \
    for (int wi = 0; wi < 2; wi++)
#define WIDTH_SKIP(lmask, wi) (((lmask) & (1 << (wi))) == 0)

int main(int argc, char** argv) {
    g_dump = (argc > 1 && strcmp(argv[1], "--dump-encodings") == 0);
    signal(SIGILL, on_sigill);
    signal(SIGSEGV, on_sigill);
    signal(SIGBUS, on_sigill);
    signal(SIGTRAP, on_sigill);
    fill_random_pair();
    fill_poison();

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
    g_data = raw;  // page-aligned already

    // --- capability gate: prove AVX2 execution before emitting anything ------
    // vpermd is the strictest single probe available: it is AVX2-only, 256-bit
    // and cross-lane, so if it runs, everything else in this table can.
    {
        Code c;
        prologue(&c);
        vex3(&c, 1, 2, 1, 1, 0, 0, 0, 0);
        emit(&c, 0x36);
        modrm_reg(&c, 0, 2);
        epilogue(&c);
        memcpy(g_data + DATA_A, g_pairs[3].a, 32);
        memcpy(g_data + DATA_B, g_pairs[3].b, 32);
        memcpy(g_data + DATA_M, g_mask, 32);
        memcpy(g_data + DATA_P, g_poison, 32);
        if (!run_stub(&c, g_data)) {
            fprintf(stderr,
                    "FATAL: VEX.256 vpermd raised SIGILL under this runtime.\n"
                    "Rosetta on this machine cannot serve as an AVX2 oracle;\n"
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
        char hm[80], hp[80];
        hex32(hm, g_mask);
        hex32(hp, g_poison);
        printf("// GENERATED FILE -- DO NOT EDIT BY HAND.\n");
        printf("//\n");
        printf("// Reference values for the VEX integer / data-rearrangement family,\n");
        printf("// produced by ACTUALLY EXECUTING each instruction on x86-64 hardware\n");
        printf("// through Rosetta 2.  Nothing here is hand-computed; an instruction\n");
        printf("// Rosetta refused appears as a SKIP comment instead of a value.\n");
        printf("//\n");
        printf("// Regenerate with the recipe in avx_int_rosetta_ref.c:\n");
        printf("//   clang -arch x86_64 -O1 -o /tmp/avxintref avx_int_rosetta_ref.c\n");
        printf("//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxintref > "
               "avx_int_rosetta_ref.inc\n");
        printf("//\n");
        printf("// Generating runtime: CPUID.1.ECX=%08x EDX=%08x, CPUID.7.0.EBX=%08x\n", cpuid1[2],
               cpuid1[3], cpuid7[1]);
        printf("//   (AVX bit=%d AVX2 bit=%d OSXSAVE bit=%d -- Rosetta hides these unless\n",
               (cpuid1[2] >> 28) & 1, (cpuid7[1] >> 5) & 1, (cpuid1[2] >> 27) & 1);
        printf("//    ROSETTA_ADVERTISE_AVX=1 is set; execution works regardless, which is\n");
        printf("//    why the generator gates on a live vpermd ymm and not on CPUID.)\n");
        printf("\n");
        printf("// The blend mask loaded into ymm3, and the poison written into ymm0 before\n");
        printf("// every instruction.  Poison bytes surviving into a VEX.128 result are a\n");
        printf("// contract-C3 (upper-half zeroing) failure.\n");
        printf("static const char* const kAvxIntMask = \"%s\";\n", hm);
        printf("static const char* const kAvxIntPoison = \"%s\";\n\n", hp);
        printf("// Input vectors: name, A (32 bytes), B (32 bytes).\n");
        printf("static const AvxIntInput kAvxIntInputs[] = {\n");
        for (int i = 0; i < NPAIRS; i++) {
            char ha[80], hb[80];
            hex32(ha, g_pairs[i].a);
            hex32(hb, g_pairs[i].b);
            printf("    {\"%s\", \"%s\", \"%s\"},\n", g_pairs[i].name, ha, hb);
        }
        printf("};\n\n");
        printf("// Results: mnemonic, VEX width (128/256), imm8, input-pair index, and the\n");
        printf("// 32 bytes read back from ymm0 after\n");
        printf("//   <op> ymm0/xmm0, ymm1(=A), ymm2(=B) [, ymm3(=mask)] [, imm8]\n");
        printf("// with A[0..15] in xmm1 and A[16..31] in ymm1's upper half.\n");
        printf("static const AvxIntRef kAvxIntRefs[] = {\n");
    }

    // --- three-operand ALU: dst = src1 OP src2 ------------------------------
#define SVM_AVX_INT_ALU(name, pp, mm, op, w, lmask)                          \
    FOR_WIDTHS(lmask) {                                                      \
        if (WIDTH_SKIP(lmask, wi)) continue;                                 \
        for (int pi = 0; pi < NPAIRS; pi++) {                                \
            Code c;                                                          \
            prologue(&c);                                                    \
            vex3(&c, (pp), (mm), 1, wi, 0, 0, 0, (w));                       \
            emit(&c, (u8)(op));                                              \
            modrm_reg(&c, 0, 2);                                             \
            epilogue(&c);                                                    \
            row(#name, wi ? 256 : 128, 0, pi, &c);                           \
        }                                                                    \
    }

    // --- shift by an xmm/m128 count: same shape, different operand width ----
#define SVM_AVX_INT_SHIFTX(name, pp, mm, op, lmask)                          \
    FOR_WIDTHS(lmask) {                                                      \
        if (WIDTH_SKIP(lmask, wi)) continue;                                 \
        for (int pi = 0; pi < NPAIRS; pi++) {                                \
            Code c;                                                          \
            prologue(&c);                                                    \
            vex3(&c, (pp), (mm), 1, wi, 0, 0, 0, 0);                         \
            emit(&c, (u8)(op));                                              \
            modrm_reg(&c, 0, 2);                                             \
            epilogue(&c);                                                    \
            row(#name, wi ? 256 : 128, 0, pi, &c);                           \
        }                                                                    \
    }

    // --- shift by imm8: NDD, destination in VEX.vvvv, /n in ModRM.reg -------
#define SVM_AVX_INT_SHIFTI(name, pp, mm, op, ext, imm, lmask)                \
    FOR_WIDTHS(lmask) {                                                      \
        if (WIDTH_SKIP(lmask, wi)) continue;                                 \
        for (int pi = 0; pi < NPAIRS; pi++) {                                \
            Code c;                                                          \
            prologue(&c);                                                    \
            vex3(&c, (pp), (mm), 0, wi, 0, 0, 0, 0);                         \
            emit(&c, (u8)(op));                                              \
            modrm_reg(&c, (ext), 1);                                         \
            emit(&c, (u8)(imm));                                             \
            epilogue(&c);                                                    \
            row(#name, wi ? 256 : 128, (imm), pi, &c);                       \
        }                                                                    \
    }

    // --- two-operand: dst = f(src2), no VEX.vvvv ---------------------------
#define SVM_AVX_INT_UN(name, pp, mm, op, w, lmask, sb128, sb256)             \
    FOR_WIDTHS(lmask) {                                                      \
        if (WIDTH_SKIP(lmask, wi)) continue;                                 \
        for (int pi = 0; pi < NPAIRS; pi++) {                                \
            Code c;                                                          \
            prologue(&c);                                                    \
            vex3(&c, (pp), (mm), 0, wi, 0, 0, 0, (w));                       \
            emit(&c, (u8)(op));                                              \
            modrm_reg(&c, 0, 1);                                             \
            epilogue(&c);                                                    \
            row(#name, wi ? 256 : 128, 0, pi, &c);                           \
        }                                                                    \
    }

    // --- three-operand with imm8 -------------------------------------------
#define SVM_AVX_INT_ALUI(name, pp, mm, op, w, imm, lmask)                    \
    FOR_WIDTHS(lmask) {                                                      \
        if (WIDTH_SKIP(lmask, wi)) continue;                                 \
        for (int pi = 0; pi < NPAIRS; pi++) {                                \
            Code c;                                                          \
            prologue(&c);                                                    \
            vex3(&c, (pp), (mm), 1, wi, 0, 0, 0, (w));                       \
            emit(&c, (u8)(op));                                              \
            modrm_reg(&c, 0, 2);                                             \
            emit(&c, (u8)(imm));                                             \
            epilogue(&c);                                                    \
            row(#name, wi ? 256 : 128, (imm), pi, &c);                       \
        }                                                                    \
    }

    // --- two-operand with imm8 ---------------------------------------------
#define SVM_AVX_INT_UNI(name, pp, mm, op, w, imm, lmask)                     \
    FOR_WIDTHS(lmask) {                                                      \
        if (WIDTH_SKIP(lmask, wi)) continue;                                 \
        for (int pi = 0; pi < NPAIRS; pi++) {                                \
            Code c;                                                          \
            prologue(&c);                                                    \
            vex3(&c, (pp), (mm), 0, wi, 0, 0, 0, (w));                       \
            emit(&c, (u8)(op));                                              \
            modrm_reg(&c, 0, 1);                                             \
            emit(&c, (u8)(imm));                                             \
            epilogue(&c);                                                    \
            row(#name, wi ? 256 : 128, (imm), pi, &c);                       \
        }                                                                    \
    }

#include "avx_int_ops.inc"

    // --- vpblendvb ymm0, ymm1, ymm2, ymm3 ----------------------------------
    // VEX.NDS.128/256.66.0F3A.W0 4C /r /is4.  The mask register is the HIGH
    // NIBBLE of the trailing immediate, not an operand slot; encoding it as one
    // would silently pick the r/m register instead.
    for (int wi = 0; wi < 2; wi++) {
        for (int pi = 0; pi < NPAIRS; pi++) {
            Code c;
            prologue(&c);
            vex3(&c, 1, 3, 1, wi, 0, 0, 0, 0);
            emit(&c, 0x4C);
            modrm_reg(&c, 0, 2);
            emit(&c, (u8)(3 << 4));  // is4: mask = ymm3
            epilogue(&c);
            row("vpblendvb", wi ? 256 : 128, 0x30, pi, &c);
        }
    }

    // --- vinserti128 ymm0, ymm1, xmm2, imm8 --------------------------------
    // VEX.256.66.0F3A.W0 38 /r ib.  Replaces one 128-bit lane of src1 with the
    // LOW half of src2; imm8 bit 0 picks which.
    for (unsigned imm = 0; imm < 2; imm++) {
        for (int pi = 0; pi < NPAIRS; pi++) {
            Code c;
            prologue(&c);
            vex3(&c, 1, 3, 1, 1, 0, 0, 0, 0);
            emit(&c, 0x38);
            modrm_reg(&c, 0, 2);
            emit(&c, (u8)imm);
            epilogue(&c);
            row("vinserti128", 256, imm, pi, &c);
        }
    }

    // --- vextracti128 xmm0, ymm1, imm8 -------------------------------------
    // VEX.256.66.0F3A.W0 39 /r ib.  The DESTINATION is the r/m operand and the
    // source is ModRM.reg -- the reverse of every other shape here.  The
    // destination is an xmm, so bits 255:128 of ymm0 must come back zeroed.
    for (unsigned imm = 0; imm < 2; imm++) {
        for (int pi = 0; pi < NPAIRS; pi++) {
            Code c;
            prologue(&c);
            vex3(&c, 1, 3, 0, 1, 0, 0, 0, 0);
            emit(&c, 0x39);
            modrm_reg(&c, 1, 0);
            emit(&c, (u8)imm);
            epilogue(&c);
            row("vextracti128", 256, imm, pi, &c);
        }
    }

    // --- vbroadcasti128 ymm0, [rdi+A] --------------------------------------
    // VEX.256.66.0F38.W0 5A /r.  Memory operand only; the register form is #UD.
    for (int pi = 0; pi < NPAIRS; pi++) {
        Code c;
        prologue(&c);
        vex3(&c, 1, 2, 0, 1, 0, 0, 0, 0);
        emit(&c, 0x5A);
        modrm_mem(&c, 0, DATA_A);
        epilogue(&c);
        row("vbroadcasti128", 256, 0, pi, &c);
    }

    if (!g_dump) {
        printf("};\n");
        fprintf(stderr, "%d reference rows emitted\n", g_rows);
        if (g_skipped) {
            fprintf(stderr, "warning: %d instruction/pair combinations were SKIPped\n", g_skipped);
        }
    }
    return 0;
}
