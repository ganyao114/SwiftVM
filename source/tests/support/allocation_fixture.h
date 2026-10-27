#pragma once

#include "runtime/backend/arm64/compile/allocation_preparation.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::test {

class AllocationFixtureOutput {
protected:
    AllocationFixtureOutput(u32 capacity,
                            const backend::GPRSMask& gprs,
                            const backend::FPRSMask& fprs,
                            const FeatureSet& features)
            : allocation(capacity, gprs, fprs, features), actions(allocation.View()) {}
    compile::CompileWorkspace workspace;
    compile::SemanticFacts facts;
    compile::Allocation allocation;
    backend::arm64::UnitActions actions;
};

// Real physical/action owners outlive every borrowed preparation interface.
class AllocationFixture final : private AllocationFixtureOutput,
                                public backend::arm64::AllocationPreparation {
public:
    AllocationFixture(u32 capacity,
                      const backend::GPRSMask& gprs,
                      const backend::FPRSMask& fprs,
                      const FeatureSet& features)
            : AllocationFixtureOutput(capacity, gprs, fprs, features)
            , AllocationPreparation(AllocationFixtureOutput::allocation,
                                    AllocationFixtureOutput::facts,
                                    AllocationFixtureOutput::workspace,
                                    AllocationFixtureOutput::actions) {}
};

}  // namespace swift::runtime::test
