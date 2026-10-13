#pragma once

#include <unordered_map>
#include <unordered_set>
#include "runtime/backend/loongarch64/jit/register_plan.h"

namespace swift::runtime::backend::loongarch64 {

struct FlagsEffect {
    ir::Flags reads{ir::Flags::None};
    ir::Flags writes{ir::Flags::None};
    // Only definitely replaced bits may kill backward demand. Unknown
    // lowerings can conservatively dirty every bit without killing demand.
    ir::Flags replaces{ir::Flags::None};
    bool observes_before{};
    bool synchronized_after{};
    bool exits_before{};
    bool exits_after{};
    bool opaque{};
};

struct FlagsPlanStats {
    u32 instructions{}, reachable{}, dirty_iterations{}, demand_iterations{};
    u32 dirty_boundaries{}, clean_boundaries{}, opaque{}, exit_paths{};
};

// Software flags already reside in r31. This analysis tracks which guest
// flag bits may differ from canonical State memory on any incoming path.
// It never selects or executes LBT. Entry starts synchronized by the
// prologue's ReloadFlags; every helper/fault observation needs all bits.
// CFG coordinates match RegisterPlan and the lowered FunctionPlan body.
class FlagsPlan {
public:
    using Targets = RegisterPlan::Targets;
    using Stops = RegisterPlan::Stops;
    using Exits = RegisterPlan::Exits;
    using Instructions = std::unordered_set<ir::Inst*>;

    static FlagsPlan Build(ir::Block* block, const Targets& targets = {},
                           const Stops& stops = {}, const Exits& exits = {},
                           const Instructions& halt_checks = {});
    static FlagsEffect Effect(ir::Inst* inst);
    static ir::Flags ConditionDemand(ir::Cond condition);

    // An unreachable instruction is conservatively considered dirty when
    // emitted; its state never contributes to reachable exit analysis.
    ir::Flags At(ir::Inst* inst) const;
    ir::Flags After(ir::Inst* inst) const;
    ir::Flags NeededBefore(ir::Inst* inst) const { return needed_before.at(inst); }
    ir::Flags NeededAfter(ir::Inst* inst) const { return needed_after.at(inst); }
    ir::Flags NeededWrite(ir::Inst* inst) const {
        return effects.at(inst).writes & NeededAfter(inst);
    }
    bool Reachable(ir::Inst* inst) const { return reachable.contains(inst); }
    const FlagsEffect& EffectAt(ir::Inst* inst) const { return effects.at(inst); }
    ir::Flags ExitDirty() const { return exit_dirty; }
    const FlagsPlanStats& Stats() const { return stats; }

private:
    std::unordered_map<ir::Inst*, ir::Flags> before, after, needed_before, needed_after;
    std::unordered_map<ir::Inst*, FlagsEffect> effects;
    Instructions reachable;
    ir::Flags exit_dirty{ir::Flags::None};
    FlagsPlanStats stats{};
};

}  // namespace swift::runtime::backend::loongarch64
