//
// Created by SwiftGan on 2021/1/2.
//

#pragma once

#include <array>
#include "base/common_funcs.h"
#include "runtime/common/bit_fields.h"
#include "runtime/common/types.h"

namespace swift::x86 {

using namespace swift::runtime;

struct CPUFlagsBit {
    constexpr static auto Carry = 0;
    constexpr static auto Parity = 1;
    constexpr static auto FlagAF = 2;
    constexpr static auto Zero = 3;
    constexpr static auto Signed = 4;
    constexpr static auto Overflow = 5;
    constexpr static auto Direction = 6;
};

enum class CPUFlags : u8 {
    Carry = 1 << CPUFlagsBit::Carry,
    Overflow = 1 << CPUFlagsBit::Overflow,
    Signed = 1 << CPUFlagsBit::Signed,
    Zero = 1 << CPUFlagsBit::Zero,
    Parity = 1 << CPUFlagsBit::Parity,
    FlagAF = 1 << CPUFlagsBit::FlagAF,
    Direction = 1 << CPUFlagsBit::Direction,
    FlagsAll = Carry | Overflow | Signed | Zero | Parity | FlagAF
};

enum class InterruptReason : u32 {
    SVC,
    HLT,
    BRK,
    ILL_CODE,
    PAGE_FATAL,
    FALLBACK
};

DECLARE_ENUM_FLAG_OPERATORS(CPUFlags)

enum Register {
    RAX = 0,
    RCX = 1,
    RDX = 2,
    RBX = 3,
    RSP = 4,
    RBP = 5,
    RSI = 6,
    RDI = 7,
    R8 = 8,
    R9 = 9,
    R10 = 10,
    R11 = 11,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    R15 = 15,
    kLastCpuRegister = 15,
    kNumberOfCpuRegisters = 16,
    kNoRegister = -1,  // Signals an illegal register.
    RIP = 74
};

enum FloatRegister {
    XMM0 = 0,
    XMM1 = 1,
    XMM2 = 2,
    XMM3 = 3,
    XMM4 = 4,
    XMM5 = 5,
    XMM6 = 6,
    XMM7 = 7,
    XMM8 = 8,
    XMM9 = 9,
    XMM10 = 10,
    XMM11 = 11,
    XMM12 = 12,
    XMM13 = 13,
    XMM14 = 14,
    XMM15 = 15,
    kNumberOfFloatRegisters = 16
};

union RegW {
    u16 word;
    struct {
        u8 low;
        u8 high;
    };
};

union RegD {
    u32 dword;
    struct {
        RegW low;
        RegW high;
    };
};

union Reg {
    u64 qword;
    struct {
        RegD low;
        RegD high;
    };
};

union Xmm {
    double d[2];
    float f[4];

    u64 l[2];
    u32 i[4];
    u16 s[8];
    u8 b[16];
};

union EFlags {
    using CF = BitField<0, 1, u32>;  // carry: set to true when an arithmetic carry occurs
    using PF = BitField<1, 1, u32>;  // parity: set to true if the number of bits set in the low 8
                                     // bits of the result is even
    using AF = BitField<2, 1, u32>;  // adjust: set to true if operation on least significant 4 bits
                                     // caused carry
    using ZF = BitField<3, 1, u32>;  // zero: set if operation result is 0
    using SF = BitField<4, 1, u32>;  // sign: set if most significant bit of result is 1
    using OF = BitField<5, 1, u32>;  // overflow: set when the result has a sign different from the
                                     // expected one (carry into ^ carry out)
    using DF = BitField<6, 1, u32>;  // direction: controls increment/decrement of D register after
                                     // string instructions
    using Flags = u32;

    Flags flags;
    CF cf;
    PF pf;
    AF af;
    ZF zf;
    SF sf;
    OF of;
    DF df;
};

constexpr u8 GetEFlagBit(CPUFlags flag) {
    switch (flag) {
        case CPUFlags::Carry:
            return EFlags::CF::pos;
        case CPUFlags::Parity:
            return EFlags::PF::pos;
        case CPUFlags::FlagAF:
            return EFlags::AF::pos;
        case CPUFlags::Zero:
            return EFlags::ZF::pos;
        case CPUFlags::Signed:
            return EFlags::SF::pos;
        case CPUFlags::Overflow:
            return EFlags::OF::pos;
        case CPUFlags::Direction:
            return EFlags::DF::pos;
        default:
            PANIC();
    }
    return 0;
}

struct ThreadContext64 {
    union {
        std::array<Reg, 16> regs;
        struct {
            Reg rax;
            Reg rcx;
            Reg rdx;
            Reg rbx;
            Reg rsp;
            Reg rbp;
            Reg rsi;
            Reg rdi;
            Reg r8;
            Reg r9;
            Reg r10;
            Reg r11;
            Reg r12;
            Reg r13;
            Reg r14;
            Reg r15;
        };
    };
    Reg es, cs, ss, ds, fs, gs;
    Reg pc;
    std::array<Xmm, 16> xmms;
    EFlags ef;
    InterruptReason interrupt;
};

}  // namespace swift::x86
