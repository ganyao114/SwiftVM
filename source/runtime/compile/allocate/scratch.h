#pragma once

#include "runtime/ir/terminal.h"

namespace swift::runtime::ir {
class Inst;
}

namespace swift::runtime::compile {

class AllocationWriter;
struct ScratchNeed {
    u8 gpr, fpr;
    bool operator==(const ScratchNeed&) const = default;
};

struct ScratchRequirements {
    u32 gpr{}, fpr{};
    u32 outside_value_pool{};
    u32 reload_headroom{};
    bool overlap_gpr_reloads{};
    u32 spill_outside_value_pool{};
};

struct ScratchReloads {
    u32 gpr{}, fpr{};
};

// Prepared target data describes one site; the common checker only reads
// physical locations and counts distinct reloads.
[[nodiscard]] bool CheckInstructionScratch(const AllocationWriter& allocation,
                                           const ir::Inst& inst,
                                           const ScratchRequirements& need,
                                           ScratchReloads extra = {});
[[nodiscard]] ScratchReloads CountTerminalReloads(const AllocationWriter& allocation,
                                                  const ir::Terminal& terminal);

}  // namespace swift::runtime::compile
