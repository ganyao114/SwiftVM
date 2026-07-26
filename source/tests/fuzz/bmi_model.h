// A direct transcription of the Intel SDM Vol. 2 pseudo-code for every
// instruction in bmi_ops.inc.  It exists to ADJUDICATE the two emulator
// oracles, not to be an oracle itself: Rosetta and Unicorn disagree on four
// distinct points here, and something independent of both has to say which is
// right.  Written straight from the manual's operation blocks, deliberately
// literally rather than cleverly.
//
// Included by bmi_unicorn_check.c (three-way diff) and by bmi_test.cpp (which
// asserts the committed reference table still matches it, so a regenerated
// table cannot silently drift).

#ifndef SVM_BMI_MODEL_H
#define SVM_BMI_MODEL_H

#include <string.h>

typedef struct {
    unsigned long long dst, dst2;
    int cf, of, zf, sf;
    int has_flags;  // 0 = the instruction leaves every flag alone
} BmiModelOut;

static unsigned long long bmi_mask(int width) {
    return width == 64 ? ~0ull : 0xFFFFFFFFull;
}

static unsigned long long bmi_pdep(unsigned long long src, unsigned long long mask) {
    unsigned long long out = 0, bit = 1;
    while (mask) {
        unsigned long long low = mask & (~mask + 1);
        if (src & bit) {
            out |= low;
        }
        mask ^= low;
        bit <<= 1;
    }
    return out;
}

static unsigned long long bmi_pext(unsigned long long src, unsigned long long mask) {
    unsigned long long out = 0, bit = 1;
    while (mask) {
        unsigned long long low = mask & (~mask + 1);
        if (src & low) {
            out |= bit;
        }
        mask ^= low;
        bit <<= 1;
    }
    return out;
}

// a = the value in rcx/r11 (and rdx), b = the value in rsi/r10 or in memory.
// Which of the two is "SRC1" depends on the shape; see bmi_ops.inc.
static BmiModelOut bmi_model(const char* name,
                             int width,
                             unsigned imm,
                             unsigned long long a,
                             unsigned long long b) {
    BmiModelOut o;
    const unsigned long long m = bmi_mask(width);
    const unsigned long long src1 = a & m;  // vvvv operand
    const unsigned long long src2 = b & m;  // r/m operand
    const unsigned long long msb = 1ull << (width - 1);
    memset(&o, 0, sizeof(o));
    o.has_flags = 1;

    if (strcmp(name, "andn") == 0) {
        o.dst = (~src1) & src2 & m;
        o.cf = 0;
        o.of = 0;
    } else if (strcmp(name, "blsr") == 0) {
        o.dst = ((src2 - 1) & src2) & m;
        o.cf = src2 == 0;
        o.of = 0;
    } else if (strcmp(name, "blsmsk") == 0) {
        o.dst = ((src2 - 1) ^ src2) & m;
        o.cf = src2 == 0;
        o.of = 0;
    } else if (strcmp(name, "blsi") == 0) {
        // SDM: "IF SRC = 0 THEN CF := 0 ELSE CF := 1" -- the OPPOSITE of BLSR
        // and BLSMSK.  This is the single point Unicorn gets backwards.
        o.dst = ((0 - src2) & src2) & m;
        o.cf = src2 != 0;
        o.of = 0;
    } else if (strcmp(name, "bextr") == 0) {
        // START = SRC2[7:0], LEN = SRC2[15:8], and neither is reduced modulo
        // the operand size.  SRC1 here is the r/m operand and the CONTROL word
        // is the vvvv operand, i.e. `a`.
        const unsigned start = (unsigned)(a & 0xFF);
        const unsigned len = (unsigned)((a >> 8) & 0xFF);
        unsigned long long v = 0;
        if (start < (unsigned)width) {
            v = src2 >> start;
        }
        if (len < (unsigned)width) {
            v &= (len == 0) ? 0ull : ((1ull << len) - 1);
        }
        o.dst = v & m;
        o.cf = 0;
        o.of = 0;
        o.sf = -1;  // architecturally undefined
    } else if (strcmp(name, "bzhi") == 0) {
        // N = SRC2[7:0] where SRC2 is the vvvv operand; SRC1 is r/m.  N is NOT
        // reduced modulo the operand size, and CF is "N > OperandSize - 1".
        const unsigned n = (unsigned)(a & 0xFF);
        unsigned long long v = src2;
        if (n < (unsigned)width) {
            v &= (n == 0) ? 0ull : ((1ull << n) - 1);
        }
        o.dst = v & m;
        o.cf = n > (unsigned)(width - 1);
        o.of = 0;
    } else if (strcmp(name, "pdep") == 0) {
        o.dst = bmi_pdep(src1, src2) & m;
        o.has_flags = 0;
    } else if (strcmp(name, "pext") == 0) {
        o.dst = bmi_pext(src1, src2) & m;
        o.has_flags = 0;
    } else if (strcmp(name, "mulx") == 0) {
        // The implicit multiplicand is rDX, which the harness seeds with `a`.
        o.has_flags = 0;
        if (width == 32) {
            const unsigned long long p = (a & 0xFFFFFFFFull) * (b & 0xFFFFFFFFull);
            o.dst = (p >> 32) & 0xFFFFFFFFull;  // ModRM.reg = HIGH
            o.dst2 = p & 0xFFFFFFFFull;         // VEX.vvvv = LOW
        } else {
            const unsigned __int128 p = (unsigned __int128)a * b;
            o.dst = (unsigned long long)(p >> 64);
            o.dst2 = (unsigned long long)p;
        }
    } else if (strcmp(name, "shlx") == 0 || strcmp(name, "shrx") == 0 ||
               strcmp(name, "sarx") == 0) {
        // The count IS reduced modulo the operand size for these three.
        const unsigned n = (unsigned)(a & (unsigned)(width - 1));
        o.has_flags = 0;
        if (name[1] == 'h' && name[2] == 'l') {
            o.dst = (src2 << n) & m;
        } else if (name[0] == 's' && name[1] == 'h') {
            o.dst = (src2 >> n) & m;
        } else {
            const long long sv = (long long)(width == 64 ? src2
                                                         : (unsigned long long)(long long)(int)src2);
            o.dst = ((unsigned long long)(sv >> n)) & m;
        }
    } else if (strncmp(name, "rorx", 4) == 0) {
        const unsigned n = imm & (unsigned)(width - 1);
        o.has_flags = 0;
        o.dst = n == 0 ? src2 : ((src2 >> n) | (src2 << (width - n))) & m;
    } else if (strcmp(name, "tzcnt") == 0 || strcmp(name, "lzcnt") == 0) {
        unsigned long long v = 0;
        if (src2 == 0) {
            v = (unsigned long long)width;
        } else if (name[0] == 't') {
            while (((src2 >> v) & 1) == 0) {
                ++v;
            }
        } else {
            unsigned long long i = (unsigned long long)width;
            while (i-- > 0) {
                if ((src2 >> i) & 1) {
                    break;
                }
                ++v;
            }
        }
        o.dst = v;
        o.cf = src2 == 0;   // CF = source was zero
        o.zf = v == 0;      // ZF = RESULT is zero, not the source
        o.of = 0;           // SDM says undefined; hardware clears it
        o.sf = -1;
        return o;
    } else {
        o.has_flags = -1;  // unknown instruction
        return o;
    }
    if (o.has_flags == 1) {
        o.zf = (o.dst & m) == 0;
        if (o.sf != -1) {
            o.sf = (o.dst & msb) != 0;
        }
    }
    return o;
}

#endif  // SVM_BMI_MODEL_H
