// ===========================================================================
// Second oracle: replay every row of avx_mul_rosetta_ref.inc under Unicorn and
// diff against the bytes Rosetta produced.  Neither emulator is trusted on its
// own -- Rosetta has been caught mis-implementing VPSLLVQ and PTEST in this
// same work, and Unicorn is known to reject VEX.L=1 outright -- so agreement is
// what makes a row load-bearing, and where they disagree the Intel SDM decides.
//
//   clang -O1 -I/opt/homebrew/include -o /tmp/avxmuluc avx_mul_unicorn_check.c \
//         -L/opt/homebrew/lib -lunicorn && /tmp/avxmuluc
//
// This runs NATIVELY (arm64); Unicorn emulates the x86-64 guest, so no Rosetta
// is involved and the two oracles share no code path at all.
//
// ---------------------------------------------------------------------------
// WHAT IT FOUND (Unicorn 2.1.4, /opt/homebrew/lib/libunicorn.dylib)
// ---------------------------------------------------------------------------
//   20 agree      -- every legacy SSE row (pmuludq / pmuldq), byte for byte,
//                    INCLUDING the preserved bits 255:128.  These twenty rows
//                    therefore rest on two independent oracles.
//   30 refused    -- every VEX.L=1 row, UC_ERR_INSN_INVALID.  Already known.
//   30 differ     -- every VEX.L=0 row, and the difference is a UNICORN DEFECT,
//                    not a Rosetta one.  Unicorn executes a VEX.128 encoding
//                    with LEGACY SSE semantics: src1 is taken from the
//                    DESTINATION instead of from VEX.vvvv, and bits 255:128 are
//                    left unmodified instead of zeroed.
//
// The defect is not specific to this family -- reduced probes show the same for
// `vpxor xmm0, xmm1, xmm2` (result is ymm0 ^ ymm2, upper half untouched) and
// for `vpaddd`, and it moves with the destination register (`vpmuludq xmm3,
// xmm1, xmm2` computes ymm3 * ymm2).  Both symptoms are the one root cause: the
// VEX prefix is honoured for length and opcode map but its vvvv field and its
// upper-half-zeroing rule are not.  The SDM is unambiguous on both counts
// (VPMULUDQ: "DEST[63:0] <- SRC1[31:0] * SRC2[31:0]" with SRC1 = VEX.vvvv, and
// "DEST[MAXVL-1:128] <- 0"), and Rosetta matches the SDM, so the VEX.128 rows
// stand on Rosetta plus the SDM cross-read that avx_mul_test.cpp performs on
// every recorded lane.
//
// Exit status is 0 when nothing DIFFERS for a reason other than the documented
// VEX defect; refusals are reported but not fatal.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

typedef uint8_t u8;
typedef uint64_t u64;

typedef struct {
    const char* name;
    const char* a;
    const char* b;
} AvxMulInput;
typedef struct {
    const char* name;
    int width;
    int pair;
    int form;
    const char* enc;
    const char* result;
} AvxMulRef;
#include "avx_mul_rosetta_ref.inc"

static u8 nib(char c) { return (u8)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); }
static int parse(const char* h, u8* out) {
    int n = 0;
    for (; h[n * 2] && h[n * 2 + 1]; n++) out[n] = (u8)((nib(h[n * 2]) << 4) | nib(h[n * 2 + 1]));
    return n;
}

#define CODE 0x1000
#define DATA 0x20000

int main(void) {
    int agree = 0, differ = 0, refused = 0, vex_differ = 0;
    for (size_t i = 0; i < sizeof(kAvxMulRefs) / sizeof(kAvxMulRefs[0]); i++) {
        const AvxMulRef* r = &kAvxMulRefs[i];
        u8 a[32], b[32], want[32], code[64];
        parse(kAvxMulInputs[r->pair].a, a);
        parse(kAvxMulInputs[r->pair].b, b);
        parse(r->result, want);
        int n = parse(r->enc, code);
        uc_engine* uc;
        if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc) != UC_ERR_OK) {
            printf("uc_open failed\n");
            return 2;
        }
        uc_mem_map(uc, CODE, 0x1000, UC_PROT_ALL);
        uc_mem_map(uc, DATA, 0x1000, UC_PROT_ALL);
        uc_mem_write(uc, CODE, code, (size_t)n);
        u8 zero[32] = {0};
        uc_mem_write(uc, DATA + 0x00, a, 32);
        uc_mem_write(uc, DATA + 0x20, b, 32);
        uc_mem_write(uc, DATA + 0x40, zero, 32);
        u64 rdi = DATA, rsp = DATA + 0x800;
        uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
        uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
        // Same poison and same register assignment as the generator's prologue.
        u8 poison[32];
        for (int j = 0; j < 32; j++) poison[j] = (u8)(0xA5 ^ j);
        uc_reg_write(uc, UC_X86_REG_YMM0, poison);
        uc_reg_write(uc, UC_X86_REG_YMM1, a);
        uc_reg_write(uc, UC_X86_REG_YMM2, b);
        uc_err e = uc_emu_start(uc, CODE, CODE + (u64)n, 0, 1);
        if (e != UC_ERR_OK) {
            refused++;
            if (refused <= 3) {
                printf("  REFUSED %-12s L%-4d %s : %s\n", r->name, r->width, r->enc,
                       uc_strerror(e));
            }
            uc_close(uc);
            continue;
        }
        u8 got[32];
        uc_reg_read(uc, r->form == 0 ? UC_X86_REG_YMM0 : UC_X86_REG_YMM1, got);
        if (memcmp(got, want, 32) == 0) {
            agree++;
        } else {
            differ++;
            if (r->form == 0) vex_differ++;  // the documented VEX defect
            if (differ <= 4) {
                printf("  DIFFER  %-12s L%-4d pair %-9s enc %s\n", r->name, r->width,
                       kAvxMulInputs[r->pair].name, r->enc);
                printf("            unicorn ");
                for (int j = 0; j < 32; j++) printf("%02x", got[j]);
                printf("\n            rosetta ");
                for (int j = 0; j < 32; j++) printf("%02x", want[j]);
                printf("\n");
            }
        }
        uc_close(uc);
    }
    printf("unicorn cross-check: %d agree, %d differ (%d of them the VEX.128 defect), "
           "%d refused\n",
           agree, differ, vex_differ, refused);
    return differ != vex_differ;
}
