#pragma once

#include <array>
#include <memory>
#include "runtime/compile/allocate/gpr_inputs.h"
#include "runtime/compile/analysis/facts.h"

namespace swift::runtime::compile {
class AllocationInputs;
class BlockAnalysisIndex;
}  // namespace swift::runtime::compile
namespace swift::runtime {
struct FeatureSet;
}

namespace swift::runtime::backend::arm64 {
enum class WWriteAliases : u8 { Raw, Resolved };
// Follow only the projections used by the physical W-write proof. The root
// keeps its operand view; a GetHostGPR root still needs a caller's committed
// capture/width-owner proof and is never certified by HasKnownWWrite alone.
ir::Value WWriteRoot(ir::Value, WWriteAliases);
bool HasKnownWWrite(ir::Value, WWriteAliases = WWriteAliases::Raw);

// One target input visit, followed by sparse candidate processing. The builder
// borrows the unique semantic index and never reads ordinary physical mapping.
class GPRInputBuilder final {
public:
    GPRInputBuilder(const compile::AllocationInputs&,
                    const FeatureSet& physical_features,
                    compile::BlockAnalysisIndex&,
                    const compile::BlockUseEnds*,
                    const ir::Block*,
                    std::unique_ptr<compile::GPRInputs>&,
                    std::unique_ptr<compile::WidthInputs>&,
                    std::unique_ptr<compile::GPRWriteInputs>&);
    void Describe(const ir::Inst&, size_t position);
    void Finish();

private:
    compile::GPRInputs& Data();
    compile::WidthInputs& WidthData();
    compile::GPRWriteInputs& WriteData();
    void DescribePublication(const ir::Inst&);
    void DescribeWrites();
    void DescribeLongSteps();
    const compile::AllocationInputs& input;
    const FeatureSet& physical_features;
    compile::BlockAnalysisIndex& uses;
    const compile::BlockUseEnds* ends;
    const ir::Block* block;
    std::unique_ptr<compile::GPRInputs>& data;
    std::unique_ptr<compile::WidthInputs>& width;
    std::unique_ptr<compile::GPRWriteInputs>& writes;
    u32 first_read{}, first_view{}, first_w_publication{}, first_width{}, first_step{},
            blocked_homes{};
    u32 first_write{};
    bool publications_enabled{}, coalescing_enabled{}, width_enabled{}, full_exchange{},
            has_publications{};
    u8 fixed_class_pools{};
    std::array<const ir::Inst*, 32> latest_store{};
    StackVector<size_t, 16> publication_candidates;
    StackVector<compile::GPRInputs::Clobber, 16> clobbers;
    StackVector<compile::GPRInputs::ConsumerRule, 16> consumer_rules;
    struct HostWrite {
        u16 home, instruction;
    };
    StackVector<HostWrite, 8> host_writes;
    Vector<size_t> write_candidates;
    Vector<compile::GPRWriteInputs::Capture> captures;
};

}  // namespace swift::runtime::backend::arm64
