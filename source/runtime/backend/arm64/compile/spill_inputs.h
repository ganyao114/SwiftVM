#pragma once

#include <memory>
#include "runtime/compile/allocate/spill_inputs.h"

namespace swift::runtime::ir {
class Inst;
}

namespace swift::runtime::compile {
class BlockAnalysisIndex;
}

namespace swift::runtime::backend::arm64 {
// Participates in the existing target input visit. All operands and instruction
// positions are borrowed from the shared block index, never copied as a graph.
class SpillInputBuilder final {
public:
    SpillInputBuilder(const ir::Block*,
                      const compile::BlockAnalysisIndex&,
                      std::unique_ptr<compile::SpillInputs>&);
    void Describe(const ir::Inst&, u32 position, bool has_integer_operands);
    void Finish();

private:
    const ir::Block* block;
    const compile::BlockAnalysisIndex& uses;
    std::unique_ptr<compile::SpillInputs>& data;
    Vector<compile::SpillInputs::Site> sites;
    Vector<compile::SpillInputs::Segment> segments;
    u32 segment_start{};
    bool has_integer_uses{};
};
}  // namespace swift::runtime::backend::arm64
