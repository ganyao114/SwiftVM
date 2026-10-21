#pragma once

#include <optional>
#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {
struct RegionPFAFPlan {
    struct Operand {
        enum class Kind : u8 { None, Imm, HostGPR, Uniform } kind{};
        u64 value{};
        u8 offset{};
        auto operator<=>(const Operand&) const = default;
    };
    const ir::Inst* producer{};
    u8 width{};
    Operand left, right;
};
// Consumes an already-eligible backedge/region tail. This stage proves that
// cold-edge operand rereads retain the original narrow Sub inputs.
std::optional<RegionPFAFPlan> BuildRegionPFAFPlan(const ir::Block& block,
        bool dead_successor, const ir::Inst* producer, const ir::Inst* final_save);
}
