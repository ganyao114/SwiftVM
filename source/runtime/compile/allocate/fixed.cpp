#include "internal.h"

namespace swift::runtime::compile::detail {
bool LinearScan::MapPreallocatedGPR(u32 id, bool single_home) {
    const auto* fixed = reg_alloc->FixedGPRDefinitionAt(id);
    if (!fixed) return false;
    if (fixed->preserved) return false;
    // Single-home views can be zero-cost spill victims in the existing
    // lowering. Decide whether to remove their intervals only after peak
    // value-class pressure is known, before any assignment or eviction.
    if (!single_home && !fixed->version && (!fixed->elided || fixed->publication == UINT32_MAX))
        return false;
    MapFixedRead(id, fixed->home);
    fixed_gpr_alias_end[id] = fixed->end;
    reg_alloc->SetLiveEnd(id, fixed->last_use);
    // Fixed publications are a live-binding mechanism: with live
    // semantics off, the planner only reproves a narrow last-use
    // subset, and chains outside it (e.g. a ZeroExtend of an address
    // computation) abort the seal.  Leave such stores unmarked so
    // they take the explicit Value publication path.
    if (!fixed->elided && fixed->publication != UINT32_MAX && features.ra_coalesce_live)
        reg_alloc->MarkHostWriteCoalesced(fixed->publication);
    return true;
}

void LinearScan::FinalizeSingleHomePreallocation() {
    bool candidates = false;
    for (const auto& interval : live_interval) {
        const auto* fixed = reg_alloc->FixedGPRDefinitionAt(interval.inst->Id());
        candidates |=
                fixed && !fixed->preserved && (!fixed->elided || fixed->publication == UINT32_MAX);
    }
    if (!candidates) return;
    const u32 count = InstrCount();
    Vector<s32> changes(count + 1);
    for (const auto& interval : live_interval) {
        if (IsFloatValue(interval.inst)) continue;
        ++changes[interval.start];
        --changes[std::min<u32>(interval.end + 1, count)];
    }
    s32 live = 0, peak = 0;
    for (auto change : changes) {
        live += change;
        peak = std::max(peak, live);
    }
    auto pool = reg_alloc->GetGprs();
    const u32 available = pool.GetClearCount();
    const u32 capacity = available > gpr_reserve ? available - gpr_reserve : 0;
    if (static_cast<u32>(peak) > capacity) {
        // Under pressure, removing cheap captures can perturb spill
        // eviction and destroy useful pending-write forwarding. Preserve
        // those existing choices until spill cost models target elision.
        for (const auto& interval : live_interval) {
            const auto* fixed = reg_alloc->FixedGPRDefinitionAt(interval.inst->Id());
            if (fixed && !fixed->preserved && (!fixed->elided || fixed->publication == UINT32_MAX))
                reg_alloc->PlanFixedGPRDefinition(interval.inst->Id(), {});
        }
        return;
    }
    std::erase_if(live_interval, [&](const auto& interval) {
        const auto* fixed = reg_alloc->FixedGPRDefinitionAt(interval.inst->Id());
        if (!fixed || fixed->preserved || (fixed->elided && fixed->publication != UINT32_MAX))
            return false;
        return MapPreallocatedGPR(interval.inst->Id(), true);
    });
}

void LinearScan::FinalizeFixedGPRReadConstraints() {
    auto finalize = [&](const Block* lir_block) {
        for (auto& inst : lir_block->GetInstList()) {
            const auto* fixed = reg_alloc->FixedGPRDefinitionAt(inst.Id());
            if (fixed && fixed->preserved && fixed->transfer == inst.Id() &&
                fixed->width <= sizeof(u32) &&
                reg_alloc->ValueType(Value{&inst}) == compile::AllocationWriter::GPR &&
                reg_alloc->ValueGPR(Value{&inst}).id == fixed->home &&
                reg_alloc->IsHostReadCoalesced(inst.Id())) {
                auto residence = *fixed;
                residence.preserved = false;
                residence.end = residence.last_use;
                residence.transfer = UINT32_MAX;
                reg_alloc->PlanFixedGPRDefinition(inst.Id(), residence);
                continue;
            }
            if (!fixed || fixed->preserved || !fixed->elided || fixed->publication != UINT32_MAX ||
                fixed->width > sizeof(u32))
                continue;
            ASSERT(reg_alloc->ValueType(Value{&inst}) == compile::AllocationWriter::GPR);
            if (reg_alloc->IsWidthChainCoalesced(inst.Id()) ||
                reg_alloc->ValueGPR(Value{&inst}).id != fixed->home) {
                // Wait until every ownership transaction has committed or
                // rolled back. No target location is queried during RA.
                reg_alloc->PlanFixedGPRDefinition(inst.Id(), {});
            }
        }
    };
    if (function) {
        for (auto& hir_block : function->GetHIRBlocksRPO()) finalize(hir_block.GetBlock());
    } else {
        finalize(block);
    }
}

bool LinearScan::FixedConstraintConflict(const Block* lir_block,
                                         u16 target,
                                         u32 begin,
                                         u32 end) const {
    for (auto& inst : lir_block->GetInstList()) {
        if (reg_alloc->HasFixedGPRDefinition(inst.Id()) &&
            reg_alloc->FixedGPRHomeIntersects(Value{&inst}, target, begin, end, inst.Id()))
            return true;
    }
    return false;
}

void LinearScan::PrepareFixedPreferences() { FinalizeSingleHomePreallocation(); }

bool LinearScan::MapFixedInput(u32 id, u32 end, u32 local_end, u32 owner_end) {
    // Final numbered values are visited in ascending order. Advancing one
    // cursor avoids a sparse lookup for every ordinary instruction.
    while (fixed_read_cursor < inputs.fixed_reads.size() &&
           inputs.fixed_reads[fixed_read_cursor].value < id)
        ++fixed_read_cursor;
    if (fixed_read_cursor == inputs.fixed_reads.size() ||
        inputs.fixed_reads[fixed_read_cursor].value != id)
        return false;
    const auto& read = inputs.fixed_reads[fixed_read_cursor];
    if (read.reg_class == RegisterClass::FPR) {
        if (reg_alloc->FPRCoalescingDisabled() || end > owner_end ||
            value_liveness.FPRWrites().Crosses(read.home, id, end))
            return false;
        reg_alloc->MapRegister(id, HostFPR{read.home});
        reg_alloc->MarkHostReadCoalesced(id);
        if (scalar_insert && read.track_use_end) fixed_fpr_read_ends.emplace(id, local_end);
        return true;
    }
    if (reg_alloc->PreservedGPRDefinitionAt(id) ||
        value_liveness.GPRWrites().Crosses(read.home, id, end))
        return false;
    MapFixedRead(id, read.home);
    fixed_gpr_alias_end[id] = end;
    return true;
}
bool LinearScan::CoalescingValueUsesStayInBlock(const Block* lir_block, Value value) const {
    auto may_coalesce = [&](u32 id) {
        const auto* fixed = reg_alloc->FixedGPRDefinitionAt(id);
        if (fixed && fixed->preserved && (fixed->transfer != id || fixed->width > sizeof(u32)))
            return false;
        // A single-use result can only coalesce into its own publication
        // home. Narrow reads can be adopted by a later width transaction.
        // A two-segment transfer must retain its point-specific locations.
        return !fixed || !fixed->elided ||
               (fixed->publication == UINT32_MAX && fixed->width <= sizeof(u32));
    };
    return may_coalesce(value.Id()) && may_coalesce(ResolveBitCastSource(value).Id()) &&
           ValueUsesStayInBlock(lir_block, value);
}

void LinearScan::ReuseFixedPublications() {
    if (!inputs.gpr || !inputs.PolicyFor(reg_alloc->GetGprs()).fixed_publications) return;
    for (const auto& candidate : inputs.gpr->publications) {
        if (reg_alloc->HasFixedGPRDefinition(candidate.value) ||
            FixedConstraintConflict(
                    candidate.block, candidate.home, candidate.value, candidate.publication))
            continue;
        reg_alloc->MapRegister(candidate.value, HostGPR{candidate.home});
        if (candidate.source != UINT16_MAX && !reg_alloc->HasFixedGPRDefinition(candidate.source) &&
            !FixedConstraintConflict(
                    candidate.block, candidate.home, candidate.source, candidate.publication))
            reg_alloc->MapRegister(candidate.source, HostGPR{candidate.home});
    }
}

}  // namespace swift::runtime::compile::detail
