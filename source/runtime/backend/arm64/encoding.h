#pragma once

#include "runtime/externals/vixl/aarch64/assembler-aarch64.h"

namespace swift::runtime::backend::arm64::encoding {
namespace v = vixl::aarch64;
using v::Condition;
using v::Extend;
using v::MemOperand;
using v::Operand;
using v::Register;
using v::Shift;
using v::VRegister;

inline Register X(unsigned code) { return Register(code, 64); }

inline Register W(unsigned code) { return Register(code, 32); }

inline Register R(unsigned code, unsigned bits) { return bits == 32 ? W(code) : X(code); }

inline Register Base(unsigned code) { return code == 31 ? Register(v::sp) : X(code); }

inline VRegister V(unsigned code, unsigned bits, unsigned lanes = 1) {
    return VRegister(code, bits, lanes);
}

inline VRegister Q(unsigned code) { return V(code, 128); }

inline VRegister D(unsigned code) { return V(code, 64); }

inline VRegister S(unsigned code) { return V(code, 32); }

inline VRegister Lanes(unsigned code, unsigned lane_bits, unsigned bits = 128) {
    return V(code, bits, bits / lane_bits);
}
// This VIXL capture has no AES/SHA entry points or 64-bit polynomial
// multiply form. Keep their exact, typed register fields at this one encoder.
enum class CryptoUnary : std::uint32_t {
    AesEncrypt = 0x4e284800,
    AesDecrypt = 0x4e285800,
    AesMixColumns = 0x4e286800,
    AesInverseMixColumns = 0x4e287800,
    Sha256Schedule0 = 0x5e282800,
};
enum class CryptoTernary : std::uint32_t {
    PolynomialMultiply = 0x0ee0e000,
    PolynomialMultiplyHigh = 0x4ee0e000,
    Sha256Hash = 0x5e004000,
    Sha256Hash2 = 0x5e005000,
    Sha256Schedule1 = 0x5e006000,
};

inline void Crypto(v::Assembler& sink, CryptoUnary operation, VRegister result, VRegister source) {
    VIXL_ASSERT(result.Is128Bits() && source.Is128Bits());
    sink.dc32(static_cast<std::uint32_t>(operation) | (source.GetCode() << 5) | result.GetCode());
}

inline void Crypto(v::Assembler& sink,
                   CryptoTernary operation,
                   VRegister result,
                   VRegister left,
                   VRegister right) {
    VIXL_ASSERT(result.Is128Bits() && left.Is128Bits() && right.Is128Bits());
    sink.dc32(static_cast<std::uint32_t>(operation) | (right.GetCode() << 16) |
              (left.GetCode() << 5) | result.GetCode());
}

// A real encoded A64 immediate. Selection and emission share the one VIXL
// decode; it is never stored beside IR nodes or passed across compile calls.
struct LogicalMask {
    unsigned n, s, r;

    bool Encode(std::uint64_t value, unsigned width) {
        return v::Assembler::IsImmLogical(value, width, &n, &s, &r);
    }

    void Emit(v::Assembler& sink, Register result, Register input, v::LogicalOp op) const {
        sink.LogicalImmediate(result, input, n, s, r, op);
    }
};

// Runtime patching uses the same exact VIXL fields as normal emission. No
// temporary assembler, allocation, implicit label or pool exists on this path.
inline std::uint32_t Branch(std::int64_t words) {
    return v::B | v::Assembler::ImmUncondBranch(words);
}

inline std::uint32_t LoadLiteral16(std::int64_t words) {
    return v::LDR_x_lit | v::Assembler::ImmLLiteral(words) | v::Assembler::Rt(v::x16);
}

inline std::uint32_t Retarget19(std::uint32_t instruction, std::int64_t words) {
    return (instruction & ~v::ImmCondBranch_mask) | v::Assembler::ImmCondBranch(words);
}

inline bool Imm19(std::uint32_t instruction) {
    return (instruction & v::CompareBranchFMask) == v::CompareBranchFixed ||
           (instruction & v::ConditionalBranchFMask) == v::ConditionalBranchFixed;
}
enum class OrderedKind { None, Load, Store };

inline OrderedKind OrderedAccess(std::uint32_t word) {
    const auto* instruction = v::Instruction::Cast(&word);
    if (instruction->Mask(v::LoadStoreRCpcUnscaledOffsetFMask) ==
        v::LoadStoreRCpcUnscaledOffsetFixed) {
        switch (instruction->Mask(v::LoadStoreRCpcUnscaledOffsetMask)) {
            case v::STLURB:
            case v::STLURH:
            case v::STLUR_w:
            case v::STLUR_x:
                return OrderedKind::Store;
            case v::LDAPURB:
            case v::LDAPURH:
            case v::LDAPUR_w:
            case v::LDAPUR_x:
            case v::LDAPURSB_w:
            case v::LDAPURSB_x:
            case v::LDAPURSH_w:
            case v::LDAPURSH_x:
            case v::LDAPURSW:
                return OrderedKind::Load;
            default:
                return OrderedKind::None;
        }
    }
    constexpr auto fields = ~(v::LSSize_mask | v::Rn_mask | v::Rt_mask);
    switch (word & fields) {
        case (v::LDARB_w | v::Rs_mask | v::Rt2_mask) & fields:
        case (v::LDAPRB | v::Rs_mask) & fields:
            return OrderedKind::Load;
        case (v::STLRB_w | v::Rs_mask | v::Rt2_mask) & fields:
            return OrderedKind::Store;
        default:
            return OrderedKind::None;
    }
}

// Decode only an actual ordered access emitted by this backend. The cold
// access uses the same registers, signedness and byte displacement; VIXL
// remains the sole instruction encoder.
inline void PlainOrderedAccess(v::Assembler& sink, std::uint32_t word) {
    const auto* instruction = v::Instruction::Cast(&word);
    const auto rt = static_cast<unsigned>(instruction->GetRt());
    const auto rn = static_cast<unsigned>(instruction->GetRn());
    const auto bits = 8u << instruction->GetLSSize();
    if (instruction->Mask(v::LoadStoreRCpcUnscaledOffsetFMask) ==
        v::LoadStoreRCpcUnscaledOffsetFixed) {
        const MemOperand memory(Base(rn), instruction->GetImmLS());
        switch (instruction->Mask(v::LoadStoreRCpcUnscaledOffsetMask)) {
            case v::STLURB:
                sink.sturb(W(rt), memory, v::RequireUnscaledOffset);
                return;
            case v::STLURH:
                sink.sturh(W(rt), memory, v::RequireUnscaledOffset);
                return;
            case v::STLUR_w:
                sink.stur(W(rt), memory, v::RequireUnscaledOffset);
                return;
            case v::STLUR_x:
                sink.stur(X(rt), memory, v::RequireUnscaledOffset);
                return;
            case v::LDAPURSB_w:
                sink.ldursb(W(rt), memory, v::RequireUnscaledOffset);
                return;
            case v::LDAPURSB_x:
                sink.ldursb(X(rt), memory, v::RequireUnscaledOffset);
                return;
            case v::LDAPURSH_w:
                sink.ldursh(W(rt), memory, v::RequireUnscaledOffset);
                return;
            case v::LDAPURSH_x:
                sink.ldursh(X(rt), memory, v::RequireUnscaledOffset);
                return;
            case v::LDAPURSW:
                sink.ldursw(X(rt), memory, v::RequireUnscaledOffset);
                return;
            case v::LDAPURB:
                sink.ldurb(W(rt), memory, v::RequireUnscaledOffset);
                return;
            case v::LDAPURH:
                sink.ldurh(W(rt), memory, v::RequireUnscaledOffset);
                return;
            default:
                sink.ldur(R(rt, bits), memory, v::RequireUnscaledOffset);
                return;
        }
    }
    const MemOperand memory(Base(rn));
    if (OrderedAccess(word) == OrderedKind::Load) {
        if (bits == 8)
            sink.ldrb(W(rt), memory, v::RequireScaledOffset);
        else if (bits == 16)
            sink.ldrh(W(rt), memory, v::RequireScaledOffset);
        else
            sink.ldr(R(rt, bits), memory, v::RequireScaledOffset);
    } else {
        if (bits == 8)
            sink.strb(W(rt), memory, v::RequireScaledOffset);
        else if (bits == 16)
            sink.strh(W(rt), memory, v::RequireScaledOffset);
        else
            sink.str(R(rt, bits), memory, v::RequireScaledOffset);
    }
}

struct Branch19 {
    Condition condition{v::al};
    Register value{v::xzr};
    bool compare{}, nonzero{};

    static Branch19 Flags(unsigned condition) {
        return {static_cast<Condition>(condition), v::xzr, false, false};
    }

    static Branch19 Zero(Register value, bool nonzero) { return {v::al, value, true, nonzero}; }

    Branch19 Inverse() const {
        auto result = *this;
        if (compare)
            result.nonzero = !nonzero;
        else
            result.condition = static_cast<Condition>(condition ^ 1);
        return result;
    }

    void Emit(v::Assembler& sink, std::int64_t words) const {
        if (!compare)
            sink.b(words, condition);
        else if (nonzero)
            sink.cbnz(value, words);
        else
            sink.cbz(value, words);
    }
};
}  // namespace swift::runtime::backend::arm64::encoding
