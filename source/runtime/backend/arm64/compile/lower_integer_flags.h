#pragma once

#include "lower_flags.h"
#include "runtime/compile/analysis/block_index.h"

namespace swift::runtime::backend::arm64 {

// Short-lived matching inputs. A forwarded input is a certified receiving
// edge, not an arbitrary replacement Value or a second consumer graph.
// Demand borrows the immutable packed block facts and one selected-branch
// override. The owner rejects foreign definitions reusing the same numeric ID.
struct IntegerFlagsInputs {
    const BitVector& earlier_coverage;
    const compile::BlockAnalysisIndex& demand_owner;
    compile::PseudoFlagsView demands;
    const IntegerBranchAction* selected_branch{};
    std::span<const IntegerFlagsAction> forwarded_inputs;
    bool canonical_carry{};
    const PstateEligibility* pstate{};
};

struct IntegerFlagsActions final {
    std::vector<IntegerFlagsAction> actions;
    std::vector<u16> covered;
    [[nodiscard]] const IntegerFlagsAction* Find(u32 id) const;
    [[nodiscard]] const IntegerFlagsAction* Input(u32 id) const;
    [[nodiscard]] const IntegerFlagsAction* Carry(u32 id) const;
    [[nodiscard]] const IntegerFlagsAction* Compare(u32 id) const;
    [[nodiscard]] bool Covered(u32 id) const;
};

bool HasNarrowIntegerFlagsCandidates(const ir::Block&);
IntegerFlagsActions LowerNarrowIntegerFlags(compile::BlockAnalysisIndex&,
                                            compile::CompileWorkspace&,
                                            const IntegerFlagsInputs&);
IntegerFlagsActions LowerIntegerFlags(compile::BlockAnalysisIndex&,
                                      compile::CompileWorkspace&,
                                      compile::AllocationView,
                                      const compile::PseudoFlagsPlan&,
                                      const IntegerBranchAction*,
                                      const BitVector& earlier_coverage,
                                      const PstateEligibility*,
                                      bool canonical_carry);
}  // namespace swift::runtime::backend::arm64
