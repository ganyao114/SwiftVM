#include "runtime/compile/workspace.h"
#include "writer.h"

#include <algorithm>

#include "runtime/compile/allocate/controller.h"
#include "runtime/compile/allocate/transaction.h"
#include "runtime/compile/analysis/semantic_facts.h"

namespace swift::runtime::compile {
const FixedValidationInputs* AllocationWriter::PreparedFixedValidation(const FeatureSet& features,
                                                                       u64 contract_key) const {
    const auto* data = allocation_state ? allocation_state->inputs.fixed_validation.get() : nullptr;
    return data && allocation_state->inputs.features == features &&
                           data->contract_key == contract_key
                   ? data
                   : nullptr;
}

SemanticFacts& AllocationWriter::PrepareFacts() {
    ASSERT(semantic_facts.HasInput());
    return semantic_facts;
}

SemanticFacts& AllocationWriter::PrepareFacts(const ir::Block* block) {
    if (!semantic_facts.HasInput()) semantic_facts.BeginInput(block, nullptr);
    ASSERT(semantic_facts.Contains(block));
    return semantic_facts;
}

AllocationWriter::AllocationWriter(Allocation& result,
                                   SemanticFacts& facts,
                                   CompileWorkspace& workspace,
                                   u32 fixed_gpr_homes)
        : AllocationView(result.View())
        , result(result)
        , fixed_gpr_homes(fixed_gpr_homes)
        , semantic_facts(facts)
        , workspace(workspace)
        , gpr_preservations(result)
        , gpr_home_index(result) {
    ASSERT(!result.writer_active && !result.View().GPRLocationsFrozen() && !facts.writer_active);
    workspace.AcquireAllocation();
    result.writer_active = true;
    facts.writer_active = true;
}

AllocationWriter::~AllocationWriter() {
    ASSERT(result.writer_active && semantic_facts.writer_active && !allocation_edit);
    workspace.ReleaseAllocation();
    semantic_facts.writer_active = false;
    result.writer_active = false;
}

void AllocationWriter::ReserveFPR(u16 home) {
    ASSERT(!result.gpr_locations_frozen && home < 32);
    result.fprs.Mark(home);
}

void AllocationWriter::ResetAllocations() {
    ASSERT(!allocation_edit);
    location_edits_closed = false;
    elided_publications.clear();
    ++result.allocation_attempt;
    result.gpr_preservation_actions.clear();
    BeginGPRLocationAnalysis();
    std::fill(result.alloc_result.begin(), result.alloc_result.end(), Map{});
    std::fill(result.live_ends.begin(), result.live_ends.end(), UINT32_MAX);
    result.spill_reloads.clear();
    result.spill_reload_region_count = 0;
    std::fill(result.coalesced_host_writes.begin(), result.coalesced_host_writes.end(), false);
    std::fill(result.coalesced_host_reads.begin(), result.coalesced_host_reads.end(), false);
    std::fill(result.width_chain_anchors.begin(), result.width_chain_anchors.end(), UINT32_MAX);
    std::fill(result.width_component_owners.begin(),
              result.width_component_owners.end(),
              WidthComponentOwner{});
    std::fill(result.low32_copy_sources.begin(), result.low32_copy_sources.end(), UINT32_MAX);
    std::fill(result.aes_chain_targets.begin(), result.aes_chain_targets.end(), UINT16_MAX);
    result.pshufd_direct.clear();
}

void AllocationWriter::MapRegister(u32 id, ir::HostFPR fpr) {
    ASSERT_MSG(!result.gpr_locations_frozen, "allocation changed after plan freeze");
    auto& map = result.alloc_result[id];
    if (allocation_edit && (map.type != FPR || map.slot != fpr.id || map.fixed_gpr))
        allocation_edit->RecordValue(id);
    map.type = FPR;
    map.slot = fpr.id;
    map.fixed_gpr = false;
}

void AllocationWriter::MapRegister(u32 id, ir::HostGPR gpr) {
    ASSERT_MSG(!result.gpr_locations_frozen, "allocation changed after plan freeze");
    if (allocation_state && !locations_editing) {
        if (const auto* fixed = FixedGPRDefinitionAt(id);
            fixed && !fixed->preserved && fixed->home != gpr.id)
            PlanFixedGPRDefinition(id, {});
    }
    auto& map = result.alloc_result[id];
    if (allocation_edit && (map.type != GPR || map.slot != gpr.id || map.fixed_gpr))
        allocation_edit->RecordValue(id);
    map.type = GPR;
    map.slot = gpr.id;
    map.fixed_gpr = false;
}

void AllocationWriter::MapFixedRegister(u32 id, ir::HostGPR gpr) {
    ASSERT_MSG(!result.gpr_locations_frozen, "allocation changed after plan freeze");
    ASSERT((gpr.id < 32 && (fixed_gpr_homes & (1u << gpr.id))));
    auto& map = result.alloc_result[id];
    if (allocation_edit && (map.type != GPR || map.slot != gpr.id || !map.fixed_gpr))
        allocation_edit->RecordValue(id);
    map.type = GPR;
    map.slot = gpr.id;
    map.fixed_gpr = true;
}

void AllocationWriter::MapMemSpill(u32 id, ir::SpillSlot slot) {
    ASSERT(!allocation_edit);
    ASSERT_MSG(!result.gpr_locations_frozen, "allocation changed after plan freeze");
    if (allocation_state && !locations_editing) {
        if (const auto* fixed = FixedGPRDefinitionAt(id); fixed && !fixed->preserved)
            PlanFixedGPRDefinition(id, {});
    }
    auto& map = result.alloc_result[id];
    map.type = MEM;
    map.slot = slot.offset;
    map.fixed_gpr = false;
}

void AllocationWriter::MapSpillReload(u32 value_id,
                                      u32 first_use,
                                      u32 last_use,
                                      ir::HostGPR reg,
                                      bool owns_all_uses,
                                      bool fault_backed) {
    ASSERT_MSG(!result.gpr_locations_frozen, "spill reload changed after plan freeze");
    ASSERT(!allocation_edit);
    value_id = ResolveId(value_id);
    ASSERT(result.alloc_result[value_id].type == MEM);
    ASSERT(first_use <= last_use);
    if (result.spill_reloads.size() < result.alloc_result.size()) {
        result.spill_reloads.resize(result.alloc_result.size());
    }
    const u32 region = result.spill_reload_region_count++;
    for (u32 id = first_use; id <= last_use; ++id) {
        ASSERT(id < result.alloc_result.size());
        auto& reloads = result.spill_reloads[id];
        ASSERT(std::none_of(reloads.begin(), reloads.end(), [value_id](const auto& reload) {
            return reload.value == value_id;
        }));
        reloads.push_back(SpillReload{
                .region = region,
                .value = value_id,
                .reg = reg.id,
                .owns_all_uses = owns_all_uses,
                .fault_backed = fault_backed,
        });
        result.alloc_result[id].dirty_gprs.Mark(reg.id);
    }
}

void AllocationWriter::MapReference(u32 from, u32 to) {
    ASSERT_MSG(!result.gpr_locations_frozen, "allocation changed after plan freeze");
    auto& map = result.alloc_result[to];
    if (allocation_edit && (map.type != REF || map.slot != from || map.fixed_gpr))
        allocation_edit->RecordValue(to);
    map.type = REF;
    map.slot = from;
    map.fixed_gpr = false;
}

void AllocationWriter::MarkHostWriteCoalesced(u32 id) {
    ASSERT(!result.gpr_locations_frozen);
    ASSERT(id < result.coalesced_host_writes.size());
    if (allocation_edit && !result.coalesced_host_writes[id]) allocation_edit->RecordValue(id);
    result.coalesced_host_writes[id] = true;
}

void AllocationWriter::MarkHostReadCoalesced(u32 id) {
    ASSERT(!result.gpr_locations_frozen);
    ASSERT(id < result.coalesced_host_reads.size());
    if (allocation_edit && !result.coalesced_host_reads[id]) allocation_edit->RecordValue(id);
    result.coalesced_host_reads[id] = true;
}

void AllocationWriter::MarkWidthChainCoalesced(u32 id, u32 anchor_id) {
    ASSERT(!result.gpr_locations_frozen);
    ASSERT(id < result.width_chain_anchors.size());
    if (allocation_edit && result.width_chain_anchors[id] != anchor_id)
        allocation_edit->RecordValue(id);
    ASSERT(anchor_id < result.width_chain_anchors.size());
    result.width_chain_anchors[id] = anchor_id;
}

void AllocationWriter::MarkLow32CopyCoalesced(u32 id, u32 source_id) {
    ASSERT(!allocation_edit);
    ASSERT(!result.gpr_locations_frozen);
    ASSERT(id < result.low32_copy_sources.size());
    ASSERT(source_id < result.low32_copy_sources.size());
    result.low32_copy_sources[id] = source_id;
}

bool AllocationWriter::FreezeWidthComponentOwner(u32 anchor_id, u16 target, bool high_zero) {
    ASSERT(!result.gpr_locations_frozen);
    ASSERT(anchor_id < result.width_component_owners.size());
    auto& owner = result.width_component_owners[anchor_id];
    if (owner.Valid()) {
        return owner.target == target && owner.high_zero == high_zero;
    }
    if (allocation_edit) allocation_edit->RecordValue(anchor_id);
    owner.target = target;
    owner.high_zero = high_zero;
    return true;
}

bool AllocationWriter::CommitWidthComponentOwner(u32 anchor_id, u16 target) {
    ASSERT(!result.gpr_locations_frozen);
    ASSERT(anchor_id < result.width_component_owners.size());
    auto& owner = result.width_component_owners[anchor_id];
    if (!owner.Valid() || owner.target != target) {
        return false;
    }
    if (allocation_edit && !owner.committed) allocation_edit->RecordValue(anchor_id);
    owner.committed = true;
    return true;
}

void AllocationWriter::MarkAesChainTied(u32 id, u16 target) {
    ASSERT(!allocation_edit);
    ASSERT(!result.gpr_locations_frozen);
    ASSERT(id < result.aes_chain_targets.size());
    result.aes_chain_targets[id] = target;
}

void AllocationWriter::MarkPshufdDirect(u32 id) {
    ASSERT(!allocation_edit);
    ASSERT(!result.gpr_locations_frozen);
    ASSERT(id < result.alloc_result.size());
    if (result.pshufd_direct.empty()) result.pshufd_direct.resize(result.alloc_result.size());
    result.pshufd_direct[id] = true;
}

void AllocationWriter::SetLiveEnd(u32 id, u32 end) {
    ASSERT(!allocation_edit);
    ASSERT(!result.gpr_locations_frozen);
    ASSERT(id < result.live_ends.size());
    result.live_ends[id] = end;
}

void AllocationWriter::SetActiveRegs(swift::u32 id, GPRSMask gprs, FPRSMask fprs) {
    ASSERT(!result.gpr_locations_frozen);
    auto& map = result.alloc_result[id];
    if (allocation_edit && (map.dirty_gprs != gprs || map.dirty_fprs != fprs))
        allocation_edit->RecordValue(id);
    map.dirty_gprs = gprs;
    map.dirty_fprs = fprs;
}

void AllocationWriter::PermuteGPRHomes(u16 left, u16 right) {
    ASSERT(!allocation_edit);
    ASSERT(!result.gpr_locations_frozen);
    ASSERT(left < 31 && right < 31);
    const auto swap_mask_bits = [=](GPRSMask& mask) {
        const bool left_set = mask.Get(left);
        const bool right_set = mask.Get(right);
        if (left_set != right_set) {
            left_set ? mask.Clear(left) : mask.Mark(left);
            right_set ? mask.Clear(right) : mask.Mark(right);
        }
    };
    for (auto& map : result.alloc_result) {
        if (map.type == GPR) {
            if (map.slot == left)
                map.slot = right;
            else if (map.slot == right)
                map.slot = left;
        }
        swap_mask_bits(map.dirty_gprs);
    }
}

void AllocationWriter::FlattenRefChains() {
    const auto count = static_cast<u32>(result.alloc_result.size());
    for (u32 id = 0; id < count; ++id) {
        if (result.alloc_result[id].type != REF) continue;
        u32 cursor = id;
        // Bounded walk: a cycle would be an allocator bug; leave the
        // entry untouched instead of looping forever.
        for (u32 steps = 0; steps <= count; ++steps) {
            const u32 next = result.alloc_result[cursor].slot;
            if (next >= count || result.alloc_result[next].type != REF) {
                if (next < count) result.alloc_result[id].slot = next;
                break;
            }
            cursor = next;
        }
    }
}

}  // namespace swift::runtime::compile
