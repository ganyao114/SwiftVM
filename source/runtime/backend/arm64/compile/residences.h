#pragma once

#include <vector>

#include "actions.h"
#include "runtime/compile/allocate/constraints.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {

// Build the final compact rows directly. The block identity protects local
// queries before UnitActions takes ownership; instruction IDs alone do not
// distinguish independently numbered blocks. Queries never sort or allocate.
class GPRResidences final {
public:
    GPRResidences() = default;
    GPRResidences(GPRResidences&&) = default;
    GPRResidences& operator=(GPRResidences&&) = default;
    GPRResidences(const GPRResidences&) = delete;
    GPRResidences& operator=(const GPRResidences&) = delete;

    void Reset(const ir::Block*);
    void Record(const ir::Inst*, GPRResidenceAction::Flag);
    void RecordResultHome(const ir::Inst*, u16 home);
    [[nodiscard]] const GPRResidenceAction* Find(const ir::Inst*) const;
    [[nodiscard]] bool Has(const ir::Inst* inst, GPRResidenceAction::Flag flag) const {
        const auto* row = Find(inst);
        return row && row->Has(flag);
    }

private:
    friend class AllocationPreparation;
    [[nodiscard]] GPRResidenceAction* Prepare(const ir::Inst*);
    const ir::Block* block{};
    std::vector<GPRResidenceAction> rows;
};

struct GPRBlockPreparation {
    GPRResidences residences;
    std::vector<compile::PhysicalGPRWrite> writes;
};

}  // namespace swift::runtime::backend::arm64
