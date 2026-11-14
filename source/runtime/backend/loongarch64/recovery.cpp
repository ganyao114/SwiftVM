#include <cstring>
#include "abi_layout.inc"
#include "recovery.h"
#include "runtime/arch/x86/flags.h"
#include "runtime/execution/fault.h"

namespace swift::runtime::backend::loongarch64 {
void RestoreGuestState(execution::FaultContext& context,
                       std::span<const std::byte> gprs,
                       std::uint8_t lbt_recovery,
                       std::uint8_t lbt_dirty_mask,
                       std::uint32_t lbt_eflags) noexcept {
    if (context.flags_offset == UINT32_MAX) return;
    std::uint64_t packed{};
    std::memcpy(&packed, gprs.data() + SVM_LA_FLAGS_REG * 8, sizeof(packed));
    if ((lbt_recovery & LbtRecoveryLive) && lbt_dirty_mask) {
        const auto ef = static_cast<std::uint64_t>(lbt_eflags) ^
                        ((lbt_recovery & LbtRecoveryCarryXor) ? 1ULL : 0ULL);
        if (lbt_dirty_mask & 0x01u)
            packed = (packed & ~x86::flags::Carry) | ((ef & 1ULL) << 29);
        if (lbt_dirty_mask & 0x02u)
            packed = (packed & ~UINT64_C(0xff)) | ((ef & 4ULL) ? 0ULL : 1ULL);
        if (lbt_dirty_mask & 0x04u)
            packed = (packed & ~x86::flags::Auxiliary) | (((ef >> 4) & 1ULL) << 26);
        if (lbt_dirty_mask & 0x08u)
            packed = (packed & ~x86::flags::Zero) | (((ef >> 6) & 1ULL) << 30);
        if (lbt_dirty_mask & 0x10u)
            packed = (packed & ~x86::flags::Sign) | (((ef >> 7) & 1ULL) << 31);
        if (lbt_dirty_mask & 0x20u)
            packed = (packed & ~x86::flags::Overflow) | (((ef >> 11) & 1ULL) << 28);
    }
    std::memcpy(reinterpret_cast<std::uint8_t*>(context.state) + context.flags_offset,
                &packed,
                sizeof(packed));
}
}  // namespace swift::runtime::backend::loongarch64
