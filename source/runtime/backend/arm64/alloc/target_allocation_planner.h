#pragma once
#include "pinned_gpr_allocation.h"
#include "gpr_block_plan.h"
#include "target_analysis_traits.h"

namespace swift::runtime::ir { struct UniformInfo; }

namespace swift::runtime::backend::arm64 {
// A RegAlloc transaction owns analysis and constraints. No encoder, temporary
// register pool, labels or emission flags state are available to this component.
class ARMTargetPlanner final : DeleteCopyAndMove {
public:
    ARMTargetPlanner(RegAlloc& allocation, const Config& config, FeatureSet features,
                     const ir::UniformInfo& uniform_info)
        : allocation(allocation), config(config), features(features), uniform_info(uniform_info),
          gpr_builder(allocation, this->features, guest_state_map),
          use_memory_base(config.memory_base != nullptr || config.page_table != nullptr) {}
    void Complete(ir::Block* block, ir::HIRFunction* function, bool can_replay);

private:
    struct PendingGPRBlock {
        ir::Block* block;
        std::shared_ptr<GPRBlockPlan> actions;
        GPRPlanDraft draft;
        std::unique_ptr<BlockAnalysisIndex> analysis;
    };
    using MemoryUpdate = GPRMemoryUpdate;
    using PinnedLoadUpdate = GPRLoadUpdate;
    using PinnedMemoryAddress = GPRMemoryAddress;
    struct PinnedMemorySource { u16 target{}; u32 live_begin{}; };
    PendingGPRBlock DeclareGPRBlock(ir::Block* block);
    static void BindDeclaredGPRHomes(std::span<const PendingGPRBlock> blocks, RegAlloc& allocation,
                                     const FeatureSet& features);
    bool ReproveCoalescedHostWrite(ir::Inst* inst) const;
    void PrepareGPRPublicationPlan(ir::Block* block);
    std::optional<MemoryUpdate> MatchMemoryUpdate(ir::Inst* update) const;
    std::optional<MemoryUpdate> MatchPreIndexMemoryUpdate(ir::Inst* update) const;
    std::optional<MemoryUpdate> MatchBiasedMemoryUpdate(ir::Inst* update) const;
    void PrepareMemoryUpdates(ir::Block* block);
    std::optional<PinnedLoadUpdate> MatchPinnedLoadUpdate(ir::Inst* update);
    void PreparePinnedLoadUpdates(ir::Block* block);
    std::optional<PinnedMemorySource> MatchPinnedMemorySource(ir::Value source) const;
    std::optional<PinnedMemoryAddress> MatchPinnedMemoryAddress(ir::Inst* address) const;
    std::optional<u16> MatchPinnedMemoryValue(ir::Inst* extract) const;
    void PreparePinnedMemoryValues(ir::Block* block);
    void PreparePinnedMemoryAddresses(ir::Block* block);
    void PreparePinnedZeroExtendPublications(ir::Block* block);

    GPRBlockPlan& MutableGPRPlan() { ASSERT(allocation.TargetPlanning()); return *building_gpr_plan; }
    bool IsGPRMappedTo(ir::Value value, u16 home) const {
        return allocation.ValueType(value) == RegAlloc::GPR && allocation.ValueGPR(value).id == home;
    }
    bool IsSpilled(ir::Value value) const { return allocation.ValueType(value) == RegAlloc::MEM; }
    bool IsGPRZeroExtendElided(ir::Inst* inst) const { return gpr_builder.Facts().zero_extends.contains(inst); }
    std::optional<FixedGPRValue> FixedGPRHomeForUse(ir::Value value, const ir::Inst* consumer) const {
        return value.Defined() && consumer ? allocation.GPRUseHome(value.Id(), consumer->Id()) : std::nullopt;
    }
    PseudoFlagsDemand GetPseudoFlags(ir::Inst* inst) const { return gpr_plan->pseudo_flags.For(inst); }
    bool MayFaultOrObserve(const ir::Inst& inst) const { return TargetMayFaultOrObserve(inst, guest_state_map); }
    bool CanUseZeroStoreRegister(ir::Value value) const;

    RegAlloc& allocation;
    const Config& config;
    FeatureSet features;
    const ir::UniformInfo& uniform_info;
    GuestStateMap guest_state_map;
    GPRPlanBuilder gpr_builder;
    const bool use_memory_base;
    ir::Block* cur_block{};
    std::shared_ptr<GPRBlockPlan> building_gpr_plan;
    const GPRBlockPlan* gpr_plan{&EmptyGPRBlockPlan()};
};

void CompleteARM64TargetPlanning(const std::shared_ptr<Module>& module, RegAlloc& allocation,
                                ir::Block* block, ir::HIRFunction* function, bool can_replay);
// Caller preparation happens after the final IR/ordinary allocation choice and
// before creating an encoder. A completed stale unit is never replanned here.
[[nodiscard]] bool CompleteARM64UnitForEmission(const std::shared_ptr<Module>& module,
        RegAlloc& allocation, ir::Block* block, ir::HIRFunction* function);
// Cheap identity/attempt/options gate. Exact IR/CFG preflight remains separate.
[[nodiscard]] bool ARM64UnitReadyForEmission(const std::shared_ptr<Module>& module,
        const RegAlloc& allocation, ir::Block* block, ir::HIRFunction* function);
// Narrow standalone-HIR admission; never renumbers or mutates parent HIR.
[[nodiscard]] std::optional<u32> ARM64StandaloneHIRCapacity(const ir::Block& block);
} // namespace swift::runtime::backend::arm64
