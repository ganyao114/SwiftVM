#pragma once

#include <span>
#include "constraints.h"
#include "runtime/ir/ir_types.h"

namespace swift::runtime::ir {
class Inst;
}
namespace swift::runtime::compile {
class AllocationView;
class BlockAnalysisIndex;

// Semantic writer/extension rules for an immutable IR input. Borrow the unique
// instruction index; physical locations and target writebacks are separate.
struct FixedValidationInputs final {
    enum Flag : u8 {
        WidthAlias = 1,
        HostRead = 2,
        FullRead = 4,
        HostWrite = 8,
        NarrowConstant = 16
    };
    struct Site {
        u32 clobbers;
        u16 instruction;
        u8 flags, home;
    };
    struct Step {
        const ir::Inst* instruction;
        const ir::Inst* source;
        ir::ValueType source_type;
        bool parent;
    };
    struct BlockRange {
        const ir::Block* block;
        const BlockAnalysisIndex* index;
    };
    [[nodiscard]] const Site* SiteFor(u32 instruction) const;
    [[nodiscard]] const Step* StepFor(const ir::Inst*) const;
    [[nodiscard]] const BlockRange* BlockFor(const ir::Block*) const;
    Vector<Site> sites;
    Vector<Step> steps;
    Vector<BlockRange> blocks;
    u64 contract_key{};
};

// Reads the current physical view. Extra writes describe target actions which
// are not represented by ordinary IR definitions; no FeatureSet is queried.
void VerifyFixedConstraints(const ir::Block*,
                            const AllocationView&,
                            const FixedValidationInputs&,
                            std::span<const PhysicalGPRWrite> writes = {});
// fail_kind: 0=holds, 1=home placement, 2=overwritten residence, 3=preserved shape.
[[nodiscard]] bool CheckFixedConstraints(const ir::Block*,
                                         const AllocationView&,
                                         const FixedValidationInputs&,
                                         std::span<const PhysicalGPRWrite> writes = {},
                                         u32* fail_kind = nullptr);
}  // namespace swift::runtime::compile
