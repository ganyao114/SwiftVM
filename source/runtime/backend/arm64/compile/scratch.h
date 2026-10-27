#pragma once

#include <span>
#include <vector>

#include "runtime/backend/arm64/compile/effects.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
class CompileWorkspace;
struct FlagsDemand;
}
namespace swift::runtime::backend::arm64 {

struct SpillInputFacts {
    const ir::Inst* definition{};
    bool last_use{};
};

// The public manual encoder can prepare its own operand transport. Normal
// translation consumes the sparse certificates produced by LowerScratch.
[[nodiscard]] SpillInputFacts PrepareSpillInput(const ir::Inst* consumer, u32 value_id);

struct InstructionScratchFacts {
    u32 fixed_clobbers{}, audit_clobbers{}, leases{};
    compile::ScratchNeed budget{kDefaultScratchGPR, kDefaultScratchFPR};
    bool vixl_x87{};
    bool has_value_uses : 1 {};
    bool unobserved_mul_right : 1 {};
    bool div128_native : 1 {};
    SpillForwardConsumerKind spill_consumer : 2 {SpillForwardConsumerKind::Ordinary};
    bool operator==(const InstructionScratchFacts&) const = default;
};
struct UnitScratchFacts {
    u32 terminal_leases{};
    bool extended_pool{}, scratch_only{}, level3_indirect{}, prepared{};
    bool operator==(const UnitScratchFacts&) const = default;
};
struct ScratchPreparationInputs {
    GPRSMask pool{};
    u32 pin{};
    bool exec_prof{}, static_regs{}, flags_regs{};
    bool xpool{}, precise{}, fpr_precise{};
    bool operator==(const ScratchPreparationInputs&) const = default;
};

// Equal mandatory facts for consecutive IDs within one block. Gaps and block
// boundaries remain distinct; this is not a default for unlisted instructions.
struct ScratchRange {
    InstructionScratchFacts facts;
    u16 first, last;
};

// Only a local MEM producer actually used by this consumer needs a record.
// The definition identity is necessary transport data; retaining it avoids
// searching Lambda/linked Params operands again when a pending write flushes.
struct SpillInputAction {
    const ir::Inst* definition;
    u16 consumer, producer;
    bool last_use;
    [[nodiscard]] u32 Key() const { return (u32{consumer} << 16) | producer; }
};

struct InstructionScratchView {
    std::span<const ScratchRange> ranges;
    const ir::Block* block{};
    [[nodiscard]] const InstructionScratchFacts* For(const ir::Inst*, size_t& next) const;
    [[nodiscard]] const InstructionScratchFacts* For(const ir::Inst* inst) const {
        size_t next = ranges.size();
        return For(inst, next);
    }
};

struct ScratchActions {
    UnitScratchFacts unit;
    std::vector<ScratchRange> ranges;
    std::vector<SpillInputAction> inputs;
};

[[nodiscard]] ScratchPreparationInputs CaptureScratchPreparationInputs(const GPRSMask&,
                                                                       const FeatureSet&);
[[nodiscard]] UnitScratchFacts PrepareUnitScratch(const GPRSMask&, const FeatureSet&);
[[nodiscard]] InstructionScratchFacts PrepareInstructionScratch(const ir::Inst&,
                                                                const GPRSMask&,
                                                                const FeatureSet&);
[[nodiscard]] InstructionScratchFacts PrepareInstructionScratchWithBlock(const ir::Inst&,
                                                                         const GPRSMask&,
                                                                         const FeatureSet&,
                                                                         const ir::Block*);
namespace detail {
// All required graphs have been prepared by the caller. MEM transport uses
// exact occurrence counts from the current consumer, not a reverse-use graph.
[[nodiscard]] ScratchActions LowerScratch(const ir::Block*,
                                          compile::AllocationView,
                                          const FeatureSet&,
                                          const UnitScratchFacts&,
                                          const compile::BlockAnalysisIndex&,
                                          compile::CompileWorkspace&);
}  // namespace detail

}  // namespace swift::runtime::backend::arm64
