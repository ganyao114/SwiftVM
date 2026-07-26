// ===========================================================================
// VEX blend / extract / maskmov reference generator -- real x86-64 AVX under
// Rosetta 2.
// ===========================================================================
//
// Covers the ten encodings listed in avx_blend_ops.inc.  Same oracle and the
// same traps as avx_fp2_rosetta_ref.c:
//
//   * Rosetta does NOT advertise AVX through CPUID unless the process starts
//     with ROSETTA_ADVERTISE_AVX=1.  Execution works either way, so support is
//     decided by EXECUTING an instruction and catching SIGILL, never by
//     reading CPUID.
//   * Rosetta is an emulator with its own defects (VPSLLVQ's shift count
//     measured truncated to 32 bits; XSAVE writing extra bytes; a
//     non-deterministic PF out of vptest; an abort rather than a signal when a
//     256-bit store crosses into an unmapped page).  A Rosetta result is
//     evidence, not proof: anything surprising is cross-read against the Intel
//     SDM before it is believed, and where the two disagree the SDM wins and
//     the disagreement is recorded.
//
// HOW TO REGENERATE  (Apple Silicon Mac with Rosetta 2)
// ----------------------------------------------------
//   cd source/tests/fuzz
//   clang -arch x86_64 -O1 -o /tmp/avxblendref avx_blend_rosetta_ref.c
//   ROSETTA_ADVERTISE_AVX=1 arch -x86_64 /tmp/avxblendref > avx_blend_rosetta_ref.inc
//
// Add --dump-encodings to print each stub's bytes to stderr for auditing
// against a disassembler.
//
// WHAT A ROW CONTAINS, AND WHY IT CONTAINS THE ENCODING
// ----------------------------------------------------
// Every row carries the LITERAL BYTES of the sequence under test, and
// avx_blend_test.cpp replays those bytes rather than re-encoding from the
// shared table.  The two sides therefore cannot assemble different
// instructions, which is the vacuous-pass failure mode a table-driven
// encoder on each side has.
//
// Each stub is
//
//     vmovdqu ymm0, [rdi+DATA_P]      ; poison the destination
//     vmovdqu ymm1, [rdi+DATA_A]      ; A  (SRC1 / the store's data source)
//     vmovdqu ymm2, [rdi+DATA_B]      ; B  (SRC2)
//     vmovdqu ymm3, [rdi+DATA_M]      ; M  (the blend selector / the mask)
//     <the recorded byte sequence>
//     <capture tail, per form>
//     vzeroupper
//     ret
//
// and the three forms differ only in where the answer is read from:
//
//   form 0  ymm0, read back with a 256-bit store the recorded bytes EXCLUDE
//           (the test reads the register out of ThreadContext64 instead, so a
//           broken vmovdqu cannot mask a broken handler).  Reading all 32
//           bytes of a poisoned ymm0 is what MEASURES contract C3.
//   form 1  the 32-byte capture slot at [rdi+DATA_O], pre-filled with 0xCC.
//           The recorded bytes include whatever writes it.  For a masked store
//           the surviving 0xCC bytes are the measurement: they are the
//           elements hardware did NOT write.
//   form 2  rax, appended by a `mov [rdi+DATA_O], rax` the recorded bytes
//           EXCLUDE.  rax is pre-set to -1 INSIDE the recorded bytes so a
//           32-bit destination that failed to zero bits 63:32 is visible.
//
// NOTHING HERE IS HAND-COMPUTED.  Every value is the literal bytes Rosetta
// wrote.  An instruction Rosetta refuses becomes a SKIP comment, never a value
// filled in from the manual.
//
// THE FAULT PROBE
// ---------------
// vmaskmov's defining property -- a masked-off element neither faults nor is
// written -- cannot be a data row, because a row records a value and this is
// the ABSENCE of a signal.  main() therefore ends with four probes that place
// the upper 16 bytes of a 256-bit access on a PROT_NONE page and mask those
// elements off; each prints a `// MASKFAULT` comment saying whether the access
// completed.  Those comments are documentation of what the oracle does.  The
// same property is ASSERTED against SwiftVM in avx_blend_test.cpp, which is
// where it has to hold.

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

// Must agree with the poison avx_blend_test.cpp writes into ThreadContext64
// for register 0.
#define BLEND_POISON(i) ((u8)(0xA5u ^ (unsigned)(i)))

// ---------------------------------------------------------------------------
// Input triples.  A and B are the two blend sources, M the selector / mask.
//
// Both 128-bit lanes of every vector differ from each other, so an
// implementation that derived the upper half from the lower half's operands
// (the way SwiftVM's two-halves split is most easily broken) cannot pass.
// Beyond that each triple aims at a specific failure:
//
//   bytes    every byte distinct within its lane, so a wrong lane index shows
//            up as a wrong VALUE rather than as merely wrong data.  M
//            alternates its per-dword sign bit.
//   signs    M's sign bits form a non-repeating pattern that differs between
//            the two 128-bit halves and between adjacent dwords -- so a
//            vblendvps that read the wrong half, or a vblendvpd that consulted
//            bit 31 instead of bit 63 of each qword, produces a different
//            answer.
//   msbonly  M is exactly 0x80000000 / 0x7FFFFFFF per dword: the ONLY thing
//            that differs between the selected and unselected lanes is the
//            most significant bit.  An implementation that tested any other
//            bit, or tested "non-zero", fails here and nowhere else.
//   allset   every mask bit 1: blends take SRC2 everywhere, masked loads load
//            everything, masked stores store everything.
//   allclr   every mask bit 0: the mirror case, and for a masked store the
//            reference is 32 bytes of untouched 0xCC.
//   f32nan   NaN payloads, signalling NaNs, infinities, signed zero.  These
//            instructions are all bitwise, so the value is a canary: a path
//            that accidentally went through a float unit would quiet a
//            signalling NaN and be caught.
//   random   whatever the structured cases are blind to.
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    u8 a[32];
    u8 b[32];
    u8 m[32];
} Trip;

static Trip g_trips[16];
static int g_ntrips;

static void put32(u8* p, int i, u32 v) { memcpy(p + i * 4, &v, 4); }

static u32 f32bits(float f) {
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}

#define QNAN_A32 0x7FC00111u
#define QNAN_B32 0x7FC00222u
#define SNAN_A32 0x7F800333u
#define INF32 0x7F800000u
#define NINF32 0xFF800000u
#define NZERO32 0x80000000u

static Trip* new_trip(const char* name) {
    Trip* t = &g_trips[g_ntrips++];
    memset(t, 0, sizeof(*t));
    t->name = name;
    return t;
}

static u32 lcg(u32* s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

static void build_trips(void) {
    {
        Trip* t = new_trip("bytes");
        for (int i = 0; i < 32; i++) {
            t->a[i] = (u8)(0x10 + i);      // 0x10..0x2F, all distinct
            t->b[i] = (u8)(0xF0 - i * 3);  // distinct, wraps through 0x00
        }
        for (int i = 0; i < 8; i++) {
            put32(t->m, i, (i & 1) ? 0x80000000u : 0x7F000000u);
        }
    }
    {
        // The sign pattern below is deliberately NOT symmetric between the two
        // 128-bit halves and NOT the same for the even and odd dwords of a
        // qword, so vblendvps/vblendvpd, and the 32- vs 64-bit masked moves,
        // all disagree with each other's wrong answers.
        Trip* t = new_trip("signs");
        static const u32 sgn[8] = {0x80000001u, 0x00000002u, 0x00000003u, 0x80000004u,
                                   0x00000005u, 0x80000006u, 0x80000007u, 0x00000008u};
        for (int i = 0; i < 8; i++) {
            put32(t->a, i, 0x11110000u + (u32)i);
            put32(t->b, i, 0x22220000u + (u32)(i * 7));
            put32(t->m, i, sgn[i]);
        }
    }
    {
        Trip* t = new_trip("msbonly");
        static const int on[8] = {1, 0, 0, 1, 1, 1, 0, 0};
        for (int i = 0; i < 8; i++) {
            put32(t->a, i, 0xAAAA0000u + (u32)i);
            put32(t->b, i, 0x55550000u + (u32)i);
            put32(t->m, i, on[i] ? 0x80000000u : 0x7FFFFFFFu);
        }
    }
    {
        Trip* t = new_trip("allset");
        for (int i = 0; i < 8; i++) {
            put32(t->a, i, 0x01020304u + (u32)i);
            put32(t->b, i, 0xF0E0D0C0u - (u32)i);
            put32(t->m, i, 0xFFFFFFFFu);
        }
    }
    {
        Trip* t = new_trip("allclr");
        for (int i = 0; i < 8; i++) {
            put32(t->a, i, 0x0A0B0C0Du + (u32)i);
            put32(t->b, i, 0x90A0B0C0u - (u32)i);
            put32(t->m, i, 0x00000000u);
        }
    }
    {
        Trip* t = new_trip("f32nan");
        const u32 a[8] = {QNAN_A32,       SNAN_A32, NZERO32, f32bits(1.5f),
                          f32bits(-0.0f), INF32,    NINF32,  f32bits(-3.25f)};
        const u32 b[8] = {f32bits(1.0f),  QNAN_B32, INF32,   f32bits(4.0f),
                          f32bits(-5.5f), NINF32,   NZERO32, f32bits(0.0f)};
        // A float mask: the sign bit of a negative float IS the selector bit,
        // which is how compilers actually produce vblendv* masks (from a
        // comparison result or from a sign).
        const u32 m[8] = {f32bits(-1.0f), f32bits(1.0f),  f32bits(-0.0f), f32bits(0.0f),
                          NINF32,         INF32,          QNAN_A32,       0x80000000u};
        for (int i = 0; i < 8; i++) {
            put32(t->a, i, a[i]);
            put32(t->b, i, b[i]);
            put32(t->m, i, m[i]);
        }
    }
    {
        Trip* t = new_trip("random");
        u32 s = 0xC0FFEE01u;
        for (int i = 0; i < 8; i++) {
            put32(t->a, i, lcg(&s));
            put32(t->b, i, lcg(&s));
            put32(t->m, i, lcg(&s));
        }
    }
}

// ---------------------------------------------------------------------------
// Encoder.  All fields un-inverted; always the 3-byte C4 form and always a
// disp32 memory operand, so the recorded bytes are uniform and easy to
// disassemble (and DATA_P at 0x80 does not have to fit in a SIGNED disp8).
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

// ModRM for [base + disp32]; base 7 is rdi and 6 is rsi, neither needs a SIB.
static void modrm_base(Code* c, int reg, int base, int disp) {
    emit(c, (u8)(0x80 | ((reg & 7) << 3) | (base & 7)));
    emit(c, (u8)(disp & 0xFF));
    emit(c, (u8)((disp >> 8) & 0xFF));
    emit(c, (u8)((disp >> 16) & 0xFF));
    emit(c, (u8)((disp >> 24) & 0xFF));
}
static void modrm_mem(Code* c, int reg, int disp) { modrm_base(c, reg, 7, disp); }
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
    ld256(c, 3, DATA_M);
    c->mark = c->n;  // everything from here is what the test replays
}
static void epilogue(Code* c) {
    emit(c, 0xC5);
    emit(c, 0xF8);
    emit(c, 0x77);  // vzeroupper
    emit(c, 0xC3);  // ret
}
// mov rax, -1  (sign-extended imm32)
static void seed_rax(Code* c) {
    emit(c, 0x48);
    emit(c, 0xC7);
    emit(c, 0xC0);
    emit(c, 0xFF);
    emit(c, 0xFF);
    emit(c, 0xFF);
    emit(c, 0xFF);
}
// mov [rdi+DATA_O], rax
static void store_rax(Code* c) {
    emit(c, 0x48);
    emit(c, 0x89);
    modrm_mem(c, 0, DATA_O);
}

// ---------------------------------------------------------------------------
// Execution and fault trapping.
// ---------------------------------------------------------------------------
typedef void (*stub_fn)(u8* data);
typedef void (*stub2_fn)(u8* a, u8* b);

static u8* g_page;
static jmp_buf g_jb;
static u8* g_data;
static int g_dump;
static int g_skipped;
static int g_rows;
static int g_pair;  // the triple `row` should use; set by the driver loop

static void on_fault(int s) {
    (void)s;
    longjmp(g_jb, 1);
}

static void install_code(const Code* c) {
    if (mprotect(g_page, 4096, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect rw");
        exit(2);
    }
    memcpy(g_page, c->b, (size_t)c->n);
    if (mprotect(g_page, 4096, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect rx");
        exit(2);
    }
}

static int run_stub(const Code* c) {
    install_code(c);
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

// Finish `c` for the given form, run it for the current triple, print the row.
static void row(const char* mnemonic, int width, int form, Code* c) {
    char enc[300];
    hexbytes(enc, c->b + c->mark, c->n - c->mark);
    // The capture tail is appended AFTER the encoding is recorded, so forms 0
    // and 2 do not make the test replay a store it does not need.
    if (form == 0) {
        st256(c, 0, DATA_O);
    } else if (form == 2) {
        store_rax(c);
    }
    epilogue(c);
    if (g_dump) {
        char all[300];
        hexbytes(all, c->b, c->n);
        fprintf(stderr, "%-18s L%-4d trip%-2d form%d  %s   (full %s)\n", mnemonic, width, g_pair,
                form, enc, all);
        return;
    }
    memcpy(g_data + DATA_A, g_trips[g_pair].a, 32);
    memcpy(g_data + DATA_B, g_trips[g_pair].b, 32);
    memcpy(g_data + DATA_M, g_trips[g_pair].m, 32);
    memset(g_data + DATA_O, 0xCC, 32);
    for (int i = 0; i < 32; i++) {
        g_data[DATA_P + i] = BLEND_POISON(i);
    }
    if (!run_stub(c)) {
        printf("    // SKIP %s L=%d trip%d(%s) form%d: Rosetta refused this encoding\n", mnemonic,
               width, g_pair, g_trips[g_pair].name, form);
        g_skipped++;
        return;
    }
    char out[80];
    hexbytes(out, g_data + DATA_O, 32);
    printf("    {\"%s\", %d, %d, %d, \"%s\", \"%s\"},\n", mnemonic, width, g_pair, form, enc, out);
    g_rows++;
}

// ---------------------------------------------------------------------------
// Per-shape emission.
// ---------------------------------------------------------------------------
enum { REG_FORM = 0, MEM_FORM = 1 };

// dst = ymm0, VEX.vvvv = ymm1, r/m = ymm2 or [rdi+DATA_B], trailing imm8.
static void gen_imm_form(const char* name, int map, int op, int imm, int l, int mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, 1, map, 1, l, 0, 0, 0, 0);
    emit(&c, (u8)op);
    if (mem) {
        modrm_mem(&c, 0, DATA_B);
    } else {
        modrm_reg(&c, 0, 2);
    }
    emit(&c, (u8)imm);
    row(name, l ? 256 : 128, 0, &c);
}

// vextractps r32/m32, xmm1, imm8.  ModRM.reg is the SOURCE.
static void gen_extractps(int imm, int mem) {
    Code c;
    c.n = 0;
    prologue(&c);
    if (!mem) {
        seed_rax(&c);  // inside the recorded bytes: the zero-extension canary
    }
    vex3(&c, 1, 3, 0, 0, 0, 0, 0, 0);
    emit(&c, 0x17);
    if (mem) {
        modrm_mem(&c, 1, DATA_O);
    } else {
        modrm_reg(&c, 1, 0);  // ModRM.reg = xmm1 (source), r/m = eax
    }
    emit(&c, (u8)imm);
    row(mem ? "vextractps.m" : "vextractps.r", 128, mem ? 1 : 2, &c);
}

// vmaskmov* ymm0, ymmMASK, [rdi+DATA_B].
static void gen_maskload(const char* name, int op, int mask_reg, int l) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, 1, 2, mask_reg, l, 0, 0, 0, 0);
    emit(&c, (u8)op);
    modrm_mem(&c, 0, DATA_B);
    row(name, l ? 256 : 128, 0, &c);
}

// vmaskmov* [rdi+DATA_O], ymm3, ymmSRC.
static void gen_maskstore(const char* name, int op, int src_reg, int l) {
    Code c;
    c.n = 0;
    prologue(&c);
    vex3(&c, 1, 2, 3, l, 0, 0, 0, 0);
    emit(&c, (u8)op);
    modrm_mem(&c, src_reg, DATA_O);
    row(name, l ? 256 : 128, 1, &c);
}

// ---------------------------------------------------------------------------
// The fault probe: does a masked-off element touch memory at all?
//
// `edge` points 16 bytes before a PROT_NONE page, so a 256-bit access covers
// 16 readable bytes and 16 unmapped ones.  The mask clears every element in
// the second half.  Hardware must complete the access; anything that reads or
// writes the whole 32 bytes takes SIGSEGV/SIGBUS and is reported as such.
// ---------------------------------------------------------------------------
static void fault_probe(const char* name, int op, int store) {
    static u8* guard;
    if (!guard) {
        guard = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (guard == MAP_FAILED) {
            printf("    // MASKFAULT: probe mmap failed, no measurement\n");
            return;
        }
        if (mprotect(guard + 4096, 4096, PROT_NONE) != 0) {
            printf("    // MASKFAULT: probe mprotect failed, no measurement\n");
            return;
        }
    }
    u8* edge = guard + 4096 - 16;
    memset(guard + 4096 - 16, 0x5A, 16);

    // The mask lives at [rsi+DATA_M]; clear the upper 128 bits so the elements
    // that land on the guard page are masked off.
    for (int i = 0; i < 8; i++) {
        put32(g_data + DATA_M, i, i < 4 ? 0x80000000u : 0x00000000u);
    }
    memset(g_data + DATA_O, 0xCC, 32);
    for (int i = 0; i < 32; i++) {
        g_data[DATA_P + i] = BLEND_POISON(i);
    }

    Code c;
    c.n = 0;
    // ymm3 = mask, ymm1 = data source; both from the SAFE pointer in rsi.
    vex3(&c, 2, 1, 0, 1, 0, 0, 0, 0);
    emit(&c, 0x6F);
    modrm_base(&c, 3, 6, DATA_M);
    vex3(&c, 2, 1, 0, 1, 0, 0, 0, 0);
    emit(&c, 0x6F);
    modrm_base(&c, 1, 6, DATA_A);
    // The instruction under probe, addressing the straddling pointer in rdi.
    vex3(&c, 1, 2, 3, 1, 0, 0, 0, 0);
    emit(&c, (u8)op);
    modrm_base(&c, store ? 1 : 0, 7, 0);
    if (!store) {
        vex3(&c, 2, 1, 0, 1, 0, 0, 0, 0);
        emit(&c, 0x7F);
        modrm_base(&c, 0, 6, DATA_O);
    }
    epilogue(&c);

    install_code(&c);
    if (setjmp(g_jb) == 0) {
        ((stub2_fn)(void*)g_page)(edge, g_data);
        char out[80];
        if (store) {
            hexbytes(out, edge, 16);
            printf("    // MASKFAULT %s %s: completed, wrote %s to the mapped half\n", name,
                   store ? "store" : "load", out);
        } else {
            hexbytes(out, g_data + DATA_O, 32);
            printf("    // MASKFAULT %s load: completed, ymm0 = %s\n", name, out);
        }
    } else {
        printf("    // MASKFAULT %s %s: FAULTED -- the oracle does not suppress the fault\n", name,
               store ? "store" : "load");
    }
    // Restore the mask the data rows expect.
    memcpy(g_data + DATA_M, g_trips[0].m, 32);
}

int main(int argc, char** argv) {
    g_dump = (argc > 1 && strcmp(argv[1], "--dump-encodings") == 0);
    signal(SIGILL, on_fault);
    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);
    signal(SIGTRAP, on_fault);
    signal(SIGFPE, on_fault);
    build_trips();

    g_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_page == MAP_FAILED) {
        perror("mmap code");
        return 2;
    }
    g_data = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_data == MAP_FAILED) {
        perror("mmap data");
        return 2;
    }

    if (!g_dump) {
        printf("// Generated by avx_blend_rosetta_ref.c under Rosetta 2 -- DO NOT EDIT.\n");
        printf("// Every value below is the literal bytes real x86-64 AVX wrote.\n");
        printf("static const AvxBlendInput kAvxBlendInputs[] = {\n");
        for (int i = 0; i < g_ntrips; i++) {
            char a[80], b[80], m[80];
            hexbytes(a, g_trips[i].a, 32);
            hexbytes(b, g_trips[i].b, 32);
            hexbytes(m, g_trips[i].m, 32);
            printf("    {\"%s\", \"%s\", \"%s\", \"%s\"},\n", g_trips[i].name, a, b, m);
        }
        printf("};\n\n");
        printf("static const AvxBlendRef kAvxBlendRefs[] = {\n");
    }

    // vblendps: imm8 values whose two nibbles differ, so a half-swap in the
    // 256-bit form is visible; 0x00 / 0xFF pin the degenerate ends.
    static const int ps_imms[] = {0x00, 0xFF, 0x0F, 0xF0, 0x5A, 0xA5, 0x01, 0x83};
    // vblendpd uses imm8[1:0] at VEX.128 and imm8[3:0] at VEX.256; values that
    // differ in bits 3:2 catch a 256-bit form reusing the low pair.
    static const int pd_imms[] = {0x00, 0x0F, 0x03, 0x0C, 0x05, 0x0A, 0x01, 0x06};
    // vinsertps: the full COUNT_S x COUNT_D grid with ZMASK = 0, then four
    // ZMASK values against one fixed lane pair.  The memory rows reuse the same
    // list, where COUNT_S must be IGNORED -- so rows 0x00/0x40/0x80/0xC0 must
    // all give the SAME answer for a memory operand and different answers for
    // a register one.
    static const int ins_imms[] = {0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60,
                                   0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0,
                                   0xE0, 0xF0, 0x9F, 0x91, 0x98, 0x9A};
    // The /is4 selector register, plus one row whose LOW nibble is non-zero
    // (architecturally ignored) and one selecting ymm1 / ymm2 so a handler
    // reading VEX.vvvv or ModRM.rm as the selector is caught.
    struct {
        int is4;
        int low;
    } static const is4s[] = {{3, 0x0}, {1, 0x0}, {2, 0x0}, {3, 0xB}};

    for (g_pair = 0; g_pair < g_ntrips; g_pair++) {
        for (int l = 0; l < 2; l++) {
            for (int mem = 0; mem < 2; mem++) {
                for (unsigned i = 0; i < sizeof(ps_imms) / sizeof(ps_imms[0]); i++) {
                    gen_imm_form("vblendps", 3, 0x0C, ps_imms[i], l, mem);
                }
                for (unsigned i = 0; i < sizeof(pd_imms) / sizeof(pd_imms[0]); i++) {
                    gen_imm_form("vblendpd", 3, 0x0D, pd_imms[i], l, mem);
                }
                for (unsigned i = 0; i < sizeof(is4s) / sizeof(is4s[0]); i++) {
                    const int imm = (is4s[i].is4 << 4) | is4s[i].low;
                    gen_imm_form("vblendvps", 3, 0x4A, imm, l, mem);
                    gen_imm_form("vblendvpd", 3, 0x4B, imm, l, mem);
                }
            }
        }
        // vextractps and vinsertps are VEX.128 only.
        for (int imm = 0; imm < 4; imm++) {
            gen_extractps(imm, 0);
            gen_extractps(imm, 1);
        }
        for (int mem = 0; mem < 2; mem++) {
            for (unsigned i = 0; i < sizeof(ins_imms) / sizeof(ins_imms[0]); i++) {
                gen_imm_form("vinsertps", 3, 0x21, ins_imms[i], 0, mem);
            }
        }
        for (int l = 0; l < 2; l++) {
            // mask = ymm3, and mask = ymm0 -- the latter ALIASES the
            // destination and encodes VEX.vvvv as raw 1111b, which is also the
            // "no such operand" marker.  A decoder that treats 1111b as absent
            // gets this row wrong.
            gen_maskload("vmaskmovps.ld", 0x2C, 3, l);
            gen_maskload("vmaskmovps.ld", 0x2C, 0, l);
            gen_maskload("vmaskmovpd.ld", 0x2D, 3, l);
            gen_maskload("vmaskmovpd.ld", 0x2D, 0, l);
            gen_maskstore("vmaskmovps.st", 0x2E, 1, l);
            gen_maskstore("vmaskmovps.st", 0x2E, 2, l);
            gen_maskstore("vmaskmovpd.st", 0x2F, 1, l);
            gen_maskstore("vmaskmovpd.st", 0x2F, 2, l);
        }
    }

    if (!g_dump) {
        printf("};\n");
        fault_probe("vmaskmovps", 0x2C, 0);
        fault_probe("vmaskmovpd", 0x2D, 0);
        fault_probe("vmaskmovps", 0x2E, 1);
        fault_probe("vmaskmovpd", 0x2F, 1);
        fprintf(stderr, "rows=%d skipped=%d\n", g_rows, g_skipped);
    }
    return 0;
}
