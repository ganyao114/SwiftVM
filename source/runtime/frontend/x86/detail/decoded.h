#pragma once

#include <cstring>
#include <span>
#include <vector>

#include "../instruction.h"
#include "distorm.h"
#include "mnemonics.h"

namespace swift::x86::detail {
inline Op DecodeOpcode(std::uint16_t opcode) noexcept {
    switch (opcode) {
#define X86_OP(name)                                                                               \
    case I_##name:                                                                                 \
        return Op::name;
#define X86_EXTRA_OP(name)
#include "../opcodes.inc"
#undef X86_OP
#undef X86_EXTRA_OP
        default:
            return Op::Unsupported;
    }
}

[[gnu::always_inline]] inline std::uint8_t DecodeRegister(std::uint8_t reg) noexcept {
    // The adapter translates contiguous vendor banks into independently owned
    // guest banks. No vendor identifier escapes this function.
    if (reg <= R_DIL) return static_cast<std::uint8_t>(Register::RAX + reg - R_RAX);
    if (reg >= R_ST0 && reg <= R_ST7) return static_cast<std::uint8_t>(Register::ST0 + reg - R_ST0);
    if (reg >= R_XMM0 && reg <= R_XMM15)
        return static_cast<std::uint8_t>(Register::XMM0 + reg - R_XMM0);
    if (reg >= R_MM0 && reg <= R_MM7) return static_cast<std::uint8_t>(Register::MM0 + reg - R_MM0);
    switch (reg) {
        case R_NONE:
            return Register::None;
        case R_RIP:
            return Register::RIP;
        case R_ES:
            return Register::ES;
        case R_CS:
            return Register::CS;
        case R_SS:
            return Register::SS;
        case R_DS:
            return Register::DS;
        case R_FS:
            return Register::FS;
        case R_GS:
            return Register::GS;
        default:
            return Register::Other;
    }
}

inline GuestOperandKind DecodeOperand(std::uint8_t type) noexcept {
    switch (type) {
        case O_NONE:
            return GuestOperandKind::None;
        case O_REG:
            return GuestOperandKind::Register;
        case O_IMM:
            return GuestOperandKind::Immediate;
        case O_IMM1:
            return GuestOperandKind::ImmediateSecond;
        case O_IMM2:
            return GuestOperandKind::ImmediateThird;
        case O_DISP:
            return GuestOperandKind::Displacement;
        case O_SMEM:
            return GuestOperandKind::Memory;
        case O_MEM:
            return GuestOperandKind::IndexedMemory;
        case O_PC:
            return GuestOperandKind::Relative;
        case O_PTR:
            return GuestOperandKind::FarPointer;
        default:
            return GuestOperandKind::Other;
    }
}

inline FlowKind DecodeFlow(std::uint8_t meta) noexcept {
    switch (META_GET_FC(meta)) {
        case FC_CND_BRANCH:
            return FlowKind::Conditional;
        case FC_UNC_BRANCH:
            return FlowKind::Jump;
        case FC_CALL:
            return FlowKind::Call;
        case FC_RET:
            return FlowKind::Return;
        case FC_SYS:
        case FC_INT:
            return FlowKind::Service;
        default:
            return FlowKind::Linear;
    }
}

inline std::uint16_t DecodeFlags(std::uint16_t flags) noexcept {
    // Both sides describe actual guest RFLAGS positions. Validate that fact
    // here rather than repeating eight conditional translations per mask.
    static_assert(GuestFlag::Carry == D_CF && GuestFlag::Parity == D_PF &&
                  GuestFlag::Auxiliary == D_AF && GuestFlag::Zero == D_ZF &&
                  GuestFlag::Sign == D_SF && GuestFlag::Interrupt == D_IF &&
                  GuestFlag::Direction == D_DF && GuestFlag::Overflow == D_OF);
    return flags &
           (GuestFlag::Carry | GuestFlag::Parity | GuestFlag::Auxiliary | GuestFlag::Zero |
            GuestFlag::Sign | GuestFlag::Interrupt | GuestFlag::Direction | GuestFlag::Overflow);
}

// Width corrections for known parser differences are applied once, before
// the owned semantic record is published. The byte scan remains outside the
// timed vendor call. Trusted parser fixtures may omit bytes and use its mode.
inline void CorrectWidths(InstructionData& out, std::span<const std::uint8_t> bytes) noexcept {
    bool word = out.operand_bits == 16;
    bool rex_w = out.operand_bits == 64;
    const auto prefixes = [&] {
        if (bytes.empty()) return;
        std::uint8_t rex{};
        word = false;
        for (const auto byte : bytes) {
            if (byte == 0x0f) break;
            if (byte >= 0x40 && byte <= 0x4f)
                rex = byte;
            else {
                rex = 0;
                if (byte == 0x66) word = true;
            }
        }
        rex_w = (rex & 8) != 0;
    };
    const auto resize = [](DecodedOperand& operand, unsigned bits) {
        if (operand.type == GuestOperandKind::Register) {
            const unsigned old_base = operand.size == 64   ? Register::RAX
                                      : operand.size == 32 ? Register::EAX
                                                           : Register::AX;
            if (operand.index < old_base || operand.index - old_base >= 16) return;
            const unsigned new_base = bits == 64   ? Register::RAX
                                      : bits == 32 ? Register::EAX
                                                   : Register::AX;
            operand.index = static_cast<std::uint8_t>(new_base + operand.index - old_base);
        }
        operand.size = static_cast<std::uint16_t>(bits);
    };
    const auto xmm_to_mmx = [](DecodedOperand& operand) {
        if (operand.type == GuestOperandKind::Register &&
            operand.index >= Register::XMM0 && operand.index <= Register::XMM15) {
            operand.index =
                    static_cast<std::uint8_t>(Register::MM0 + operand.index - Register::XMM0);
            operand.size = 64;
        } else if (operand.type == GuestOperandKind::Memory ||
                   operand.type == GuestOperandKind::IndexedMemory) {
            operand.size = 64;
        }
    };
    switch (out.opcode) {
        case Op::PADDQ:
            // The parser reports the SSE2 form even without the mandatory 66
            // prefix, so the 0F D4 MMX encoding would otherwise reach the
            // vector path with XMM operands.
            if (out.ops[0].size == 128) {
                prefixes();
                if (!word) {
                    xmm_to_mmx(out.ops[0]);
                    xmm_to_mmx(out.ops[1]);
                }
            }
            break;
        case Op::CVTPD2PI:
        case Op::CVTTPD2PI:
            // The parser reports the XMM destination of the 66 0F 2D/2C
            // forms; the architectural destination is an MMX register.
            xmm_to_mmx(out.ops[0]);
            break;
        case Op::LDDQU:
        case Op::MOVSLDUP:
        case Op::MOVSHDUP:
        case Op::MOVNTDQA:
            // The parser omits or narrows the 128-bit memory source for
            // these legacy moves. MOVDDUP is excluded: its memory source is
            // architecturally 64-bit.
            if (out.ops[1].type != GuestOperandKind::None && out.ops[1].size != 128)
                resize(out.ops[1], 128);
            break;
        case Op::FXCH:
            // The parser omits the implicit ST1 on D9 C9. Publish an explicit
            // guest operand, so no consumer needs that vendor convention.
            if (out.ops[0].type == GuestOperandKind::None) {
                unsigned index = 1;
                if (!bytes.empty()) {
                    if (bytes.size() != 2 || bytes[0] != 0xd9 || (bytes[1] & 0xf8) != 0xc8) break;
                    index = bytes[1] & 7;
                }
                out.ops[0] = {GuestOperandKind::Register,
                              static_cast<std::uint8_t>(Register::ST0 + index),
                              80};
            }
            break;
        case Op::CMPXCHG:
        case Op::XADD:
        case Op::BSF:
        case Op::BSR:
        case Op::TZCNT:
        case Op::LZCNT:
        case Op::POPCNT:
            if (out.ops[0].size != 8 && out.ops[0].size != 64) {
                prefixes();
                if (word)
                    for (unsigned i = 0; i < 2; ++i)
                        if (out.ops[i].size == 32) resize(out.ops[i], 16);
            }
            break;
        case Op::CRC32: {
            prefixes();
            const unsigned bits = out.ops[1].size == 8 ? 8 : rex_w ? 64 : word ? 16 : 32;
            resize(out.ops[0], 32);  // CRC32 r64 still defines a zero-high U32.
            if (out.ops[1].size != bits) resize(out.ops[1], bits);
            break;
        }
        case Op::PCMPESTRI:
        case Op::PCMPESTRM:
            prefixes();
            out.operand_bits = rex_w ? 64 : 32;
            break;
        case Op::CVTSI2SS:
        case Op::CVTSI2SD:
            prefixes();
            resize(out.ops[1], rex_w ? 64 : 32);
            break;
        case Op::CVTTSS2SI:
        case Op::CVTSS2SI:
        case Op::CVTSD2SI:
        case Op::CVTTSD2SI:
            prefixes();
            resize(out.ops[0], rex_w ? 64 : 32);
            break;
        default:
            break;
    }
}

struct DecoderAccess {
    static Instruction Own(const InstructionData& data) noexcept {
        Instruction out;
        out.data = data;
        return out;
    }

    static Instruction Own(const _DInst& raw, std::span<const std::uint8_t> bytes = {}) noexcept {
        Instruction owned;
        auto& out = owned.data;
        std::memcpy(&out.immediate, &raw.imm, sizeof(out.immediate));
        out.disp = raw.disp;
        out.addr = raw.addr;
        out.opcode = DecodeOpcode(raw.opcode);
        for (unsigned i = 0; i < out.ops.size(); ++i) {
            const auto kind = DecodeOperand(raw.ops[i].type);
            const auto index = kind == GuestOperandKind::Register ||
                                               kind == GuestOperandKind::Memory ||
                                               kind == GuestOperandKind::IndexedMemory
                                       ? DecodeRegister(raw.ops[i].index)
                                       : std::uint8_t{Register::None};
            out.ops[i] = {kind, index, raw.ops[i].size};
        }
        out.flags =
                raw.flags == FLAG_NOT_DECODABLE
                        ? InstructionFlag::Invalid
                        : ((raw.flags & FLAG_LOCK) ? InstructionFlag::Locked : 0) |
                                  ((raw.flags & FLAG_REP) ? InstructionFlag::Repeat : 0) |
                                  ((raw.flags & FLAG_REPNZ) ? InstructionFlag::RepeatNotEqual : 0) |
                                  ((raw.flags & FLAG_IMM_SIGNED) ? InstructionFlag::ImmediateSigned
                                                                 : 0) |
                                  ((raw.flags & FLAG_PRIVILEGED_INSTRUCTION)
                                           ? InstructionFlag::Privileged
                                           : 0);
        out.flags_read = DecodeFlags(raw.testedFlagsMask);
        out.flags_written = DecodeFlags(raw.modifiedFlagsMask);
        out.flags_undefined = DecodeFlags(raw.undefinedFlagsMask);
        out.size = raw.size;
        out.segment = DecodeRegister(SEGMENT_GET(raw.segment));
        out.base = DecodeRegister(raw.base);
        out.scale = raw.scale;
        out.address_bits = static_cast<std::uint8_t>(16u << FLAG_GET_ADDRSIZE(raw.flags));
        out.operand_bits = static_cast<std::uint8_t>(16u << FLAG_GET_OPSIZE(raw.flags));
        out.flow = DecodeFlow(raw.meta);
        out.unit_boundary = out.flow != FlowKind::Linear || out.opcode == Op::CPUID ||
                            out.opcode == Op::UD2 || out.opcode == Op::INT_3 ||
                            out.opcode == Op::LDMXCSR || out.opcode == Op::STOS ||
                            out.opcode == Op::MOVS || out.opcode == Op::LODS ||
                            out.opcode == Op::CMPS || out.opcode == Op::SCAS;
        switch (out.opcode) {
            case Op::FXSAVE:
            case Op::FXSAVE64:
            case Op::FXRSTOR:
            case Op::FXRSTOR64:
            case Op::XSAVE:
            case Op::XSAVE64:
            case Op::XSAVEOPT:
            case Op::XSAVEOPT64:
            case Op::XSAVEC:
            case Op::XSAVEC64:
            case Op::XRSTOR:
            case Op::XRSTOR64:
                out.unit_boundary = true;
                break;
            default:
                break;
        }
        CorrectWidths(out, bytes);
        return owned;
    }
};

// Trusted hand-made vendor fixtures explicitly normalize once. Production
// callers pass their existing vector<Instruction> directly, with no copy.
inline std::vector<Instruction> FixtureNormalize(std::span<const _DInst> records) {
    std::vector<Instruction> result;
    result.reserve(records.size());
    for (const auto& record : records) result.push_back(DecoderAccess::Own(record));
    return result;
}
}  // namespace swift::x86::detail
