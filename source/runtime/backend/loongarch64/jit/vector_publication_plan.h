#pragma once

#include <unordered_map>
#include <vector>
#include "vector_allocation_plan.h"

namespace swift::runtime::backend::loongarch64 {

struct VectorPublicationStats {
    bool accepted{};
    u32 instructions{}, mapped_values{}, reachable{}, cfg_edges{}, iterations{};
    u32 native_definitions{}, phi_definitions{}, canonical_boundaries{};
    u32 clean_publications{}, dirty_publications{};
};

// A bit denotes a mapped SSA value whose raw128 home may differ from its
// current dynamic definition on any incoming path. Meets are unions over the
// complete CFG, rather than facts inherited from the previous lexical emitter.
// Only guaranteed publications and full home definitions clear bits. Hidden
// barriers and conditional phi-source publications are deliberately omitted:
// they can make a home cleaner, but cannot invalidate this conservative model.
class VectorPublicationPlan {
public:
    static VectorPublicationPlan Build(ir::Block* block, const VectorAllocationPlan& allocation,
            const RegisterPlan::Targets& targets = {}, const RegisterPlan::Stops& stops = {},
            const RegisterPlan::Exits& exits = {});

    // At is before BeginInstruction, after an incoming edge's complete phi
    // copies. After is after the emitter/common continuation. In particular,
    // an inactive final Barrier must use After for a just-produced native value.
    // Unknown, unreachable and budget-rejected coordinates are always dirty.
    bool DirtyAt(ir::Inst* inst, ir::Inst* value) const;
    bool DirtyAfter(ir::Inst* inst, ir::Inst* value) const;
    bool Reachable(ir::Inst* inst) const;
    const VectorPublicationStats& Stats() const { return stats; }

private:
    bool Dirty(const std::vector<std::vector<u64>>& states, ir::Inst* inst, ir::Inst* value) const;
    std::unordered_map<ir::Inst*, u32> positions, values;
    std::vector<std::vector<u64>> before, after;
    std::vector<bool> reachable;
    VectorPublicationStats stats;
};

} // namespace swift::runtime::backend::loongarch64
