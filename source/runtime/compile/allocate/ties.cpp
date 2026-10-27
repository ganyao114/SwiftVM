#include "internal.h"

namespace swift::runtime::compile::detail {
void LinearScan::InstallInputConstraints() {
    for (u32 homes = inputs.declared_gprs; homes; homes &= homes - 1)
        reg_alloc->DeclareGuestGPR(std::countr_zero(homes));
    if (!reg_alloc->FixedGPRConstraintsReady()) {
        for (const auto& entry : inputs.fixed)
            reg_alloc->PlanFixedGPRDefinition(entry.value, entry.definition);
        reg_alloc->FinishFixedGPRConstraints();
    }
    if (FlagsRegsEnabled()) {
        if (spill_slots.size() < 2) {
            GrowSpillStack(2 - spill_slots.size());
        }
        spill_slots[0] = true;
        spill_slots[1] = true;
        max_spill_slot = std::max<u32>(max_spill_slot, 1);
    }
}

bool LinearScan::TryTieGPR(LiveInterval& current, u32 source, bool allow_fixed_owner) {
    if (source == UINT16_MAX || source == current.inst->Id() ||
        reg_alloc->ValueType(source) != compile::AllocationWriter::GPR) {
        return false;
    }
    // Extending a residence through a result must respect that result's
    // physical clobbers, even when the source's shorter interval was safe.
    const auto home = reg_alloc->ValueGPR(source).id;
    if (IntervalFixedClobbers(current) & (1u << home)) {
        bool own_publication = allow_fixed_owner;
        for (u32 point = current.start; point <= current.end && point < fixed_gpr_clobbers.size();
             ++point)
            if (fixed_gpr_clobbers[point] & (u32{1} << home))
                own_publication &= inputs.Publishes(point, current.inst->Id(), home);
        if (!own_publication) return false;
    }

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
    if (!removed) {
        if (!allow_fixed_owner || source >= fixed_gpr_alias_end.size() ||
            fixed_gpr_alias_end[source] != current.start) {
            return false;
        }
        const auto source_gpr = reg_alloc->ValueGPR(source);
        if (!reg_alloc->GetGprs().Get(source_gpr.id)) {
            return false;
        }
    }
    reg_alloc->MapRegister(current.inst->Id(), reg_alloc->ValueGPR(source));
    return true;
}

bool LinearScan::TryTieTargetGPR(LiveInterval& interval) {
    for (const auto& tie : inputs.TiesFor(interval.inst->Id())) {
        if (tie.integer_width && !intwidth_tie) continue;
        if (TryTieGPR(interval, tie.source, tie.allow_fixed_owner)) return true;
    }
    return false;
}

bool LinearScan::DirectlyFeedsMemory(const Inst* inst) const {
    return inputs.DirectlyFeedsMemory(inst->Id());
}

bool LinearScan::TryAllocateCallABIGPR(LiveInterval& interval) {
    if (!function || IsFloatValue(interval.inst)) return false;
    const auto target = inputs.PreferredGPR(interval.inst->Id());
    if (target == UINT16_MAX || active_gprs.Get(target) ||
        (IntervalFixedClobbers(interval) & (u32{1} << target)))
        return false;
    active_gprs.Mark(target);
    reg_alloc->MapRegister(interval.inst->Id(), HostGPR{target});
    return true;
}

void LinearScan::MapFixedRead(u32 id, u16 target) {
    if (fixed_class && (inputs.fixed_gpr_homes & (u32{1} << target))) {
        reg_alloc->MapFixedRegister(id, HostGPR{target});
    } else {
        reg_alloc->MapRegister(id, HostGPR{target});
    }
}

void LinearScan::InitializeTargetInputs() {
    fixed_class = inputs.PolicyFor(reg_alloc->GetGprs()).fixed_class;
    InitializeFixedClobbers();
}

void LinearScan::InitializeFixedClobbers() {
    fixed_gpr_clobbers.assign(InstrCount(), 0);
    for (auto& points : fixed_gpr_clobber_points) points.clear();
    for (const auto& site : inputs.clobbers)
        if (site.instruction < fixed_gpr_clobbers.size())
            fixed_gpr_clobbers[site.instruction] |= site.mask;
    if (inputs.UsesDeclaredPool(reg_alloc->GetGprs()))
        for (const auto& site : inputs.declared_clobber_deltas)
            if (site.instruction < fixed_gpr_clobbers.size())
                fixed_gpr_clobbers[site.instruction] ^= site.mask;
    for (const auto& publication : inputs.publications)
        if (publication.instruction < fixed_gpr_clobbers.size() &&
            !reg_alloc->IsHostWriteCoalesced(publication.instruction))
            fixed_gpr_clobbers[publication.instruction] |= u32{1} << publication.home;
    const auto add_placements = [&](const Block* block) {
        for (const auto& inst : block->GetInstList()) {
            if (inst.Id() >= fixed_gpr_clobbers.size()) continue;
            if (const auto* fixed = reg_alloc->FixedGPRDefinitionAt(inst.Id());
                fixed && !fixed->elided)
                fixed_gpr_clobbers[inst.Id()] |= u32{1} << fixed->home;
        }
    };
    if (function)
        for (const auto* block : function->GetHIRBlocks()) add_placements(block->GetBlock());
    else
        add_placements(block);
    fixed_gpr_clobber_codes = 0;
    for (u32 id = 0; id < fixed_gpr_clobbers.size(); ++id) {
        u32 bits = fixed_gpr_clobbers[id];
        fixed_gpr_clobber_codes |= bits;
        while (bits) {
            const auto code = std::countr_zero(bits);
            fixed_gpr_clobber_points[code].push_back(id);
            bits &= bits - 1;
        }
    }
}

void LinearScan::RecordActiveRegs(u32 id) {
    auto base_gprs = reg_alloc->GetGprs();
    auto base_fprs = reg_alloc->GetFprs();
    const u32 allocated_gprs = active_gprs.GetMarkedCount() - base_gprs.GetMarkedCount();
    const u32 allocated_fprs = active_fprs.GetMarkedCount() - base_fprs.GetMarkedCount();
    max_live_gpr = std::max(max_live_gpr, allocated_gprs + active_spill_gpr);
    max_live_fpr = std::max(max_live_fpr, allocated_fprs + active_spill_fpr);
    auto gprs = active_gprs;
    if (id < fixed_gpr_clobbers.size()) {
        u32 fixed = fixed_gpr_clobbers[id];
        while (fixed) {
            gprs.Mark(std::countr_zero(fixed));
            fixed &= fixed - 1;
        }
    }
    reg_alloc->SetActiveRegs(id, gprs, active_fprs);
}

u32 LinearScan::IntervalFixedClobbers(const LiveInterval& interval) const {
    u32 result = 0;
    // Only codes with at least one clobber point can intersect.
    u32 codes = fixed_gpr_clobber_codes;
    while (codes) {
        const auto code = std::countr_zero(codes);
        codes &= codes - 1;
        const auto& points = fixed_gpr_clobber_points[code];
        const auto it = std::lower_bound(points.begin(), points.end(), interval.start);
        if (it != points.end() && *it <= interval.end) {
            result |= 1u << code;
        }
    }
    return result;
}
}  // namespace swift::runtime::compile::detail
