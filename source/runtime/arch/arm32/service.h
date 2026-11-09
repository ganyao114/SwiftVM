#pragma once
#include <cstdint>

namespace swift::runtime::arm32 {
enum class StateServiceKind : std::uint8_t {
    None,
    LoadExclusive,
    StoreExclusive,
    ClearExclusive,
    ReadTLS,
    WriteTLS,
    CacheFlush,
    WriteFPSCR,
    WriteCPSR
};

struct StateServiceRequest {
    StateServiceKind kind{};
    std::uint8_t size_log2{}, address_register{}, first_register{}, second_register{},
            status_register{};
    std::uint8_t instruction_bytes{4}, offset_words{};
    bool ordered{};
};

constexpr std::uint32_t EncodeStateService(StateServiceRequest r) noexcept {
    return unsigned(r.kind) | (unsigned(r.size_log2) << 4) | (unsigned(r.address_register) << 6) |
           (unsigned(r.first_register) << 10) | (unsigned(r.second_register) << 14) |
           (unsigned(r.status_register) << 18) | (unsigned(r.instruction_bytes == 4) << 22) |
           (unsigned(r.offset_words) << 23) | (unsigned(r.ordered) << 31);
}

constexpr StateServiceRequest DecodeStateService(std::uint32_t word) noexcept {
    return {static_cast<StateServiceKind>(word & 15),
            std::uint8_t((word >> 4) & 3),
            std::uint8_t((word >> 6) & 15),
            std::uint8_t((word >> 10) & 15),
            std::uint8_t((word >> 14) & 15),
            std::uint8_t((word >> 18) & 15),
            std::uint8_t((word & (1u << 22)) ? 4 : 2),
            std::uint8_t((word >> 23) & 255),
            bool(word >> 31)};
}
}  // namespace swift::runtime::arm32
