#pragma once

#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "runtime/ir/block.h"

namespace swift::runtime::backend::loongarch64 {

using LiveSet = std::unordered_set<ir::Inst*>;

// Coordinates refer to the predecessor's final instruction and the target
// entry instruction (or its BindLabel anchor). Sources are read before the
// incoming edge's parallel phi copies overwrite any destination.
struct PhiEdge {
    ir::Inst* from{};
    ir::Inst* to{};
    ir::Value source{};
};

struct RegisterPlanStats {
    u32 nodes{}, instructions{}, iterations{}, max_live{};
};

class RegisterPlan {
public:
    using Targets = std::unordered_map<ir::Inst*, ir::Inst*>;
    using Stops = std::unordered_set<ir::Inst*>;
    using Exits = std::unordered_map<ir::Inst*, ir::Terminal>;

    static RegisterPlan Build(ir::Block* block, const Targets& targets = {},
                              const Stops& stops = {}, std::span<const PhiEdge> phi_edges = {},
                              const Exits& exits = {}, const ir::Terminal* terminal = nullptr);

    const LiveSet& At(ir::Inst* inst) const { return before.at(inst); }
    const LiveSet& After(ir::Inst* inst) const { return after.at(inst); }
    // Edge sets describe values needed before parallel phi copies. Phi
    // destinations are killed at their target entry; edge sources are added.
    const LiveSet& ForBranch(ir::Inst* inst) const { return taken.at(inst); }
    const LiveSet& ForFallthrough(ir::Inst* inst) const { return fallthrough.at(inst); }
    const RegisterPlanStats& Stats() const { return stats; }
    u32 Position(ir::Inst* inst) const { return positions.at(inst); }

    static constexpr u32 kNoNextUse = UINT32_MAX;
    // Lexical next-use position is only an eviction hint. A value with no
    // later lexical use can remain live through a backedge; discard decisions
    // must use At/After/edge sets, never this sentinel.
    u32 NextUse(ir::Inst* value, ir::Inst* from) const;

private:
    std::unordered_map<ir::Inst*, LiveSet> before, after, taken, fallthrough;
    std::unordered_map<ir::Inst*, u32> positions;
    std::unordered_map<ir::Inst*, std::vector<u32>> uses;
    RegisterPlanStats stats{};
};

}  // namespace swift::runtime::backend::loongarch64
