#include "decode.h"

#include <algorithm>
#include <chrono>

#include "detail/decoded.h"
#include "detail/modrm.h"

namespace swift::x86 {
using u64 = std::uint64_t;

// Shared by the two byte-decoding entries and the emitter's prefix gate.
// The runtime advertises neither IBT nor a CET shadow-stack environment.
std::uint8_t DisabledCetHintSize(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() < 4 || bytes[0] != 0xf3) return 0;
    std::size_t opcode = 1;
    if (bytes[1] >= 0x40 && bytes[1] <= 0x4f) {
        if (bytes[1] & 4) return 0;  // REX.R must not turn the /1 extension into /9.
        opcode = 2;
    }
    if (bytes.size() < opcode + 3 || bytes[opcode] != 0x0f || bytes[opcode + 1] != 0x1e) return 0;
    const auto modrm = bytes[opcode + 2];
    if (opcode == 1 && (modrm == 0xfa || modrm == 0xfb)) return 4;  // Exact ENDBR64/32.
    // RDSSPD/Q: /1, mod11, REX.W selects width and REX.B the high register.
    // Disabled shadow stacks make this a NOP: even a 32-bit form preserves
    // the complete destination carrier, rather than writing/zeroing EAX.
    return (modrm & 0xf8) == 0xc8 ? static_cast<std::uint8_t>(opcode + 3) : 0;
}

DecodeOneResult DecodeOne(std::span<const std::uint8_t> bytes,
                          std::uint64_t pc,
                          DecodeTiming* timing,
                          runtime::x86::GuestCpuModel model) {
    DecodeOneResult out;
    if (bytes.empty() || !bytes.data()) {
        out.error = DecodeError::Truncated;
        return out;
    }
    const auto extended = detail::DecodeExtended(bytes, pc, timing);
    if (extended.handled) {
        out.error = extended.error;
        if (out.error == DecodeError::None)
            out.instruction = detail::DecoderAccess::Own(
                    extended.instruction, bytes.first(extended.instruction.size));
        return out;
    }
    _CodeInfo code{};
    code.codeOffset = pc;
    code.code = bytes.data();
    code.codeLen = static_cast<int>(std::min<std::size_t>(bytes.size(), 15));
    code.dt = Decode64Bits;
    code.features = DF_REPORT_INCOMPLETE;
    _DInst instruction{};
    unsigned used{};
    _DecodeResult parsed;
    if (timing) {
        const auto begin = std::chrono::steady_clock::now();
        parsed = distorm_decompose(&code, &instruction, 1, &used);
        const auto end = std::chrono::steady_clock::now();
        timing->nanoseconds += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        ++timing->calls;
    } else
        parsed = distorm_decompose(&code, &instruction, 1, &used);
    bool adapted_hint{};
    if (bytes[0] == 0xf3) {
        if (const auto size = DisabledCetHintSize(bytes)) {
            instruction = {};
            instruction.addr = pc;
            instruction.size = size;
            instruction.opcode = I_NOP;
            used = 1;
            adapted_hint = true;
            const bool endbr = size == 4 && (bytes[3] == 0xfa || bytes[3] == 0xfb);
            if (endbr ? model.cet_ibt : model.cet_shadow_stack)
                out.error = DecodeError::UnsupportedGuestFeature;
        }
    }
    if (parsed == DECRES_INCOMPLETE && !used && !adapted_hint)
        out.error = code.codeLen < 15 ? DecodeError::Truncated : DecodeError::InvalidEncoding;
    else if (!used)
        out.error =
                parsed == DECRES_INPUTERR ? DecodeError::InvalidEncoding : DecodeError::Truncated;
    else if (used != 1 || instruction.flags == FLAG_NOT_DECODABLE || !instruction.size ||
             instruction.size > 15)
        out.error = DecodeError::InvalidEncoding;
    else if (instruction.size > bytes.size())
        out.error = DecodeError::Truncated;
    if (out.error == DecodeError::None)
        out.instruction =
                detail::DecoderAccess::Own(instruction, bytes.first(instruction.size), adapted_hint);
    return out;
}

}  // namespace swift::x86
