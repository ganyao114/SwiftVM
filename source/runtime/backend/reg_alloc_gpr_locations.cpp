#include "reg_alloc.h"

namespace swift::runtime::backend {

RegAlloc::GPRPlacement& RegAlloc::GPRPlacementFor(u32 id) {
    ASSERT(id < alloc_result.size());
    if (gpr_placements.empty()) gpr_placements.resize(alloc_result.size());
    auto& placement = gpr_placements[id];
    if (placement.epoch != gpr_location_epoch) {
        const auto fixed = placement.fixed;
        placement = {};
        placement.fixed = fixed;
        placement.epoch = gpr_location_epoch;
    }
    return placement;
}

const RegAlloc::GPRPlacement* RegAlloc::CurrentGPRPlacement(u32 id) const {
    return id < gpr_placements.size() && gpr_placements[id].epoch == gpr_location_epoch
            ? &gpr_placements[id] : nullptr;
}

void RegAlloc::PlanFixedGPRDefinition(u32 id, FixedGPRDefinition definition) {
    if (definition.home == UINT16_MAX) {
        // A completed width-coalescing transaction may turn a read view into
        // an actual capture/W write in its chosen fixed home. Its own proof
        // then replaces this earlier read-elision constraint.
        const auto* previous = FixedGPRDefinitionAt(id);
        ASSERT(previous && (!previous->elided || previous->publication == UINT32_MAX));
        if (previous->publication != UINT32_MAX)
            fixed_gpr_publications.erase(previous->publication);
        GPRPlacementFor(id).fixed = {};
        return;
    }
    ASSERT(IsFixedGPRHome(definition.home) && gprs.Get(definition.home));
    GPRPlacementFor(id).fixed = definition;
    if (definition.publication != UINT32_MAX) {
        fixed_gpr_publications.emplace(definition.publication, definition.owner);
    }
}

const RegAlloc::FixedGPRDefinition* RegAlloc::FixedGPRDefinitionAt(u32 id) const {
    return id < gpr_placements.size() && gpr_placements[id].fixed.home != UINT16_MAX
            ? &gpr_placements[id].fixed : nullptr;
}

bool RegAlloc::HasFixedGPRDefinition(u32 id) const {
    return FixedGPRDefinitionAt(id) != nullptr;
}

bool RegAlloc::IsFixedGPRDefinitionElided(u32 id) const {
    const auto* definition = FixedGPRDefinitionAt(id);
    return definition && definition->elided;
}

bool RegAlloc::NeedsFixedGPRPublication(u32 id) const {
    return fixed_gpr_publications.contains(id);
}

bool RegAlloc::FixedGPRPublicationOwns(u32 publication, u32 id) const {
    const auto* definition = FixedGPRDefinitionAt(id);
    return definition && definition->publication == publication;
}

bool RegAlloc::FixedGPRHomeIntersects(ir::Value value, u16 home,
                                    u32 begin, u32 end, u32 ordinary_end) const {
    if (const auto* fixed = FixedGPRDefinitionAt(value.Id())) {
        if (fixed->home == home && value.Id() <= end && fixed->end > begin) return true;
        return fixed->transferred_home == home && fixed->transfer < end &&
               fixed->last_use > begin;
    }
    return ValueType(value) == GPR && ValueGPR(value).id == home &&
           value.Id() <= end && ordinary_end > begin;
}

void RegAlloc::BeginGPRLocationAnalysis() {
    ++gpr_location_epoch;
    ASSERT(gpr_location_epoch != 0);
    // Exact uses belong to the block being prepared. Range constraints above
    // are function-owned and survive this reset and allocation retries.
    gpr_use_homes.clear();
}

void RegAlloc::AssignGPRValueHome(u32 id, u16 home) {
    auto& placement = GPRPlacementFor(id);
    if (placement.result == UINT16_MAX) placement.result = home;
    if (placement.whole_value == UINT16_MAX) {
        placement.whole_value = home;
        placement.begin = id;
    }
}

void RegAlloc::AssignGPRTransferredHome(u32 id, u16 home, u32 begin, u32 end) {
    auto& placement = GPRPlacementFor(id);
    if (placement.whole_value == UINT16_MAX) {
        placement.whole_value = home;
        placement.begin = begin;
        placement.end = end;
    }
}

void RegAlloc::AssignGPRLowView(u32 id, u16 home) {
    auto& placement = GPRPlacementFor(id);
    if (placement.low_view == UINT16_MAX) placement.low_view = home;
}

std::optional<u16> RegAlloc::GPRValueHome(ir::Value value, u32 consumer) const {
    const auto* placement = CurrentGPRPlacement(value.Id());
    if (!placement) return std::nullopt;
    if (consumer == value.Id() && placement->result != UINT16_MAX) return placement->result;
    return placement->whole_value != UINT16_MAX && placement->begin <= consumer &&
           consumer <= placement->end
            ? std::optional<u16>{placement->whole_value} : std::nullopt;
}

std::optional<u16> RegAlloc::GPRLowView(u32 id) const {
    const auto* placement = CurrentGPRPlacement(id);
    return placement && placement->low_view != UINT16_MAX
            ? std::optional<u16>{placement->low_view} : std::nullopt;
}

bool RegAlloc::HasGPRValueHome(u32 id) const {
    const auto* placement = CurrentGPRPlacement(id);
    return placement && placement->whole_value != UINT16_MAX;
}

void RegAlloc::RegisterGPRUseHome(u32 id, u32 consumer, FixedGPRValue value,
                                bool selected) {
    GPRPlacementFor(id).has_use_homes = true;
    const u64 key = (u64{id} << 32) | consumer;
    const auto found = gpr_use_homes.find(key);
    if (selected && found == gpr_use_homes.end()) ++GPRPlacementFor(id).additional_uses;
    if (found == gpr_use_homes.end() || selected || !found->second.selected) {
        gpr_use_homes.insert_or_assign(key, GPRUse{value, gpr_location_epoch, selected});
    }
}

std::optional<FixedGPRValue> RegAlloc::GPRUseHome(u32 id, u32 consumer) const {
    const auto* placement = CurrentGPRPlacement(id);
    if (!placement || !placement->has_use_homes) return std::nullopt;
    const auto found = gpr_use_homes.find((u64{id} << 32) | consumer);
    return found == gpr_use_homes.end() ? std::nullopt : std::optional{found->second.value};
}

u32 RegAlloc::AdditionalGPRResidentUses(u32 id) const {
    const auto* placement = CurrentGPRPlacement(id);
    return placement ? placement->additional_uses : 0;
}

RegAlloc::GPRLocation RegAlloc::GPRLocationAt(ir::Value value, u32 consumer,
                                           GPRView view) const {
    if (!value.Defined()) return {};
    const u32 id = value.Id();
    const u8 bits = value.Type() == ir::ValueType::U64 ? 64 : 32;
    const auto* stored = id < gpr_placements.size() ? &gpr_placements[id] : nullptr;
    const auto* placement = stored && stored->epoch == gpr_location_epoch ? stored : nullptr;
    if (placement) {
        if (view != GPRView::Low32) {
            if (consumer == id && placement->result != UINT16_MAX)
                return {GPR, placement->result, 64, true};
            if (placement->whole_value != UINT16_MAX && placement->begin <= consumer &&
                consumer <= placement->end) return {GPR, placement->whole_value, 64, true};
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
            const u32 width = value.Type() == ir::ValueType::VOID ? sizeof(u32)
                                    : ir::GetValueSizeByte(value.Type());
            const auto found = placement->has_use_homes
                    ? gpr_use_homes.find((u64{id} << 32) | consumer) : gpr_use_homes.end();
            if (found != gpr_use_homes.end() && found->second.selected &&
                width <= sizeof(u32) && found->second.value.width == width)
                return {GPR, found->second.value.home, 32, true};
            if (placement->low_view != UINT16_MAX)
                return {GPR, placement->low_view,
                        static_cast<u8>(view == GPRView::Value && width == 8 ? 64 : 32), true};
            if (found != gpr_use_homes.end() && found->second.value.width == width &&
                (width <= 4 || (view == GPRView::Value && width == 8)))
                return {GPR, found->second.value.home, static_cast<u8>(width == 8 ? 64 : 32), true};
        }
    }
    if (stored && stored->fixed.home != UINT16_MAX) {
        const auto& fixed = stored->fixed;
        const u16 home = consumer > fixed.transfer && consumer <= fixed.last_use
                ? fixed.transferred_home : fixed.home;
        return {GPR, home, static_cast<u8>(view == GPRView::Low32 ? 32 : bits), true};
    }
    // Resolve references once, rather than once for ValueType and again for
    // ValueGPR/ValueMem. Spill recipes stay descriptions until JitContext loads.
    const auto& mapping = alloc_result[ResolveId(id)];
    GPRLocation location{mapping.type,
                         static_cast<u16>(mapping.type == GPR || mapping.type == MEM ? mapping.slot : 0),
                         bits};
    if (mapping.type == MEM) {
        if (const auto* reload = SpillReloadAt(value, consumer)) location.reload_register = reload->reg;
    }
    return location;
}

}  // namespace swift::runtime::backend
