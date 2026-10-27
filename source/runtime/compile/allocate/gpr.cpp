#include "internal.h"

namespace swift::runtime::compile::detail {
namespace {
// Physical copy decisions borrow the request tables and the unique raw-use
// index. No target opcode classification or callback is needed here.
class GPRCopyCoalescing {
public:
    GPRCopyCoalescing(const Block* block,
                      AllocationWriter& allocation,
                      const AllocationInputs& input,
                      const GPRInputs::BlockRange& range,
                      const BlockUseEnds& ends,
                      BlockAnalysisIndex& uses)
            : block(block)
            , allocation(allocation)
            , input(input)
            , data(*input.gpr)
            , range(range)
            , ends(ends)
            , uses(uses) {}

    bool LiveView(const Inst& bridge, Value source) {
        if (!source.Defined() || bridge.GetUses() == 0 || bridge.Id() >= ends.size() ||
            source.Id() >= ends.size() || allocation.ValueType(source) != AllocationWriter::GPR ||
            allocation.ValueType(Value{&bridge}) != AllocationWriter::GPR)
            return false;
        const auto source_reg = allocation.ValueGPR(source).id;
        const auto bridge_reg = allocation.ValueGPR(Value{&bridge}).id;
        const auto end = ends[bridge.Id()];
        if (Crosses(bridge.Id(), end, source_reg) || Crosses(bridge.Id(), end, bridge_reg) ||
            source_reg == bridge_reg || end <= bridge.Id() || ends[source.Id()] < end)
            return false;
        for (const auto& scan : block->GetInstList()) {
            if (scan.Id() < bridge.Id()) continue;
            if (scan.Id() > end) break;
            if (!allocation.DirtyGPR(scan.Id()).Get(source_reg)) return false;
        }
        bool has_consumer = false;
        uses.PrepareUses();
        for (const auto& use : uses.Uses(&bridge)) {
            const auto& scan = *use.consumer;
            if (scan.Id() < bridge.Id() || scan.Id() > end) continue;
            has_consumer = true;
            if (!ConsumerFits(scan, source_reg, bridge_reg)) return false;
        }
        return has_consumer;
    }

    bool Transfer(const Inst& bridge, Value source) {
        if (!FinalView(bridge, source)) return false;
        const auto source_reg = allocation.ValueGPR(source).id;
        const auto bridge_reg = allocation.ValueGPR(Value{&bridge}).id;
        const auto end = ends[bridge.Id()];
        if (Crosses(bridge.Id(), end, source_reg) || Crosses(bridge.Id(), end, bridge_reg) ||
            source_reg == bridge_reg)
            return false;
        for (const auto& other : block->GetInstList()) {
            if (other.Id() <= bridge.Id() || other.Id() > end || !other.HasValue()) continue;
            if (allocation.ValueType(Value{&other}) == AllocationWriter::GPR &&
                allocation.ValueGPR(Value{&other}).id == source_reg)
                return false;
        }
        uses.PrepareUses();
        for (const auto& use : uses.Uses(&bridge)) {
            const auto& scan = *use.consumer;
            if (scan.Id() < bridge.Id() || scan.Id() > end) continue;
            if ((data.ConsumerFlags(scan.Id()) & GPRInputs::ForbidTransfer) ||
                !ConsumerFits(scan, source_reg, bridge_reg))
                return false;
        }
        if (!Rewrite(bridge.Id(), end, bridge_reg, source_reg)) return false;
        allocation.MapReference(source.Id(), bridge.Id());
        allocation.MarkLow32CopyCoalesced(bridge.Id(), source.Id());
        return true;
    }

    bool Recolor(const Inst& bridge, Value source, bool source_allowed) {
        if (!source_allowed || !FinalView(bridge, source) ||
            allocation.IsHostReadCoalesced(source.Id()) ||
            allocation.IsHostWriteCoalesced(source.Id()) ||
            allocation.AllocationId(source) != source.Id() ||
            allocation.AllocationId(Value{&bridge}) != bridge.Id())
            return false;
        const auto source_reg = allocation.ValueGPR(source).id;
        const auto bridge_reg = allocation.ValueGPR(Value{&bridge}).id;
        if (Crosses(bridge.Id(), ends[bridge.Id()], source_reg) ||
            Crosses(bridge.Id(), ends[bridge.Id()], bridge_reg) || source_reg == bridge_reg ||
            Crosses(source.Id() + 1, bridge.Id(), bridge_reg))
            return false;
        if (Overlaps(bridge_reg, source.Id(), bridge.Id(), source.Id(), bridge.Id()) ||
            Overlaps(source_reg, source.Id(), bridge.Id(), source.Id(), bridge.Id()))
            return false;
        if (!Rewrite(source.Id(), bridge.Id(), source_reg, bridge_reg)) return false;
        allocation.MapReference(bridge.Id(), source.Id());
        allocation.MarkLow32CopyCoalesced(bridge.Id(), source.Id());
        return true;
    }

    bool Check(const Inst& inst) const {
        return CheckInstructionScratch(
                allocation, inst, input.ScratchFor(inst.Id(), allocation.GetGprs()));
    }

private:
    bool FinalView(const Inst& bridge, Value source) const {
        return source.Defined() && bridge.GetUses() != 0 && source.Id() < ends.size() &&
               bridge.Id() < ends.size() && ends[source.Id()] == bridge.Id() &&
               ends[bridge.Id()] > bridge.Id() && !allocation.IsFixedGPR(source.Id()) &&
               !allocation.IsFixedGPR(bridge.Id()) &&
               allocation.ValueType(source) == AllocationWriter::GPR &&
               allocation.ValueType(Value{&bridge}) == AllocationWriter::GPR;
    }
    bool Crosses(u32 begin, u32 end, u16 home) const {
        return data.CrossesClobber(range, begin, end, home);
    }
    bool ConsumerFits(const Inst& inst, u16 source, u16 bridge) const {
        const auto type = allocation.ValueType(Value{&inst});
        if (type == AllocationWriter::GPR) {
            const auto home = allocation.ValueGPR(Value{&inst}).id;
            return home != source && home != bridge;
        }
        return type != AllocationWriter::NONE ||
               (data.ConsumerFlags(inst.Id()) & GPRInputs::AllowNoResult);
    }
    bool Rewrite(u32 begin, u32 end, u16 from, u16 to) {
        struct ActiveRegs {
            u32 id;
            backend::GPRSMask gprs;
            backend::FPRSMask fprs;
        };
        Vector<ActiveRegs> changed;
        for (const auto& inst : block->GetInstList()) {
            if (inst.Id() < begin) continue;
            if (inst.Id() > end) break;
            auto gprs = allocation.DirtyGPR(inst.Id());
            const auto fprs = allocation.DirtyFPR(inst.Id());
            changed.push_back({inst.Id(), gprs, fprs});
            gprs.Clear(from);
            gprs.Mark(to);
            allocation.SetActiveRegs(inst.Id(), gprs, fprs);
            if (!Check(inst)) {
                for (const auto& old : changed)
                    allocation.SetActiveRegs(old.id, old.gprs, old.fprs);
                return false;
            }
        }
        return true;
    }
    bool Overlaps(u16 home, u32 begin, u32 end, u32 first_exempt, u32 second_exempt) const {
        for (const auto& value : block->GetInstList()) {
            if (!value.HasValue() || allocation.ValueType(Value{&value}) != AllocationWriter::GPR ||
                allocation.ValueGPR(Value{&value}).id != home)
                continue;
            const auto owner = allocation.AllocationId(Value{&value});
            if (owner == first_exempt || owner == second_exempt || owner > end) continue;
            if (owner >= begin ||
                (begin > 0 && allocation.ValueLiveAfter(Value{&value}, begin - 1)))
                return true;
        }
        return false;
    }
    const Block* block;
    AllocationWriter& allocation;
    const AllocationInputs& input;
    const GPRInputs& data;
    const GPRInputs::BlockRange& range;
    const BlockUseEnds& ends;
    BlockAnalysisIndex& uses;
};
}  // namespace

void LinearScan::CoalesceLow32Copies() {
    if (!inputs.gpr || !inputs.PolicyFor(reg_alloc->GetGprs()).gpr_coalescing) return;
    const auto coalesce = [&](const Block* lir_block) {
        const auto* range = inputs.gpr->BlockFor(lir_block);
        if (!range || !range->view_count) return;
        auto& graph = facts.PrepareIndex(lir_block);
        const auto nodes = graph.Instructions();
        const auto& ends = facts.PrepareBlockUses(lir_block);
        GPRCopyCoalescing physical{lir_block, *reg_alloc, inputs, *range, ends, graph};
        for (const auto& candidate : inputs.gpr->Views(*range)) {
            if (!(candidate.flags & GPRInputs::Low32)) continue;
            const auto& bridge = *nodes[candidate.position];
            if (reg_alloc->IsWidthChainCoalesced(bridge.Id()) ||
                reg_alloc->IsLow32CopyCoalesced(bridge.Id()) ||
                !CoalescingValueUsesStayInBlock(lir_block, Value{&bridge}))
                continue;
            const auto source = candidate.source ? Value{candidate.source} : Value{};
            if (const auto* fixed = reg_alloc->FixedGPRDefinitionAt(bridge.Id());
                fixed && !fixed->preserved)
                continue;
            if (!CoalescingValueUsesStayInBlock(lir_block, source)) continue;
            if (physical.LiveView(bridge, source)) {
                reg_alloc->MapReference(source.Id(), bridge.Id());
                reg_alloc->MarkLow32CopyCoalesced(bridge.Id(), source.Id());
                continue;
            }
            if (physical.Transfer(bridge, source) ||
                physical.Recolor(bridge, source, candidate.flags & GPRInputs::RecolorSource))
                continue;
            if (candidate.wrapper == UINT32_MAX) continue;
            const auto& wrapper = *nodes[candidate.wrapper];
            if (reg_alloc->IsWidthChainCoalesced(wrapper.Id()) || !source.Defined() ||
                reg_alloc->ValueType(source) != AllocationWriter::GPR ||
                reg_alloc->ValueType(Value{&wrapper}) != AllocationWriter::GPR)
                continue;
            const auto source_reg = reg_alloc->ValueGPR(source).id;
            if (source_reg == reg_alloc->ValueGPR(Value{&wrapper}).id) continue;
            const auto old_gprs = reg_alloc->DirtyGPR(wrapper.Id());
            const auto fprs = reg_alloc->DirtyFPR(wrapper.Id());
            auto checked = old_gprs;
            checked.Mark(source_reg);
            reg_alloc->SetActiveRegs(wrapper.Id(), checked, fprs);
            if (!physical.Check(bridge) || !physical.Check(wrapper)) {
                reg_alloc->SetActiveRegs(wrapper.Id(), old_gprs, fprs);
                continue;
            }
            reg_alloc->MapReference(source.Id(), bridge.Id());
            reg_alloc->MarkLow32CopyCoalesced(bridge.Id(), source.Id());
        }
    };
    if (function)
        for (const auto* member : function->GetHIRBlocks()) coalesce(member->GetBlock());
    else
        coalesce(block);
}

void LinearScan::CoalesceGuestGPRReads(const Block* lir_block) {
    if (!inputs.gpr || !inputs.PolicyFor(reg_alloc->GetGprs()).gpr_coalescing) return;
    const auto* range = inputs.gpr->BlockFor(lir_block);
    if (!range || !range->read_count) return;
    const auto& ends = facts.PrepareBlockUses(lir_block);
    const auto nodes = facts.PrepareIndex(lir_block).Instructions();
    const u32 pool = &inputs.PolicyFor(reg_alloc->GetGprs()) - inputs.pools.data();
    for (const auto& candidate : inputs.gpr->Reads(*range)) {
        const auto& read = *nodes[candidate.position];
        if (reg_alloc->IsWidthChainCoalesced(read.Id()) ||
            !CoalescingValueUsesStayInBlock(lir_block, Value{&read}) ||
            !reg_alloc->GetGprs().Get(candidate.home) || read.Id() >= ends.size())
            continue;
        const auto read_end = std::max<u32>(read.Id(), ends[read.Id()]);
        bool conflict = false;
        for (const auto& node : lir_block->GetInstList()) {
            if (!node.HasValue() || !reg_alloc->IsWidthChainCoalesced(node.Id()) ||
                reg_alloc->ValueType(Value{&node}) != AllocationWriter::GPR ||
                reg_alloc->ValueGPR(Value{&node}).id != candidate.home)
                continue;
            const auto node_end =
                    node.Id() < ends.size() ? std::max<u32>(node.Id(), ends[node.Id()]) : node.Id();
            conflict |= node.Id() <= read_end && node_end >= read.Id();
        }
        if (conflict) continue;
        bool high_zero = candidate.high_zero;
        if (candidate.source && reg_alloc->IsWidthChainCoalesced(candidate.source->Id())) {
            const auto anchor = reg_alloc->WidthChainAnchor(candidate.source->Id());
            if (reg_alloc->HasWidthComponentOwner(anchor)) {
                if (!reg_alloc->WidthComponentOwnerCommitted(anchor) ||
                    !reg_alloc->IsHostWriteCoalesced(candidate.publication) ||
                    reg_alloc->WidthComponentOwnerTarget(anchor) != candidate.home)
                    continue;
                high_zero = reg_alloc->WidthComponentOwnerHighZero(anchor);
            }
        }
        if (!high_zero) continue;
        if (candidate.fixed_class_pools & (1u << pool))
            reg_alloc->MapFixedRegister(read.Id(), HostGPR{candidate.home});
        else
            reg_alloc->MapRegister(read.Id(), HostGPR{candidate.home});
        reg_alloc->MarkHostReadCoalesced(read.Id());
    }
}
}  // namespace swift::runtime::compile::detail
