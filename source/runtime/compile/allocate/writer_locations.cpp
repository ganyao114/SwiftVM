#include "runtime/compile/analysis/semantic_facts.h"
#include "writer.h"

#include "runtime/compile/allocate/controller.h"
#include "runtime/compile/allocate/transaction.h"

namespace swift::runtime::compile {

void AllocationWriter::SetAllocationUnit(const ir::Block* block, const ir::HIRFunction* function) {
    ASSERT(!locations_editing && !allocation_edit);
    // Reject a foreign sealed input before changing any physical state.
    semantic_facts.BeginInput(block, function);
    const bool previous_request = allocation_block || allocation_function;
    allocation_state.reset();
    result.gpr_locations_frozen = false;
    result.gpr_placements.clear();
    result.fixed_gpr_publications.clear();
    result.fixed_gpr_constraints_ready = false;
    result.gpr_preservation_actions.clear();
    result.gpr_use_homes.clear();
    InvalidateFixedGPRHomes();
    InvalidatePreservations();
    allocation_block = block;
    allocation_function = function;
    // A new request discards old mappings. Physical retries instead call
    // ResetAllocations directly, retaining the current fixed declarations.
    if (previous_request) ResetAllocations();
    input_pending = true;
    fpr_contract_retries = 0;
    fpr_coalescing_disabled = false;
}

void AllocationWriter::ClearElidedPublications() {
    for (u32 id : elided_publications) result.alloc_result[id].publication_elided = false;
    elided_publications.clear();
}

void AllocationWriter::ElidePublication(u32 id) {
    ASSERT(locations_editing && !GPRLocationsFrozen() && id < MapCount());
    if (result.alloc_result[id].publication_elided) return;
    elided_publications.push_back(id);
    result.alloc_result[id].publication_elided = true;
}

void AllocationWriter::CloseLocationEdits() {
    ASSERT(locations_editing && !GPRLocationsFrozen());
    location_edits_closed = true;
}

void AllocationWriter::BeginLocationEditing() {
    ASSERT(!allocation_edit && !locations_editing && !GPRLocationsFrozen());
    ClearElidedPublications();
    location_edits_closed = false;
    locations_editing = true;
    FlattenRefChains();
    BeginGPRLocationAnalysis();
}

void AllocationWriter::EndLocationEditing() {
    ASSERT(locations_editing);
    FreezeGPRLocations();
    locations_editing = false;
}

void AllocationWriter::AbortLocationEditing() noexcept {
    if (!locations_editing) return;
    locations_editing = false;
    for (auto& placement : result.gpr_placements) placement.fixed = {};
    result.fixed_gpr_publications.clear();
    result.fixed_gpr_constraints_ready = false;
    allocation_block = nullptr;
    allocation_function = nullptr;
    input_pending = false;
    allocation_state.reset();
    InvalidateFixedGPRHomes();
    InvalidatePreservations();
    ResetAllocations();
}

AllocationWriter::GPRPlacement& AllocationWriter::GPRPlacementFor(u32 id) {
    ASSERT_MSG(!result.gpr_locations_frozen, "GPR placement changed after plan freeze");
    ASSERT(id < result.alloc_result.size());
    if (allocation_edit) allocation_edit->RecordPlacement(id);
    if (result.gpr_placements.empty()) result.gpr_placements.resize(result.alloc_result.size());
    auto& placement = result.gpr_placements[id];
    if (placement.epoch != result.gpr_location_epoch) {
        const auto fixed = placement.fixed;
        placement = {};
        placement.fixed = fixed;
        placement.epoch = result.gpr_location_epoch;
    }
    return placement;
}

void AllocationWriter::PlanFixedGPRDefinition(u32 id, FixedGPRDefinition definition) {
    if (allocation_edit) {
        const auto* old = FixedGPRDefinitionAt(id);
        // Preserved prefixes also own capture actions; this edit only changes
        // non-preserved placements. A future residence edit must include those
        // actions explicitly rather than silently reusing this journal.
        ASSERT(!definition.preserved && (!old || !old->preserved));
        allocation_edit->RecordPublication(definition.publication);
        if (old) allocation_edit->RecordPublication(old->publication);
    }
    InvalidateFixedGPRHomes();
    const auto* old_definition = FixedGPRDefinitionAt(id);
    if (definition.preserved || (old_definition && old_definition->preserved))
        InvalidatePreservations();
    if (definition.home == UINT16_MAX) {
        // A completed width-coalescing transaction may turn a read view into
        // an actual capture/W write in its chosen fixed home. Its own proof
        // then replaces this earlier read-elision constraint.
        const auto* previous = FixedGPRDefinitionAt(id);
        ASSERT(previous && (!previous->elided || previous->publication == UINT32_MAX));
        if (previous->publication != UINT32_MAX)
            result.fixed_gpr_publications.erase(previous->publication);
        GPRPlacementFor(id).fixed = {};
        return;
    }
    ASSERT((definition.home < 32 && (fixed_gpr_homes & (1u << definition.home))) &&
           result.gprs.Get(definition.home));
    GPRPlacementFor(id).fixed = definition;
    if (definition.publication != UINT32_MAX) {
        result.fixed_gpr_publications.emplace(definition.publication, definition.owner);
    }
}

void AllocationWriter::BuildGPRPreservationActions() {
    ASSERT(!result.gpr_locations_frozen);
    result.gpr_preservation_actions.clear();
    for (u32 id = 0; id < result.gpr_placements.size(); ++id) {
        const auto& fixed = result.gpr_placements[id].fixed;
        if (!fixed.preserved) continue;
        const auto& mapping = result.alloc_result[ResolveId(id)];
        ASSERT(mapping.type == GPR || mapping.type == MEM);
        ASSERT(fixed.transfer >= id && fixed.transfer <= fixed.last_use);
        u16 scratch = UINT16_MAX;
        if (mapping.type == MEM && (fixed.width != 8 || fixed.bit_offset)) {
            if (const auto* reload = SpillReloadAt(id, fixed.transfer))
                scratch = reload->reg;
            else {
                const auto available =
                        result.alloc_result[fixed.transfer].dirty_gprs.GetFirstClear();
                ASSERT_MSG(available >= 0, "no register for declared GPR capture scratch");
                scratch = static_cast<u16>(available);
            }
        }
        result.gpr_preservation_actions[fixed.transfer].push_back({id,
                                                                   fixed.home,
                                                                   mapping.type,
                                                                   mapping.slot,
                                                                   fixed.width,
                                                                   fixed.bit_offset,
                                                                   scratch});
    }
}

void AllocationWriter::BeginGPRLocationAnalysis() {
    ASSERT(!allocation_edit);
    result.gpr_locations_frozen = false;
    ++result.gpr_location_epoch;
    ASSERT(result.gpr_location_epoch != 0);
    // Exact uses belong to the block being prepared. Range constraints above
    // are function-owned and survive this reset and allocation retries.
    result.gpr_use_homes.clear();
}

void AllocationWriter::FreezeGPRLocations() {
    ASSERT(!allocation_edit);
    ASSERT_MSG(!result.gpr_locations_frozen, "GPR locations already frozen");
    // Derived conflict indexes belong to preparation consumers. Freezing the
    // physical result does not require building an unused index.
    for (const auto& placement : result.gpr_placements) {
        if (placement.epoch != result.gpr_location_epoch) continue;
        const auto valid_home = [](u16 home) { return home == UINT16_MAX || home < 31; };
        ASSERT(valid_home(placement.result) && valid_home(placement.low_view) &&
               valid_home(placement.whole_value));
        ASSERT(placement.whole_value == UINT16_MAX || placement.begin <= placement.end);
    }
    for (const auto& [key, use] : result.gpr_use_homes) {
        const auto& value = use.value;
        ASSERT(use.epoch == result.gpr_location_epoch && value.home < 31);
        ASSERT(value.width == 1 || value.width == 2 || value.width == 4 || value.width == 8);
        ASSERT(value.extension.known_zero_above <= 64 &&
               value.extension.sign_extended_from <= value.extension.sign_extended_to &&
               value.extension.sign_extended_to <= 64);
    }
    FlattenRefChains();
    result.gpr_locations_frozen = true;
}

void AllocationWriter::AssignGPRValueHome(u32 id, u16 home) {
    auto& placement = GPRPlacementFor(id);
    if (placement.result == UINT16_MAX) placement.result = home;
    if (placement.whole_value == UINT16_MAX) {
        placement.whole_value = home;
        placement.begin = id;
    }
}

void AllocationWriter::AssignGPRTransferredHome(u32 id, u16 home, u32 begin, u32 end) {
    auto& placement = GPRPlacementFor(id);
    if (placement.whole_value == UINT16_MAX) {
        placement.whole_value = home;
        placement.begin = begin;
        placement.end = end;
    }
}

void AllocationWriter::AssignGPRLowView(u32 id, u16 home) {
    // Preservation ends the architectural residence at transfer. A global low
    // view of that home cannot describe the displaced SSA suffix; it used to
    // make target binding extend the prefix and discard the capture.
    const auto* preserved = PreservedGPRDefinitionAt(id);
    if (preserved && preserved->home == home) return;
    auto& placement = GPRPlacementFor(id);
    if (placement.low_view == UINT16_MAX) placement.low_view = home;
}

void AllocationWriter::RegisterGPRUseHome(u32 id,
                                          u32 consumer,
                                          FixedGPRValue value,
                                          bool selected) {
    const auto* preserved = PreservedGPRDefinitionAt(id);
    if (preserved && consumer >= preserved->transfer && value.home == preserved->home) return;
    GPRPlacementFor(id).has_use_homes = true;
    const u64 key = (u64{id} << 32) | consumer;
    const auto found = result.gpr_use_homes.find(key);
    if (selected && found == result.gpr_use_homes.end()) ++GPRPlacementFor(id).additional_uses;
    if (found == result.gpr_use_homes.end() || selected || !found->second.selected) {
        if (allocation_edit) allocation_edit->RecordUse(key);
        result.gpr_use_homes.insert_or_assign(key,
                                              GPRUse{value, result.gpr_location_epoch, selected});
    }
}

}  // namespace swift::runtime::compile
