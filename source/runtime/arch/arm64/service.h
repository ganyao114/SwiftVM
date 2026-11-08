#pragma once

#include <cstdint>

namespace swift::runtime::arm64 {

// Request published through GuestState::service_word before StateService exit.
// The PC remains at the requesting instruction until the owner commits it.
enum class StateServiceKind : std::uint8_t {
    None,
    LoadExclusive,
    StoreExclusive,
    ClearExclusive,
    ReadVirtualCounter,
    ReadPhysicalCounter,
    ReadCounterFrequency,
    ReadCacheType,
    DataCacheCleanPoU,
    DataCacheCleanPoC,
    DataCacheCleanInvalidatePoC,
    InstructionCacheInvalidatePoU,
};

struct StateServiceRequest {
    StateServiceKind kind{};
    std::uint8_t size_log2{};
    bool pair{}, ordered{};
    // The address register uses SP for 31; data/status registers use ZR.
    std::uint8_t address_register{31}, first_register{31}, second_register{31}, status_register{31};

    constexpr unsigned TransferBytes() const noexcept {
        return (1u << size_log2) * (pair ? 2u : 1u);
    }
};

// [3:0] kind, [5:4] element-size log2, [6] pair, [7] acquire/release,
// [12:8] address Rn, [17:13] data Rt, [22:18] data Rt2, [27:23] status Rs.
// [31:28] are zero. The frontend validates allocated instruction shapes.
constexpr std::uint32_t EncodeStateService(StateServiceRequest r) noexcept {
    return std::uint32_t(r.kind) | (std::uint32_t(r.size_log2) << 4) |
           (std::uint32_t(r.pair) << 6) | (std::uint32_t(r.ordered) << 7) |
           (std::uint32_t(r.address_register) << 8) | (std::uint32_t(r.first_register) << 13) |
           (std::uint32_t(r.second_register) << 18) | (std::uint32_t(r.status_register) << 23);
}

constexpr StateServiceRequest DecodeStateService(std::uint32_t word) noexcept {
    return {StateServiceKind(word & 15),
            std::uint8_t((word >> 4) & 3),
            bool(word & 64),
            bool(word & 128),
            std::uint8_t((word >> 8) & 31),
            std::uint8_t((word >> 13) & 31),
            std::uint8_t((word >> 18) & 31),
            std::uint8_t((word >> 23) & 31)};
}
}  // namespace swift::runtime::arm64
