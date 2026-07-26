// ===========================================================================
// Second oracle: re-derive every row of bmi_rosetta_ref.inc with Unicorn and
// diff.  Neither emulator is trusted on its own -- Rosetta has been caught
// mis-implementing VPSLLVQ and XSAVE in this same work, and Unicorn is known
// to reject VEX.L=1 outright -- so a row is only usable when the two agree.
// Where they disagree the Intel SDM decides and the disagreement is recorded.
//
//   clang -O1 -I/opt/homebrew/include -o /tmp/bmiuc bmi_unicorn_check.c \
//         -L/opt/homebrew/lib -lunicorn && /tmp/bmiuc
//
// This runs NATIVELY (arm64); Unicorn emulates the x86-64 guest, so no Rosetta
// is involved and the two oracles share no code path at all.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unicorn/unicorn.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#include "bmi_shared.h"
#include "bmi_model.h"
#include "bmi_rosetta_ref.inc"

#define CODE_BASE 0x100000ull
#define DATA_BASE 0x200000ull
#define STACK_BASE 0x300000ull

static const int UCREG[16] = {
        UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_RBX,
        UC_X86_REG_RSP, UC_X86_REG_RBP, UC_X86_REG_RSI, UC_X86_REG_RDI,
        UC_X86_REG_R8,  UC_X86_REG_R9,  UC_X86_REG_R10, UC_X86_REG_R11,
        UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15};

int main(void) {
    size_t rows = sizeof(kBmiRefs) / sizeof(kBmiRefs[0]);
    size_t checked = 0, disagree = 0, uc_failed = 0, rose_bad = 0, uc_bad = 0;
    for (size_t r = 0; r < rows; ++r) {
        const BmiRef* ref = &kBmiRefs[r];
        const Op* o = NULL;
        for (int i = 0; i < NOPS; ++i) {
            if (strcmp(g_ops[i].name, ref->name) == 0) {
                o = &g_ops[i];
            }
        }
        const Pair* p = NULL;
        for (int i = 0; i < NPAIRS; ++i) {
            if (strcmp(g_pairs[i].name, ref->pair) == 0) {
                p = &g_pairs[i];
            }
        }
        if (!o || !p) {
            printf("row %zu: unknown op/pair\n", r);
            return 1;
        }
        Assign a = assign_for(o->shape, ref->variant);
        Buf c;
        c.n = 0;
        emit_insn(&c, o, ref->width == 64, &a);

        uc_engine* uc;
        if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc) != UC_ERR_OK) {
            return 1;
        }
        uc_mem_map(uc, CODE_BASE, 0x1000, UC_PROT_ALL);
        uc_mem_map(uc, DATA_BASE, 0x1000, UC_PROT_ALL);
        uc_mem_map(uc, STACK_BASE, 0x1000, UC_PROT_ALL);
        uc_mem_write(uc, CODE_BASE, c.b, (size_t)c.n);
        uc_mem_write(uc, DATA_BASE, &p->b, 8);
        for (int i = 0; i < 16; ++i) {
            u64 v = seed_for(i, p);
            if (i == 13) {
                v = DATA_BASE;
            } else if (i == 4) {
                v = STACK_BASE + 0x800;
            }
            uc_reg_write(uc, UCREG[i], &v);
        }
        u64 fl = 0x202;
        uc_reg_write(uc, UC_X86_REG_EFLAGS, &fl);
        uc_err e = uc_emu_start(uc, CODE_BASE, CODE_BASE + (u64)c.n, 0, 0);
        if (e != UC_ERR_OK) {
            if (uc_failed++ < 10) {
                printf("UC FAIL %s w%d v%d %s: %s\n", ref->name, ref->width, ref->variant,
                       ref->pair, uc_strerror(e));
            }
            uc_close(uc);
            continue;
        }
        u64 got[16];
        for (int i = 0; i < 16; ++i) {
            uc_reg_read(uc, UCREG[i], &got[i]);
        }
        uc_reg_read(uc, UC_X86_REG_EFLAGS, &fl);
        const int cf = (fl >> 0) & 1, pf = (fl >> 2) & 1, zf = (fl >> 6) & 1;
        const int sf = (fl >> 7) & 1, of = (fl >> 11) & 1;
        int clean = 1;
        for (int i = 0; i < NREG; ++i) {
            const int rg = REG_ORDER[i];
            if (rg == a.dst || rg == a.dst2) {
                continue;
            }
            if (got[rg] != seed_for(rg, p)) {
                clean = 0;
            }
        }
        const u64 dst = got[a.dst];
        const u64 dst2 = a.dst2 >= 0 ? got[a.dst2] : 0;
        ++checked;
        // Three-way: the SDM model decides between the two emulators.
        const BmiModelOut mo = bmi_model(o->name, ref->width, (unsigned)o->imm, p->a, p->b);
        const int rose_ok = ref->dst == mo.dst && ref->dst2 == mo.dst2 && ref->clean == 1 &&
                            (!mo.has_flags ? (!ref->cf && !ref->of && !ref->zf && !ref->sf)
                                           : (ref->cf == mo.cf && ref->of == mo.of &&
                                              ref->zf == mo.zf &&
                                              (mo.sf == -1 || ref->sf == mo.sf)));
        const int uc_ok = dst == mo.dst && dst2 == mo.dst2 && clean == 1 &&
                          (!mo.has_flags ? (!cf && !of && !zf && !sf)
                                         : (cf == mo.cf && of == mo.of && zf == mo.zf &&
                                            (mo.sf == -1 || sf == mo.sf)));
        if (!rose_ok) {
            ++rose_bad;
        }
        if (!uc_ok) {
            ++uc_bad;
        }
        if (rose_ok && uc_ok) {
            uc_close(uc);
            continue;
        }
        if (disagree++ < 40) {
            printf("%-7s w%-2d v%d %-9s  sdm dst=%016llx dst2=%016llx c%d o%d z%d s%d fl%d\n"
                   "                          rosetta%s dst=%016llx dst2=%016llx c%d o%d z%d s%d\n"
                   "                          unicorn%s dst=%016llx dst2=%016llx c%d o%d z%d s%d\n",
                   ref->name, ref->width, ref->variant, ref->pair,
                   (unsigned long long)mo.dst, (unsigned long long)mo.dst2, mo.cf, mo.of, mo.zf,
                   mo.sf, mo.has_flags,
                   rose_ok ? " OK " : " BAD", (unsigned long long)ref->dst,
                   (unsigned long long)ref->dst2, ref->cf, ref->of, ref->zf, ref->sf,
                   uc_ok ? " OK " : " BAD", (unsigned long long)dst, (unsigned long long)dst2,
                   cf, of, zf, sf);
        }
        uc_close(uc);
    }
    printf("\nrows=%zu checked=%zu unicorn_refused=%zu\n", rows, checked, uc_failed);
    printf("rows where ROSETTA disagrees with the SDM model: %zu\n", rose_bad);
    printf("rows where UNICORN disagrees with the SDM model: %zu\n", uc_bad);
    return rose_bad != 0;
}
