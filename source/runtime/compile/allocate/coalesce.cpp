#include "internal.h"
#include "transaction.h"

namespace swift::runtime::compile::detail {
void LinearScan::FinishCoalescing() {
    if (features.ra_width_chain) {
        CoalesceGuestRegisterAccessesAndWidthChains();
    } else {
        CoalesceGuestRegisterAccesses();
        CoalesceWidthChains();
    }
    CoalesceLow32Copies();
    CoalesceFPRPublications(xmm_resident);
    FinalizeFixedGPRReadConstraints();
}
void LinearScan::CoalesceGuestRegisterAccesses() {
    if (!inputs.PolicyFor(reg_alloc->GetGprs()).gpr_coalescing) {
        return;
    }

    auto coalesce_block = [&](const Block* lir_block) {
        CoalesceGuestGPRWrites(lir_block, features.ra_width_chain);
        CoalescePinnedWViewInputs(lir_block);
        CoalesceGuestGPRReads(lir_block);
    };

    if (function) {
        for (auto* hir_block : function->GetHIRBlocks()) {
            coalesce_block(hir_block->GetBlock());
        }
    } else {
        coalesce_block(block);
    }
}

void LinearScan::CoalesceGuestRegisterAccessesAndWidthChains() {
    const bool host_accesses = inputs.PolicyFor(reg_alloc->GetGprs()).gpr_coalescing;
    auto transact_block = [&](const Block* lir_block) {
        // Establish the exact gate-OFF result first.  Width components are
        // an extension of the already-shipped W-alpha/LONG decisions, not
        // competitors for the same fixed home: choosing a new component
        // must never evict a write/read tie that the baseline already
        // proved.  The staged v2 transaction therefore starts from this
        // immutable baseline and rolls back to it, rather than to the raw
        // linear-scan allocation.
        if (host_accesses) {
            CoalesceGuestGPRWrites(lir_block, false);
            CoalescePinnedWViewInputs(lir_block);
            CoalesceGuestGPRReads(lir_block);
        }
        auto baseline_long = CollectLongWidthChainBridges(lir_block);
        CoalesceWidthChainBridges(lir_block, false, baseline_long);
        const auto& instructions = lir_block->GetInstList();
        compile::AllocationEdit edit{
                *reg_alloc,
                instructions.empty() ? 0u : static_cast<u32>(instructions.front().Id()),
                instructions.empty() ? 0u : static_cast<u32>(instructions.back().Id()) + 1};

        // Freeze every multi-publication owner before either half can
        // alter a baseline mapping.  All subsequent decisions run against
        // one staged allocation and become visible only if the final owner
        // validation succeeds.
        RecipeWidthComponentOwners(lir_block);
        if (host_accesses) {
            CoalesceGuestGPRWrites(lir_block, features.ra_width_chain);
            CoalescePinnedWViewInputs(lir_block);
        }

        auto long_bridge = CollectLongWidthChainBridges(lir_block);
        CoalesceWidthChainBridges(lir_block, features.ra_width_chain, long_bridge);
        u32 new_zero_emission_sites = 0;
        for (auto& inst : lir_block->GetInstList()) {
            if (reg_alloc->IsHostWriteCoalesced(inst.Id()) && !edit.OriginalHostWrite(inst.Id())) {
                ++new_zero_emission_sites;
            }
            if (reg_alloc->IsWidthChainCoalesced(inst.Id()) &&
                edit.OriginalWidthAnchor(inst.Id()) == UINT32_MAX && inputs.gpr &&
                (inputs.gpr->ConsumerFlags(inst.Id()) & GPRInputs::WidthEmissionSite)) {
                ++new_zero_emission_sites;
            }
        }
        // In a region, v2 exists for the multi-node matrix/CRC components,
        // not the short scalar chains that made the original general gate
        // move otherwise unrelated STREAM units.  An audit over the
        // complete STREAM image found a maximum of nine new zero-emission
        // sites in any LIR block, while the seven CoreMark matrix blocks
        // have 11..17.  Single-block compilation keeps the proof-complete
        // small components covered by the focused allocator/emitter tests;
        // they cannot perturb a neighbouring region member's layout.
        // Keep either decision transactional: a rejected component
        // contributes no partial owner, mapping or active-mask state.
        const u32 min_width_component_sites =
                inputs.PolicyFor(reg_alloc->GetGprs()).width_component_sites;
        if (new_zero_emission_sites < min_width_component_sites ||
            !ValidateWidthComponentTransaction(lir_block)) {
            edit.Rollback();
        } else
            edit.Commit();
    };

    if (function) {
        for (auto* hir_block : function->GetHIRBlocks()) {
            transact_block(hir_block->GetBlock());
        }
    } else {
        transact_block(block);
    }
}
}  // namespace swift::runtime::compile::detail
