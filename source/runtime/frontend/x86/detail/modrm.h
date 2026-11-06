#pragma once

#include "../decode.h"
#include <algorithm>

namespace swift::x86::detail {
// One bounded cursor owns byte consumption. The semantic record keeps the
// resolved operands, not a persistent copy of encoding fields.
struct ByteCursor {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    DecodeError error{};
    explicit ByteCursor(std::span<const std::uint8_t> bytes)
            : bytes(bytes.first(std::min<std::size_t>(bytes.size(), 15))) {}
    std::uint8_t Take() noexcept {
        if (at == bytes.size()) {
            error = bytes.size() == 15 ? DecodeError::InvalidEncoding : DecodeError::Truncated;
            return 0;
        }
        return bytes[at++];
    }
    std::uint32_t Word32() noexcept {
        const auto a = Take(), b = Take(), c = Take(), d = Take();
        return std::uint32_t{a} | (std::uint32_t{b} << 8) | (std::uint32_t{c} << 16) |
               (std::uint32_t{d} << 24);
    }
};
struct EncodingPrefix {
    std::uint8_t rex{}, mandatory{}, segment{Register::None};
    bool address32{}, locked{}, word{}, repeat{}, repeat_not_equal{};
};
inline EncodingPrefix ReadPrefixes(ByteCursor& cursor) noexcept {
    EncodingPrefix prefix;
    while (cursor.at < cursor.bytes.size()) {
        const auto byte = cursor.bytes[cursor.at];
        if (byte >= 0x40 && byte <= 0x4f) {
            prefix.rex = byte;
            ++cursor.at;
            continue;
        }
        switch (byte) {
            case 0x26:
                prefix.segment = Register::ES;
                break;
            case 0x2e:
                prefix.segment = Register::CS;
                break;
            case 0x36:
                prefix.segment = Register::SS;
                break;
            case 0x3e:
                prefix.segment = Register::DS;
                break;
            case 0x64:
                prefix.segment = Register::FS;
                break;
            case 0x65:
                prefix.segment = Register::GS;
                break;
            case 0x67:
                prefix.address32 = true;
                break;
            case 0x66:
                prefix.mandatory = 1;
                prefix.word = true;
                break;
            case 0xf3:
                prefix.mandatory = 2;
                prefix.repeat = true;
                break;
            case 0xf2:
                prefix.mandatory = 3;
                prefix.repeat_not_equal = true;
                break;
            case 0xf0:
                prefix.locked = true;
                break;
            default:
                return prefix;
        }
        prefix.rex = 0;  // REX only applies immediately before the opcode.
        ++cursor.at;
    }
    return prefix;
}
inline DecodedOperand RegisterOperand(unsigned slot, unsigned bits, bool vector = false) noexcept {
    const auto first = vector       ? Register::XMM0
                       : bits == 64 ? Register::RAX
                       : bits == 32 ? Register::EAX
                       : bits == 16 ? Register::AX
                                    : Register::AL;
    return {GuestOperandKind::Register,
            static_cast<std::uint8_t>(first + slot),
            static_cast<std::uint16_t>(bits)};
}
struct ModRM {
    std::uint8_t reg{}, rm{}, extension{}, mod{};
    bool sib{};
    DecodedOperand memory{};
};
inline ModRM ReadModRM(ByteCursor& cursor,
                       InstructionData& out,
                       unsigned r,
                       unsigned x,
                       unsigned b,
                       bool vsib = false) noexcept {
    ModRM fields;
    const auto byte = cursor.Take();
    fields.mod = byte >> 6;
    fields.extension = (byte >> 3) & 7;
    fields.reg = static_cast<std::uint8_t>(fields.extension | (r << 3));
    fields.rm = static_cast<std::uint8_t>((byte & 7) | (b << 3));
    if (fields.mod == 3 || cursor.error != DecodeError::None) return fields;
    unsigned base = byte & 7, index = 0;
    bool no_base = false, no_index = true, rip = false;
    if (base == 4) {
        const auto sib = cursor.Take();
        fields.sib = true;
        out.scale = static_cast<std::uint8_t>(1u << (sib >> 6));
        index = ((sib >> 3) & 7) | (x << 3);
        no_index = !vsib && ((sib >> 3) & 7) == 4 && !x;
        base = sib & 7;
        no_base = fields.mod == 0 && base == 5;
    } else if (base == 5 && fields.mod == 0) {
        no_base = true;
        rip = true;
    }
    if (rip)
        fields.memory = {GuestOperandKind::Memory, Register::RIP, 0};
    else {
        const auto bank = out.address_bits == 32 ? Register::EAX : Register::RAX;
        if (!no_index) {
            out.base = no_base ? Register::None : static_cast<std::uint8_t>(bank + base + (b << 3));
            fields.memory = {GuestOperandKind::IndexedMemory,
                             static_cast<std::uint8_t>((vsib ? Register::XMM0 : bank) + index),
                             0};
        } else if (no_base)
            fields.memory = {GuestOperandKind::Displacement, Register::None, 0};
        else
            fields.memory = {
                    GuestOperandKind::Memory, static_cast<std::uint8_t>(bank + base + (b << 3)), 0};
    }
    if (fields.mod == 2 || (fields.mod == 0 && no_base))
        out.disp = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(static_cast<std::int32_t>(cursor.Word32())));
    else if (fields.mod == 1)
        out.disp = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(static_cast<std::int8_t>(cursor.Take())));
    return fields;
}
inline DecodedOperand RmOperand(const ModRM& fields,
                                unsigned bits,
                                bool vector = false,
                                unsigned register_bits = 0) noexcept {
    if (fields.mod == 3)
        return RegisterOperand(fields.rm, register_bits ? register_bits : bits, vector);
    auto operand = fields.memory;
    operand.size = static_cast<std::uint16_t>(bits);
    return operand;
}
struct ExtendedDecodeResult {
    InstructionData instruction{};
    DecodeError error{};
    bool handled{};
};
ExtendedDecodeResult DecodeExtended(std::span<const std::uint8_t>,
                                    std::uint64_t,
                                    DecodeTiming*) noexcept;
ExtendedDecodeResult DecodeVex(ByteCursor&, EncodingPrefix, std::uint64_t, DecodeTiming*) noexcept;
}  // namespace swift::x86::detail
