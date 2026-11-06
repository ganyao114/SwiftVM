#pragma once

#include <cstddef>
#include <cstdint>

namespace swift::runtime::platform {

// GuestMemory checks the complete logical range and write/SMC policy first.
// Kernel copies return a fault without unwinding through a C++ signal frame.
bool ReadGuestBytes(void* destination, std::uint64_t source, std::size_t size) noexcept;
bool WriteGuestBytes(std::uint64_t destination, const void* source, std::size_t size) noexcept;

} // namespace swift::runtime::platform
