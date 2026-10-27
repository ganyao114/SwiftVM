#include "runtime/common/perf_stats.h"
#include "runtime/compile/allocate/internal.h"

namespace swift::runtime::compile::detail {
void LinearScan::CollectSpillUses() {
    if (spill_uses) return;
    PerfScope2 perf_evict_collect{GetPerfStats2().evict_collect};
    spill_uses = &facts.PrepareSpillUses();
    workspace.PrepareSpillCursor(InstrCount());
}

void LinearScan::FillAllocFacts(LiveInterval& interval) const {
    interval.alloc_kind = reg_alloc->ValueType(Value{interval.inst});
    interval.alloc_code = interval.alloc_kind == AllocationWriter::GPR
                                  ? reg_alloc->ValueGPR(interval.inst->Id()).id
                          : interval.alloc_kind == AllocationWriter::FPR
                                  ? reg_alloc->ValueFPR(interval.inst->Id()).id
                                  : u16{};
}

void LinearScan::SelectEvictionCandidate(const LiveInterval& current, bool is_float) {
    PerfScope2 perf_evict_select{GetPerfStats2().evict_select};
    if (!select_eviction || has_eviction_candidate) {
        return;
    }
    // A retry spills the entire interval, including uses before this
    // pressure point. Account for that cost before comparing next uses.
    // Build this index only on pressure; spill-free units pay no extra walk.
    CollectSpillUses();
    if (!eviction_facts_ready) {
        eviction_facts_ready = true;
        for (auto& active : active_lives) FillAllocFacts(active);
        for (auto& active : fast_active_lives) FillAllocFacts(active);
    }
    auto cheaper = [&](const LiveInterval& left, const LiveInterval& right) {
        const auto& left_uses = (*spill_uses)[left.inst->Id()];
        const auto& right_uses = (*spill_uses)[right.inst->Id()];
        if (left_uses.size() != right_uses.size()) {
            return left_uses.size() < right_uses.size();
        }
        auto next_use = [&](const LiveInterval& interval, const auto& uses) {
            // Intervals are visited in start order, so each value's
            // next-use cursor only moves forward across evaluations.
            auto& cursor = workspace.SpillCursor()[interval.inst->Id()];
            while (cursor < uses.size() && uses[cursor] < current.start) ++cursor;
            return cursor == uses.size() ? UINT32_MAX : uses[cursor];
        };
        const auto left_next = next_use(left, left_uses);
        const auto right_next = next_use(right, right_uses);
        if (left_next != right_next) return left_next > right_next;
        if (left.end != right.end) return left.end > right.end;
        return left.inst->Id() < right.inst->Id();
    };
    const u32 forbidden = is_float ? 0 : IntervalFixedClobbers(current);
    const LiveInterval* best = &current;
    auto consider = [&](const LiveInterval& active) {
        if (is_float ? active.alloc_kind != AllocationWriter::FPR
                     : active.alloc_kind != AllocationWriter::GPR) {
            return;
        }
        const u32 code = active.alloc_code;
        if (is_float) {
            if (reg_alloc->GetFprs().Get(code)) {
                return;
            }
        } else {
            // A fixed-home/tied static register cannot be returned to the
            // value pool, and a register forbidden across the incoming
            // interval would not make that interval allocatable anyway.
            if (reg_alloc->GetGprs().Get(code) || (forbidden & (1u << code))) {
                return;
            }
        }
        if (cheaper(active, *best)) {
            best = &active;
        }
    };
    if (single_block_fast_path) {
        for (const auto& active : fast_active_lives) {
            consider(active);
        }
    } else {
        for (const auto& active : active_lives) {
            consider(active);
        }
    }
    if (best != &current) {
        has_eviction_candidate = true;
        eviction_candidate = best->inst->Id();
    }
}

void LinearScan::GrowSpillStack(u32 new_item_size) {
    spill_slots.resize(spill_slots.size() + new_item_size);
}

void LinearScan::SpillAtInterval(LiveInterval& interval) {
    auto is_float = IsFloatValue(interval.inst);
    auto slot_size = is_float ? 2 : 1;
    u32 slot{};
    if (is_float) {
        s32 found{-1};
        for (int i = 0; i + 1 < spill_slots.size(); i += 2) {
            if (!spill_slots[i] && !spill_slots[i + 1]) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            // A SIMD spill occupies two consecutive u64 slots and the JIT
            // accesses it with a 16-byte Ldr/Str. State::spill_area is
            // alignas(16), so the pair must start on an even slot for the
            // scaled (multiple-of-16) offset form to encode; pad by one
            // when a preceding scalar spill left the stack odd-sized. The
            // pad slot stays free and is reclaimed by the next GPR spill.
            if (spill_slots.size() & 1u) {
                GrowSpillStack(1);
            }
            found = spill_slots.size();
            GrowSpillStack(slot_size);
        }
        slot = found;
        reg_alloc->MapMemSpill(interval.inst->Id(), ir::SpillSlot{static_cast<u16>(slot)});
        // Both halves must be marked: the scalar path below scans every
        // index, so leaving slot+1 clear would hand the upper half of this
        // 16-byte value to a later GPR spill and let the two destroy each
        // other on every reload.
        spill_slots[slot] = true;
        spill_slots[slot + 1] = true;
    } else {
        auto itr = std::find(spill_slots.begin(), spill_slots.end(), false);
        if (itr != spill_slots.end()) {
            slot = std::distance(spill_slots.begin(), itr);
            reg_alloc->MapMemSpill(interval.inst->Id(), ir::SpillSlot{static_cast<u16>(slot)});
            spill_slots[slot] = true;
        } else {
            // Grow the stack. The new slot is the first appended index,
            // i.e. the size *before* growing — NOT spill_slot_cursor,
            // which still holds the index handed out by the previous
            // grow and would alias this value onto its predecessor's
            // slot (silently, since both stay in range).
            slot = spill_slots.size();
            GrowSpillStack(slot_size);
            reg_alloc->MapMemSpill(interval.inst->Id(), ir::SpillSlot{static_cast<u16>(slot)});
            spill_slots[slot] = true;
        }
    }
    // The JIT keeps spilled values in State::spill_area, which holds
    // exactly kMaxSpillSlots u64 slots (a spilled SIMD value takes two).
    // Fail loudly instead of handing out a slot that would silently
    // overwrite the uniform buffer following the spill area.
    ASSERT_MSG(slot + slot_size <= kMaxSpillSlots,
               "spill area exhausted: slot {} (+{}) >= {} reserved slots",
               slot,
               slot_size,
               kMaxSpillSlots);
    spill_count++;
    if (is_float) {
        ++active_spill_fpr;
    } else {
        ++active_spill_gpr;
    }
    max_spill_slot = std::max(max_spill_slot, slot + slot_size - 1);
}

void LinearScan::FreeSpill(u32 slot) {
    ASSERT(spill_slots[slot]);
    spill_slots[slot] = false;
}
}  // namespace swift::runtime::compile::detail
