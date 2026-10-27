#pragma once

#include "runtime/compile/analysis/semantic_facts.h"

#include "allocation.h"
#include "inputs.h"
#include "runtime/common/svm_config.h"
#include "runtime/compile/analysis/facts.h"
#include "runtime/compile/unit.h"

namespace swift::runtime::compile {

// This state lives from initial allocation through target admission. Physical
// retries borrow the optimized input's facts and the writer's caller-owned
// workspace. Neither state nor mutable cursors escape into generated code.
struct AllocationState final {
    AllocationState(AllocationInputs inputs,
                    SemanticFacts& facts,
                    bool block_intervals,
                    bool scalar_insert,
                    bool intwidth_tie,
                    bool spill_evict)
            : inputs(std::move(inputs))
            , block_intervals(block_intervals)
            , scalar_insert(scalar_insert)
            , intwidth_tie(intwidth_tie)
            , spill_evict(spill_evict)
            , facts(facts) {
        if (block_intervals) this->block_intervals = facts.CanUseBlockIntervals();
    }

    const AllocationInputs inputs;
    bool block_intervals{}, scalar_insert{}, intwidth_tie{}, spill_evict{};
    SemanticFacts& facts;
};

class AllocationController final {
public:
    template <class Unit> static AllocationState& Begin(Unit* unit,
                                                        AllocationWriter& allocation,
                                                        AllocationInputs inputs,
                                                        bool block_intervals,
                                                        bool scalar_insert,
                                                        bool intwidth_tie,
                                                        bool spill_evict);
    // Target admission owns the decision and loop. These operations only
    // rebuild physical results or release this request's input state. The
    // caller continues to own the shared facts and workspace.
    static void RetryWithoutFPRCoalescing(AllocationWriter& allocation);
    static void Finish(AllocationWriter& allocation);
};

}  // namespace swift::runtime::compile
