#pragma once

#include <memory>
#include <unordered_set>
#include "runtime/compile/allocate/fixed_validation.h"

namespace swift::runtime {
struct FeatureSet;
}
namespace swift::runtime::compile {
class AllocationWriter;
}

namespace swift::runtime::backend::arm64 {
// Identity of the global/static-home policy used by this semantic description.
u64 FixedValidationContractKey(const FeatureSet&);

class FixedValidationBuilder final {
public:
    explicit FixedValidationBuilder(const FeatureSet&);
    void BeginBlock(const ir::Block*, const compile::BlockAnalysisIndex&);
    void Describe(const ir::Inst&);
    std::unique_ptr<const compile::FixedValidationInputs> Finish();

private:
    void DescribeSteps(const ir::Inst*);
    const FeatureSet& features;
    std::unique_ptr<compile::FixedValidationInputs> data;
    std::unordered_set<const ir::Inst*> described_steps;
};

// A preparation-scope description: reuse a matching allocation request, or
// explicitly describe a manual/audit input with its supplied feature snapshot.
// Nothing in this object is retained by an emission result.
class FixedValidationDescription final {
public:
    FixedValidationDescription(const ir::Block*,
                               const compile::AllocationWriter&,
                               const FeatureSet&,
                               const compile::BlockAnalysisIndex&);
    [[nodiscard]] const compile::FixedValidationInputs& Inputs() const { return *data; }

private:
    std::unique_ptr<const compile::FixedValidationInputs> owned;
    const compile::FixedValidationInputs* data{};
};
}  // namespace swift::runtime::backend::arm64
