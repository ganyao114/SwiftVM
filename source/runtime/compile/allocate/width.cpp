#include "internal.h"

#include "runtime/compile/analysis/block_index.h"

namespace swift::runtime::compile::detail {
namespace {
bool Consumes(const BlockAnalysisIndex& graph, const Inst* source, const Inst* consumer) {
    const auto uses = graph.ResolvedUses(source);
    return std::any_of(
            uses.begin(), uses.end(), [&](const auto& use) { return use.consumer == consumer; });
}
}  // namespace

void LinearScan::RecipeWidthComponentOwners(const Block* lir_block) {
    if (!features.ra_width_chain || !inputs.gpr || !inputs.gpr->width) return;
    const auto* range = inputs.gpr->BlockFor(lir_block);
    if (!range) return;
    for (const auto& pub : std::span{inputs.gpr->width->publications}.subspan(range->first_width,
                                                                              range->width_count)) {
        if (!(pub.flags & WidthInputs::Owner) || !reg_alloc->GetGprs().Get(pub.home) ||
            reg_alloc->ValueType(Value{pub.produced}) != AllocationWriter::GPR ||
            reg_alloc->ValueType(Value{pub.stored}) != AllocationWriter::GPR)
            continue;
        // The first eligible publication in IR order freezes the owner, from
        // the baseline mapping. Neither later reads nor writes retry a home.
        (void)reg_alloc->FreezeWidthComponentOwner(
                pub.produced->Id(), pub.home, pub.flags & WidthInputs::HighZero);
    }
}

bool LinearScan::ValidateWidthComponentTransaction(const Block* lir_block) {
    Vector<u8> committed_store(reg_alloc->MapCount());
    for (const auto& inst : lir_block->GetInstList()) {
        if (!inst.HasValue() || !reg_alloc->IsWidthChainCoalesced(inst.Id())) continue;
        const u32 anchor = reg_alloc->WidthChainAnchor(inst.Id());
        if (reg_alloc->HasWidthComponentOwner(anchor) &&
            (!reg_alloc->WidthComponentOwnerCommitted(anchor) ||
             reg_alloc->ValueType(Value{&inst}) != AllocationWriter::GPR ||
             reg_alloc->ValueGPR(Value{&inst}).id != reg_alloc->WidthComponentOwnerTarget(anchor)))
            return false;
    }
    for (const auto& pub : inputs.PublicationsFor(lir_block)) {
        if (!reg_alloc->IsHostWriteCoalesced(pub.instruction) || pub.source == UINT16_MAX ||
            !reg_alloc->IsWidthChainCoalesced(pub.source))
            continue;
        const u32 anchor = reg_alloc->WidthChainAnchor(pub.source);
        if (!reg_alloc->HasWidthComponentOwner(anchor) ||
            !reg_alloc->WidthComponentOwnerCommitted(anchor) ||
            reg_alloc->WidthComponentOwnerTarget(anchor) != pub.home)
            return false;
        committed_store[anchor] = 1;
    }
    for (u32 anchor = 0; anchor < reg_alloc->MapCount(); ++anchor)
        if (reg_alloc->WidthComponentOwnerCommitted(anchor) && !committed_store[anchor])
            return false;
    return true;
}

Vector<u16> LinearScan::CollectLongWidthChainBridges(const Block* lir_block) {
    Vector<u16> bridges;
    if (!features.ra_width_chain_long || !inputs.gpr || !inputs.gpr->width) return bridges;
    const auto* range = inputs.gpr->BlockFor(lir_block);
    if (!range || !range->step_count) return bridges;
    const auto& width = *inputs.gpr->width;
    const auto nodes = facts.PrepareIndex(lir_block).Instructions();
    Vector<u32> steps;
    for (u32 index = range->first_step; index < range->first_step + range->step_count; ++index) {
        const auto& pub = width.publications[width.steps[index].publication];
        if (reg_alloc->ValueType(Value{pub.produced}) == AllocationWriter::GPR &&
            reg_alloc->ValueType(Value{pub.stored}) == AllocationWriter::GPR &&
            reg_alloc->ValueGPR(Value{pub.produced}).id == pub.home &&
            reg_alloc->ValueGPR(Value{pub.stored}).id == pub.home)
            steps.push_back(index);
    }
    const auto linked = [&](u32 previous, u32 current) {
        const auto& step = width.steps[current];
        const auto links = std::span{width.links}.subspan(step.first_link, step.link_count);
        return std::binary_search(links.begin(), links.end(), previous);
    };
    const auto mark = [&](size_t begin, size_t end) {
        if (end - begin < width.minimum_long_steps) return;
        for (size_t at = begin; at < end; ++at) {
            const auto& step = width.steps[steps[at]];
            for (const auto index :
                 std::span{width.views}.subspan(step.first_view, step.view_count)) {
                const auto& view = inputs.gpr->views[index];
                if (reg_alloc->ValueType(Value{view.source}) == AllocationWriter::GPR)
                    bridges.push_back(nodes[view.position]->Id());
            }
        }
    };
    size_t begin = 0;
    for (size_t at = 1; at <= steps.size(); ++at) {
        if (at < steps.size() && linked(steps[at - 1], steps[at])) continue;
        mark(begin, at);
        begin = at;
    }
    std::sort(bridges.begin(), bridges.end());
    bridges.erase(std::unique(bridges.begin(), bridges.end()), bridges.end());
    return bridges;
}

void LinearScan::CoalesceWidthChainBridges(const Block* lir_block,
                                           bool enabled,
                                           const Vector<u16>& long_bridge) {
    if (!inputs.gpr) return;
    const auto* range = inputs.gpr->BlockFor(lir_block);
    if (!range || !range->view_count) return;
    auto& graph = facts.PrepareIndex(lir_block);
    const auto nodes = graph.Instructions();
    const auto& ends = facts.PrepareBlockUses(lir_block);
    const auto publications = inputs.PublicationsFor(lir_block);
    const auto component_anchor = [&](u32 id) {
        return reg_alloc->IsWidthChainCoalesced(id) ? reg_alloc->WidthChainAnchor(id) : id;
    };
    const auto in_component = [&](u32 id, u32 anchor) {
        return id != UINT16_MAX && component_anchor(id) == anchor;
    };
    for (const auto& view : inputs.gpr->Views(*range)) {
        if (!(view.flags & GPRInputs::WidthBridge)) continue;
        const auto& bridge = *nodes[view.position];
        const auto source = view.source ? Value{view.source} : Value{};
        const bool long_candidate =
                std::binary_search(long_bridge.begin(), long_bridge.end(), bridge.Id());
        if (!enabled && !long_candidate) continue;
        const bool component_w_write = (view.flags & GPRInputs::CaptureU32) && source.Defined() &&
                                       reg_alloc->IsWidthChainCoalesced(source.Id()) &&
                                       reg_alloc->WidthChainAnchor(source.Id()) == source.Id();
        if (!source.Defined() ||
            (!(view.flags & GPRInputs::KnownWWrite) && !component_w_write &&
             !(long_candidate && (view.flags & GPRInputs::CaptureU32))) ||
            reg_alloc->ValueType(source) != AllocationWriter::GPR ||
            reg_alloc->ValueType(Value{&bridge}) != AllocationWriter::GPR ||
            bridge.Id() >= ends.size() || !bridge.GetUses() ||
            !CoalescingValueUsesStayInBlock(lir_block, source) ||
            !CoalescingValueUsesStayInBlock(lir_block, Value{&bridge}))
            continue;
        const u16 target = reg_alloc->ValueGPR(source).id;
        if (!long_candidate &&
            (target == reg_alloc->ValueGPR(Value{&bridge}).id || reg_alloc->GetGprs().Get(target)))
            continue;
        const u32 end = std::max<u32>(bridge.Id(), ends[bridge.Id()]);
        const u32 anchor = component_anchor(source.Id());
        graph.PrepareResolvedUses();
        bool blocked = false;
        for (const auto* other : nodes) {
            if (other == &bridge || !other->HasValue() || other->IsBitCastOperation() ||
                in_component(other->Id(), anchor) ||
                reg_alloc->ValueType(Value{other}) != AllocationWriter::GPR ||
                reg_alloc->ValueGPR(Value{other}).id != target)
                continue;
            const u32 other_end = other->Id() < ends.size()
                                          ? std::max<u32>(other->Id(), ends[other->Id()])
                                          : other->Id();
            if (other->Id() <= end && other_end >= bridge.Id() &&
                !(long_candidate && other->Id() == end && Consumes(graph, &bridge, other))) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;
        for (const auto* scan : nodes) {
            if (scan->Id() < bridge.Id() || scan->Id() > end) continue;
            if (scan->Id() < fixed_gpr_clobbers.size() &&
                (fixed_gpr_clobbers[scan->Id()] & (1u << target))) {
                blocked = true;
                break;
            }
        }
        for (const auto& pub : publications) {
            if (pub.instruction >= bridge.Id() && pub.instruction <= end && pub.home == target &&
                !in_component(pub.source, anchor)) {
                blocked = true;
                break;
            }
        }
        // A preceding publication proof is replayed by emission. A later
        // width decision must preserve its producer, wrapper and home window.
        if (!long_candidate && !blocked && inputs.gpr->width)
            for (const auto& pub : std::span{inputs.gpr->width->publications}.subspan(
                         range->first_width, range->width_count)) {
                if (!reg_alloc->IsHostWriteCoalesced(pub.instruction)) continue;
                if (pub.stored == &bridge || pub.produced == &bridge ||
                    (pub.produced && Consumes(graph, &bridge, pub.produced)) ||
                    (pub.home == target && bridge.Id() < pub.instruction && pub.produced &&
                     end >= pub.produced->Id())) {
                    blocked = true;
                    break;
                }
            }
        if (blocked) continue;
        struct SavedMask {
            u32 id;
            backend::GPRSMask gprs;
            backend::FPRSMask fprs;
        };
        Vector<SavedMask> saved;
        for (const auto* scan : nodes) {
            if (scan->Id() < bridge.Id() || scan->Id() > end) continue;
            auto gprs = reg_alloc->DirtyGPR(scan->Id());
            const auto fprs = reg_alloc->DirtyFPR(scan->Id());
            saved.push_back({scan->Id(), gprs, fprs});
            gprs.Mark(target);
            reg_alloc->SetActiveRegs(scan->Id(), gprs, fprs);
        }
        bool verified = true;
        for (const auto* scan : nodes)
            if (scan->Id() >= bridge.Id() && scan->Id() <= end && !CheckInstr(scan, 0, 0)) {
                verified = false;
                break;
            }
        if (!verified) {
            for (const auto& old : saved) reg_alloc->SetActiveRegs(old.id, old.gprs, old.fprs);
            continue;
        }
        reg_alloc->MapRegister(bridge.Id(), HostGPR{target});
        reg_alloc->MarkWidthChainCoalesced(bridge.Id(), anchor);
    }
}

void LinearScan::CoalescePinnedWViewInputs(const Block* lir_block) {
    if (!inputs.gpr || !inputs.PolicyFor(reg_alloc->GetGprs()).gpr_coalescing) return;
    const auto* range = inputs.gpr->BlockFor(lir_block);
    if (!range || !range->view_count || !range->w_publication_count) return;
    auto& graph = facts.PrepareIndex(lir_block);
    const auto nodes = graph.Instructions();
    const auto& ends = facts.PrepareBlockUses(lir_block);
    const auto publications = inputs.PublicationsFor(lir_block);
    const u32 pool = &inputs.PolicyFor(reg_alloc->GetGprs()) - inputs.pools.data();
    for (const auto& view : inputs.gpr->Views(*range)) {
        if (!(view.flags & GPRInputs::Low32)) continue;
        const auto& bridge = *nodes[view.position];
        if (bridge.GetUses() != 1 || reg_alloc->IsWidthChainCoalesced(bridge.Id()) ||
            bridge.Id() >= ends.size() ||
            !CoalescingValueUsesStayInBlock(lir_block, Value{&bridge}))
            continue;
        const auto source = view.source ? Value{view.source} : Value{};
        if (!source.Defined() || source.Id() >= ends.size() || ends[source.Id()] != bridge.Id() ||
            reg_alloc->ValueType(source) != AllocationWriter::GPR ||
            !CoalescingValueUsesStayInBlock(lir_block, source))
            continue;
        const u16 target = reg_alloc->ValueGPR(source).id;
        if (!(inputs.fixed_gpr_homes & (1u << target)) || !reg_alloc->GetGprs().Get(target))
            continue;
        const Inst* consumer = nullptr;
        u32 count = 0;
        graph.PrepareResolvedUses();
        for (const auto& use : graph.ResolvedUses(&bridge)) {
            consumer = use.consumer;
            count += use.count;
        }
        if (count != 1 || !consumer ||
            !(inputs.gpr->ConsumerFlags(consumer->Id()) & GPRInputs::PinnedWProducer) ||
            reg_alloc->ValueType(Value{consumer}) != AllocationWriter::GPR ||
            reg_alloc->ValueGPR(Value{consumer}).id != target ||
            ends[bridge.Id()] != consumer->Id())
            continue;
        const auto candidates = std::span{inputs.gpr->w_publications}.subspan(
                range->first_w_publication, range->w_publication_count);
        if (std::none_of(candidates.begin(), candidates.end(), [&](const auto& pub) {
                return pub.instruction > consumer->Id() && pub.home == target &&
                       pub.producer == consumer->Id() &&
                       reg_alloc->IsHostWriteCoalesced(pub.instruction);
            }))
            continue;
        const u32 anchor = reg_alloc->IsWidthChainCoalesced(source.Id())
                                   ? reg_alloc->WidthChainAnchor(source.Id())
                                   : source.Id();
        const u32 end = ends[bridge.Id()];
        bool interfered = false;
        for (const auto& pub : publications) {
            if (pub.instruction > bridge.Id() && pub.instruction <= end && pub.home == target &&
                pub.source != bridge.Id() && pub.source != consumer->Id() &&
                pub.source != source.Id()) {
                interfered = true;
                break;
            }
        }
        if (interfered) continue;
        for (const auto* other : nodes) {
            if (!other->HasValue() || other->IsBitCastOperation() || other == &bridge ||
                other == consumer || other->Id() >= ends.size() ||
                reg_alloc->ValueType(Value{other}) != AllocationWriter::GPR ||
                reg_alloc->ValueGPR(Value{other}).id != target)
                continue;
            const bool same_component =
                    other->Id() == anchor || (reg_alloc->IsWidthChainCoalesced(other->Id()) &&
                                              reg_alloc->WidthChainAnchor(other->Id()) == anchor);
            if (!same_component && other->Id() <= end && ends[other->Id()] >= bridge.Id()) {
                interfered = true;
                break;
            }
        }
        if (interfered) continue;
        const auto old = reg_alloc->Mapping(bridge.Id());
        if (old.type != AllocationWriter::GPR) continue;
        if (view.fixed_class_pools & (1u << pool))
            reg_alloc->MapFixedRegister(bridge.Id(), HostGPR{target});
        else
            reg_alloc->MapRegister(bridge.Id(), HostGPR{target});
        if (!CheckInstr(&bridge, 0, 0) || !CheckInstr(consumer, 0, 0)) {
            if (old.fixed_gpr)
                reg_alloc->MapFixedRegister(bridge.Id(), HostGPR{static_cast<u16>(old.slot)});
            else
                reg_alloc->MapRegister(bridge.Id(), HostGPR{static_cast<u16>(old.slot)});
            continue;
        }
        reg_alloc->MarkWidthChainCoalesced(bridge.Id(), anchor);
    }
}

void LinearScan::CoalesceWidthChains() {
    if (!features.ra_width_chain && !features.ra_width_chain_long) return;
    const auto coalesce = [&](const Block* lir_block) {
        const auto bridges = CollectLongWidthChainBridges(lir_block);
        CoalesceWidthChainBridges(lir_block, features.ra_width_chain, bridges);
    };
    if (function)
        for (const auto& member : function->GetHIRBlocksRPO()) coalesce(member.GetBlock());
    else
        coalesce(block);
}
}  // namespace swift::runtime::compile::detail
