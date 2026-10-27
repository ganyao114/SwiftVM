#include "runtime/common/perf_stats.h"
#include "runtime/compile/allocate/internal.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::compile::detail {
LinearScan::LinearScan(const HIRFunction* function,
                       AllocationWriter* alloc,
                       const AllocationOptions& options,
                       const AllocationAttempt& attempt,
                       compile::SemanticFacts& facts,
                       compile::CompileWorkspace& workspace,
                       const AllocationInputs& inputs)
        : LinearScan(function, nullptr, alloc, options, attempt, facts, workspace, inputs) {}

LinearScan::LinearScan(const Block* block,
                       AllocationWriter* alloc,
                       const AllocationOptions& options,
                       const AllocationAttempt& attempt,
                       compile::SemanticFacts& facts,
                       compile::CompileWorkspace& workspace,
                       const AllocationInputs& inputs)
        : LinearScan(nullptr, block, alloc, options, attempt, facts, workspace, inputs) {}

LinearScan::LinearScan(const HIRFunction* function,
                       const Block* block,
                       AllocationWriter* alloc,
                       const AllocationOptions& options,
                       const AllocationAttempt& attempt,
                       compile::SemanticFacts& facts,
                       compile::CompileWorkspace& workspace,
                       const AllocationInputs& inputs)
        : function(function)
        , block(block)
        , reg_alloc(alloc)
        , facts(facts)
        , inputs(inputs)
        , value_liveness(facts.PrepareValueLiveness())
        , workspace(workspace)
        , gpr_reserve(attempt.gpr_reserve)
        , fpr_reserve(attempt.fpr_reserve)
        , single_block_fast_path(function && options.single_block_fast_path)
        , scalar_insert(options.scalar_insert)
        , intwidth_tie(options.intwidth_tie)
        , forced_spills(attempt.forced_spills)
        , select_eviction(attempt.select_eviction)
        , finish_on_candidate(attempt.finish_on_candidate)
        , features(inputs.features)
        , xmm_resident(options.xmm_resident) {
    workspace.BeginAttempt();
    active_gprs = alloc->GetGprs();
    active_fprs = alloc->GetFprs();
    const u32 capacity = function ? function->MaxInstrCount() : block->MaxInstrId();
    live_interval.reserve(function ? capacity : block->GetInstList().size());
    fixed_gpr_alias_end.resize(capacity);
    InitializeTargetInputs();
    if (single_block_fast_path) fast_active_lives.reserve(capacity);
}

void LinearScan::AllocateRegisters() {
    InstallInputConstraints();
    active_gprs = reg_alloc->GetGprs();
    InitializeFixedClobbers();
    // Step 1: Collect live intervals
    PerfScope2 perf_collect_live{GetPerfStats2().collect_live};
    if (function) {
        if (single_block_fast_path) {
            CollectLiveIntervalsSingleBlock(function);
        } else {
            CollectLiveIntervals(function);
        }
    } else {
        CollectLiveIntervals(block);
    }
    if (function && !single_block_fast_path) {
        const auto& cfg_ends = facts.PrepareCFGUseEnds();
        for (auto& interval : live_interval)
            if (interval.inst->Id() < cfg_ends.size())
                interval.end = std::max(interval.end, cfg_ends[interval.inst->Id()]);
    }
    for (auto& interval : live_interval) {
        if (const auto* fixed = reg_alloc->PreservedGPRDefinitionAt(interval.inst->Id())) {
            interval.start = fixed->transfer;
            interval.end = std::max(interval.end, fixed->last_use);
        }
    }
    PrepareFixedPreferences();
    perf_collect_live.Stop();

    // Step 2: Sort live intervals
    PerfScope2 perf_sort{GetPerfStats2().regalloc_sort};
    std::sort(live_interval.begin(), live_interval.end());
    perf_sort.Stop();

    // Step 3: Alloc Registers
    PerfScope2 perf_assign{GetPerfStats2().regalloc_assign};
    //
    // Instructions that define no value (VOID ops such as StoreMemory)
    // own no live interval, but the JIT still asks GetTmpX/GetTmpV for
    // scratch registers while emitting them. Those pick the first
    // register not marked in the per-instruction dirty mask
    // (Allocation::DirtyGPR/FPR), so that mask must exist for *every*
    // instruction: without it a VOID op would see an all-clear mask and
    // clobber a live value (or a reserved runtime register). Fill each
    // gap between interval starts with the register set active right
    // after the previous interval was processed. That set is a
    // conservative superset of the registers truly live at any point
    // inside the gap: no interval starts inside the gap, and an
    // interval already expired there ended before the gap began.
    const u32 instr_count = InstrCount();
    u32 next_id = 0;
    auto fill_gap = [&](u32 end) {
        end = std::min(end, instr_count);
        for (; next_id < end; next_id++) {
            RecordActiveRegs(next_id);
        }
    };
    for (auto& interval : live_interval) {
        reg_alloc->SetLiveEnd(interval.inst->Id(), interval.end);
        fill_gap(interval.start);

        ExpireOldIntervals(interval);

        if (TryAllocateCallABIGPR(interval)) {
        } else if (IsForcedSpill(interval)) {
            SpillAtInterval(interval);
        } else if (!IsFloatValue(interval.inst)) {
            if (TryTieTargetGPR(interval)) {
            } else if (auto alloc = AllocGPR(interval); alloc >= 0) {
                reg_alloc->MapRegister(interval.inst->Id(), HostGPR{(u16)alloc});
            } else {
                SelectEvictionCandidate(interval, false);
                if (has_eviction_candidate && !finish_on_candidate) return;
                SpillAtInterval(interval);
            }
        } else {
            if (TryTieFPR(interval)) {
                // The destination inherits the source's live physical
                // register. The source interval was removed from the
                // active set without freeing it, and the result interval
                // is added below. This is the post-RA move kill: no copy is
                // introduced for the emitter to remove later.
            } else if (auto alloc = AllocFPR(); alloc >= 0) {
                reg_alloc->MapRegister(interval.inst->Id(), HostFPR{(u16)alloc});
            } else {
                SelectEvictionCandidate(interval, true);
                if (has_eviction_candidate && !finish_on_candidate) return;
                SpillAtInterval(interval);
            }
        }
        // EVERY interval joins the active set, spilled ones included.
        //
        // A spilled interval used not to be recorded here, which made the
        // MEM arm of ExpireOldIntervals (and FreeSpill with it) dead code:
        // a stack slot handed out was a stack slot lost, so the high-water
        // mark tracked the *total* number of spills in a compilation unit
        // rather than the number live at once. State::spill_area holds
        // kMaxSpillSlots slots and the allocator asserts past that, so a
        // unit that merely spilled often enough — not one that needed many
        // slots at once — aborted the guest. Recycling makes the ceiling a
        // function of peak simultaneous pressure, which is the quantity the
        // 64-slot reservation was sized for.
        //
        // The lifetime model is the same one the register arms already
        // trust: `end` is the last instruction that names the value, so the
        // slot is reusable exactly when the register would have been.
        if (eviction_facts_ready) FillAllocFacts(interval);
        if (single_block_fast_path) {
            fast_active_lives.push_back(interval);
            std::push_heap(fast_active_lives.begin(),
                           fast_active_lives.end(),
                           [](const LiveInterval& left, const LiveInterval& right) {
                               return left.end > right.end;
                           });
        } else {
            active_lives.push_back(interval);
        }
        RecordActiveRegs(interval.start);
        next_id = std::max(next_id, interval.start + 1);
    }
    // Fill any remaining instructions after the last interval start.
    fill_gap(instr_count);
    if (!has_eviction_candidate || finish_on_candidate) {
        BuildSpillReloads();
    }
    perf_assign.Stop();

    ReuseFixedPublications();
    FinishCoalescing();
    if (spill_count && AllocationDiagnosticsEnabled()) {
        LOG_WARNING("Allocate: {} value(s) spilled to stack slots (highest slot {})",
                    spill_count,
                    max_spill_slot);
    }
}

void LinearScan::ExpireOldIntervals(LiveInterval& current) {
    if (single_block_fast_path) {
        const auto later_end = [](const LiveInterval& left, const LiveInterval& right) {
            return left.end > right.end;
        };
        while (!fast_active_lives.empty() && fast_active_lives.front().end < current.start) {
            std::pop_heap(fast_active_lives.begin(), fast_active_lives.end(), later_end);
            ReleaseInterval(fast_active_lives.back());
            fast_active_lives.pop_back();
        }
        return;
    }
    for (auto it = active_lives.begin(); it != active_lives.end();) {
        if (it->end < current.start) {
            ReleaseInterval(*it);
            it = active_lives.erase(it);  // Remove expired intervals
        } else {
            ++it;
        }
    }
}

void LinearScan::ReleaseInterval(const LiveInterval& interval) {
    auto value_type = reg_alloc->ValueType(ir::Value(interval.inst));
    if (value_type == AllocationWriter::GPR) {
        FreeGPR(reg_alloc->ValueGPR(interval.inst->Id()).id);
    } else if (value_type == AllocationWriter::FPR) {
        FreeFPR(reg_alloc->ValueFPR(interval.inst->Id()).id);
    } else {
        auto slot = reg_alloc->ValueMem(interval.inst->Id()).offset;
        FreeSpill(slot);
        if (IsFloatValue(interval.inst)) {
            FreeSpill(slot + 1);
            ASSERT(active_spill_fpr > 0);
            --active_spill_fpr;
        } else {
            ASSERT(active_spill_gpr > 0);
            --active_spill_gpr;
        }
    }
}

int LinearScan::AllocGPR(const LiveInterval& interval) {
    if (static_cast<u32>(active_gprs.GetClearCount()) <= gpr_reserve) {
        return -1;
    }
    const u32 forbidden = IntervalFixedClobbers(interval);
    for (u32 code = 0; code < active_gprs.GetAllCount(); ++code) {
        if (!active_gprs.Get(code) && !(forbidden & (1u << code))) {
            active_gprs.Mark(code);
            return static_cast<int>(code);
        }
    }
    return -1;
}

int LinearScan::AllocFPR() {
    if (static_cast<u32>(active_fprs.GetClearCount()) <= fpr_reserve) {
        return -1;
    }
    if (auto alloc = active_fprs.GetFirstClear(); alloc >= 0) {
        active_fprs.Mark(alloc);
        return alloc;
    }
    return -1;
}

void LinearScan::FreeGPR(u32 id) {
    ASSERT(active_gprs.Get(id));
    // A tied induction result may temporarily own a statically reserved
    // guest register. Expiry ends the SSA interval but must never return
    // that architectural register to the scratch/value pool.
    if (!reg_alloc->GetGprs().Get(id)) {
        active_gprs.Clear(id);
    }
}

void LinearScan::FreeFPR(u32 id) {
    ASSERT(active_fprs.Get(id));
    if (!reg_alloc->GetFprs().Get(id)) {
        active_fprs.Clear(id);
    }
}
}  // namespace swift::runtime::compile::detail
