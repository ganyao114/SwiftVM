#include "allocation.h"

#include <algorithm>
#include <cstdio>
#if defined(__linux__) && (!defined(__ANDROID__) || __ANDROID_API__ >= 33)
#include <execinfo.h>
#include <unistd.h>
#endif
#include "base/logging.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::compile {

Allocation::Allocation(u32 instr_size,
                       const GPRSMask& gprs,
                       const FPRSMask& fprs,
                       const FeatureSet& features)
        : alloc_result(instr_size)
        , live_ends(instr_size, UINT32_MAX)
        , coalesced_host_writes(instr_size)
        , coalesced_host_reads(instr_size)
        , width_chain_anchors(instr_size, UINT32_MAX)
        , width_component_owners(instr_size)
        , low32_copy_sources(instr_size, UINT32_MAX)
        , aes_chain_targets(instr_size, UINT16_MAX)
        , gprs(gprs)
        , fprs(fprs)
        , features(features) {}

bool AllocationView::IsHostWriteCoalesced(u32 id) const {
    return id < result->alloc_result.size() &&
           (result->coalesced_host_writes[id] || result->alloc_result[id].publication_elided);
}

bool AllocationView::IsHostReadCoalesced(u32 id) const {
    return id < result->coalesced_host_reads.size() && result->coalesced_host_reads[id];
}

bool AllocationView::IsWidthChainCoalesced(u32 id) const {
    return id < result->width_chain_anchors.size() && result->width_chain_anchors[id] != UINT32_MAX;
}

u32 AllocationView::WidthChainAnchor(u32 id) const {
    ASSERT(IsWidthChainCoalesced(id));
    return result->width_chain_anchors[id];
}

bool AllocationView::IsLow32CopyCoalesced(u32 id) const {
    return id < result->low32_copy_sources.size() && result->low32_copy_sources[id] != UINT32_MAX;
}

u32 AllocationView::Low32CopySource(u32 id) const {
    ASSERT(IsLow32CopyCoalesced(id));
    return result->low32_copy_sources[id];
}

bool AllocationView::HasWidthComponentOwner(u32 anchor_id) const {
    return anchor_id < result->width_component_owners.size() &&
           result->width_component_owners[anchor_id].Valid();
}

bool AllocationView::WidthComponentOwnerCommitted(u32 anchor_id) const {
    return HasWidthComponentOwner(anchor_id) && result->width_component_owners[anchor_id].committed;
}

u16 AllocationView::WidthComponentOwnerTarget(u32 anchor_id) const {
    ASSERT(HasWidthComponentOwner(anchor_id));
    return result->width_component_owners[anchor_id].target;
}

bool AllocationView::WidthComponentOwnerHighZero(u32 anchor_id) const {
    ASSERT(HasWidthComponentOwner(anchor_id));
    return result->width_component_owners[anchor_id].high_zero;
}

bool AllocationView::IsAesChainTied(u32 id) const {
    return id < result->aes_chain_targets.size() && result->aes_chain_targets[id] != UINT16_MAX;
}

u16 AllocationView::AesChainTarget(u32 id) const {
    ASSERT(IsAesChainTied(id));
    return result->aes_chain_targets[id];
}

bool AllocationView::SharesFPR(const ir::Value& left, const ir::Value& right) const {
    if (!left.Defined() || !right.Defined() || left.Id() >= result->alloc_result.size() ||
        right.Id() >= result->alloc_result.size())
        return false;
    const auto& lhs = result->alloc_result[ResolveId(left.Id())];
    if (lhs.type != FPR || lhs.slot >= 32) return false;
    const auto& rhs = result->alloc_result[ResolveId(right.Id())];
    return rhs.type == FPR && lhs.slot == rhs.slot;
}

bool AllocationView::IsFixedGPR(u32 id) const {
    id = ResolveId(id);
    return id < result->alloc_result.size() && result->alloc_result[id].type == GPR &&
           result->alloc_result[id].fixed_gpr;
}

bool AllocationView::ValueLiveAfter(const ir::Value& value, u32 instruction_id) const {
    if (!value.Defined()) {
        return false;
    }
    const u32 id = ResolveId(value.Id());
    return id >= result->live_ends.size() || result->live_ends[id] == UINT32_MAX ||
           result->live_ends[id] > instruction_id;
}

ir::HostFPR AllocationView::ValueFPR(const ir::Value& value) const { return ValueFPR(value.Id()); }

ir::HostGPR AllocationView::ValueGPR(const ir::Value& value) const { return ValueGPR(value.Id()); }

ir::SpillSlot AllocationView::ValueMem(const ir::Value& value) const {
    return ValueMem(value.Id());
}

ir::HostGPR AllocationView::ValueGPR(u32 id) const {
    id = ResolveId(id);
    ASSERT(result->alloc_result[id].type == GPR);
    return ir::HostGPR{result->alloc_result[id].slot};
}

ir::HostFPR AllocationView::ValueFPR(u32 id) const {
    id = ResolveId(id);
    ASSERT(result->alloc_result[id].type == FPR);
    return ir::HostFPR{result->alloc_result[id].slot};
}

ir::SpillSlot AllocationView::ValueMem(u32 id) const {
    id = ResolveId(id);
    ASSERT(result->alloc_result[id].type == MEM);
    return ir::SpillSlot{result->alloc_result[id].slot};
}

u32 AllocationView::AllocationId(const ir::Value& value) const { return ResolveId(value.Id()); }

const AllocationView::SpillReload* AllocationView::SpillReloadAt(u32 value_id,
                                                                 u32 instruction_id) const {
    value_id = ResolveId(value_id);
    const auto* reloads = SpillReloadsAt(instruction_id);
    if (!reloads) {
        return nullptr;
    }
    const auto it = std::find_if(reloads->begin(), reloads->end(), [value_id](const auto& reload) {
        return reload.value == value_id;
    });
    return it == reloads->end() ? nullptr : &*it;
}

const AllocationView::SpillReload* AllocationView::SpillReloadAt(const ir::Value& value,
                                                                 u32 instruction_id) const {
    return SpillReloadAt(value.Id(), instruction_id);
}

bool AllocationView::HasSpillReload(u32 value_id, u32 instruction_id) const {
    return SpillReloadAt(value_id, instruction_id) != nullptr;
}

const Vector<AllocationView::SpillReload>* AllocationView::SpillReloadsAt(
        u32 instruction_id) const {
    if (instruction_id >= result->spill_reloads.size() ||
        result->spill_reloads[instruction_id].empty()) {
        return nullptr;
    }
    return &result->spill_reloads[instruction_id];
}

AllocationView::Type AllocationView::ValueType(const ir::Value& value) const {
    const u32 vid = value.Id();
    if (vid >= result->alloc_result.size()) {
        auto* def = value.Def();
        SVM_DIAG_PRINT(Runtime,
                       "[OOB-ValueType] value.Id()=%u alloc_size=%zu def=%p op=%u\n",
                       vid,
                       result->alloc_result.size(),
                       (void*)def,
                       def ? (unsigned)def->GetOp() : 0xffffu);
#if defined(__linux__) && (!defined(__ANDROID__) || __ANDROID_API__ >= 33)
        void* bt[32];
        int n = backtrace(bt, 32);
        backtrace_symbols_fd(bt, n, STDERR_FILENO);
#endif
    }
    return ValueType(vid);
}

AllocationView::Type AllocationView::ValueType(u32 id) const {
    ASSERT(id < result->alloc_result.size());
    return result->alloc_result[ResolveId(id)].type;
}

u32 AllocationView::ResolveId(u32 id) const {
    while (true) {
        if (id >= result->alloc_result.size()) {
            SVM_DIAG_PRINT(Runtime,
                           "[OOB-ResolveId] id=%u alloc_size=%zu (read past end)\n",
                           id,
                           result->alloc_result.size());
            return id;
        }
        if (result->alloc_result[id].type != REF) {
            break;
        }
        const u32 next = result->alloc_result[id].slot;
        if (next >= result->alloc_result.size()) {
            SVM_DIAG_PRINT(Runtime,
                           "[OOB-ResolveId] REF chain: id=%u slot=%u alloc_size=%zu\n",
                           id,
                           next,
                           result->alloc_result.size());
        }
        id = next;
    }
    return id;
}

const GPRSMask& AllocationView::GetGprs() const { return result->gprs; }

const FPRSMask& AllocationView::GetFprs() const { return result->fprs; }

u32 AllocationView::SpillCount() const {
    return static_cast<u32>(std::count_if(result->alloc_result.begin(),
                                          result->alloc_result.end(),
                                          [](const Map& map) { return map.type == MEM; }));
}

const AllocationView::GPRPlacement* AllocationView::CurrentGPRPlacement(u32 id) const {
    return id < result->gpr_placements.size() &&
                           result->gpr_placements[id].epoch == result->gpr_location_epoch
                   ? &result->gpr_placements[id]
                   : nullptr;
}

const FixedGPRDefinition* AllocationView::FixedGPRDefinitionAt(u32 id) const {
    return id < result->gpr_placements.size() && result->gpr_placements[id].fixed.home != UINT16_MAX
                   ? &result->gpr_placements[id].fixed
                   : nullptr;
}

bool AllocationView::HasFixedGPRDefinition(u32 id) const {
    return FixedGPRDefinitionAt(id) != nullptr;
}

bool AllocationView::IsFixedGPRDefinitionElided(u32 id) const {
    const auto* definition = FixedGPRDefinitionAt(id);
    return definition && definition->elided;
}

const FixedGPRDefinition* AllocationView::PreservedGPRDefinitionAt(u32 id) const {
    const auto* definition = FixedGPRDefinitionAt(ResolveId(id));
    return definition && definition->preserved ? definition : nullptr;
}

const Vector<AllocationView::GPRPreservationAction>& AllocationView::GPRPreservationsBefore(
        u32 instruction) const {
    static const Vector<GPRPreservationAction> empty;
    const auto found = result->gpr_preservation_actions.find(instruction);
    return found == result->gpr_preservation_actions.end() ? empty : found->second;
}

bool AllocationView::NeedsFixedGPRPublication(u32 id) const {
    return result->fixed_gpr_publications.contains(id);
}

bool AllocationView::FixedGPRPublicationOwns(u32 publication, u32 id) const {
    const auto* definition = FixedGPRDefinitionAt(id);
    return definition && definition->publication == publication;
}

bool AllocationView::FixedGPRHomeIntersects(
        ir::Value value, u16 home, u32 begin, u32 end, u32 ordinary_end) const {
    if (const auto* fixed = FixedGPRDefinitionAt(value.Id())) {
        if (fixed->home == home && value.Id() <= end && fixed->end > begin) return true;
        if (fixed->preserved) {
            return ValueType(value) == GPR && ValueGPR(value).id == home &&
                   fixed->transfer <= end && fixed->last_use > begin;
        }
        return fixed->transferred_home == home && fixed->transfer < end && fixed->last_use > begin;
    }
    return ValueType(value) == GPR && ValueGPR(value).id == home && value.Id() <= end &&
           ordinary_end > begin;
}

std::optional<u16> AllocationView::GPRValueHome(ir::Value value, u32 consumer) const {
    return GPRValueHome(value.Id(), consumer);
}

std::optional<u16> AllocationView::GPRValueHome(u32 id, u32 consumer) const {
    const auto* placement = CurrentGPRPlacement(id);
    if (!placement) return std::nullopt;
    if (consumer == id && placement->result != UINT16_MAX) return placement->result;
    return placement->whole_value != UINT16_MAX && placement->begin <= consumer &&
                           consumer <= placement->end
                   ? std::optional<u16>{placement->whole_value}
                   : std::nullopt;
}

std::optional<u16> AllocationView::GPRLowView(u32 id) const {
    const auto* placement = CurrentGPRPlacement(id);
    return placement && placement->low_view != UINT16_MAX ? std::optional<u16>{placement->low_view}
                                                          : std::nullopt;
}

bool AllocationView::HasGPRValueHome(u32 id) const {
    const auto* placement = CurrentGPRPlacement(id);
    return placement && placement->whole_value != UINT16_MAX;
}

std::optional<FixedGPRValue> AllocationView::GPRUseHome(u32 id,
                                                        u32 consumer,
                                                        bool selected_only) const {
    const auto* placement = CurrentGPRPlacement(id);
    if (!placement || !placement->has_use_homes) return std::nullopt;
    const auto found = result->gpr_use_homes.find((u64{id} << 32) | consumer);
    if (found == result->gpr_use_homes.end() || (selected_only && !found->second.selected))
        return std::nullopt;
    const auto* preserved = PreservedGPRDefinitionAt(id);
    if (preserved && consumer >= preserved->transfer && found->second.value.home == preserved->home)
        return std::nullopt;
    return found->second.value;
}

u32 AllocationView::AdditionalGPRResidentUses(u32 id) const {
    const auto* placement = CurrentGPRPlacement(id);
    return placement ? placement->additional_uses : 0;
}

AllocationView::GPRLocation AllocationView::GPRLocationAt(ir::Value value,
                                                          u32 consumer,
                                                          GPRView view) const {
    if (!value.Defined()) return {};
    return GPRLocationAt(value.Id(), value.Type(), consumer, view);
}

AllocationView::GPRLocation AllocationView::GPRLocationAt(u32 id,
                                                          ir::ValueType value_type,
                                                          u32 consumer,
                                                          GPRView view) const {
    const u32 resolved_id = ResolveId(id);
    const u8 bits = value_type == ir::ValueType::U64 || value_type == ir::ValueType::S64 ? 64 : 32;
    const auto* fixed_definition = FixedGPRDefinitionAt(resolved_id);
    const auto* preserved =
            fixed_definition && fixed_definition->preserved ? fixed_definition : nullptr;
    if (preserved) {
        if (view == GPRView::Value || view == GPRView::Low32) {
            const auto use = GPRUseHome(id, consumer, true);
            const auto width =
                    value_type == ir::ValueType::VOID ? 4 : ir::GetValueSizeByte(value_type);
            if (use && use->width == width)
                return {GPR,
                        use->home,
                        static_cast<u8>(width == 8 ? 64 : 32),
                        true,
                        UINT16_MAX,
                        use->extension};
        }
        if (consumer < preserved->transfer)
            return {GPR,
                    preserved->home,
                    static_cast<u8>(view == GPRView::Low32 ? 32 : bits),
                    true};
        // The suffix is an ordinary SSA location, never an inferred guest
        // copy or an override selected by target preparation.
        const auto& mapping = result->alloc_result[resolved_id];
        GPRLocation location{mapping.type, mapping.slot, bits};
        if (mapping.type == MEM) {
            if (const auto* reload = SpillReloadAt(id, consumer))
                location.reload_register = reload->reg;
        }
        return location;
    }
    const auto* stored = id < result->gpr_placements.size() ? &result->gpr_placements[id] : nullptr;
    const auto* placement =
            stored && stored->epoch == result->gpr_location_epoch ? stored : nullptr;
    // Definition residence deliberately excludes inferred operand copies and
    // low views: those do not define a complete SSA value in a guest home.
    if (view == GPRView::Definition) {
        if (const auto home = GPRValueHome(id, consumer)) return {GPR, *home, 64, true};
    }
    if (placement && view != GPRView::Definition) {
        if (view != GPRView::Low32) {
            if (consumer == id && placement->result != UINT16_MAX)
                return {GPR, placement->result, bits, true};
            if (placement->whole_value != UINT16_MAX && placement->begin <= consumer &&
                consumer <= placement->end)
                return {GPR, placement->whole_value, bits, true};
        }
        // Canonical SSA queries consume explicit residences and views. An
        // inferred architectural copy is an operand choice, not a new SSA
        // definition location; probing it is reserved for Value/Low32 queries.
        if (view == GPRView::WholeValue && placement->low_view != UINT16_MAX)
            return {GPR, placement->low_view, bits, true};
        if (view != GPRView::WholeValue &&
            (placement->has_use_homes || placement->low_view != UINT16_MAX)) {
            // A negative ordinary-location probe needs neither a use-map hash
            // nor width analysis. VOID-typed unused helper results use W.
            const u32 width = value_type == ir::ValueType::VOID ? sizeof(u32)
                                                                : ir::GetValueSizeByte(value_type);
            const auto found = placement->has_use_homes
                                       ? result->gpr_use_homes.find((u64{id} << 32) | consumer)
                                       : result->gpr_use_homes.end();
            if (found != result->gpr_use_homes.end() && found->second.selected &&
                width <= sizeof(u32) && found->second.value.width == width)
                return {GPR,
                        found->second.value.home,
                        32,
                        true,
                        UINT16_MAX,
                        found->second.value.extension};
            if (placement->low_view != UINT16_MAX)
                return {GPR,
                        placement->low_view,
                        static_cast<u8>(view == GPRView::Value && width == 8 ? 64 : 32),
                        true};
            if (found != result->gpr_use_homes.end() && found->second.value.width == width &&
                (width <= 4 || (view == GPRView::Value && width == 8)))
                return {GPR,
                        found->second.value.home,
                        static_cast<u8>(width == 8 ? 64 : 32),
                        true,
                        UINT16_MAX,
                        found->second.value.extension};
        }
    }
    if (view != GPRView::Definition && stored && stored->fixed.home != UINT16_MAX) {
        const auto& fixed = stored->fixed;
        const u16 home = consumer > fixed.transfer && consumer <= fixed.last_use
                                 ? fixed.transferred_home
                                 : fixed.home;
        return {GPR, home, static_cast<u8>(view == GPRView::Low32 ? 32 : bits), true};
    }
    // Resolve references once, rather than once for ValueType and again for
    // ValueGPR/ValueMem. Spill recipes stay descriptions until JitContext loads.
    const auto& mapping = result->alloc_result[resolved_id];
    GPRLocation location{
            mapping.type,
            static_cast<u16>(mapping.type == GPR || mapping.type == MEM ? mapping.slot : 0),
            bits};
    if (mapping.type == MEM) {
        if (const auto* reload = SpillReloadAt(id, consumer))
            location.reload_register = reload->reg;
    }
    return location;
}

}  // namespace swift::runtime::compile
