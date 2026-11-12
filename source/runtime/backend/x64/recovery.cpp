#include <cstring>
#include "recovery.h"
#include "runtime/execution/fault.h"

namespace swift::runtime::backend::x64 {
bool RestoreGuestState(execution::FaultContext& c,
                       const FaultSite& s,
                       std::span<const std::byte> regs,
                       std::uint64_t native_flags) noexcept {
    const auto mode = s.recovery[0] & 3;
    if (mode > 2 || (s.recovery[0] & ~15u) || (mode != 2 && s.recovery[0] != mode) ||
        regs.size() < 16 * 8)
        return false;
    if (mode == 2 && c.flags_offset != UINT32_MAX) {
        std::uint64_t packed{}, saved{}, stack{};
        std::memcpy(&packed, regs.data() + 3 * 8, 8);
        const auto native_mask = s.recovery[1] & 15, saved_mask = s.recovery[1] >> 4;
        if (native_mask & saved_mask) return false;
        if (saved_mask) {
            const auto offset = s.recovery[2] | (std::uint32_t{s.recovery[3]} << 8) |
                                (std::uint32_t{s.recovery[7]} << 16);
            std::memcpy(&stack, regs.data() + 4 * 8, 8);
            if (!stack || (offset & 7) || stack > UINT64_MAX - offset - 8) return false;
            std::memcpy(&saved, reinterpret_cast<const void*>(stack + offset), 8);
        }
        constexpr unsigned source_bits[]{0, 11, 6, 7}, destination_bits[]{29, 28, 30, 31};
        for (unsigned bit = 0; bit < 4; ++bit) {
            if (!((native_mask | saved_mask) & (1u << bit))) continue;
            const bool use_saved = saved_mask & (1u << bit);
            auto value = ((use_saved ? saved : native_flags) >> source_bits[bit]) & 1;
            if (!bit) value ^= (s.recovery[0] >> (use_saved ? 3 : 2)) & 1;
            const auto mask = std::uint64_t{1} << destination_bits[bit];
            packed = (packed & ~mask) | (value << destination_bits[bit]);
        }
        std::memcpy(static_cast<std::uint8_t*>(c.state) + c.flags_offset, &packed, 8);
    }
    if (c.flags_offset != UINT32_MAX && !s.recovery[0])
        std::memcpy(static_cast<std::uint8_t*>(c.state) + c.flags_offset, regs.data() + 3 * 8, 8);
    if (c.pinned_uniform_offset != UINT32_MAX && c.pinned_uniform_slots) {
        constexpr unsigned pinned[]{13, 14, 15, 5};
        auto* base = static_cast<std::uint8_t*>(c.state) + c.pinned_uniform_offset;
        for (unsigned i = 0; i < c.pinned_uniform_slots && i < 4; ++i)
            std::memcpy(base + i * 8, regs.data() + pinned[i] * 8, 8);
    }
    return true;
}
}  // namespace swift::runtime::backend::x64
