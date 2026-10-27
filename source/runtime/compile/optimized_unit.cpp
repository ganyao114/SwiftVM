#include "optimized_unit.h"

#include "runtime/common/svm_config.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {

struct OptimizedUnit::Storage {
    Storage(CompileUnit input, const FeatureSet& features, size_t decoded_blocks)
            : unit(std::move(input)), features(features), decoded_blocks(decoded_blocks) {
        ASSERT(unit.IsSealed());
        if (unit.GetFunction())
            facts.BeginInput(nullptr, unit.GetFunction());
        else
            facts.BeginInput(unit.GetBlock(), nullptr);
        (void)facts.PrepareValueLiveness();
    }
    CompileUnit unit;
    SemanticFacts facts;
    const FeatureSet features;
    const size_t decoded_blocks;
};

OptimizedUnit::OptimizedUnit(std::unique_ptr<Storage> storage) noexcept
        : storage(std::move(storage)) {}
OptimizedUnit::~OptimizedUnit() = default;
OptimizedUnit::OptimizedUnit(OptimizedUnit&&) noexcept = default;
OptimizedUnit& OptimizedUnit::operator=(OptimizedUnit&&) noexcept = default;

OptimizedUnit OptimizedUnit::FromFinalIR(CompileUnit unit,
                                         const FeatureSet& features,
                                         size_t decoded_blocks) {
    ASSERT(unit.IsSealed());
    const auto& config = GetSvmConfig();
    if (config.dump_ir && config.dump_ir_post) {
        const auto dump = [](const ir::Block* block) {
            SVM_DIAG_FORMAT(Codegen,
                            "--- final-ir block {:#x} ---\n{}\n",
                            block->GetStartLocation().Value(),
                            block->ToString());
        };
        if (const auto* function = unit.GetFunction()) {
            for (const auto& block : function->GetHIRBlocksRPO()) dump(block.GetBlock());
        } else {
            dump(unit.GetBlock());
        }
    }
    return OptimizedUnit{std::make_unique<Storage>(std::move(unit), features, decoded_blocks)};
}
const CompileUnit& OptimizedUnit::Unit() const {
    ASSERT(storage);
    return storage->unit;
}
const ir::Block* OptimizedUnit::Block() const { return Unit().GetBlock(); }
const ir::HIRFunction* OptimizedUnit::Function() const { return Unit().GetFunction(); }
const FeatureSet& OptimizedUnit::Features() const {
    ASSERT(storage);
    return storage->features;
}
size_t OptimizedUnit::DecodedBlocks() const {
    ASSERT(storage);
    return storage->decoded_blocks;
}
const SemanticFacts& OptimizedUnit::Facts() const {
    ASSERT(storage);
    return storage->facts;
}
SemanticFacts& OptimizedUnit::PrepareFacts() {
    ASSERT(storage);
    return storage->facts;
}
CompileUnit OptimizedUnit::TakeIR() && {
    ASSERT(storage && !storage->facts.writer_active);
    auto unit = std::move(storage->unit);
    storage.reset();
    return unit;
}

}  // namespace swift::runtime::compile
