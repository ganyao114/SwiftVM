#pragma once

#include <cstdint>
#include <cstring>
#include <span>

namespace swift::tests {
inline bool ContainsMemory(std::uint64_t base,
                           std::size_t capacity,
                           std::uint64_t address,
                           std::size_t size) noexcept {
    return address >= base && address - base <= capacity && size <= capacity - (address - base);
}

inline bool ReadMemory(std::span<const std::uint8_t> memory,
                       std::uint64_t base,
                       std::uint64_t address,
                       std::span<std::uint8_t> output) noexcept {
    if (!ContainsMemory(base, memory.size(), address, output.size())) return false;
    std::memcpy(output.data(), memory.data() + (address - base), output.size());
    return true;
}

inline bool WriteMemory(std::span<std::uint8_t> memory,
                        std::uint64_t base,
                        std::uint64_t address,
                        std::span<const std::uint8_t> input) noexcept {
    if (!ContainsMemory(base, memory.size(), address, input.size())) return false;
    std::memcpy(memory.data() + (address - base), input.data(), input.size());
    return true;
}
}  // namespace swift::tests
