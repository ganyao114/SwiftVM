#include <cstring>
#include "recovery.h"
#include "runtime/execution/fault.h"

namespace swift::runtime::backend::x64 {
bool RestoreGuestState(execution::FaultContext& c,
                       const FaultSite& s,
                       std::span<const std::byte> regs) noexcept {
    if (s.recovery[0] > 1 || regs.size() < 16 * 8) return false;
    if (c.flags_offset != UINT32_MAX && !s.recovery[0])
        std::memcpy(static_cast<std::uint8_t*>(c.state) + c.flags_offset, regs.data() + 3 * 8, 8);
    return true;
}
}  // namespace swift::runtime::backend::x64
