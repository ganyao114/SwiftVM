#include "transaction.h"

#include <algorithm>

namespace swift::runtime::compile {

RecolorFailure AllocationEdit::RecolorGPR(std::span<const u32> values,
                                          u16 home,
                                          bool fixed_class,
                                          std::span<const GPRRecolorSite> sites) {
    using Allocation = AllocationWriter;
    ASSERT(active && allocation.allocation_edit == this);
    if (sites.empty() || values.empty() || allocation.LocationEditsClosed())
        return RecolorFailure::InvalidWindow;
    if (home >= 31 || !allocation.result.gprs.Get(home)) return RecolorFailure::HomeNotReserved;
    for (size_t i = 0; i < sites.size(); ++i)
        if (sites[i].instruction >= allocation.MapCount() ||
            (i && sites[i - 1].instruction >= sites[i].instruction))
            return RecolorFailure::InvalidWindow;
    const auto begin = sites.front().instruction, end = sites.back().instruction;
    // This snapshot belongs to this call, not the transaction's first-write
    // journal: one edit may recolor the same component more than once.
    struct RecolorValue {
        u32 id;
        u16 old_home{};
        bool was_gpr{}, was_fixed{};
    };
    static_assert(sizeof(RecolorValue) == 8);
    Vector<RecolorValue> roots;
    for (auto id : values) {
        if (id >= allocation.MapCount()) return RecolorFailure::UnsupportedLocation;
        roots.push_back({allocation.ResolveId(id)});
    }
    std::sort(roots.begin(), roots.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    roots.erase(std::unique(roots.begin(), roots.end(), [](const auto& a, const auto& b) {
                    return a.id == b.id;
                }),
                roots.end());
    const auto find_root = [&](u32 id) -> const RecolorValue* {
        id = allocation.ResolveId(id);
        const auto found = std::lower_bound(
                roots.begin(), roots.end(), id, [](const auto& root, u32 value) {
                    return root.id < value;
                });
        return found != roots.end() && found->id == id ? &*found : nullptr;
    };
    const auto contains = [&](u32 id) {
        return find_root(id) != nullptr;
    };
    u32 released = 0;
    for (auto& root : roots) {
        const auto id = root.id;
        const auto& map = allocation.result.alloc_result[id];
        if (map.type != Allocation::GPR && map.type != Allocation::MEM)
            return RecolorFailure::UnsupportedLocation;
        if (id < begin || allocation.result.live_ends[id] < id ||
            allocation.result.live_ends[id] > end)
            return RecolorFailure::OutsideWindow;
        if (const auto* fixed = allocation.FixedGPRDefinitionAt(id)) {
            if (fixed->preserved) return RecolorFailure::CapturedValue;
            if (fixed->home != home) return RecolorFailure::FixedResidence;
        }
        root.old_home = map.slot;
        root.was_gpr = map.type == Allocation::GPR;
        root.was_fixed = map.fixed_gpr;
        if (map.type == Allocation::GPR && map.slot != home) released |= 1u << map.slot;
    }
    const auto old_value = [&](u32 id) -> const RecolorValue& {
        const auto* found = find_root(id);
        ASSERT(found);
        return *found;
    };
    const auto already_home = [&](u32 id) {
        const auto& map = allocation.result.alloc_result[allocation.ResolveId(id)];
        return map.type == Allocation::GPR && map.slot == home;
    };
    // Existing co-owned views cannot be split between the old and new homes.
    // A component already committed to another publication keeps that owner.
    for (u32 id = 0; id < allocation.MapCount(); ++id) {
        if (allocation.HasWidthComponentOwner(id) && contains(id) &&
            allocation.WidthComponentOwnerTarget(id) != home)
            return RecolorFailure::WidthOwner;
        const auto anchor = allocation.result.width_chain_anchors[id];
        if (anchor != UINT32_MAX && (contains(id) || contains(anchor))) {
            if ((!contains(id) && !already_home(id)) ||
                (!contains(anchor) && !already_home(anchor)))
                return RecolorFailure::PartialAliasGroup;
            if (allocation.HasWidthComponentOwner(anchor) &&
                allocation.WidthComponentOwnerTarget(anchor) != home)
                return RecolorFailure::WidthOwner;
        }
        const auto source = allocation.result.low32_copy_sources[id];
        if (source != UINT32_MAX && (contains(id) || contains(source)) &&
            ((!contains(id) && !already_home(id)) || (!contains(source) && !already_home(source))))
            return RecolorFailure::PartialAliasGroup;
        if (!contains(id)) continue;
        if (const auto* p = allocation.CurrentGPRPlacement(id)) {
            const auto& old = old_value(id);
            const auto compatible = [&](u16 target) {
                return target == UINT16_MAX || target == home ||
                       (old.was_gpr && target == old.old_home);
            };
            if (!compatible(p->result) || !compatible(p->whole_value) || !compatible(p->low_view))
                return RecolorFailure::FixedResidence;
        }
    }
    for (const auto& [key, use] : allocation.result.gpr_use_homes) {
        const auto value = static_cast<u32>(key >> 32);
        if (!contains(value)) continue;
        const auto& old = old_value(value);
        // These entries carry independent extension facts. A caller must
        // explicitly reprove that residence rather than rename the register.
        if (old.was_gpr && old.old_home != home && use.value.home == old.old_home)
            return RecolorFailure::ConsumerHome;
    }
    for (u32 point = 0; point < allocation.result.spill_reloads.size(); ++point)
        for (const auto& reload : allocation.result.spill_reloads[point])
            if (contains(reload.value) && (point < begin || point > end))
                return RecolorFailure::OutsideWindow;

    for (const auto& old : roots) {
        if (old.was_gpr && old.old_home == home && old.was_fixed == fixed_class)
            continue;
        if (fixed_class)
            allocation.MapFixedRegister(old.id, ir::HostGPR{home});
        else
            allocation.MapRegister(old.id, ir::HostGPR{home});
    }
    for (u32 id = 0; id < allocation.result.gpr_placements.size(); ++id) {
        if (!contains(id)) continue;
        const auto& old = old_value(id);
        if (!old.was_gpr || old.old_home == home) continue;
        const auto* current = allocation.CurrentGPRPlacement(id);
        if (!current) continue;
        if (current->result != old.old_home && current->whole_value != old.old_home &&
            current->low_view != old.old_home)
            continue;
        auto& p = allocation.GPRPlacementFor(id);
        if (p.result == old.old_home) p.result = home;
        if (p.whole_value == old.old_home) p.whole_value = home;
        if (p.low_view == old.old_home) p.low_view = home;
    }
    for (u32 point = begin; point <= end && point < allocation.result.spill_reloads.size();
         ++point) {
        auto& entries = allocation.result.spill_reloads[point];
        if (std::none_of(entries.begin(), entries.end(), [&](const auto& r) {
                return contains(r.value);
            }))
            continue;
        RecordReloads(point);
        for (const auto& reload : entries)
            if (contains(reload.value)) released |= 1u << reload.reg;
        std::erase_if(entries, [&](const auto& r) { return contains(r.value); });
    }
    if (released) {
        // Rebuild only the released register bits, preserving reservations,
        // opcode/terminal clobbers, other live values, and other reload owners.
        struct Event {
            u32 point;
            u16 reg;
            s8 delta;
        };
        Vector<Event> events;
        for (u32 id = 0; id < allocation.MapCount(); ++id) {
            const auto& map = allocation.result.alloc_result[id];
            if (map.type != Allocation::GPR || !(released & (1u << map.slot))) continue;
            const auto* capture = allocation.PreservedGPRDefinitionAt(id);
            const auto birth = capture ? capture->transfer : id;
            const auto death = allocation.result.live_ends[id];
            if (death < begin || birth > end) continue;
            events.push_back({std::max(birth, begin), map.slot, 1});
            if (death < end) events.push_back({death + 1, map.slot, -1});
        }
        std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
            return a.point < b.point;
        });
        std::array<u32, 32> live{};
        size_t next = 0;
        for (const auto& site : sites) {
            while (next < events.size() && events[next].point <= site.instruction) {
                const auto& event = events[next++];
                live[event.reg] += event.delta;
            }
            auto gprs = allocation.DirtyGPR(site.instruction);
            auto fprs = allocation.DirtyFPR(site.instruction);
            for (u16 reg = 0; reg < 31; ++reg) {
                if (!(released & (1u << reg))) continue;
                bool occupied = allocation.result.gprs.Get(reg) ||
                                (site.fixed_clobbers & (1u << reg)) || live[reg];
                if (site.instruction < allocation.result.spill_reloads.size())
                    for (const auto& reload : allocation.result.spill_reloads[site.instruction])
                        occupied |= reload.reg == reg;
                if (occupied)
                    gprs.Mark(reg);
                else
                    gprs.Clear(reg);
            }
            allocation.SetActiveRegs(site.instruction, gprs, fprs);
        }
    }
    allocation.InvalidateFixedGPRHomes();
    allocation.InvalidatePreservations();
    return RecolorFailure::None;
}

}  // namespace swift::runtime::compile
