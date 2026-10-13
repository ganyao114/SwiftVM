#pragma once

#include <array>
#include <cstddef>
#include <utility>
#include "cpu.h"
#include "runtime/include/config.h"

namespace swift::arm64 {

// The frontend owns the architectural layout. Each descriptor is one full
// X register, including SP; PC, PSTATE, vector and system state are excluded.
inline std::span<runtime::UniformRangeDesc> Arm64GprUniformRanges() {
    static auto ranges = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<runtime::UniformRangeDesc, 32>{
            runtime::UniformRangeDesc{
                u32(I < 29 ? offsetof(ThreadContext64, r) + I * sizeof(u64)
                    : I == 29 ? offsetof(ThreadContext64, fp)
                    : I == 30 ? offsetof(ThreadContext64, lr)
                              : offsetof(ThreadContext64, sp)), sizeof(u64)}...};
    }(std::make_index_sequence<32>{});
    return ranges;
}

// Sink only architectural registers. PC/PSTATE and system state retain their
// original publication order; LoongArch flushes these register stores before
// faults, helpers, local branches and every guest retirement boundary.
inline std::span<runtime::UniformRangeDesc> Arm64StoreSinkUniformRanges() {
    static auto ranges = []<std::size_t... I>(std::index_sequence<I...>) {
        const auto gprs = Arm64GprUniformRanges();
        return std::array<runtime::UniformRangeDesc, 64>{
            (I < 32 ? gprs[I] : runtime::UniformRangeDesc{
                u32(offsetof(ThreadContext64, v) + (I - 32) * sizeof(u128)), sizeof(u128)})...};
    }(std::make_index_sequence<64>{});
    return ranges;
}

} // namespace swift::arm64
