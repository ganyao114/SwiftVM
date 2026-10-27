#pragma once

#include <array>
#include <vector>
#include "runtime/backend/context.h"
#include "runtime/backend/link_manager.h"
#include "runtime/backend/retirement_control.h"
#include "runtime/include/config.h"

namespace swift::runtime::backend::arm64 {

// One stable object is embedded by address in one region trampoline. It is
// deliberately runtime-side metadata: State remains ABI/layout compatible.
struct RegionLinkContext {
    LinkManager* manager{};
    const CodeRegion* region{};
    void* return_host{};
    void* dispatcher{};
    std::shared_ptr<RetirementControl> retirement_control;
};

// Cold AAPCS64 helper called by the generated trampoline.
extern "C" void* RegionLinkTrampolineSlow(RegionLinkContext* context,
                                           State* state,
                                           const void* rx_site);

// Produces the final-form per-region trampoline. The returned bytes are copied
// to a CodeCache allocation by CodeCache::InitializeRegionTrampoline.
struct RegionLinkTrampolineCode {
    std::vector<u8> code{};
    u32 canonical_offset{};
    u32 pending_flags_offset{};
    u32 return_offset{};
    u32 pair_call_offset{};
    std::array<u32, 16> flags_mask_offsets{};
    std::array<u32, 16> flags_mask_token_offsets{};
};

constexpr u32 kFlagsMergeOffsetFromPending = 4 * sizeof(u32);
constexpr u32 kFlagsMergeTokenOffsetFromPending = 8 * sizeof(u32);
constexpr u32 kReturnFlagsMergeOffsetFromPending = 13 * sizeof(u32);
constexpr u32 kReturnFlagsMergeTokenOffsetFromPending = 17 * sizeof(u32);
constexpr u32 kCycleFlagsMergeOffsetFromPending = 22 * sizeof(u32);
constexpr u32 kCycleFlagsMergeTokenOffsetFromPending = 26 * sizeof(u32);
constexpr u32 kCycleReasonOffsetFromPending = 31 * sizeof(u32);

[[nodiscard]] RegionLinkTrampolineCode BuildRegionLinkTrampoline(
        const Config& config,
        RegionLinkContext* context,
        const FeatureSet& features);

}  // namespace swift::runtime::backend::arm64
