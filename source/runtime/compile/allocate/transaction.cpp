#include "transaction.h"

namespace swift::runtime::compile {

AllocationEdit::AllocationEdit(AllocationWriter& allocation) : AllocationEdit(allocation, 0, 0) {}

AllocationEdit::AllocationEdit(AllocationWriter& allocation, u32 local_begin, u32 local_limit)
        : allocation(allocation), local_begin(local_begin), local_limit(local_limit) {
    ASSERT(!allocation.result.gpr_locations_frozen && !allocation.allocation_edit);
    ASSERT(local_begin <= local_limit && local_limit <= allocation.MapCount());
    allocation.allocation_edit = this;
}

AllocationEdit::~AllocationEdit() {
    if (active) Rollback();
}

void AllocationEdit::RecordValue(u32 id) {
    ASSERT(active && id < allocation.result.alloc_result.size());
    if (OriginalValue(id)) return;
    const auto slot = static_cast<u32>(values.size());
    values.push_back({id,
                      allocation.result.alloc_result[id],
                      allocation.result.width_component_owners[id],
                      allocation.result.width_chain_anchors[id],
                      allocation.result.coalesced_host_writes[id],
                      allocation.result.coalesced_host_reads[id]});
    if (local_begin <= id && id < local_limit) {
        if (local_slots.empty()) local_slots.assign(local_limit - local_begin, UINT32_MAX);
        local_slots[id - local_begin] = slot;
    } else
        external_slots.emplace(id, slot);
}

const AllocationEdit::ValueState* AllocationEdit::OriginalValue(u32 id) const {
    if (local_begin <= id && id < local_limit) {
        if (local_slots.empty() || local_slots[id - local_begin] == UINT32_MAX) return nullptr;
        return &values[local_slots[id - local_begin]];
    }
    const auto found = external_slots.find(id);
    return found == external_slots.end() ? nullptr : &values[found->second];
}

void AllocationEdit::RecordPlacement(u32 id) {
    ASSERT(active && id < allocation.result.alloc_result.size());
    if (placements.contains(id)) return;
    placements.emplace(id,
                       id < allocation.result.gpr_placements.size()
                               ? allocation.result.gpr_placements[id]
                               : AllocationWriter::GPRPlacement{});
}

void AllocationEdit::RecordPublication(u32 instruction) {
    ASSERT(active);
    if (instruction == UINT32_MAX || publications.contains(instruction)) return;
    const auto found = allocation.result.fixed_gpr_publications.find(instruction);
    publications.emplace(instruction,
                         found == allocation.result.fixed_gpr_publications.end()
                                 ? std::nullopt
                                 : std::optional<u32>{found->second});
}

void AllocationEdit::RecordUse(u64 key) {
    ASSERT(active);
    if (uses.contains(key)) return;
    const auto found = allocation.result.gpr_use_homes.find(key);
    uses.emplace(key,
                 found == allocation.result.gpr_use_homes.end()
                         ? std::nullopt
                         : std::optional<AllocationWriter::GPRUse>{found->second});
}

void AllocationEdit::RecordReloads(u32 instruction) {
    if (!reloads.contains(instruction))
        reloads.emplace(instruction, allocation.result.spill_reloads[instruction]);
}

bool AllocationEdit::OriginalHostWrite(u32 id) const {
    ASSERT(active && id < allocation.result.alloc_result.size());
    const auto* original = OriginalValue(id);
    return original ? original->host_write : allocation.result.coalesced_host_writes[id];
}

u32 AllocationEdit::OriginalWidthAnchor(u32 id) const {
    ASSERT(active && id < allocation.result.alloc_result.size());
    const auto* original = OriginalValue(id);
    return original ? original->width_anchor : allocation.result.width_chain_anchors[id];
}

void AllocationEdit::Commit() {
    ASSERT(active && allocation.allocation_edit == this);
    allocation.allocation_edit = nullptr;
    if (!values.empty() || !placements.empty() || !publications.empty() || !uses.empty() ||
        !reloads.empty())
        ++allocation.result.allocation_attempt;
    active = false;
}

void AllocationEdit::Rollback() {
    ASSERT(active && allocation.allocation_edit == this);
    allocation.allocation_edit = nullptr;
    for (const auto& state : values) {
        const auto id = state.id;
        allocation.result.alloc_result[id] = state.mapping;
        allocation.result.width_component_owners[id] = state.owner;
        allocation.result.width_chain_anchors[id] = state.width_anchor;
        allocation.result.coalesced_host_writes[id] = state.host_write;
        allocation.result.coalesced_host_reads[id] = state.host_read;
    }
    for (const auto& [id, state] : placements) allocation.result.gpr_placements[id] = state;
    for (const auto& [instruction, owner] : publications) {
        if (owner)
            allocation.result.fixed_gpr_publications[instruction] = *owner;
        else
            allocation.result.fixed_gpr_publications.erase(instruction);
    }
    for (const auto& [key, use] : uses) {
        if (use)
            allocation.result.gpr_use_homes[key] = *use;
        else
            allocation.result.gpr_use_homes.erase(key);
    }
    for (auto& [instruction, saved] : reloads)
        allocation.result.spill_reloads[instruction] = std::move(saved);
    if (!values.empty() || !placements.empty() || !reloads.empty()) {
        allocation.InvalidateFixedGPRHomes();
        allocation.InvalidatePreservations();
    }
    active = false;
}

}  // namespace swift::runtime::compile
