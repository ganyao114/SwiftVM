#pragma once

#include "actions.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
class CompileWorkspace;
}
namespace swift::runtime::backend::arm64 {

struct IntegerFlagsActions;

// Preparation-only sparse output; final flags join the unit instruction table.
struct ScalarActions final {
    struct Flag {
        u16 instruction;
        ScalarFlag flags;
        bool operator==(const Flag&) const = default;
    };
    std::vector<NarrowAction> operations;
    std::vector<Flag> flags;
    [[nodiscard]] const NarrowAction* Narrow(u32 id) const;
    [[nodiscard]] bool Has(u32 id, ScalarFlag flag) const;
    void Finish();
};

// Borrow earlier selections for this block. Matching proves local instruction
// identity before querying their IDs; no projected pointer sets are needed.
struct ScalarBitInputs final {
    const ScalarActions* copies{};
    const IntegerFlagsActions* integer_flags{};
    const IntegerBranchAction* branch{};
    const BitVector* earlier_coverage{};
};

ScalarActions LowerScalarCopies(const compile::BlockAnalysisIndex&);
ScalarActions LowerNarrowBits(const compile::BlockAnalysisIndex&,
                              compile::CompileWorkspace&,
                              compile::AllocationView,
                              const ScalarBitInputs&);

// Pure preparation query: local uniform forwarding uses the shared ordered
// roster and never requests either consumer graph or a mutable block view.
[[nodiscard]] bool SelectNativeDiv128(const ir::Inst&, const compile::BlockAnalysisIndex&);
// Explicit manual-encoder preparation, including blockless SSA-only proofs.
// A uniform load must be found in this exact block before walking its prefix.
[[nodiscard]] bool PrepareManualDiv128(const ir::Inst&, const ir::Block*);
}  // namespace swift::runtime::backend::arm64
