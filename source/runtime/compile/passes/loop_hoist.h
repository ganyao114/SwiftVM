#pragma once

#include "base/types.h"

namespace swift::runtime { struct FeatureSet; }
namespace swift::runtime::ir { class Inst; class HIRFunction; }

namespace swift::runtime::compile {
class CompileUnit;
struct GuestStateLayout;
struct FlagsDemand;

struct HoistRegisterSite {
    u32 clobbers{};
    u32 fixed_definition{};
    u32 scratch{};
};

struct LoopHoistTarget {
    // Set bits are usable dynamic registers, excluding architectural homes.
    u32 value_registers{};
    HoistRegisterSite (*describe)(u32, const ir::Inst&, const FlagsDemand&,
                                  const FeatureSet&){};
};

struct LoopHoistResult {
    u32 candidates{};
    u32 gprs{};
    u32 constants{};
    u32 pressure_rejections{};
};

// All decisions precede physical allocation and final numbering. No recipe,
// original instruction order or alternate allocation survives this call.
LoopHoistResult HoistLoopInvariants(ir::HIRFunction* function,
                                  const GuestStateLayout& layout,
                                  CompileUnit& unit, const LoopHoistTarget& target,
                                  const FeatureSet& features);

}  // namespace swift::runtime::compile
