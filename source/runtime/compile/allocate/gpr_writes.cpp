#include "internal.h"

#include "runtime/compile/analysis/block_index.h"

namespace swift::runtime::compile::detail {

void LinearScan::CoalesceGuestGPRWrites(const Block* lir_block, bool width_enabled) {
    if (!inputs.gpr || !inputs.gpr->writes ||
        !inputs.PolicyFor(reg_alloc->GetGprs()).gpr_coalescing)
        return;
    const auto* range = inputs.gpr->BlockFor(lir_block);
    if (!range || !range->write_count) return;
    const auto& data = *inputs.gpr->writes;
    const auto nodes = facts.PrepareIndex(lir_block).Instructions();
    const auto& ends = facts.PrepareBlockUses(lir_block);
    const auto publications = inputs.PublicationsFor(lir_block);
    const auto captures =
            std::span{data.captures}.subspan(range->first_capture, range->capture_count);
    const u32 pool = &inputs.PolicyFor(reg_alloc->GetGprs()) - inputs.pools.data();
    const auto mapped_to = [&](Value value, u16 target) {
        return value.Defined() && reg_alloc->ValueType(value) == AllocationWriter::GPR &&
               reg_alloc->ValueGPR(value).id == target;
    };
    const auto capture_at = [&](u32 id, u16 home) {
        const auto found = std::lower_bound(
                captures.begin(), captures.end(), id, [](const auto& row, u32 instruction) {
                    return row.instruction < instruction;
                });
        return found != captures.end() && found->instruction == id && found->home == home;
    };
    for (const auto& candidate :
         std::span{data.writes}.subspan(range->first_write, range->write_count)) {
        if (!(candidate.modes & (width_enabled ? GPRWriteInputs::Width : GPRWriteInputs::Baseline)))
            continue;
        const auto& store = *nodes[candidate.position];
        const u16 target = candidate.home;
        if (!reg_alloc->GetGprs().Get(target)) continue;
        const auto* producer = candidate.produced;
        const auto* wrapper = candidate.stored;
        const Value produced{producer}, stored{wrapper};
        const bool zero_extend = candidate.modes & GPRWriteInputs::ZeroExtend;
        const bool width_root = width_enabled && (candidate.modes & GPRWriteInputs::WidthRoot);
        if (reg_alloc->ValueType(produced) != AllocationWriter::GPR ||
            !CoalescingValueUsesStayInBlock(lir_block, produced) ||
            (zero_extend && !CoalescingValueUsesStayInBlock(lir_block, stored)))
            continue;
        if (width_root && (!reg_alloc->HasWidthComponentOwner(producer->Id()) ||
                           reg_alloc->WidthComponentOwnerTarget(producer->Id()) != target))
            continue;
        if (width_root && std::any_of(captures.begin(), captures.end(), [&](const auto& read) {
                return read.home == target && reg_alloc->IsHostReadCoalesced(read.instruction);
            }))
            continue;
        Vector<u32> component;
        if (width_root) {
            component.push_back(producer->Id());
            // A narrow load joins only after its actual location is known.
            // IDs can name a valid predecessor outside this block as before.
            if (candidate.narrow_load != UINT32_MAX &&
                reg_alloc->ValueType(candidate.narrow_load) == AllocationWriter::GPR)
                component.push_back(candidate.narrow_load);
            if (zero_extend) component.push_back(wrapper->Id());
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& view : inputs.gpr->Views(*range)) {
                    if (!(view.flags & GPRInputs::WidthBridge) || !view.source) continue;
                    const u32 id = nodes[view.position]->Id();
                    if (std::find(component.begin(), component.end(), id) == component.end() &&
                        std::find(component.begin(), component.end(), view.source->Id()) !=
                                component.end()) {
                        component.push_back(id);
                        changed = true;
                    }
                }
            }
            if (std::any_of(component.begin(), component.end(), [&](u32 id) {
                    return reg_alloc->IsHostReadCoalesced(id);
                }))
                continue;
        }
        if (mapped_to(produced, target) && (!zero_extend || mapped_to(stored, target))) continue;
        bool blocked = false;
        for (const auto* other : nodes) {
            if (other == producer || other == wrapper || other == &store || !other->HasValue() ||
                other->IsBitCastOperation())
                continue;
            if (other->Id() > store.Id() && capture_at(other->Id(), target)) continue;
            if (reg_alloc->FixedGPRPublicationOwns(store.Id(), other->Id())) continue;
            const u32 end = other->Id() < ends.size() ? ends[other->Id()] : other->Id();
            if (reg_alloc->FixedGPRHomeIntersects(
                        Value{other}, target, producer->Id(), candidate.live_end, end)) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;
        // This is a physical home check on raw operands, not target matching.
        // Alias resolution uses the same generic IR identity as the use graph.
        producer->ForEachValue([&](Value input) {
            const auto root = ResolveBitCastSource(input);
            if (mapped_to(root, target) && root.Id() < ends.size() &&
                ends[root.Id()] > producer->Id())
                blocked = true;
        });
        if (blocked) continue;
        if (width_root) {
            u32 start = producer->Id(), end = producer->Id();
            for (const auto id : component) {
                start = std::min(start, id);
                end = std::max(end, id < ends.size() ? std::max(id, ends[id]) : id);
            }
            const auto contains = [&](u32 id) {
                return std::find(component.begin(), component.end(), id) != component.end();
            };
            for (const auto* other : nodes) {
                if (contains(other->Id()) || other == &store || !other->HasValue() ||
                    other->IsBitCastOperation() || !mapped_to(Value{other}, target))
                    continue;
                const u32 other_end = other->Id() < ends.size()
                                              ? std::max<u32>(other->Id(), ends[other->Id()])
                                              : other->Id();
                if (other->Id() <= end && other_end >= start) {
                    blocked = true;
                    break;
                }
            }
            for (const auto* scan : nodes) {
                if (blocked || scan->Id() < start || scan->Id() > end) continue;
                if ((scan->Id() < fixed_gpr_clobbers.size() &&
                     (fixed_gpr_clobbers[scan->Id()] & (1u << target))) ||
                    (scan->Id() < producer->Id() &&
                     (inputs.gpr->ConsumerFlags(scan->Id()) & GPRInputs::PublicationObserver) &&
                     !contains(scan->Id()))) {
                    blocked = true;
                    break;
                }
            }
            for (const auto& pub : publications)
                if (pub.instruction >= start && pub.instruction <= end &&
                    pub.instruction != store.Id() && pub.home == target) {
                    blocked = true;
                    break;
                }
            if (blocked) continue;
            struct SavedMask {
                u32 id;
                backend::GPRSMask gprs;
                backend::FPRSMask fprs;
            };
            Vector<SavedMask> saved;
            for (const auto* scan : nodes) {
                if (scan->Id() < start || scan->Id() > end) continue;
                auto gprs = reg_alloc->DirtyGPR(scan->Id());
                const auto fprs = reg_alloc->DirtyFPR(scan->Id());
                saved.push_back({scan->Id(), gprs, fprs});
                gprs.Mark(target);
                reg_alloc->SetActiveRegs(scan->Id(), gprs, fprs);
            }
            bool verified = true;
            for (const auto* scan : nodes)
                if (scan->Id() >= start && scan->Id() <= end && !CheckInstr(scan, 0, 0)) {
                    verified = false;
                    break;
                }
            if (!verified) {
                for (const auto& old : saved) reg_alloc->SetActiveRegs(old.id, old.gprs, old.fprs);
                continue;
            }
        }
        if (width_root && !reg_alloc->CommitWidthComponentOwner(producer->Id(), target)) continue;
        const auto map = [&](u32 id) {
            if (candidate.fixed_class_pools & (1u << pool))
                reg_alloc->MapFixedRegister(id, HostGPR{target});
            else
                reg_alloc->MapRegister(id, HostGPR{target});
        };
        map(producer->Id());
        if (zero_extend) map(wrapper->Id());
        reg_alloc->MarkHostWriteCoalesced(store.Id());
        if (width_root)
            for (const auto id : component) {
                map(id);
                reg_alloc->MarkWidthChainCoalesced(id, producer->Id());
            }
    }
}
}  // namespace swift::runtime::compile::detail
