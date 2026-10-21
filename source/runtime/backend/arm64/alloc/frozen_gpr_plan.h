#pragma once

#include <map>
#include <memory>
#include <unordered_set>
#include <vector>
#include "fixed_gpr_constraints.h"
#include "target_analysis_traits.h"
#include "runtime/backend/reg_alloc.h"

namespace swift::runtime::backend::arm64 {

class BlockAnalysisIndex;

struct SpillInputFacts {
    ir::Inst* definition{};
    bool last_use{};
};

// Compatibility for manual emitters without an admitted unit certificate.
[[nodiscard]] SpillInputFacts PrepareSpillInput(ir::Inst* consumer, u32 value_id);

struct GPRResidenceFacts {
    std::unordered_set<ir::Inst*> zero_extends, sign_extends, resident_values;
    std::map<const ir::Inst*, u16> result_homes;
};

// Compact boundary between target discovery and final allocation. No recipes,
// consumer index, GuestStateMap or code-emission resources cross this boundary.
struct GPRPlanDraft {
    ir::Block* block{};
    GPRResidenceFacts facts;
    std::vector<GPRPhysicalWrite> writes;
};

// Opcode and target-policy decisions are made before emission. Dirty registers
// and spill forwarding remain dynamic, so emission only filters these leases.
struct InstructionScratchFacts {
    u32 fixed_clobbers{}, audit_clobbers{}, leases{};
    ScratchNeed budget{kDefaultScratchGPR, kDefaultScratchFPR};
    bool vixl_x87{};
    // Demand/observer facts share the existing record padding. They are read
    // during allocation; emitters do not inspect the use or pseudo-use graph.
    bool has_value_uses : 1 {};
    bool unobserved_mul_right : 1 {};
    bool div128_native : 1 {};
    SpillForwardConsumerKind spill_consumer : 2 {SpillForwardConsumerKind::Ordinary};
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
    bool xpool{}, precise{}, fpr_precise{}, nan_coldpath{};
    bool operator==(const ScratchPreparationInputs&) const = default;
};
static_assert(sizeof(InstructionScratchFacts) == 16);
[[nodiscard]] ScratchPreparationInputs CaptureScratchPreparationInputs(
        const GPRSMask& pool, const FeatureSet& features);
[[nodiscard]] UnitScratchFacts PrepareUnitScratch(const GPRSMask& pool, const FeatureSet& features);
[[nodiscard]] InstructionScratchFacts PrepareInstructionScratch(
        const ir::Inst& inst, const GPRSMask& pool, const FeatureSet& features);
[[nodiscard]] InstructionScratchFacts PrepareInstructionScratchWithBlock(
        const ir::Inst& inst, const GPRSMask& pool, const FeatureSet& features, ir::Block* block);

class FrozenGPRPlan final {
public:
    using View = RegAlloc::GPRView;
    using Location = RegAlloc::GPRLocation;
    struct OperandPlan {
        enum class SpillInputState : u8 { Unprepared, Live, Last };
        const ir::Inst* consumer{};
        u8 argument{};
        SpillForwardConsumerKind spill_consumer{SpillForwardConsumerKind::Ordinary};
        u32 element{};
        ir::Value value{};
        ir::ValueType type{};
        bool spill_clobber_valid{};
        SpillInputState spill_input{SpillInputState::Unprepared};
        u32 spill_clobbers{};
    };
    static_assert(sizeof(OperandPlan) == 32);
    static std::shared_ptr<const FrozenGPRPlan> Seal(RegAlloc& allocation, GPRPlanDraft draft);
    static std::shared_ptr<const FrozenGPRPlan> SealWithSpillClobbers(
            RegAlloc& allocation, GPRPlanDraft draft, const FeatureSet& spill_features);
    [[nodiscard]] bool MatchesFrozenIR() const;
    [[nodiscard]] const OperandPlan* SpillConsumerFor(const ir::Inst* consumer) const;
    [[nodiscard]] std::optional<u32> SpillClobberMask(const ir::Inst* consumer) const;
    [[nodiscard]] SpillInputFacts SpillInputFor(const ir::Inst* consumer, u32 value_id) const;
    [[nodiscard]] const InstructionScratchFacts* ScratchFor(const ir::Inst* consumer) const;
    [[nodiscard]] const InstructionScratchFacts* ScratchFor(const ir::Inst* consumer, size_t& next) const {
        if (!consumer) return nullptr;
        // A cursor transports the frozen sequence; it discovers no IR facts.
        if (next < instruction_scratch.size() && instruction_scratch[next].consumer == consumer)
            return &instruction_scratch[next++].facts;
        return FindScratchFor(consumer, next);
    }
    [[nodiscard]] bool MatchesScratchInputs(const GPRSMask& pool, const FeatureSet& features) const;
    [[nodiscard]] bool HasPreparedScratch() const { return unit_scratch.prepared; }
    [[nodiscard]] bool UsesUnitScratchInputs() const { return unit_scratch.prepared && !scratch_inputs; }
    [[nodiscard]] const UnitScratchFacts& UnitScratch() const { return unit_scratch; }
    [[nodiscard]] bool MatchesAllocation() const {
        return generation == allocation.GPRLocationGeneration() && allocation_attempt == allocation.AllocationAttempt();
    }
    [[nodiscard]] u32 Generation() const { return generation; }
    [[nodiscard]] const auto& PhysicalWrites() const { return writes; }
    [[nodiscard]] const auto& Operands() const { return operands; }
    [[nodiscard]] const GPRResidenceFacts& Facts() const { return facts; }
    [[nodiscard]] Location UseLocation(ir::Value value, const ir::Inst* consumer,
                                       View view = View::Value) const;
    [[nodiscard]] bool ValueFullyResident(ir::Inst* value) const { return facts.resident_values.contains(value); }
    [[nodiscard]] std::optional<u16> ResultHome(const ir::Inst* value) const {
        const auto found = facts.result_homes.find(value);
        return found == facts.result_homes.end() ? std::nullopt : std::optional{found->second};
    }

private:
    friend class ARMTargetPlanner;
    // Only the target transaction can seal against its owning unit certificate.
    static std::shared_ptr<const FrozenGPRPlan> SealForUnit(RegAlloc& allocation,
            GPRPlanDraft draft, const FeatureSet& features, const UnitScratchFacts& unit,
            const BlockAnalysisIndex& analysis);
    [[nodiscard]] const InstructionScratchFacts* FindScratchFor(const ir::Inst* consumer, size_t& next) const;
    FrozenGPRPlan(RegAlloc& allocation, GPRPlanDraft draft, const FeatureSet* spill_features,
                  const UnitScratchFacts* unit = nullptr, const BlockAnalysisIndex* analysis = nullptr);
    const RegAlloc& allocation;
    ir::Block* block;
    GPRResidenceFacts facts;
    std::vector<GPRPhysicalWrite> writes;
    u32 generation, allocation_attempt;
    bool spill_clobber_ids_ordered{};
    std::vector<OperandPlan> operands;
    struct InstructionScratchRecord {
        const ir::Inst* consumer;
        InstructionScratchFacts facts;
    };
    // Single-instruction blocks need no separate scratch-record allocation.
    StackVector<InstructionScratchRecord, 1> instruction_scratch;
    std::optional<ScratchPreparationInputs> scratch_inputs;
    UnitScratchFacts unit_scratch;
};
}


