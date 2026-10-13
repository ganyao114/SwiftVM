#pragma once

#include <optional>
#include "register_plan.h"
#include "wide.h"
#include "runtime/backend/loongarch64/host_features.h"

namespace swift::runtime::backend::loongarch64 {

struct VectorAllocationStats {
    u32 values{}, register_values{}, spilled_values{}, interference_edges{}, max_pressure{};
};

// Whole-CFG raw128 assignments use independent LSX views, including the two
// halves of a legalized V256. LASX pairs compute only in scratch. Helpers and
// other ABI boundaries publish live inputs and restore live outputs. The local
// cache remains the straight-line/rollback baseline. v0..v3 are scratch;
// v24..v31 alias LP64D callee-saved FPRs and are never assigned.
class VectorAllocationPlan {
public:
    static constexpr u32 kFirstRegister = 8, kRegisterCount = 16;
    static VectorAllocationPlan Build(ir::Block* block, const HostFeatures& features,
            std::span<const WideVectorPair> wide, const RegisterPlan::Targets& targets = {},
            const RegisterPlan::Stops& stops = {}, const RegisterPlan::Exits& exits = {},
            bool cfg_allocation = true);
    static bool Native128(ir::Inst* inst, const HostFeatures& features);
    static bool Eligible(ir::Inst* inst);
    // GPR-only/control emitters leave the vector bank intact. Their raw-home
    // operands still need explicit publication and vector definitions reload.
    static bool PreservesRegisters(ir::Inst* inst);
    std::optional<u32> Register(ir::Inst* inst) const;
    const VectorAllocationStats& AllocationStats() const { return allocation_stats; }
    u32 CFGNodes() const { return liveness.Stats().nodes; }
    bool RegionInstruction(ir::Inst* inst) const { return region.contains(inst); }
    const WideVectorPair* WideHead(ir::Inst* inst) const;
    bool WideTail(ir::Inst* inst) const { return tails.contains(inst); }
    const LiveSet& After(ir::Inst* inst) const;
    const LiveSet& At(ir::Inst* inst) const;
    u32 NextUse(ir::Inst* value, ir::Inst* from) const { return liveness.NextUse(value, from); }
private:
    void Allocate(ir::Block* block, const RegisterPlan::Targets& targets);
    RegisterPlan liveness;
    std::unordered_set<ir::Inst*> region, tails;
    std::unordered_map<ir::Inst*, WideVectorPair> pairs;
    std::unordered_map<ir::Inst*, LiveSet> fused_inputs;
    std::unordered_map<ir::Inst*, u32> registers;
    VectorAllocationStats allocation_stats;
};

} // namespace swift::runtime::backend::loongarch64
