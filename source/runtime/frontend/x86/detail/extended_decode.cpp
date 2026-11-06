#include "modrm.h"
#include "decode_timing.h"

namespace swift::x86::detail {
ExtendedDecodeResult DecodeExtended(std::span<const std::uint8_t> bytes,
                                    std::uint64_t pc,
                                    DecodeTiming* timing) noexcept {
    ByteCursor cursor(bytes);
    const auto prefix = ReadPrefixes(cursor);
    if (cursor.at == cursor.bytes.size()) return {};
    if (cursor.bytes[cursor.at] == 0xc4 || cursor.bytes[cursor.at] == 0xc5)
        return DecodeVex(cursor, prefix, pc, timing);
    // The legacy parser predates ADX/SHA and omits the valid FFREEP alias.
    // These few extensions use the same bounded ModRM reader as VEX.
    const auto opcode = cursor.Take();
    if (opcode == 0x9b) {
        DecodeSample sample(timing);
        ExtendedDecodeResult result;
        result.handled = true;
        auto& out = result.instruction;
        out.addr = pc;
        out.opcode = Op::WAIT;
        out.address_bits = prefix.address32 ? 32 : 64;
        out.operand_bits = prefix.word ? 16 : 32;
        out.size = static_cast<std::uint8_t>(cursor.at);
        if (prefix.locked || prefix.repeat || prefix.repeat_not_equal)
            result.error = DecodeError::InvalidEncoding;
        return result;
    }
    if (opcode == 0xdf && cursor.at < cursor.bytes.size() &&
        (cursor.bytes[cursor.at] & 0xf8) == 0xc0) {
        DecodeSample sample(timing);
        ExtendedDecodeResult result;
        result.handled = true;
        auto& out = result.instruction;
        out.addr = pc;
        out.opcode = Op::FFREEP;
        out.address_bits = prefix.address32 ? 32 : 64;
        out.operand_bits = prefix.word ? 16 : 32;
        out.ops[0] = {GuestOperandKind::Register,
                      static_cast<std::uint8_t>(Register::ST0 + (cursor.Take() & 7)),
                      80};
        out.size = static_cast<std::uint8_t>(cursor.at);
        if (prefix.locked || prefix.repeat || prefix.repeat_not_equal)
            result.error = DecodeError::InvalidEncoding;
        return result;
    }
    if (opcode != 0x0f || cursor.at >= cursor.bytes.size()) return {};
    if (cursor.bytes[cursor.at] == 0xc7) {
        cursor.Take();
        if (cursor.at == cursor.bytes.size()) return {};
        const auto byte = cursor.bytes[cursor.at];
        if ((byte & 0xc0) != 0xc0 && ((byte >> 3) & 7) == 4) {
            DecodeSample sample(timing);
            ExtendedDecodeResult result;
            result.handled = true;
            auto& out = result.instruction;
            out.addr = pc;
            out.segment = prefix.segment;
            out.address_bits = prefix.address32 ? 32 : 64;
            out.operand_bits = (prefix.rex & 8) ? 64 : 32;
            const auto mod = ReadModRM(
                    cursor, out, (prefix.rex >> 2) & 1, (prefix.rex >> 1) & 1, prefix.rex & 1);
            out.opcode = (prefix.rex & 8) ? Op::XSAVEC64 : Op::XSAVEC;
            out.ops[0] = mod.memory;
            out.unit_boundary = true;
            out.size = static_cast<std::uint8_t>(cursor.at);
            result.error = cursor.error;
            if (prefix.locked || prefix.word || prefix.repeat || prefix.repeat_not_equal)
                result.error = DecodeError::InvalidEncoding;
            return result;
        }
        if ((byte & 0xc0) != 0xc0 || (((byte >> 3) & 7) != 6 && ((byte >> 3) & 7) != 7)) return {};
        DecodeSample sample(timing);
        ExtendedDecodeResult result;
        result.handled = true;
        auto& out = result.instruction;
        out.addr = pc;
        out.address_bits = prefix.address32 ? 32 : 64;
        out.operand_bits = (prefix.rex & 8) ? 64 : prefix.word ? 16 : 32;
        const auto mod = ReadModRM(cursor, out, 0, 0, prefix.rex & 1);
        out.opcode = mod.extension == 6 ? Op::RDRAND : Op::RDSEED;
        out.ops[0] = RegisterOperand(mod.rm, out.operand_bits);
        out.flags_written = GuestFlag::Carry | GuestFlag::Parity | GuestFlag::Auxiliary |
                            GuestFlag::Zero | GuestFlag::Sign | GuestFlag::Overflow;
        out.size = static_cast<std::uint8_t>(cursor.at);
        result.error = cursor.error;
        if (prefix.locked || prefix.repeat || prefix.repeat_not_equal)
            result.error = DecodeError::InvalidEncoding;
        return result;
    }
    if (cursor.bytes[cursor.at] != 0x38) return {};
    cursor.Take();
    if (cursor.at >= cursor.bytes.size()) return {};
    const auto extension = cursor.Take();
    const bool adx = extension == 0xf6 && (prefix.mandatory == 1 || prefix.mandatory == 2);
    const bool sha = extension >= 0xcb && extension <= 0xcd;
    if (!adx && !sha) return {};
    DecodeSample sample(timing);
    ExtendedDecodeResult result;
    result.handled = true;
    auto& out = result.instruction;
    out.addr = pc;
    out.segment = prefix.segment;
    out.address_bits = prefix.address32 ? 32 : 64;
    out.operand_bits = (prefix.rex & 8) ? 64 : 32;
    const auto mod =
            ReadModRM(cursor, out, (prefix.rex >> 2) & 1, (prefix.rex >> 1) & 1, prefix.rex & 1);
    if (prefix.locked || (sha && (prefix.word || prefix.repeat || prefix.repeat_not_equal)) ||
        (adx && prefix.repeat_not_equal) || (adx && prefix.word && prefix.repeat))
        result.error = DecodeError::InvalidEncoding;
    if (adx) {
        out.opcode = prefix.mandatory == 1 ? Op::ADCX : Op::ADOX;
        out.flags = prefix.repeat ? InstructionFlag::Repeat : 0;
        out.flags_read = out.flags_written =
                out.opcode == Op::ADCX ? GuestFlag::Carry : GuestFlag::Overflow;
        out.ops = {RegisterOperand(mod.reg, out.operand_bits),
                   RmOperand(mod, out.operand_bits),
                   {},
                   {}};
    } else {
        out.opcode = extension == 0xcb   ? Op::SHA256RNDS2
                     : extension == 0xcc ? Op::SHA256MSG1
                                         : Op::SHA256MSG2;
        out.ops = {RegisterOperand(mod.reg, 128, true), RmOperand(mod, 128, true), {}, {}};
    }
    out.size = static_cast<std::uint8_t>(cursor.at);
    if (cursor.error != DecodeError::None) result.error = cursor.error;
    return result;
}
}  // namespace swift::x86::detail
