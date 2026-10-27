#pragma once

#include "home_index.h"
#include "preservation_index.h"

#include <array>
#include <memory>
#include <vector>

#include "runtime/compile/allocate/allocation.h"

namespace swift::runtime::compile {
struct AllocationState;
struct FixedValidationInputs;
class SemanticFacts;
class CompileWorkspace;

// Borrows one physical result for its mutable preparation lifetime. Target
// policies provide pools/constraints; this writer owns no target actions.
class AllocationWriter : public AllocationView {
public:
    AllocationWriter(Allocation& result,
                     SemanticFacts& facts,
                     CompileWorkspace& workspace,
                     u32 fixed_gpr_homes = 0);
    ~AllocationWriter();
    AllocationWriter(const AllocationWriter&) = delete;
    AllocationWriter& operator=(const AllocationWriter&) = delete;
    AllocationWriter(AllocationWriter&&) = delete;
    AllocationWriter& operator=(AllocationWriter&&) = delete;
    [[nodiscard]] SemanticFacts& PrepareFacts();
    [[nodiscard]] SemanticFacts& PrepareFacts(const ir::Block* block);
    [[nodiscard]] CompileWorkspace& Workspace() { return workspace; }
    void SetAllocationUnit(const ir::Block* block, const ir::HIRFunction* function);
    [[nodiscard]] const ir::Block* InputBlock() const { return allocation_block; }
    [[nodiscard]] const ir::HIRFunction* InputFunction() const { return allocation_function; }
    [[nodiscard]] bool HasPendingInput() const { return input_pending; }
    [[nodiscard]] const FixedValidationInputs* PreparedFixedValidation(const FeatureSet&,
                                                                       u64 contract_key) const;
    [[nodiscard]] bool LocationsEditing() const { return locations_editing; }
    [[nodiscard]] bool LocationEditsClosed() const { return location_edits_closed; }
    [[nodiscard]] u32 FPRContractRetryCount() const { return fpr_contract_retries; }
    [[nodiscard]] bool FPRCoalescingDisabled() const { return fpr_coalescing_disabled; }
    void DisableFPRCoalescing() {
        ASSERT(!result.gpr_locations_frozen);
        fpr_coalescing_disabled = true;
    }
    void PlanFixedGPRDefinition(u32 value_id, FixedGPRDefinition definition);
    [[nodiscard]] const GPRPreservationIndex& PrepareGPRPreservations() {
        gpr_preservations.Prepare();
        return gpr_preservations;
    }
    [[nodiscard]] const GPRHomeIndex& PrepareFixedGPRHomes() {
        gpr_home_index.Prepare();
        return gpr_home_index;
    }
    [[nodiscard]] const GPRPreservationIndex* FindGPRPreservations() const {
        return gpr_preservations.Ready() ? &gpr_preservations : nullptr;
    }
    [[nodiscard]] const GPRHomeIndex* FindFixedGPRHomes() const {
        return gpr_home_index.Ready() ? &gpr_home_index : nullptr;
    }
    void BuildGPRPreservationActions();
    void FinishFixedGPRConstraints() {
        ASSERT(!result.gpr_locations_frozen);
        result.fixed_gpr_constraints_ready = true;
    }
    void BeginGPRLocationAnalysis();
    void FreezeGPRLocations();
    void AssignGPRValueHome(u32 value_id, u16 home);
    void AssignGPRTransferredHome(u32 value_id, u16 home, u32 begin, u32 end);
    void AssignGPRLowView(u32 value_id, u16 home);
    void RegisterGPRUseHome(u32 value_id, u32 consumer, FixedGPRValue value, bool selected);
    void DeclareGuestGPR(u16 home) {
        ASSERT(!result.gpr_locations_frozen && home < 31);
        result.gprs.Mark(home);
    }
    void MapRegister(u32 id, ir::HostGPR gpr);
    void MapRegister(u32 id, ir::HostFPR fpr);
    void MapFixedRegister(u32 id, ir::HostGPR gpr);
    void MapMemSpill(u32 id, ir::SpillSlot slot);
    void MapSpillReload(u32 value_id,
                        u32 first_use,
                        u32 last_use,
                        ir::HostGPR reg,
                        bool owns_all_uses = false,
                        bool fault_backed = false);
    void MapReference(u32 from, u32 to);
    void MarkHostWriteCoalesced(u32 id);
    void MarkHostReadCoalesced(u32 id);
    void MarkWidthChainCoalesced(u32 id, u32 anchor_id);
    void MarkLow32CopyCoalesced(u32 id, u32 source_id);
    bool FreezeWidthComponentOwner(u32 anchor_id, u16 target, bool high_zero);
    bool CommitWidthComponentOwner(u32 anchor_id, u16 target);
    void MarkAesChainTied(u32 id, u16 target);
    void MarkPshufdDirect(u32 id);
    void SetLiveEnd(u32 id, u32 end);
    void SetActiveRegs(u32 id, GPRSMask gprs, FPRSMask fprs);
    void PermuteGPRHomes(u16 left, u16 right);
    void ResetAllocations();
    using AllocationView::RAShape;
    [[nodiscard]] RAShapeUnitCounters& RAShape() { return result.ra_shape; }

protected:
    void ReserveFPR(u16 home);
    void BeginLocationEditing();
    void EndLocationEditing();
    void AbortLocationEditing() noexcept;
    void CloseLocationEdits();
    void ElidePublication(u32 id);

private:
    friend class AllocationEdit;
    friend class AllocationController;
    Allocation& result;
    const u32 fixed_gpr_homes;
    void ClearElidedPublications();
    Vector<u32> elided_publications;
    using GPRPlacement = Allocation::GPRPlacement;
    using GPRUse = Allocation::GPRUse;
    AllocationEdit* allocation_edit{};
    void FlattenRefChains();
    GPRPlacement& GPRPlacementFor(u32 value_id);
    const ir::Block* allocation_block{};
    const ir::HIRFunction* allocation_function{};
    std::unique_ptr<AllocationState> allocation_state;
    SemanticFacts& semantic_facts;
    CompileWorkspace& workspace;
    bool locations_editing{};
    bool location_edits_closed{};
    bool input_pending{};
    u32 fpr_contract_retries{};
    bool fpr_coalescing_disabled{};
    GPRPreservationIndex gpr_preservations;
    void InvalidatePreservations() { gpr_preservations.Invalidate(); }
    GPRHomeIndex gpr_home_index;
    void InvalidateFixedGPRHomes() { gpr_home_index.Invalidate(); }
};

}  // namespace swift::runtime::compile
