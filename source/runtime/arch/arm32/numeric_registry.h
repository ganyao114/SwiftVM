#pragma once

#include <cstdint>

namespace swift::runtime::arm32 {
// Zero is unrecognized. Identities describe mathematical leaves independently
// of the decoder, IR consumer, native backend or current process addresses.
std::uint16_t NumericCallbackIdentity(std::uintptr_t address) noexcept;
std::uintptr_t ResolveNumericCallbackIdentity(std::uint16_t identity) noexcept;
}  // namespace swift::runtime::arm32
