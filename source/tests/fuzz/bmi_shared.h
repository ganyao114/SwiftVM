// Shared BMI encoder + case table.  Included by bmi_rosetta_ref.c (the hardware
// reference generator), by bmi_unicorn_check.c (the second oracle) and by
// bmi_test.cpp (the SwiftVM differential), so all three necessarily emit the
// SAME instruction bytes for the same table row.  A wrong opcode or a wrong
// operand-role mapping therefore cannot make one side test a different
// instruction from another -- it can only make all of them wrong together, and
// that is what --dump-encodings plus a disassembler is for.
//
// C and C++ both compile this; keep it free of anything C++-only.

#ifndef SVM_BMI_SHARED_H
#define SVM_BMI_SHARED_H

// Data-block layout.
#define DATA_M 0x00    // the r/m memory operand (holds `b`)
#define DATA_IN 0x08   // NREG seeded/poisoned register values, in REG_ORDER
#define DATA_OUT 0x60  // the same NREG registers read back
#define DATA_FL 0xB8   // 5 flag bytes: C, O, Z, S, P
#define DATA_SIZE 0xC0

// The registers each stub loads before and stores after, in this order.
// rsp (4), rbp (5), rdi (7) and r13 (13) are excluded: the stub needs them.
// r8..r12 and r14 are pure bystanders; a handler that folded VEX.R or VEX.B
// into the wrong operand lands in one of them.
#define NREG 11
static const int REG_ORDER[NREG] = {0, 1, 2, 3, 6, 8, 9, 10, 11, 12, 14};

typedef struct {
    unsigned char b[512];
    int n;
} Buf;

static void emit(Buf* c, unsigned char v) { c->b[c->n++] = v; }

// All fields UN-inverted; this inverts R/X/B/vvvv exactly as the hardware
// expects.  pp: 0 none, 1 = 66, 2 = F3, 3 = F2.  mm: 2 = 0F38, 3 = 0F3A.
static void vex3(Buf* c, int pp, int mm, int vvvv, int l, int r, int x, int b, int w) {
    emit(c, 0xC4);
    emit(c, (unsigned char)(((~r & 1) << 7) | ((~x & 1) << 6) | ((~b & 1) << 5) | (mm & 0x1F)));
    emit(c, (unsigned char)(((w & 1) << 7) | ((~vvvv & 0xF) << 3) | ((l & 1) << 2) | (pp & 3)));
}

static void modrm_reg(Buf* c, int reg, int rm) {
    emit(c, (unsigned char)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

// [r13 + disp8].  r13's low three bits are 101b, which forces mod=01 with an
// explicit displacement byte even when the displacement is zero.
static void modrm_mem_r13(Buf* c, int reg, int disp) {
    emit(c, (unsigned char)(0x40 | ((reg & 7) << 3) | 5));
    emit(c, (unsigned char)disp);
}

// [r13 + disp32].  The harness block runs past offset 0x7F, and a disp8 is
// SIGNED -- encoding 0x80 as a byte addresses r13-128, i.e. off the front of
// the data page.  (That was a real bug here: every stub faulted.)
static void modrm_mem32_r13(Buf* c, int reg, int disp) {
    emit(c, (unsigned char)(0x80 | ((reg & 7) << 3) | 5));
    emit(c, (unsigned char)(disp & 0xFF));
    emit(c, (unsigned char)((disp >> 8) & 0xFF));
    emit(c, (unsigned char)((disp >> 16) & 0xFF));
    emit(c, (unsigned char)((disp >> 24) & 0xFF));
}

static void mov_load(Buf* c, int reg, int disp) {
    emit(c, (unsigned char)(0x48 | (((reg >> 3) & 1) << 2) | 1));  // REX.W + R + B(r13)
    emit(c, 0x8B);
    modrm_mem32_r13(c, reg, disp);
}

static void mov_store(Buf* c, int reg, int disp) {
    emit(c, (unsigned char)(0x48 | (((reg >> 3) & 1) << 2) | 1));
    emit(c, 0x89);
    modrm_mem32_r13(c, reg, disp);
}

// setcc byte [r13+disp32]; cc: 0x92=c 0x90=o 0x94=z 0x98=s 0x9A=p
static void setcc_mem(Buf* c, unsigned char cc, int disp) {
    emit(c, 0x41);  // REX.B so rm=101 means r13, not rbp
    emit(c, 0x0F);
    emit(c, cc);
    modrm_mem32_r13(c, 0, disp);
}

// ---------------------------------------------------------------------------
// The shared table
// ---------------------------------------------------------------------------
enum BmiShape { RVM, RMV, VM, RMI, MULX, LEG };

typedef struct {
    const char* name;
    int pp, mm, op, ext, shape, imm;
} Op;

static const Op g_ops[] = {
#define SVM_BMI_OP(name, pp, mm, op, ext, shape, imm) {#name, pp, mm, op, ext, shape, imm},
#include "bmi_ops.inc"
};
#define NOPS ((int)(sizeof(g_ops) / sizeof(g_ops[0])))

typedef struct {
    const char* name;
    unsigned long long a, b;
} Pair;

static const Pair g_pairs[] = {
#define SVM_BMI_PAIR(name, a, b) {#name, a, b},
#include "bmi_ops.inc"
};
#define NPAIRS ((int)(sizeof(g_pairs) / sizeof(g_pairs[0])))

// Register assignment per shape.  variant 0 = low registers with a register
// r/m, 1 = low registers with a MEMORY r/m, 2 = HIGH registers (r8/r9/r10/r11),
// the only variant that exercises VEX.R / VEX.B folding for this family.
typedef struct {
    int dst, dst2, vvvv, rm;  // architectural register numbers; rm < 0 = memory
} Assign;

static Assign assign_for(int shape, int variant) {
    Assign a;
    const int hi = variant == 2;
    const int dst = hi ? 8 : 0;    // r8  / rax
    const int alt = hi ? 9 : 3;    // r9  / rbx
    const int src = hi ? 11 : 1;   // r11 / rcx
    const int rmr = hi ? 10 : 6;   // r10 / rsi
    a.dst = dst;
    a.dst2 = -1;
    a.vvvv = src;
    a.rm = variant == 1 ? -1 : rmr;
    if (shape == VM) {
        a.vvvv = dst;  // BLSI/BLSMSK/BLSR put the DESTINATION in vvvv
    } else if (shape == MULX) {
        a.dst2 = alt;  // ModRM.reg = high half, vvvv = low half
        a.vvvv = alt;
    } else if (shape == RMI) {
        a.vvvv = 0;  // encoded as 1111, i.e. "no vvvv operand"
    }
    return a;
}

// The value each register is loaded with before the instruction runs.  The two
// architectural inputs are `a` (rcx / r11, and rdx which MULX reads implicitly)
// and `b` (rsi / r10).  Everything else gets a per-register poison that no
// input can collide with, so a write to the wrong register is not merely
// detectable but identifiable.
static unsigned long long seed_for(int reg, const Pair* p) {
    switch (reg) {
        case 1:   // rcx
        case 11:  // r11
            return p->a;
        case 2:  // rdx: MULX's implicit multiplicand
            return p->a;
        case 6:   // rsi
        case 10:  // r10
            return p->b;
        default:
            return 0xD1CE000000000000ull | ((unsigned long long)reg * 0x0001010101010101ull);
    }
}

// Just the instruction under test.
static void emit_insn(Buf* c, const Op* o, int w, const Assign* a) {
    const int rm_reg = a->rm < 0 ? 13 /* r13 base */ : a->rm;
    if (o->mm == 0) {
        // Legacy F3 [REX] 0F op /r (tzcnt / lzcnt).
        emit(c, 0xF3);
        const int rex = (w ? 8 : 0) | (((a->dst >> 3) & 1) << 2) | ((rm_reg >> 3) & 1);
        if (rex) {
            emit(c, (unsigned char)(0x40 | rex));
        }
        emit(c, 0x0F);
        emit(c, (unsigned char)o->op);
        if (a->rm < 0) {
            modrm_mem_r13(c, a->dst, DATA_M);
        } else {
            modrm_reg(c, a->dst, a->rm);
        }
        return;
    }
    // VEX.LZ.  vvvv is passed UN-inverted; for RMI, 0 is what encodes 1111.
    vex3(c, o->pp, o->mm, a->vvvv, 0, (a->dst >> 3) & 1, 0, (rm_reg >> 3) & 1, w);
    emit(c, (unsigned char)o->op);
    const int reg_field = o->shape == VM ? o->ext : a->dst;
    if (a->rm < 0) {
        modrm_mem_r13(c, reg_field, DATA_M);
    } else {
        modrm_reg(c, reg_field, a->rm);
    }
    if (o->shape == RMI) {
        emit(c, (unsigned char)o->imm);
    }
}

// The self-contained stub the hardware generator and the Unicorn cross-check
// execute.  bmi_test.cpp emits the same body without the prologue/epilogue --
// it seeds the guest context directly and ends the block with HLT.
static void build_stub(Buf* c, const Op* o, int w, const Assign* a) {
    int i;
    c->n = 0;
    emit(c, 0x53);              // push rbx
    emit(c, 0x55);              // push rbp
    emit(c, 0x41); emit(c, 0x54);  // push r12
    emit(c, 0x41); emit(c, 0x55);  // push r13
    emit(c, 0x41); emit(c, 0x56);  // push r14
    emit(c, 0x41); emit(c, 0x57);  // push r15
    emit(c, 0x49); emit(c, 0x89); emit(c, 0xFD);  // mov r13, rdi
    // Put EFLAGS in a KNOWN state (0x202: reserved bit 1 + IF, every
    // arithmetic flag clear) before the instruction runs.  Without this the
    // "no flags affected" instructions -- SARX/SHLX/SHRX/RORX/MULX/PDEP/PEXT,
    // which is most of BMI2 -- record whatever the caller happened to leave
    // behind, and the reference cannot distinguish "did not write CF" from
    // "wrote CF = whatever was already there".  MOV does not touch flags, so
    // the register loads below preserve this.
    emit(c, 0x48); emit(c, 0xBF);  // movabs rdi, 0x202
    emit(c, 0x02); emit(c, 0x02); emit(c, 0x00); emit(c, 0x00);
    emit(c, 0x00); emit(c, 0x00); emit(c, 0x00); emit(c, 0x00);
    emit(c, 0x57);  // push rdi
    emit(c, 0x9D);  // popfq
    for (i = 0; i < NREG; ++i) {
        mov_load(c, REG_ORDER[i], DATA_IN + i * 8);
    }
    emit_insn(c, o, w, a);
    setcc_mem(c, 0x92, DATA_FL + 0);
    setcc_mem(c, 0x90, DATA_FL + 1);
    setcc_mem(c, 0x94, DATA_FL + 2);
    setcc_mem(c, 0x98, DATA_FL + 3);
    setcc_mem(c, 0x9A, DATA_FL + 4);
    for (i = 0; i < NREG; ++i) {
        mov_store(c, REG_ORDER[i], DATA_OUT + i * 8);
    }
    emit(c, 0x41); emit(c, 0x5F);  // pop r15
    emit(c, 0x41); emit(c, 0x5E);  // pop r14
    emit(c, 0x41); emit(c, 0x5D);  // pop r13
    emit(c, 0x41); emit(c, 0x5C);  // pop r12
    emit(c, 0x5D);                 // pop rbp
    emit(c, 0x5B);                 // pop rbx
    emit(c, 0xC3);                 // ret
}

// A reference row, as printed by bmi_rosetta_ref.c.
typedef struct {
    const char* name;
    int width;
    int variant;
    const char* pair;
    unsigned long long dst;
    unsigned long long dst2;
    int cf, of, zf, sf, pf;
    int clean;
} BmiRef;

#endif  // SVM_BMI_SHARED_H
