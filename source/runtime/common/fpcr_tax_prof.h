// Default-off execution probe for W90's AFP FPCR boundary tax.
#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "runtime/common/types.h"

namespace swift::runtime {

enum class FpcrTaxCounter : u32 {
    RuntimeEntry,
    RuntimeReturn,
    DispatcherEntry,
    AsmInterpreter,
    CallHost,
    DirectHelper,
    MemoryCopy,
    StoreMxcsr,
    DirectHelperFPFree,
    CacheLookup,
    CacheHit,
    RebuildExecuted,
    Count,
};

constexpr size_t kFpcrTaxBoundaryCount =
        static_cast<size_t>(FpcrTaxCounter::StoreMxcsr) + 1;
constexpr size_t kFpcrTaxCounterCount =
        static_cast<size_t>(FpcrTaxCounter::Count);

// Diagnostic-only 1/1024 direct-helper timing. A Runtime owns one buffer, so
// generated code writes without atomics; Runtime destruction merges samples.
inline constexpr u64 kFpcrTimingSampleMask = 1023;
inline constexpr size_t kFpcrTimingMaxSamples = 8192;

struct FpcrTimingSample {
    VAddr target{};
    u64 arg1{};
    u64 fpcr_transparent{};
    u64 start{};
    u64 host_ready{};
    u64 helper_done{};
    u64 guest_ready{};
};

struct FpcrTimingBuffer {
    u64 calls{};
    u64 sample_count{};
    u64 dropped{};
    std::array<FpcrTimingSample, kFpcrTimingMaxSamples> samples{};
};

[[nodiscard]] bool FpcrTaxProfEnabled();
void FpcrTaxSubmit(std::span<const u64> counters);
[[nodiscard]] bool FpcrTaxSkipSwitchEnabled();
[[nodiscard]] bool FpcrTaxTimingEnabled();
void FpcrTimingSubmit(const FpcrTimingBuffer& buffer);

}  // namespace swift::runtime
