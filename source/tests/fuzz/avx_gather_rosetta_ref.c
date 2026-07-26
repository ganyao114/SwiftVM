// ===========================================================================
// AVX2 gather reference generator -- runs real x86-64 AVX2 under Rosetta.
// ===========================================================================
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// ----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/avxgatherref avx_gather_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxgatherref > avx_gather_rosetta_ref.inc
//
// --dump-encodings prints each stub's bytes to stderr for auditing against a
// disassembler instead of running anything.
//
// ORACLE CAVEATS, MEASURED ON THIS MACHINE
// ----------------------------------------
//   * Rosetta does NOT advertise AVX through CPUID unless the process starts
//     with ROSETTA_ADVERTISE_AVX=1.  Execution works either way, so support is
//     decided by EXECUTING a gather and catching SIGILL, never by CPUID.
//   * Rosetta ABORTS THE WHOLE PROCESS on a gather that takes a page fault in
//     an ENABLED lane: "rosetta error: unexpectedly need to EmulateForward on
//     a synchronous exception".  It is not a SIGSEGV that can be caught -- the
//     process dies.  So the restartable-fault behaviour of a real gather is
//     NOT observable through this oracle at all, and nothing here tries to
//     capture it.  (A masked-OFF lane with a wild index does not fault and is
//     covered; that is the property that matters -- see the header of
//     runtime/frontend/x86/decoder_avx_gather.cc.)
//   * Rosetta is an emulator with its own defects, so a Rosetta result is
//     evidence and not proof.  Every result shape here was cross-read against
//     the Intel SDM's gather pseudocode before the data was accepted.
//
// MEMORY LAYOUT (rdi-relative; the test replicates it byte for byte)
// ------------------------------------------------------------------
//   +0x000  INDEX     32 bytes loaded into the index register
//   +0x020  MASK      32 bytes loaded into the mask register
//   +0x040  OUT_DST   32 bytes: the destination read back (capture tail only)
//   +0x060  OUT_MSK   32 bytes: the mask register read back
//   +0x100  POISON    16 x 32 bytes, one per ymm register
//   +0x400  TABLE     4096 bytes of gathered data; the base register points at
//                     TABLE+0x800 so negative indices are legal
//
// TABLE[b] = (u8)(b * 31 + 7) -- a formula, so the test reproduces it exactly
// rather than carrying 4 KiB of literal data.  Every index used stays within
// +/-200 elements of the base, which at scale 8 plus a 0x100 displacement is
// 1856 bytes, inside the 2048 available on each side.
//
// WHAT EACH ROW CONTAINS
// ----------------------
// The recorded bytes are ONLY the gather itself.  The stub's register setup
// and the capture tail are excluded, so the test can plant the operand
// registers straight into ThreadContext64 and read the answer straight back --
// a broken vmovdqu cannot mask a broken gather handler.  A row also carries
// which architectural registers the gather used (dst / index / mask), because
// the register-numbering shapes deliberately move them around.
//
// NOTHING HERE IS HAND-COMPUTED.  Every byte printed is what the x86-64 side
// actually left in the register file.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

#define OFF_INDEX 0x000
#define OFF_MASK 0x020
#define OFF_DST 0x040
#define OFF_MSK 0x060
#define OFF_POISON 0x100
#define OFF_TABLE 0x400
#define TABLE_SIZE 4096
#define TABLE_MID (OFF_TABLE + 0x800)
#define DATA_SIZE (OFF_TABLE + TABLE_SIZE)

// Must agree with Poison() in avx_gather_test.cpp.
#define GATHER_POISON(reg, j) ((u8)(0xA5u ^ (unsigned)((reg) * 32 + (j))))
#define TABLE_BYTE(b) ((u8)((unsigned)(b) * 31u + 7u))

// ---------------------------------------------------------------------------
// Input pairs: an index vector and a mask vector, described abstractly so the
// same pair can be materialized as dword or qword elements.
//
//   idx[i]      the signed index for element i.  Materialized as a dword or a
//               sign-extended qword according to the form's index size, so the
//               negative entries exercise sign extension at BOTH widths.
//   sel[i]      the 32 bits placed in mask element i.  For a 64-bit mask
//               element it becomes the HIGH dword and the low dword is filled
//               with 0x5A5A5A5A -- if an implementation looked at any bit but
//               the element's msb, that garbage makes it visible.
//
// The set covers, deliberately: every lane on, every lane off, alternating,
// exactly one lane on, negative indices, duplicate indices (two lanes reading
// the same address), and mask patterns whose msb disagrees with their other
// bits in both directions (0x7FFFFFFF off, 0x80000000 on, 0x40000000 off,
// 0xC0000000 on, 0x00000001 off, 0xFFFFFFFF on).
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    s32 idx[8];
    u32 sel[8];
} Pair;

#define ON 0x80000000u
#define OFF 0x00000000u

static const Pair g_pairs[] = {
    {"allon", {0, 1, 2, 3, 4, 5, 6, 7}, {ON, ON, ON, ON, ON, ON, ON, ON}},
    // Every lane off AND every index wild-ish (still inside the table, since a
    // real fault aborts Rosetta): the destination must come back untouched.
    {"alloff",
     {13, -77, 200, -200, 5, 6, 7, 8},
     {0x7FFFFFFFu, 0u, 0x40000000u, 0x00000001u, 0x7F000000u, 0x0000FFFFu, 0u, 0x12345678u}},
    {"alt",
     {0, -1, 2, -3, 4, -5, 6, -7},
     {ON, OFF, 0xFFFFFFFFu, 0x00000001u, 0xC0000000u, 0x40000000u, 0x80000001u, 0x7F000000u}},
    {"neg", {-1, -2, -4, -8, -16, -32, -64, -128}, {ON, ON, ON, ON, ON, ON, ON, ON}},
    // Duplicate addresses: all eight lanes read the same element.
    {"dup", {7, 7, 7, 7, 7, 7, 7, 7}, {ON, ON, OFF, OFF, ON, OFF, ON, OFF}},
    {"zero", {0, 0, 0, 0, 0, 0, 0, 0}, {OFF, OFF, OFF, OFF, ON, ON, ON, ON}},
    {"big", {200, -200, 199, -199, 100, -100, 3, -3}, {ON, ON, ON, ON, ON, ON, ON, ON}},
    // Exactly one lane on, and it is the LAST one -- an implementation that
    // stopped early, or that wrote element 0 unconditionally, fails here.
    {"one", {1, 1, 1, 1, 1, 1, 1, 1}, {OFF, OFF, OFF, OFF, OFF, OFF, OFF, ON}},
    {"msb",
     {0, 1, 2, 3, 4, 5, 6, 7},
     {0x7FFFFFFFu, ON, 0x40000000u, 0xC0000000u, 0x00000001u, 0xFFFFFFFFu, 0u, 0x80000001u}},
};
#define NPAIRS ((int)(sizeof(g_pairs) / sizeof(g_pairs[0])))

// ---------------------------------------------------------------------------
// Addressing / register shapes.  base and index/dst/mask registers move around
// so VEX.B, VEX.X, VEX.R and vvvv are all exercised, and so the SIB encodings
// that need a forced displacement (base == rbp / r13) and the one that reuses
// the "no index" bit pattern (index == 4) are all reached.
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    int base;   // GPR number holding TABLE_MID
    int scale;  // 1, 2, 4, 8
    int disp;
    int dst, index, mask;  // vector register numbers
} Shape;

static const Shape g_shapes[] = {
    {"s4", 6 /*rsi*/, 4, 0, 0, 1, 2},
    {"s1", 6, 1, 0, 0, 1, 2},
    {"s2disp", 6, 2, 0x10, 0, 1, 2},
    {"s8neg", 6, 8, -0x10, 0, 1, 2},
    {"rbp", 5 /*rbp*/, 4, 0, 0, 1, 2},     // SIB base 101 -> forced disp8
    {"r12", 12, 4, 0, 0, 1, 2},            // SIB base 100 + VEX.B
    {"r13disp32", 13, 4, 0x100, 0, 1, 2},  // base 101 + VEX.B, disp32
    {"idx4", 6, 4, 0, 0, 4, 2},            // SIB index 100 + VEX.X=0
    {"hi", 14, 8, 0, 11, 12, 13},          // VEX.R / VEX.X / VEX.B / vvvv > 7
};
#define NSHAPES ((int)(sizeof(g_shapes) / sizeof(g_shapes[0])))

// ---------------------------------------------------------------------------
// Encoder.
// ---------------------------------------------------------------------------
typedef struct {
    u8 b[512];
    int n;
    int mark;
} Code;

static void emit(Code* c, u8 x) { c->b[c->n++] = x; }
static void emit32(Code* c, u32 x) {
    emit(c, (u8)x);
    emit(c, (u8)(x >> 8));
    emit(c, (u8)(x >> 16));
    emit(c, (u8)(x >> 24));
}

// vmovdqu ymmR, [rdi+disp32]  /  vmovdqu [rdi+disp32], ymmR
static void vmovdqu_ymm(Code* c, int reg, int disp, int store) {
    // VEX.256.F3.0F.WIG 6F /r (load) or 7F /r (store)
    emit(c, 0xC4);
    emit(c, (u8)(((reg < 8 ? 1 : 0) << 7) | (1 << 6) | (1 << 5) | 0x01));
    emit(c, (u8)(0x00 | (0xF << 3) | (1 << 2) | 0x02));
    emit(c, store ? 0x7F : 0x6F);
    emit(c, (u8)(0x80 | ((reg & 7) << 3) | 7));  // mod=10, rm=111 (rdi), disp32
    emit32(c, (u32)disp);
}

// mov r64, rsi   (REX.W 89 /r with reg=rsi)
static void mov_from_rsi(Code* c, int dstgpr) {
    emit(c, (u8)(0x48 | ((dstgpr >= 8) ? 0x01 : 0x00)));
    emit(c, 0x89);
    emit(c, (u8)(0xC0 | (6 << 3) | (dstgpr & 7)));
}

// lea rsi, [rdi+disp32]
static void lea_rsi(Code* c, int disp) {
    emit(c, 0x48);
    emit(c, 0x8D);
    emit(c, (u8)(0x80 | (6 << 3) | 7));
    emit32(c, (u32)disp);
}

static void push_reg(Code* c, int r) {
    if (r >= 8) emit(c, 0x41);
    emit(c, (u8)(0x50 + (r & 7)));
}
static void pop_reg(Code* c, int r) {
    if (r >= 8) emit(c, 0x41);
    emit(c, (u8)(0x58 + (r & 7)));
}

// The gather itself: VEX.NDS.{128,256}.66.0F38.W{0,1} op /r with a VSIB.
static void gather(Code* c, int op, int w, int l, const Shape* s) {
    const int r = s->dst, x = s->index, b = s->base;
    emit(c, 0xC4);
    emit(c, (u8)(((r < 8 ? 1 : 0) << 7) | ((x < 8 ? 1 : 0) << 6) | ((b < 8 ? 1 : 0) << 5) | 0x02));
    emit(c, (u8)((w << 7) | ((~s->mask & 0xF) << 3) | (l << 2) | 0x01));
    emit(c, (u8)op);
    int mod;
    if (s->disp == 0 && (b & 7) != 5) {
        mod = 0;
    } else if (s->disp >= -128 && s->disp <= 127) {
        mod = 1;
    } else {
        mod = 2;
    }
    emit(c, (u8)((mod << 6) | ((r & 7) << 3) | 4));  // rm = 100 -> SIB
    const int ss = s->scale == 8 ? 3 : s->scale == 4 ? 2 : s->scale == 2 ? 1 : 0;
    emit(c, (u8)((ss << 6) | ((x & 7) << 3) | (b & 7)));
    if (mod == 1) {
        emit(c, (u8)(s8)s->disp);
    } else if (mod == 2) {
        emit32(c, (u32)s->disp);
    }
}

static void prologue(Code* c, const Shape* s) {
    push_reg(c, 5);
    push_reg(c, 12);
    push_reg(c, 13);
    push_reg(c, 14);
    lea_rsi(c, TABLE_MID);
    mov_from_rsi(c, 5);
    mov_from_rsi(c, 12);
    mov_from_rsi(c, 13);
    mov_from_rsi(c, 14);
    for (int i = 0; i < 16; ++i) {
        vmovdqu_ymm(c, i, OFF_POISON + i * 32, 0);
    }
    vmovdqu_ymm(c, s->index, OFF_INDEX, 0);
    vmovdqu_ymm(c, s->mask, OFF_MASK, 0);
    c->mark = c->n;
}

static void epilogue(Code* c, const Shape* s) {
    vmovdqu_ymm(c, s->dst, OFF_DST, 1);
    vmovdqu_ymm(c, s->mask, OFF_MSK, 1);
    emit(c, 0xC5);  // vzeroupper
    emit(c, 0xF8);
    emit(c, 0x77);
    pop_reg(c, 14);
    pop_reg(c, 13);
    pop_reg(c, 12);
    pop_reg(c, 5);
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

// Materialize the pair into INDEX / MASK for a given form.
static void plant(const Pair* p, int ebits, int ibits, int n) {
    u8* idx = g_data + OFF_INDEX;
    u8* msk = g_data + OFF_MASK;
    // Lanes beyond the form's element count are filled with a recognizable
    // pattern rather than zero: a handler that read one element too many picks
    // up a huge index and a set mask bit, which shows immediately.
    memset(idx, 0x5C, 32);
    memset(msk, 0x99, 32);
    for (int i = 0; i < n; ++i) {
        if (ibits == 32) {
            const s32 v = p->idx[i];
            memcpy(idx + i * 4, &v, 4);
        } else {
            const s64 v = p->idx[i];
            memcpy(idx + i * 8, &v, 8);
        }
        if (ebits == 32) {
            const u32 m = p->sel[i];
            memcpy(msk + i * 4, &m, 4);
        } else {
            const u64 m = ((u64)p->sel[i] << 32) | 0x5A5A5A5Au;
            memcpy(msk + i * 8, &m, 8);
        }
    }
}

static void row(const char* mnemonic, int width, const Shape* s, const Pair* p, int ebits,
                int ibits, int n, Code* c) {
    char enc[300];
    hexbytes(enc, c->b + c->mark, c->n - c->mark);
    epilogue(c, s);
    if (g_dump) {
        char all[600];
        hexbytes(all, c->b, c->n);
        fprintf(stderr, "%-12s L%-4d %-10s %-8s  %s   (full %s)\n", mnemonic, width, s->name,
                p->name, enc, all);
        return;
    }
    plant(p, ebits, ibits, n);
    memset(g_data + OFF_DST, 0xCC, 32);
    memset(g_data + OFF_MSK, 0xCC, 32);
    for (int r = 0; r < 16; ++r) {
        for (int j = 0; j < 32; ++j) {
            g_data[OFF_POISON + r * 32 + j] = GATHER_POISON(r, j);
        }
    }
    // The PLANTED operand bytes are recorded alongside the encoding for the
    // same reason the encoding itself is: the test then loads exactly what the
    // hardware side loaded, instead of re-deriving the index/mask layout from
    // the form and risking the two sides agreeing on a wrong derivation.
    char ix[80], mk[80];
    hexbytes(ix, g_data + OFF_INDEX, 32);
    hexbytes(mk, g_data + OFF_MASK, 32);
    if (!run_stub(c)) {
        printf("    // SKIP %s L=%d %s %s: Rosetta refused this encoding\n", mnemonic, width,
               s->name, p->name);
        g_skipped++;
        return;
    }
    char dst[80], msk[80];
    hexbytes(dst, g_data + OFF_DST, 32);
    hexbytes(msk, g_data + OFF_MSK, 32);
    printf("    {\"%s\", %d, \"%s\", %d, %d, %d, \"%s\", \"%s\", \"%s\", \"%s\", \"%s\"},\n", mnemonic,
           width, p->name, s->dst, s->index, s->mask, enc, ix, mk, dst, msk);
    g_rows++;
}

int main(int argc, char** argv) {
    g_dump = (argc > 1 && strcmp(argv[1], "--dump-encodings") == 0);
    signal(SIGILL, on_sigill);
    g_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    g_data = mmap(NULL, (DATA_SIZE + 4095) & ~4095, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_page == MAP_FAILED || g_data == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    for (int b = 0; b < TABLE_SIZE; ++b) {
        g_data[OFF_TABLE + b] = TABLE_BYTE(b);
    }

    if (!g_dump) {
        // Support is decided by EXECUTING a gather, never by CPUID (Rosetta
        // hides the AVX bits without ROSETTA_ADVERTISE_AVX=1 while still
        // executing the instructions).
        Code probe;
        probe.n = 0;
        probe.mark = 0;
        prologue(&probe, &g_shapes[0]);
        gather(&probe, 0x90, 0, 1, &g_shapes[0]);
        epilogue(&probe, &g_shapes[0]);
        plant(&g_pairs[0], 32, 32, 8);
        if (!run_stub(&probe)) {
            fprintf(stderr,
                    "FATAL: VEX.256 vpgatherdd raised SIGILL under this runtime.\n"
                    "This generator must run under Rosetta 2 (arch -x86_64) on a host\n"
                    "whose x86-64 layer executes AVX2.  No reference data was written.\n");
            return 3;
        }
        printf("// GENERATED by avx_gather_rosetta_ref.c under Rosetta 2 -- DO NOT EDIT.\n");
        printf("// Every value is the literal bytes real x86-64 left in the register file.\n");
        printf("//\n");
        printf("// Row: {mnemonic, VEX width, input-pair name, dst reg, index reg, mask reg,\n");
        printf("//       instruction bytes, index register in, mask register in,\n");
        printf("//       destination after, mask register after}\n");
        printf("constexpr AvxGatherRef kAvxGatherRefs[] = {\n");
    }

    struct {
        const char* name;
        int opcode, w, ebits, ibits;
    } ops[] = {
#define SVM_GATHER(name, opcode, w, ebits, ibits) {#name, opcode, w, ebits, ibits},
#include "avx_gather_ops.inc"
    };

    for (int oi = 0; oi < (int)(sizeof(ops) / sizeof(ops[0])); ++oi) {
        for (int l = 0; l < 2; ++l) {
            const int vl = l ? 256 : 128;
            const int wider = ops[oi].ebits > ops[oi].ibits ? ops[oi].ebits : ops[oi].ibits;
            const int n = vl / wider;
            for (int si = 0; si < NSHAPES; ++si) {
                for (int pi = 0; pi < NPAIRS; ++pi) {
                    Code c;
                    c.n = 0;
                    c.mark = 0;
                    prologue(&c, &g_shapes[si]);
                    gather(&c, ops[oi].opcode, ops[oi].w, l, &g_shapes[si]);
                    row(ops[oi].name, vl, &g_shapes[si], &g_pairs[pi], ops[oi].ebits,
                        ops[oi].ibits, n, &c);
                }
            }
        }
    }

    if (!g_dump) {
        printf("};\n");
        fprintf(stderr, "rows=%d skipped=%d\n", g_rows, g_skipped);
    }
    return 0;
}
