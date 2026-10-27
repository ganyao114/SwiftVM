#pragma once

#include "runtime/backend/arm64/compile/compiler.h"

#include "lower_memory.h"
#include "runtime/backend/arm64/compile/effects.h"
#include "runtime/backend/arm64/compile/residence_preparation.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/analysis/guest_state.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
}
namespace swift::runtime::compile {
struct PseudoFlagsDemand;
}
namespace swift::runtime::backend::arm64 {

// One target preparation session owns short-lived selections and submits
// physical contracts. No encoder or emission state is available here.
class TargetPreparation final {
public:
    TargetPreparation(AllocationPreparation& allocation,
                      compile::CompileWorkspace& workspace,
                      const Config& config,
                      FeatureSet features,
                      const compile::GuestStateLayout& guest_layout)
            : allocation(allocation)
            , workspace(workspace)
            , config(config)
            , features(features)
            , guest_layout(guest_layout)
            , gpr_builder(allocation, this->features, register_state, guest_layout.uni_gprs)
            , use_memory_base(config.memory_base != nullptr || config.page_table != nullptr)
            , memory_selections(workspace) {}
    TargetPreparation(const TargetPreparation&) = delete;
    TargetPreparation(TargetPreparation&&) = delete;
    TargetPreparation& operator=(const TargetPreparation&) = delete;
    TargetPreparation& operator=(TargetPreparation&&) = delete;
    AllocationPreparationResult Complete(const ir::Block* block, const ir::HIRFunction* function);

private:
    struct PendingGPRBlock {
        const ir::Block* block;
        GPRBlockPreparation draft;
        compile::BlockAnalysisIndex* analysis{};
        MemorySelections memory;
        std::vector<PublicationAction> publications;
    };
    struct StoreUpdateCandidate {
        const ir::Inst* memory{};
        const ir::Inst* publication{};
        u8 home{};
        s16 offset{};
    };
    struct LoadUpdateCandidate {
        const ir::Inst* load{};
        const ir::Inst* update{};
        const ir::Inst* publication{};
        const ir::Inst* base_read{};
        u16 target{};
        s16 offset{};
    };
    struct MemorySourceCandidate {
        u16 target{};
        u32 live_begin{};
    };
    void Finalize(const ir::Block*,
                  const ir::HIRFunction*,
                  std::span<const ir::Block* const>,
                  std::span<const PendingGPRBlock>,
                  std::span<const u32> zero_stores,
                  bool ssa_valid);
    PendingGPRBlock DeclareGPRBlock(const ir::Block* block);
    bool PrepareCoalescedHostWriteProof(const ir::Inst* inst);
    void PreparePublications(const ir::Block* block);
    std::optional<StoreUpdateCandidate> MatchMemoryUpdate(const ir::Inst* update) const;
    std::optional<StoreUpdateCandidate> MatchPreIndexMemoryUpdate(const ir::Inst* update) const;
    std::optional<StoreUpdateCandidate> MatchBiasedMemoryUpdate(const ir::Inst* update) const;
    void PrepareMemoryUpdates(const ir::Block* block);
    std::optional<LoadUpdateCandidate> MatchPinnedLoadUpdate(const ir::Inst* update);
    void PreparePinnedLoadUpdates(const ir::Block* block);
    std::optional<MemorySourceCandidate> MatchPinnedMemorySource(ir::Value source);
    std::optional<AddressAction> MatchPinnedMemoryAddress(const ir::Inst* address);
    std::optional<MemoryTransferAction> PreparePinnedMemoryValue(const ir::Inst* extract);
    void PreparePinnedMemoryValues(const ir::Block* block);
    void PreparePinnedMemoryAddresses(const ir::Block* block);
    void PreparePinnedZeroExtendPublications(const ir::Block* block);

    bool IsGPRMappedTo(ir::Value value, u16 home) const {
        return allocation.ValueType(value) == AllocationPreparation::GPR &&
               allocation.ValueGPR(value).id == home;
    }
    bool IsSpilled(ir::Value value) const {
        return allocation.ValueType(value) == AllocationPreparation::MEM;
    }
    bool IsGPRZeroExtendElided(const ir::Inst* inst) const {
        return gpr_builder.Residences().Has(inst, GPRResidenceAction::ZeroExtend);
    }
    std::optional<FixedGPRValue> FixedGPRHomeForUse(ir::Value value,
                                                    const ir::Inst* consumer) const {
        return value.Defined() && consumer ? allocation.GPRUseHome(value.Id(), consumer->Id())
                                           : std::nullopt;
    }
    compile::PseudoFlagsDemand GetPseudoFlags(const ir::Inst* inst) const {
        return gpr_builder.UseFacts().PseudoFlags().For(inst);
    }
    bool MayFaultOrObserve(const ir::Inst& inst) const {
        return TargetMayFaultOrObserve(inst, features);
    }
    bool CanUseZeroStoreRegister(ir::Value value) const;

    AllocationPreparation& allocation;
    compile::CompileWorkspace& workspace;
    const Config& config;
    const FeatureSet features;
    const compile::GuestStateLayout& guest_layout;
    RegisterStatePreparation register_state;
    ResidencePreparation gpr_builder;
    const bool use_memory_base;
    const ir::Block* cur_block{};
    MemorySelectionBuilder memory_selections;
    std::vector<PublicationAction> publications;
};

}  // namespace swift::runtime::backend::arm64
