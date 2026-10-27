#pragma once

#include <memory>

#include "runtime/common/svm_config.h"
#include "runtime/compile/unit.h"

namespace swift::runtime::compile {
class SemanticFacts;

// An owning result of semantic optimization. Only sealed IR enters this stage;
// const clients cannot regain a construction writer or build analysis indexes.
class OptimizedUnit final {
public:
    [[nodiscard]] static OptimizedUnit FromFinalIR(CompileUnit unit,
                                                   const FeatureSet& features,
                                                   size_t decoded_blocks = 0);
    ~OptimizedUnit();
    OptimizedUnit(const OptimizedUnit&) = delete;
    OptimizedUnit& operator=(const OptimizedUnit&) = delete;
    OptimizedUnit(OptimizedUnit&&) noexcept;
    OptimizedUnit& operator=(OptimizedUnit&&) noexcept;

    [[nodiscard]] const CompileUnit& Unit() const;
    [[nodiscard]] const ir::Block* Block() const;
    [[nodiscard]] const ir::HIRFunction* Function() const;
    [[nodiscard]] const FeatureSet& Features() const;
    [[nodiscard]] size_t DecodedBlocks() const;
    [[nodiscard]] const SemanticFacts& Facts() const;
    [[nodiscard]] SemanticFacts& PrepareFacts();
    // The target compiler calls this after its last analysis consumer. It
    // transfers the sealed IR owner and releases all preparation side tables.
    [[nodiscard]] CompileUnit TakeIR() &&;

private:
    struct Storage;
    explicit OptimizedUnit(std::unique_ptr<Storage> storage) noexcept;
    std::unique_ptr<Storage> storage;
};
}  // namespace swift::runtime::compile
