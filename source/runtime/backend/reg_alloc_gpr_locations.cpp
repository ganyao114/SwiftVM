#include "reg_alloc.h"
#include "runtime/common/perf_stats.h"

namespace swift::runtime::backend {

void RegAlloc::SetAllocationUnit(ir::Block* block, ir::HIRFunction* function, std::function<void()> replay) {
    allocation_block = block;
    allocation_function = function;
    allocation_replay = std::move(replay);
    if (!target_planning) { target_complete = false; target_plans.clear(); target_elided_publications.clear(); }
}

std::shared_ptr<const arm64::GPRBlockPlan> RegAlloc::GPRPlanFor(const ir::Block* block) const {
    const auto found = target_plans.find(block);
    return found == target_plans.end() ? nullptr : found->second;
}

const arm64::GPRBlockPlan* RegAlloc::GPRPlanViewFor(const ir::Block* block) const {
    const auto found = target_plans.find(block);
    return found == target_plans.end() ? nullptr : found->second.get();
}

void RegAlloc::BeginTargetPlanning() {
    ASSERT(!target_planning);
    target_fpr_disabled_undo = fpr_coalescing_disabled;
    target_fixed_undo.clear();
    target_publications_undo.reset();
    verified_width_elisions.clear();
    target_planning = true;
    FlattenRefChains();
    BeginGPRLocationAnalysis();
}

void RegAlloc::EndTargetPlanning() {
    ASSERT(target_planning);
    FreezeGPRLocations();
    target_planning = false;
    target_complete = true;
    target_fixed_undo.clear();
    target_publications_undo.reset();
}

void RegAlloc::AbortTargetPlanning() noexcept {
    if (!target_planning) return;
    target_planning = false;
    target_complete = false;
    fpr_coalescing_disabled = target_fpr_disabled_undo;
    target_plans.clear();
    target_elided_publications.clear();
    for (const auto& [id, definition] : target_fixed_undo)
        gpr_placements[id].fixed = definition;
    target_fixed_undo.clear();
    if (target_publications_undo) fixed_gpr_publications.swap(*target_publications_undo);
    target_publications_undo.reset();
    gpr_home_intervals_dirty = true;
    gpr_preservation_ranges_dirty = true;
    ResetAllocations();
}

RegAlloc::TargetDeclarations RegAlloc::SaveTargetDeclarations() const {
    TargetDeclarations saved;
    for (u32 id = 0; id < gpr_placements.size(); ++id) {
        const auto& p = gpr_placements[id];
        if (p.epoch != gpr_location_epoch) continue;
        saved.homes.push_back({id, p.result, p.low_view, p.whole_value, p.begin, p.end,
                               p.additional_uses, p.has_use_homes});
    }
    for (const auto& [key, use] : gpr_use_homes)
        saved.uses.push_back({static_cast<u32>(key >> 32), static_cast<u32>(key), use.value, use.selected});
    return saved;
}

void RegAlloc::RestoreTargetDeclarations(const TargetDeclarations& saved) {
    for (const auto& home : saved.homes) {
        auto& p = GPRPlacementFor(home.value);
        p.result = home.result; p.low_view = home.low; p.whole_value = home.whole;
        p.begin = home.begin; p.end = home.end; p.additional_uses = home.additional_uses;
        p.has_use_homes = home.has_use_homes;
    }
    for (const auto& use : saved.uses)
        gpr_use_homes.emplace((u64{use.value} << 32) | use.consumer,
                             GPRUse{use.home, gpr_location_epoch, use.selected});
}

bool RegAlloc::BindTargetDefinition(u32 id, u16 home, u8 width, bool elided, u32 last, u32 publication) {
    ASSERT(target_planning && gprs.Get(home));
    // A preserved definition already proves residence over its prefix
    // (end == transfer); when the requested span ends inside that
    // prefix it satisfies the bind exactly like a plain definition,
    // so replacing it would only force a replay to move the suffix.
    if (const auto* old = FixedGPRDefinitionAt(id); old && old->home == home &&
        old->end >= last && old->elided == elided) return false;
    PlanFixedGPRDefinition(id, {.home = home, .end = last, .last_use = last,
            .publication = publication, .owner = id, .width = width,
            .elided = elided, .version = true});
    return true;
}

void RegAlloc::ReplayAllocation() {
    ASSERT(target_planning && allocation_replay);
    ++target_replays;
    const auto replay = allocation_replay;
    PerfScope2 perf_replay{GetPerfStats2().target_replay};
    ResetAllocations();
    replay();
    FlattenRefChains();
}

RegAlloc::GPRPlacement& RegAlloc::GPRPlacementFor(u32 id) {
    ASSERT_MSG(!gpr_locations_frozen, "GPR placement changed after plan freeze");
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
    if (target_planning) {
        if (!target_publications_undo) target_publications_undo.emplace(fixed_gpr_publications);
        const auto* previous = FixedGPRDefinitionAt(id);
        target_fixed_undo.try_emplace(id, previous ? *previous : FixedGPRDefinition{});
    }
    gpr_home_intervals_dirty = true;
    const auto* old_definition = FixedGPRDefinitionAt(id);
    if (definition.preserved || (old_definition && old_definition->preserved))
        gpr_preservation_ranges_dirty = true;
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

const RegAlloc::FixedGPRDefinition* RegAlloc::PreservedGPRDefinitionAt(u32 id) const {
    const auto* definition = FixedGPRDefinitionAt(ResolveId(id));
    return definition && definition->preserved ? definition : nullptr;
}

bool RegAlloc::GPRPreservationConflict(u16 home, u32 begin, u32 end) const {
    ASSERT(home < 32 && begin <= end);
    if (gpr_preservation_ranges_dirty) {
        for (auto& ranges : gpr_preservation_ranges) ranges.clear();
        for (u32 id = 0; id < gpr_placements.size(); ++id) {
            const auto& fixed = gpr_placements[id].fixed;
            if (fixed.preserved && fixed.transfer > id)
                gpr_preservation_ranges[fixed.home].emplace_back(id, fixed.transfer);
        }
        for (auto& ranges : gpr_preservation_ranges) {
            std::sort(ranges.begin(), ranges.end());
            size_t count = 0;
            for (const auto range : ranges) {
                if (count && range.first <= ranges[count - 1].second)
                    ranges[count - 1].second = std::max(ranges[count - 1].second, range.second);
                else ranges[count++] = range;
            }
            ranges.resize(count);
        }
        gpr_preservation_ranges_dirty = false;
    }
    const auto& ranges = gpr_preservation_ranges[home];
    const auto next = std::lower_bound(ranges.begin(), ranges.end(), begin,
            [](const auto& range, u32 point) { return range.second <= point; });
    return next != ranges.end() && next->first <= end;
}

bool RegAlloc::GPRTargetWriteConflict(u16 home, u32 begin, u32 end, u32 ignored, u32 ignored_second) const {
    ASSERT(home < 32 && begin <= end);
    if (gpr_home_intervals_dirty) {
        for (auto& intervals : gpr_home_intervals) intervals.clear();
        for (u32 id = 0; id < gpr_placements.size(); ++id) {
            const auto& fixed = gpr_placements[id].fixed;
            if (fixed.home == UINT16_MAX) continue;
            if (fixed.end > id) gpr_home_intervals[fixed.home].push_back({id, id, fixed.end});
            if (fixed.transferred_home != UINT16_MAX && fixed.last_use > fixed.transfer)
                gpr_home_intervals[fixed.transferred_home].push_back({id, fixed.transfer, fixed.last_use});
        }
        for (u32 reg = 0; reg < 32; ++reg) {
            auto& intervals = gpr_home_intervals[reg];
            std::sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
            auto& prefix = gpr_home_prefix_ends[reg];
            prefix.clear(); u32 longest = 0;
            for (const auto& interval : intervals) { longest = std::max(longest, interval.end); prefix.push_back(longest); }
        }
        gpr_home_intervals_dirty = false;
    }
    const auto& intervals = gpr_home_intervals[home];
    const auto& prefix = gpr_home_prefix_ends[home];
    size_t index = std::upper_bound(prefix.begin(), prefix.end(), begin) - prefix.begin();
    for (; index < intervals.size() && intervals[index].begin <= end; ++index)
        if (intervals[index].owner != ignored && intervals[index].owner != ignored_second &&
            intervals[index].end > begin) return true;
    return false;
}

void RegAlloc::BuildGPRPreservationActions() {
    ASSERT(!gpr_locations_frozen);
    gpr_preservation_actions.clear();
    for (u32 id = 0; id < gpr_placements.size(); ++id) {
        const auto& fixed = gpr_placements[id].fixed;
        if (!fixed.preserved) continue;
        const auto& mapping = alloc_result[ResolveId(id)];
        ASSERT(mapping.type == GPR || mapping.type == MEM);
        ASSERT(fixed.transfer >= id && fixed.transfer <= fixed.last_use);
        u16 scratch = UINT16_MAX;
        if (mapping.type == MEM && (fixed.width != 8 || fixed.bit_offset)) {
            if (const auto* reload = SpillReloadAt(id, fixed.transfer)) scratch = reload->reg;
            else {
                const auto available = alloc_result[fixed.transfer].dirty_gprs.GetFirstClear();
                ASSERT_MSG(available >= 0, "no register for declared GPR capture scratch");
                scratch = static_cast<u16>(available);
            }
        }
        gpr_preservation_actions[fixed.transfer].push_back(
                {id, fixed.home, mapping.type, mapping.slot, fixed.width, fixed.bit_offset, scratch});
    }
}

const Vector<RegAlloc::GPRPreservationAction>&
RegAlloc::GPRPreservationsBefore(u32 instruction) const {
    static const Vector<GPRPreservationAction> empty;
    const auto found = gpr_preservation_actions.find(instruction);
    return found == gpr_preservation_actions.end() ? empty : found->second;
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
        if (fixed->preserved) {
            return ValueType(value) == GPR && ValueGPR(value).id == home &&
                   fixed->transfer <= end && fixed->last_use > begin;
        }
        return fixed->transferred_home == home && fixed->transfer < end &&
               fixed->last_use > begin;
    }
    return ValueType(value) == GPR && ValueGPR(value).id == home &&
           value.Id() <= end && ordinary_end > begin;
}

void RegAlloc::BeginGPRLocationAnalysis() {
    gpr_locations_frozen = false;
    ++gpr_location_epoch;
    ASSERT(gpr_location_epoch != 0);
    // Exact uses belong to the block being prepared. Range constraints above
    // are function-owned and survive this reset and allocation retries.
    gpr_use_homes.clear();
}

void RegAlloc::FreezeGPRLocations() {
    ASSERT_MSG(!gpr_locations_frozen, "GPR locations already frozen");
    (void)GPRPreservationConflict(0, 0, 0);
    (void)GPRTargetWriteConflict(0, 0, 0);
    for (const auto& placement : gpr_placements) {
        if (placement.epoch != gpr_location_epoch) continue;
        const auto valid_home = [](u16 home) { return home == UINT16_MAX || home < 31; };
        ASSERT(valid_home(placement.result) && valid_home(placement.low_view) &&
               valid_home(placement.whole_value));
        ASSERT(placement.whole_value == UINT16_MAX || placement.begin <= placement.end);
    }
    for (const auto& [key, use] : gpr_use_homes) {
        const auto& value = use.value;
        ASSERT(use.epoch == gpr_location_epoch && value.home < 31);
        ASSERT(value.width == 1 || value.width == 2 || value.width == 4 || value.width == 8);
        ASSERT(value.extension.known_zero_above <= 64 &&
               value.extension.sign_extended_from <= value.extension.sign_extended_to &&
               value.extension.sign_extended_to <= 64);
    }
    FlattenRefChains();
    gpr_locations_frozen = true;
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

std::optional<FixedGPRValue> RegAlloc::GPRUseHome(u32 id, u32 consumer, bool selected_only) const {
    const auto* placement = CurrentGPRPlacement(id);
    if (!placement || !placement->has_use_homes) return std::nullopt;
    const auto found = gpr_use_homes.find((u64{id} << 32) | consumer);
    return found == gpr_use_homes.end() || (selected_only && !found->second.selected)
            ? std::nullopt : std::optional{found->second.value};
}

u32 RegAlloc::AdditionalGPRResidentUses(u32 id) const {
    const auto* placement = CurrentGPRPlacement(id);
    return placement ? placement->additional_uses : 0;
}

RegAlloc::GPRLocation RegAlloc::GPRLocationAt(ir::Value value, u32 consumer,
                                           GPRView view) const {
    if (!value.Defined()) return {};
    const u32 id = value.Id();
    const u32 resolved_id = ResolveId(id);
    const u8 bits = value.Type() == ir::ValueType::U64 || value.Type() == ir::ValueType::S64 ? 64 : 32;
    const auto* fixed_definition = FixedGPRDefinitionAt(resolved_id);
    const auto* preserved = fixed_definition && fixed_definition->preserved ? fixed_definition : nullptr;
    if (preserved) {
        if (view == GPRView::Value || view == GPRView::Low32) {
            const auto use = gpr_use_homes.find((u64{id} << 32) | consumer);
            const auto width = value.Type() == ir::ValueType::VOID ? 4 : ir::GetValueSizeByte(value.Type());
            if (use != gpr_use_homes.end() && use->second.selected && use->second.value.width == width)
                return {GPR, use->second.value.home, static_cast<u8>(width == 8 ? 64 : 32), true,
                        UINT16_MAX, use->second.value.extension};
        }
        if (consumer < preserved->transfer)
            return {GPR, preserved->home, static_cast<u8>(view == GPRView::Low32 ? 32 : bits), true};
        // The suffix is an ordinary SSA location, never an inferred guest
        // copy or an override selected by target preparation.
        const auto& mapping = alloc_result[resolved_id];
        GPRLocation location{mapping.type, mapping.slot, bits};
        if (mapping.type == MEM) {
            if (const auto* reload = SpillReloadAt(value, consumer)) location.reload_register = reload->reg;
        }
        return location;
    }
    const auto* stored = id < gpr_placements.size() ? &gpr_placements[id] : nullptr;
    const auto* placement = stored && stored->epoch == gpr_location_epoch ? stored : nullptr;
    // Definition residence deliberately excludes inferred operand copies and
    // low views: those do not define a complete SSA value in a guest home.
    if (view == GPRView::Definition) {
        if (const auto home = GPRValueHome(value, consumer))
            return {GPR, *home, 64, true};
    }
    if (placement && view != GPRView::Definition) {
        if (view != GPRView::Low32) {
            if (consumer == id && placement->result != UINT16_MAX)
                return {GPR, placement->result, bits, true};
            if (placement->whole_value != UINT16_MAX && placement->begin <= consumer &&
                consumer <= placement->end) return {GPR, placement->whole_value, bits, true};
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
                return {GPR, found->second.value.home, 32, true,
                        UINT16_MAX, found->second.value.extension};
            if (placement->low_view != UINT16_MAX)
                return {GPR, placement->low_view,
                        static_cast<u8>(view == GPRView::Value && width == 8 ? 64 : 32), true};
            if (found != gpr_use_homes.end() && found->second.value.width == width &&
                (width <= 4 || (view == GPRView::Value && width == 8)))
                return {GPR, found->second.value.home, static_cast<u8>(width == 8 ? 64 : 32), true,
                        UINT16_MAX, found->second.value.extension};
        }
    }
    if (view != GPRView::Definition && stored && stored->fixed.home != UINT16_MAX) {
        const auto& fixed = stored->fixed;
        const u16 home = consumer > fixed.transfer && consumer <= fixed.last_use
                ? fixed.transferred_home : fixed.home;
        return {GPR, home, static_cast<u8>(view == GPRView::Low32 ? 32 : bits), true};
    }
    // Resolve references once, rather than once for ValueType and again for
    // ValueGPR/ValueMem. Spill recipes stay descriptions until JitContext loads.
    const auto& mapping = alloc_result[resolved_id];
    GPRLocation location{mapping.type,
                         static_cast<u16>(mapping.type == GPR || mapping.type == MEM ? mapping.slot : 0),
                         bits};
    if (mapping.type == MEM) {
        if (const auto* reload = SpillReloadAt(value, consumer)) location.reload_register = reload->reg;
    }
    return location;
}

}  // namespace swift::runtime::backend
