#pragma once

#include <cstdint>
#include <span>

namespace swift::runtime::frontend {

struct BlockWindow {
    std::uint64_t pc{};
    std::span<const std::uint8_t> bytes;
};

struct DecodeLimits {
    std::uint32_t max_instructions{4096}, max_blocks{256};
};

// Counts only actual decoder calls when explicitly requested by the owner.
struct DecodeTiming {
    std::uint64_t nanoseconds{}, calls{};
};

struct ReadyEntryLookup {
    void* opaque{};
    bool (*ready)(void*, std::uint64_t) noexcept {};

    bool Contains(std::uint64_t pc) const noexcept { return ready && ready(opaque, pc); }
};

}  // namespace swift::runtime::frontend
