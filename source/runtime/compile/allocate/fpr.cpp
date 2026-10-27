#include "internal.h"

namespace swift::runtime::compile::detail {
namespace {
using Tables = FPRInputs;

bool MappedTo(const AllocationWriter& allocation, u16 value, u16 home) {
    return value != UINT16_MAX && allocation.ValueType(value) == Allocation::FPR &&
           allocation.ValueFPR(value).id == home;
}

bool MergeHolds(const AllocationWriter& allocation,
                const Tables::Publication& publication,
                bool scalar_insert) {
    const bool mapped = MappedTo(allocation, publication.head, publication.home);
    switch (publication.merge[scalar_insert ? 1 : 0]) {
        case Tables::MergeRule::None:
            return true;
        case Tables::MergeRule::RejectIfMapped:
            return !mapped;
        case Tables::MergeRule::ReadIfMapped:
            return !mapped || allocation.IsHostReadCoalesced(publication.head);
        case Tables::MergeRule::Mapped:
            return mapped;
        case Tables::MergeRule::ReadMapped:
            return mapped && allocation.IsHostReadCoalesced(publication.head);
    }
    return false;
}

void ApplyPublication(const Tables& tables,
                      const Tables::BlockRange& block,
                      const Tables::Publication& publication,
                      AllocationWriter& allocation,
                      bool scalar_insert) {
    if (!(publication.modes & (scalar_insert ? 2 : 1))) return;
    const auto nodes = tables.Nodes(publication);
    ASSERT(!nodes.empty());
    const bool chain = nodes.size() > 1;
    const auto in_chain = [&](u16 value) {
        return std::ranges::any_of(nodes, [&](const auto& node) { return node.value == value; });
    };
    const auto first = nodes.front().value;
    if (chain) {
        if (!allocation.IsHostReadCoalesced(publication.head) ||
            !MappedTo(allocation, publication.head, publication.home))
            return;
        for (const auto& node : nodes) {
            if (allocation.ValueType(node.value) != Allocation::FPR) return;
            for (auto input : tables.Inputs(node))
                if (MappedTo(allocation, input, publication.home)) return;
        }
    } else {
        if (allocation.ValueType(first) != Allocation::FPR) return;
        const auto current = allocation.ValueFPR(first).id;
        if ((tables.resident_homes & (u32{1} << current)) && current != publication.home) return;
        if (!MergeHolds(allocation, publication, scalar_insert)) return;
        if (MappedTo(allocation, first, publication.home)) {
            allocation.MarkHostWriteCoalesced(publication.store);
            return;
        }
        if (!publication.recolor_safe) return;
    }
    for (const auto& other : tables.Values(block)) {
        if (chain) {
            if (other.value == publication.head || in_chain(other.value) ||
                other.value >= publication.store)
                continue;
        } else if (other.value == first || other.value == publication.store ||
                   other.value > publication.end)
            continue;
        if (other.end > first && MappedTo(allocation, other.value, publication.home)) return;
    }
    if (!chain)
        for (auto input : tables.Inputs(nodes.front()))
            if (MappedTo(allocation, input, publication.home)) return;
    for (const auto& node : nodes) {
        allocation.MapRegister(node.value, HostFPR{publication.home});
        if (chain) allocation.MarkAesChainTied(node.value, publication.home);
    }
    allocation.MarkHostWriteCoalesced(publication.store);
}
}  // namespace

void LinearScan::CoalesceFPRPublications(bool enabled) {
    if (!enabled || reg_alloc->FPRCoalescingDisabled() || !inputs.fpr) return;
    for (const auto& block : inputs.fpr->blocks)
        for (const auto& publication : inputs.fpr->Publications(block))
            ApplyPublication(*inputs.fpr, block, publication, *reg_alloc, scalar_insert);
}

bool LinearScan::TryTieFPR(LiveInterval& current) {
    if (reg_alloc->FPRCoalescingDisabled() || !inputs.fpr) return false;
    const auto source = inputs.fpr->TieSource(current.inst->Id(), scalar_insert);
    if (source == UINT16_MAX || source == current.inst->Id() ||
        reg_alloc->ValueType(source) != compile::AllocationWriter::FPR)
        return false;
    const auto source_fpr = reg_alloc->ValueFPR(source);
    if (reg_alloc->GetFprs().Get(source_fpr.id)) {
        // The guest register stays live at exits and faults even if this
        // SSA read dies here. Reuse it only when the same block replaces
        // its guest value before any operation can observe the old state.
        if (fixed_fpr_read_ends.empty() ||
            !inputs.fpr->ReplacesHome(current.start, source_fpr.id)) {
            return false;
        }
        // Other reads can still require the guest value before this
        // result is published, even when this particular source dies now.
        for (const auto& [read_id, read_end] : fixed_fpr_read_ends) {
            if (read_id != source && read_id < current.end && read_end > current.start &&
                reg_alloc->ValueFPR(read_id).id == source_fpr.id) {
                return false;
            }
        }
        for (const auto& write : value_liveness.FPRWrites().For(source_fpr.id))
            if (current.start < write.position && write.position < current.end) return false;
    }
    const auto fixed_source = fixed_fpr_read_ends.find(source);
    const bool transfer_fixed_source = fixed_source != fixed_fpr_read_ends.end() &&
                                       fixed_source->second == current.start &&
                                       reg_alloc->IsHostReadCoalesced(source);

    auto matches = [&](const LiveInterval& live) {
        return live.inst->Id() == source && live.end == current.start;
    };
    bool removed = false;
    if (single_block_fast_path) {
        auto it = std::find_if(fast_active_lives.begin(), fast_active_lives.end(), matches);
        if (it != fast_active_lives.end()) {
            fast_active_lives.erase(it);
            std::make_heap(fast_active_lives.begin(),
                           fast_active_lives.end(),
                           [](const LiveInterval& left, const LiveInterval& right) {
                               return left.end > right.end;
                           });
            removed = true;
        }
    } else {
        auto it = std::find_if(active_lives.begin(), active_lives.end(), matches);
        if (it != active_lives.end()) {
            active_lives.erase(it);
            removed = true;
        }
    }
    if (!removed && !transfer_fixed_source) {
        return false;
    }
    reg_alloc->MapRegister(current.inst->Id(), source_fpr);
    return true;
}
}  // namespace swift::runtime::compile::detail
