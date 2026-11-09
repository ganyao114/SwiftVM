#include <cstring>
#include "abi_layout.inc"
#include "recovery.h"
#include "runtime/execution/fault.h"

namespace swift::runtime::backend::loongarch64 {
void RestoreGuestState(execution::FaultContext& context, std::span<const std::byte> gprs) noexcept {
    if (context.flags_offset == UINT32_MAX) return;
    std::memcpy(reinterpret_cast<std::uint8_t*>(context.state) + context.flags_offset,
                gprs.data() + SVM_LA_FLAGS_REG * 8,
                8);
}
}  // namespace swift::runtime::backend::loongarch64
