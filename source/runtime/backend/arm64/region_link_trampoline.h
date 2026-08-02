#pragma once

#include <vector>
#include "runtime/backend/context.h"
#include "runtime/backend/link_manager.h"
#include "runtime/include/config.h"

namespace swift::runtime::backend::arm64 {

// One stable object is embedded by address in one region trampoline. It is
// deliberately runtime-side metadata: State remains ABI/layout compatible.
struct RegionLinkContext {
    LinkManager* manager{};
    const CodeRegion* region{};
    void* return_host{};
    void* dispatcher{};
};

// Cold AAPCS64 helper called by the generated trampoline.
extern "C" void* RegionLinkTrampolineSlow(RegionLinkContext* context,
                                           State* state,
                                           const void* rx_site);

// Produces the final-form per-region trampoline. The returned bytes are copied
// to a CodeCache allocation by CodeCache::InitializeRegionTrampoline.
[[nodiscard]] std::vector<u8> BuildRegionLinkTrampoline(const Config& config,
                                                         RegionLinkContext* context);

}  // namespace swift::runtime::backend::arm64
