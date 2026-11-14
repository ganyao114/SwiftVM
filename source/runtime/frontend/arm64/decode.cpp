#include <chrono>
#include <cstring>
#include "aarch64/constants-aarch64.h"
#include "aarch64/decoder-aarch64.h"
#include "decode.h"
#include "instruction.h"
#include "state_service.h"

namespace swift::arm64 {
namespace {
class Selector final : public vixl::aarch64::DecoderVisitor {
public:
    detail::Family family{detail::Family::Unallocated};
#define ARM_FAMILY(name)                                                                           \
    void Visit##name(const vixl::aarch64::Instruction*) override { family = detail::Family::name; }
#include "instruction_families.inc"
#undef ARM_FAMILY
};

vixl::aarch64::Decoder& Decoder() {
    // Bionic's AArch64 static TLS ABI requires at least 64-byte alignment.
    alignas(64) static thread_local vixl::aarch64::Decoder decoder;
    return decoder;
}

std::int64_t Signed(std::uint32_t value, unsigned bits) {
    const auto sign = std::uint64_t{1} << (bits - 1);
    return std::int64_t((std::uint64_t(value) ^ sign) - sign);
}

FlowKind ClassifyFlow(std::uint32_t word) noexcept {
    if ((word & 0x7c000000u) == 0x14000000u)
        return (word & 0x80000000u) ? FlowKind::Call : FlowKind::Jump;
    if ((word & 0xff000010u) == 0x54000000u || (word & 0x7e000000u) == 0x34000000u ||
        (word & 0x7e000000u) == 0x36000000u)
        return FlowKind::Conditional;
    if ((word & 0xfffffc1fu) == 0xd65f0000u) return FlowKind::Return;
    if ((word & 0xfffffc1fu) == 0xd63f0000u) return FlowKind::Call;
    if ((word & 0xfe000000u) == 0xd6000000u) return FlowKind::Jump;
    if ((word & 0xff000000u) == 0xd4000000u) return FlowKind::Service;
    return FlowKind::Linear;
}

bool IsBoundary(std::uint32_t word, FlowKind flow) noexcept {
    return flow != FlowKind::Linear ||
           detail::StateRequest(word).kind != runtime::arm64::StateServiceKind::None ||
           ((word & 0xffe00000u) == 0xd5000000u && !(word & (1u << 21)) &&
            ((word >> 5) & 0xffff) == vixl::aarch64::FPCR) ||
           ((word & vixl::aarch64::MemBarrierMask) == vixl::aarch64::ISB);
}
}  // namespace

bool Instruction::RelativeTarget(std::uint64_t& target) const noexcept {
    if ((word & 0x7c000000u) == 0x14000000u)
        target = pc + Signed(word & 0x3ffffff, 26) * 4;
    else if ((word & 0xff000010u) == 0x54000000u || (word & 0x7e000000u) == 0x34000000u)
        target = pc + Signed((word >> 5) & 0x7ffff, 19) * 4;
    else if ((word & 0x7e000000u) == 0x36000000u)
        target = pc + Signed((word >> 5) & 0x3fff, 14) * 4;
    else
        return false;
    return true;
}

DecodeOneResult DecodeOne(std::span<const std::uint8_t> bytes,
                          std::uint64_t pc,
                          DecodeTiming* timing) {
    using Clock = std::chrono::steady_clock;
    const auto begin = timing ? Clock::now() : Clock::time_point{};
    DecodeOneResult out;
    if ((pc & 3) || pc > UINT64_MAX - 4)
        out.error = DecodeError::InvalidInput;
    else if (bytes.size() < 4)
        out.error = DecodeError::Truncated;
    else {
        out.instruction.pc = pc;
        std::memcpy(&out.instruction.word, bytes.data(), 4);
        Selector selector;
        Decoder().Decode(&selector,
                         reinterpret_cast<vixl::aarch64::Instruction*>(&out.instruction.word));
        out.instruction.family = static_cast<std::uint16_t>(selector.family);
        out.instruction.flow = ClassifyFlow(out.instruction.word);
        out.instruction.boundary = IsBoundary(out.instruction.word, out.instruction.flow);
        if (selector.family == detail::Family::Unallocated ||
            selector.family == detail::Family::Reserved)
            out.error = DecodeError::InvalidEncoding;
    }
    if (timing) {
        ++timing->calls;
        timing->nanoseconds +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
    }
    return out;
}
}  // namespace swift::arm64
