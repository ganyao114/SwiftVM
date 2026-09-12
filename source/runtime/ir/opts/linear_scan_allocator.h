#include "base/logging.h"
#pragma once
#include "register_alloc_internal.h"
#include "register_alloc_spill_reload.h"
#include "allocation_statistics.h"
#include "base/logging.h"
#include "runtime/backend/arm64/pshufd_direct.h"
#include "runtime/common/perf_stats.h"
namespace swift::runtime::ir {
class RegisterAllocTestSupport;
namespace {
// Spill/escalation 诊断打印默认关闭：它们随编译线程异步产生，会混入 guest
// stdout（sqlite speedtest 一类边跑边输出的负载会被打断行），且非用户可行动项。
// 需要排查 regalloc 行为时设 SVM_RA_DIAG=1 打开。
static bool RaDiagEnabled() {
    return GetSvmConfig().ra_diag;
}

using HostRegWriteMap = Map<u16, Vector<u32>>;

static bool LiveRangeCrossesHostRegWrite(const HostRegWriteMap& writes,
                                         u16 host_reg,
                                         u32 def_id,
                                         u32 use_end) {
    auto it = writes.find(host_reg);
    if (it == writes.end()) {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(), [def_id, use_end](u32 set_id) {
        return def_id < set_id && set_id <= use_end;
    });
}

struct AllocationOptions {
    FeatureSet features{};
    bool single_block_fast_path{};
    bool scalar_insert{};
    bool intwidth_tie{};
    bool induct_tie{};
    bool xmm_resident{GetSvmConfig().xmm_resident};
};

struct AllocationAttempt {
    u32 gpr_reserve{};
    u32 fpr_reserve{};
    const Vector<u32>* forced_spills{};
    bool select_eviction{};
};

class LinearScanAllocator {
public:
    explicit LinearScanAllocator(HIRFunction* function, backend::RegAlloc* alloc,
                                 const AllocationOptions& options,
                                 const AllocationAttempt& attempt)
        : LinearScanAllocator(function, nullptr, alloc, options, attempt) {}

    explicit LinearScanAllocator(Block* block, backend::RegAlloc* alloc,
                                 const AllocationOptions& options,
                                 const AllocationAttempt& attempt)
        : LinearScanAllocator(nullptr, block, alloc, options, attempt) {}

private:
    LinearScanAllocator(HIRFunction* function, Block* block, backend::RegAlloc* alloc,
                        const AllocationOptions& options,
                        const AllocationAttempt& attempt)
        : function(function), block(block), reg_alloc(alloc),
          gpr_reserve(attempt.gpr_reserve), fpr_reserve(attempt.fpr_reserve),
          single_block_fast_path(function && options.single_block_fast_path),
          scalar_insert(options.scalar_insert), intwidth_tie(options.intwidth_tie),
          induct_tie(options.induct_tie), forced_spills(attempt.forced_spills),
          select_eviction(attempt.select_eviction),
          shift_imm_fast(options.features.shift_imm_fast), features(options.features),
          xmm_resident(options.xmm_resident) {
        active_gprs = alloc->GetGprs();
        active_fprs = alloc->GetFprs();
        const u32 capacity = function ? function->MaxInstrCount() : block->MaxInstrId();
        live_interval.reserve(function ? capacity : block->GetInstList().size());
        fixed_gpr_alias_end.resize(capacity);
        fixed_gpr_affinity.resize(capacity, UINT16_MAX);
        fixed_gpr_affinity_store.resize(capacity, UINT32_MAX);
        call_abi_affinity.resize(capacity, UINT16_MAX);
        fixed_class = backend::FixedGPRClassEnabled(alloc->GetGprs(), features);
        InitializeFixedClobbers();
        if (function) InitializeBlockLocalValues();
        if (single_block_fast_path) fast_active_lives.reserve(capacity);
    }

public:

    [[nodiscard]] u32 SpillCount() const { return spill_count; }
    [[nodiscard]] u32 SpillHighWater() const {
        return spill_count ? max_spill_slot + 1 : 0;
    }
    [[nodiscard]] u32 MaxLiveGPR() const { return max_live_gpr; }
    [[nodiscard]] u32 MaxLiveFPR() const { return max_live_fpr; }
    [[nodiscard]] u32 GPRReserve() const { return gpr_reserve; }
    [[nodiscard]] u32 FPRReserve() const { return fpr_reserve; }
    [[nodiscard]] u32 FixedAffinityReads() const { return fixed_affinity_reads; }
    [[nodiscard]] u32 FixedAffinityWrites() const { return fixed_affinity_writes; }
    [[nodiscard]] u32 FixedHazards() const { return fixed_hazards; }
    [[nodiscard]] u32 FixedEvictions() const { return fixed_evictions; }
    [[nodiscard]] u32 FixedCopiesElided() const { return fixed_copies_elided; }
    [[nodiscard]] bool HasEvictionCandidate() const { return has_eviction_candidate; }
    [[nodiscard]] u32 EvictionCandidate() const {
        ASSERT(has_eviction_candidate);
        return eviction_candidate;
    }
    friend class ::swift::runtime::ir::RegisterAllocTestSupport;

    static u32 ScratchOnlyGPRs(OpCode op, const backend::GPRSMask& pool,
                               const FeatureSet& features) {
        u32 count = backend::X86PinExtLevel3AluScratchEnabled(pool, op) ? 1u : 0u;
        const bool level2_scratch =
                backend::X86PinExtScratchOnlyEnabled(pool, features);
        if (level2_scratch) {
            const u32 fixed = backend::FixedGPRClobbers(op, features, true);
            const bool last_result_pinned = GetSvmConfig().flags_regs;
            count += ((fixed & (1u << 12)) || last_result_pinned ? 0u : 1u) +
                     ((fixed & (1u << 13)) ? 0u : 1u);
        }
        return count;
    }

    // Re-reads the masks this scan recorded and confirms each instruction is
    // left enough free registers for its emitter (backend::ScratchBudget) plus
    // one reload register per distinct spilled value it names. This is exactly
    // the condition JitContext::GetTmpX/GetTmpV depend on, checked against the
    // data the JIT will actually consume rather than against a model of it.
    [[nodiscard]] bool Verify() {
        // Fast path, and the one every unit in the corpus takes: nothing was
        // spilled, so no instruction needs a reload register, and AllocGPR /
        // AllocFPR already refused to drop below `*_reserve` at every mask
        // they recorded. Only an opcode whose budget exceeds that reserve can
        // be short, which is a switch per instruction and no more.
        if (spill_count == 0) {
            bool ok = true;
            auto check_budgets = [&](Block* lir_block) {
                for (auto& inst : lir_block->GetInstList()) {
                    auto need = backend::ScratchBudget(inst, features);
                    if (need.gpr <= gpr_reserve && need.fpr <= fpr_reserve) {
                        continue;
                    }
                    if (inst.Id() >= reg_alloc->MapCount()) {
                        continue;
                    }
                    ok &= static_cast<u32>(reg_alloc->DirtyGPR(inst.Id()).GetClearCount()) +
                                          ScratchOnlyGPRs(inst.GetOp(),
                                                          reg_alloc->GetGprs(), features) >=
                                  need.gpr &&
                          static_cast<u32>(reg_alloc->DirtyFPR(inst.Id()).GetClearCount()) >=
                                  need.fpr;
                }
            };
            if (function) {
                for (auto* hir_block : function->GetHIRBlocks()) {
                    check_budgets(hir_block->GetBlock());
                }
            } else {
                check_budgets(block);
            }
            return ok;
        }
        bool ok = true;
        auto check_block = [&](Block* lir_block) {
            u32 last_id = 0;
            for (auto& inst : lir_block->GetInstList()) {
                last_id = std::max<u32>(last_id, inst.Id());
                ok &= CheckInstr(&inst, 0, 0);
            }
            // The block terminal is emitted after the last instruction's
            // TickIR and shares its mask, so any spilled value the terminal
            // reads reloads into that instruction's remaining headroom.
            u32 term_gpr = 0, term_fpr = 0;
            CountTerminalReloads(lir_block->GetTerminal(), term_gpr, term_fpr);
            if ((term_gpr || term_fpr) && last_id < reg_alloc->MapCount()) {
                for (auto& inst : lir_block->GetInstList()) {
                    if (inst.Id() == last_id) {
                        ok &= CheckInstr(&inst, term_gpr, term_fpr);
                        break;
                    }
                }
            }
        };
        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                check_block(hir_block->GetBlock());
            }
        } else {
            check_block(block);
        }
        return ok;
    }

    void AllocateRegisters() {
        if (FlagsRegsEnabled()) {
            if (spill_slots.size() < 2) {
                GrowSpillStack(2 - spill_slots.size());
            }
            spill_slots[0] = true;
            spill_slots[1] = true;
            max_spill_slot = std::max<u32>(max_spill_slot, 1);
        }
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
        CollectResidentFPRWrites();
        RecipeFixedGPRAffinities();
        RecipeCallReturnAffinities();
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
        // (RegAlloc::GetDirtyGPR/FPR), so that mask must exist for *every*
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

            if (TryAllocateCallABIGPR(interval) ||
                TryAllocateFixedGPR(interval)) {
            } else if (IsForcedSpill(interval)) {
                SpillAtInterval(interval);
            } else if (!IsFloatValue(interval.inst)) {
                if (TryTieMemoryOperand(interval)) {
                    // A single-use direct GetOperand and its source share a
                    // register. The GetOperand result then owns that register
                    // until the memory operation, so the emitter can remove
                    // the direct copy without extending the source SSA's
                    // live range.
                } else if (TryTieNarrowLoad(interval)) {
                    // A basic narrow load and its extension chain share one W/X
                    // register. The ARM64 emitter can then replace
                    // LDRH+SXTH (or LDRB+UXTB) with the extending load itself,
                    // and a following 32->64 W write becomes a true no-op.
                } else if (intwidth_tie && TryTieIntWidth(interval)) {
                    // A proven W-clean source dies exactly at an direct
                    // 32-bit bridge. Transfer its physical register to the
                    // result so the existing SharesGPR emitter path removes
                    // the bridge without extending the source interval.
                } else if (induct_tie && TryTieInduction(interval)) {
                    // A fixed guest GPR capture dies at this U64 induction
                    // Add. Transfer its pinned owner to the result; the
                    // emitter still requires SharesGPR before peeling.
                } else if (auto alloc = AllocGPR(interval); alloc >= 0) {
                    reg_alloc->MapRegister(interval.inst->Id(), HostGPR{(u16)alloc});
                } else {
                    SelectEvictionCandidate(interval, false);
                    SpillAtInterval(interval);
                }
            } else {
                if (TryTieScalarInsert(interval)) {
                    // The destination inherits the source's live physical
                    // register. The source interval was removed from the
                    // active set without freeing it, and the result interval
                    // is added below. This is the post-RA move kill: no copy is
                    // introduced for the emitter to remove later.
                } else if (auto alloc = AllocFPR(); alloc >= 0) {
                    reg_alloc->MapRegister(interval.inst->Id(), HostFPR{(u16)alloc});
                } else {
                    SelectEvictionCandidate(interval, true);
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
            if (single_block_fast_path) {
                fast_active_lives.push_back(interval);
                std::push_heap(fast_active_lives.begin(), fast_active_lives.end(),
                               [](const LiveInterval& left, const LiveInterval& right) {
                                   return left.end > right.end;
                               });
            } else {
                active_lives.push_back(interval);
            }
            RecordActiveRegs(interval.inst->Id());
            next_id = std::max(next_id, interval.start + 1);
        }
        // Fill any remaining instructions after the last interval start.
        fill_gap(instr_count);
        if (!has_eviction_candidate) {
            if (function) {
                RecipespillReloadRegions(function, reg_alloc, features);
            } else {
                RecipespillReloadRegions(block, reg_alloc, features);
            }
        }
        perf_assign.Stop();

        FusePinnedWriteChains();
        if (features.ra_width_chain) {
            CoalesceGuestRegisterAccessesAndWidthChains();
        } else {
            CoalesceGuestRegisterAccesses();
            CoalesceWidthChains();
        }
        CoalesceLow32Copies();
        CoalesceGuestFPRAccesses(xmm_resident);
        RecognizePshufdDirect();
        CacheConstantAddresses();
        if (spill_count && RaDiagEnabled()) {
            LOG_WARNING("RegisterAllocPass: {} value(s) spilled to stack slots (highest slot {})",
                        spill_count, max_spill_slot);
        }
    }

private:
    void RecipeCallReturnAffinities() {
        if (!function || reg_alloc->GetGprs().Get(14)) {
            return;
        }
        for (auto* hir_block : function->GetHIRBlocks()) {
            for (auto& inst : hir_block->GetBlock()->GetInstList()) {
                if (inst.GetOp() != OpCode::CallReturn) {
                    continue;
                }
                const auto value = ResolveBitCastSource(inst.GetArg<Value>(0));
                if (value.Defined() && value.Id() < call_abi_affinity.size() &&
                    !IsFloatValue(value.Def())) {
                    call_abi_affinity[value.Id()] = 14;
                }
            }
        }
    }

    bool TryAllocateCallABIGPR(LiveInterval& interval) {
        if (!function || IsFloatValue(interval.inst) ||
            interval.inst->Id() >= call_abi_affinity.size()) {
            return false;
        }
        const u16 target = call_abi_affinity[interval.inst->Id()];
        if (target == UINT16_MAX ||
            active_gprs.Get(target) ||
            (IntervalFixedClobbers(interval) & (1u << target))) {
            return false;
        }
        active_gprs.Mark(target);
        reg_alloc->MapRegister(interval.inst->Id(), HostGPR{target});
        return true;
    }

    void MapFixedRead(u32 id, u16 target) {
        if (fixed_class && backend::IsFixedGPRHome(target)) {
            reg_alloc->MapFixedRegister(id, HostGPR{target});
        } else {
            reg_alloc->MapRegister(id, HostGPR{target});
        }
    }

    void InitializeBlockLocalValues() {
        block_local_values.resize(function->MaxInstrCount(), true);
        auto mark_use = [&](HIRBlock* user, Value value) {
            value = ResolveBitCastSource(value);
            if (!value.Defined() || value.Id() >= block_local_values.size()) {
                return;
            }
            auto* definition = function->GetHIRValue(value);
            if (!definition || definition->block != user) {
                block_local_values[value.Id()] = false;
            }
        };
        for (auto* hir_block : function->GetHIRBlocks()) {
            auto* lir_block = hir_block->GetBlock();
            for (auto& inst : lir_block->GetInstList()) {
                for (auto value : inst.GetValues()) {
                    mark_use(hir_block, value);
                }
            }
            auto walk_terminal = [&](auto&& self, const Terminal& terminal_value) -> void {
                VisitVariant<void>(terminal_value, [&](const auto& edge) {
                    using T = std::decay_t<decltype(edge)>;
                    if constexpr (std::is_same_v<T, terminal::If>) {
                        mark_use(hir_block, edge.cond);
                        self(self, edge.then_);
                        self(self, edge.else_);
                    } else if constexpr (std::is_same_v<T, terminal::Switch>) {
                        mark_use(hir_block, edge.value);
                        for (const auto& item : edge.cases) {
                            self(self, item.then);
                        }
                    } else if constexpr (std::is_same_v<T, terminal::Condition>) {
                        self(self, edge.then_);
                        self(self, edge.else_);
                    } else if constexpr (std::is_same_v<T, terminal::CheckHalt>) {
                        self(self, edge.else_);
                    }
                });
            };
            walk_terminal(walk_terminal, lir_block->GetTerminal());
        }
    }

    [[nodiscard]] bool ValueUsesStayInBlock(Block* lir_block, Value value) const {
        if (!function) {
            return true;
        }
        value = ResolveBitCastSource(value);
        if (!value.Defined() || value.Id() >= block_local_values.size() ||
            !block_local_values[value.Id()]) {
            return false;
        }
        auto* definition = function->GetHIRValue(value);
        return definition && definition->block->GetBlock() == lir_block;
    }

    void RecipeFixedGPRAffinities() {
        if (!fixed_class) {
            return;
        }
        auto recipe_block = [&](Block* lir_block) {
            auto use_end = CollectGuestGPRUseEnds(lir_block, InstrCount());
            auto& list = lir_block->GetInstList();

            // Reverse-pass SRA state: a candidate store may publish its source
            // in the fixed home only when no later/earlier access in the
            // producer-to-store window creates RAW or WAW ambiguity.  Reads
            // already proven alias-safe by CollectLiveIntervals seed the same
            // physical-class range table.
            for (auto& read : list) {
                if (read.GetOp() != OpCode::GetHostGPR ||
                    read.GetArg<Imm>(1).Get() != 0 ||
                    GetValueSizeByte(read.ReturnType()) != sizeof(u64) ||
                    !reg_alloc->IsFixedGPR(read.Id())) {
                    continue;
                }
                const u32 target = read.GetArg<Imm>(0).Get();
                const u32 end = read.Id() < use_end.size()
                        ? std::max<u32>(read.Id(), use_end[read.Id()])
                        : read.Id();
                fixed_ranges[target].push_back({read.Id(), end});
                ++fixed_affinity_reads;
                ++fixed_copies_elided;
            }

            for (auto& store : list) {
                if (store.GetOp() != OpCode::SetHostGPR ||
                    store.GetArg<Imm>(2).Get() != 0) {
                    continue;
                }
                const u32 target = store.GetArg<Imm>(1).Get();
                if (!backend::IsFixedGPRHome(target) ||
                    !reg_alloc->GetGprs().Get(target)) {
                    continue;
                }
                const auto stored = ResolveBitCastSource(store.GetArg<Value>(0));
                auto* producer = stored.Def();
                if (!producer || producer->IsBitCastOperation() ||
                    !IsPinnedCoalesceProducer(producer->GetOp()) ||
                    GetValueSizeByte(stored.Type()) != sizeof(u64) ||
                    stored.Id() >= use_end.size() ||
                    use_end[stored.Id()] != store.Id() ||
                    producer->GetUses() != 1 ||
                    stored.Id() >= fixed_gpr_affinity.size()) {
                    continue;
                }

                bool hazard = false;
                for (auto& scan : list) {
                    if (scan.Id() < producer->Id() || scan.Id() >= store.Id()) {
                        continue;
                    }
                    if (&scan != producer &&
                        (IsPinnedCoalesceObserver(scan.GetOp()) ||
                         (scan.GetOp() == OpCode::GetHostGPR &&
                          scan.GetArg<Imm>(0).Get() == target) ||
                         (scan.GetOp() == OpCode::SetHostGPR &&
                          scan.GetArg<Imm>(1).Get() == target))) {
                        hazard = true;
                        break;
                    }
                    if (scan.Id() < fixed_gpr_clobbers.size() &&
                        (fixed_gpr_clobbers[scan.Id()] & (1u << target))) {
                        hazard = true;
                        break;
                    }
                }
                if (!hazard) {
                    for (auto input : producer->GetValues()) {
                        auto root = ResolveBitCastSource(input);
                        if (root.Defined() && root.Id() < use_end.size() &&
                            reg_alloc->ValueType(root) == backend::RegAlloc::GPR &&
                            reg_alloc->ValueGPR(root).id == target &&
                            use_end[root.Id()] > producer->Id()) {
                            hazard = true;
                            break;
                        }
                    }
                }
                if (!hazard) {
                    for (const auto& range : fixed_ranges[target]) {
                        if (range.first <= store.Id() &&
                            range.second >= producer->Id()) {
                            hazard = true;
                            break;
                        }
                    }
                }
                if (hazard) {
                    ++fixed_hazards;
                    continue;
                }

                fixed_gpr_affinity[stored.Id()] = static_cast<u16>(target);
                fixed_gpr_affinity_store[stored.Id()] = store.Id();
                fixed_ranges[target].push_back({producer->Id(), store.Id()});
            }
        };
        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                recipe_block(hir_block->GetBlock());
            }
        } else {
            recipe_block(block);
        }
    }

    bool TryAllocateFixedGPR(LiveInterval& interval) {
        if (!fixed_class || IsFloatValue(interval.inst) ||
            interval.inst->Id() >= fixed_gpr_affinity.size()) {
            return false;
        }
        const u16 target = fixed_gpr_affinity[interval.inst->Id()];
        if (target == UINT16_MAX) {
            return false;
        }
        // Forward-pass conflict eviction: an overlapping fixed owner keeps its
        // architectural home; the incoming candidate falls back to the value
        // class and retains the ordinary SetHostGPR copy.  No instruction is
        // reordered, and no already-mapped SSA value changes residence.
        auto conflicts = [&](const LiveInterval& active) {
            return reg_alloc->ValueType(Value{active.inst}) == backend::RegAlloc::GPR &&
                   reg_alloc->IsFixedGPR(active.inst->Id()) &&
                   reg_alloc->ValueGPR(active.inst->Id()).id == target;
        };
        const bool conflict = single_block_fast_path
                ? std::any_of(fast_active_lives.begin(), fast_active_lives.end(), conflicts)
                : std::any_of(active_lives.begin(), active_lives.end(), conflicts);
        if (conflict || (IntervalFixedClobbers(interval) & (1u << target))) {
            ++fixed_evictions;
            return false;
        }
        reg_alloc->MapFixedRegister(interval.inst->Id(), HostGPR{target});
        fixed_gpr_alias_end[interval.inst->Id()] = interval.end;
        const u32 store_id = fixed_gpr_affinity_store[interval.inst->Id()];
        if (store_id != UINT32_MAX) {
            reg_alloc->MarkHostWriteCoalesced(store_id);
            ++fixed_affinity_writes;
            ++fixed_copies_elided;
        }
        return true;
    }

    bool IsForcedSpill(const LiveInterval& interval) const {
        return forced_spills &&
               std::find(forced_spills->begin(), forced_spills->end(),
                         interval.inst->Id()) != forced_spills->end();
    }

    void SelectEvictionCandidate(const LiveInterval& current, bool is_float) {
        if (!select_eviction || has_eviction_candidate) {
            return;
        }
        const u32 forbidden = is_float ? 0 : IntervalFixedClobbers(current);
        const LiveInterval* best = nullptr;
        auto consider = [&](const LiveInterval& active) {
            if (active.end <= current.end) {
                return;
            }
            const auto type = reg_alloc->ValueType(Value{active.inst});
            if (is_float ? type != backend::RegAlloc::FPR
                         : type != backend::RegAlloc::GPR) {
                return;
            }
            if (is_float) {
                const u32 code = reg_alloc->ValueFPR(active.inst->Id()).id;
                if (reg_alloc->GetFprs().Get(code)) {
                    return;
                }
            } else {
                const u32 code = reg_alloc->ValueGPR(active.inst->Id()).id;
                // A fixed-home/tied static register cannot be returned to the
                // value pool, and a register forbidden across the incoming
                // interval would not make that interval allocatable anyway.
                if (reg_alloc->GetGprs().Get(code) || (forbidden & (1u << code))) {
                    return;
                }
            }
            if (!best || active.end > best->end ||
                (active.end == best->end && active.inst->Id() < best->inst->Id())) {
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
        if (best) {
            has_eviction_candidate = true;
            eviction_candidate = best->inst->Id();
        }
    }

    void FusePinnedWriteChains() {
        const bool skip = GetSvmConfig().no_fuse_pin_writes;
        if (skip || !backend::X86PinExtLevel2Enabled(reg_alloc->GetGprs())) {
            return;
        }
        auto fuse_block = [&](Block* lir_block) {
            auto& list = lir_block->GetInstList();
            // Full-width exchanges are represented as mutually dependent
            // GetHost/SetHost pairs. Moving any neighbouring 32-bit producer
            // into a static destination before those pairs have consumed
            // their captures can create an implicit physical-register cycle.
            // Leave such blocks entirely to the ordinary copy-preserving path.
            const bool has_full_static_exchange_shape =
                    std::any_of(list.begin(), list.end(), [](Inst& inst) {
                        if (inst.GetOp() != OpCode::SetHostGPR) {
                            return false;
                        }
                        const auto value = inst.GetArg<Value>(0);
                        return GetValueSizeByte(value.Type()) == sizeof(u64) &&
                               (!value.Def() ||
                                value.Def()->GetOp() != OpCode::ZeroExtend32To64);
                    });
            if (has_full_static_exchange_shape) {
                return;
            }
            auto observes_host = [](Inst& inst, u32 host) {
                if ((inst.GetOp() == OpCode::GetHostGPR &&
                     inst.GetArg<Imm>(0).Get() == host) ||
                    (inst.GetOp() == OpCode::SetHostGPR &&
                     inst.GetArg<Imm>(1).Get() == host)) {
                    return true;
                }
                for (auto value : inst.GetValues()) {
                    value = ResolveBitCastSource(value);
                    if (value.Def() && value.Def()->GetOp() == OpCode::GetHostGPR &&
                        value.Def()->GetArg<Imm>(0).Get() == host) {
                        return true;
                    }
                }
                return false;
            };
            for (auto zext = list.begin(); zext != list.end(); ++zext) {
                if (zext->GetOp() != OpCode::ZeroExtend32To64) {
                    continue;
                }
                const auto source = zext->GetArg<Value>(0);
                if (!source.Def() || GetValueSizeByte(source.Type()) != sizeof(u32)) {
                    continue;
                }
                auto store = list.end();
                for (auto scan = std::next(zext); scan != list.end(); ++scan) {
                    bool uses_zext = false;
                    for (auto value : scan->GetValues()) {
                        uses_zext |= value.Def() == zext.operator->();
                    }
                    if (uses_zext) {
                        store = scan;
                        break;
                    }
                }
                if (store == list.end() || store->GetOp() != OpCode::SetHostGPR ||
                    store->GetArg<Value>(0).Def() != zext.operator->() ||
                    store->GetArg<Imm>(2).Get() != 0) {
                    continue;
                }
                const u32 target = store->GetArg<Imm>(1).Get();
                if (!(target <= 9 || target == 22 || target == 23 || target == 29)) {
                    continue;
                }

                u32 source_uses = 0;
                u32 zext_uses = 0;
                bool after_store = false;
                bool target_overwritten = false;
                bool zext_used_after_overwrite = false;
                for (auto scan = list.begin(); scan != list.end(); ++scan) {
                    for (auto value : scan->GetValues()) {
                        source_uses += value.Def() == source.Def();
                        if (value.Def() == zext.operator->()) {
                            zext_uses++;
                            zext_used_after_overwrite |= target_overwritten;
                        }
                    }
                    if (after_store && scan->GetOp() == OpCode::SetHostGPR &&
                        scan->GetArg<Imm>(1).Get() == target) {
                        target_overwritten = true;
                    }
                    after_store |= scan.operator->() == store.operator->();
                }
                if (zext_uses == 0 || zext_used_after_overwrite) {
                    continue;
                }
                // Both producer/value must be block-local. GetUses() is global
                // to the HIR function, while the counts above cover this block.
                // Equality therefore excludes an unseen successor/predecessor
                // consumer before any fixed-register remap is attempted.
                if (zext->GetUses() != zext_uses ||
                    source.Def()->GetUses() != source_uses) {
                    continue;
                }

                bool target_observed_before_store = false;
                for (auto scan = std::next(zext); scan != store; ++scan) {
                    target_observed_before_store |= observes_host(*scan, target);
                }
                if (target_observed_before_store) {
                    continue;
                }

                // Publish the zext directly in the static target. It remains a
                // valid capture for later SSA users because the scan above
                // rejected every crossing write.
                reg_alloc->MapRegister(zext->Id(), HostGPR{static_cast<u16>(target)});

                auto producer = list.end();
                for (auto scan = list.begin(); scan != zext; ++scan) {
                    if (scan.operator->() == source.Def()) {
                        producer = scan;
                        break;
                    }
                }
                if (producer == list.end()) {
                    continue;
                }
                // Multi-use producers are normally left in their allocated
                // capture. A fixed-register read is the useful exception:
                // after copying source->target, later SSA users may read the
                // target until the next architectural target write. CRC's
                // initial EDI capture has exactly this shape.
                if (source_uses != 1 && producer->GetOp() != OpCode::GetHostGPR) {
                    continue;
                }
                bool source_used_after_target_write = false;
                bool later_target_write = false;
                for (auto scan = std::next(store); scan != list.end(); ++scan) {
                    if (scan->GetOp() == OpCode::SetHostGPR &&
                        scan->GetArg<Imm>(1).Get() == target) {
                        later_target_write = true;
                    }
                    if (later_target_write) {
                        for (auto value : scan->GetValues()) {
                            source_used_after_target_write |= value.Def() == source.Def();
                        }
                    }
                }
                if (source_used_after_target_write) {
                    continue;
                }
                switch (producer->GetOp()) {
                    case OpCode::GetHostGPR: {
                        const u32 source_host = producer->GetArg<Imm>(0).Get();
                        bool source_written = false;
                        bool source_used_after_write = false;
                        for (auto scan = std::next(producer); scan != list.end(); ++scan) {
                            if (scan->GetOp() == OpCode::SetHostGPR &&
                                scan->GetArg<Imm>(1).Get() == source_host) {
                                source_written = true;
                            }
                            if (source_written) {
                                for (auto value : scan->GetValues()) {
                                    source_used_after_write |= value.Def() == source.Def();
                                }
                            }
                        }
                        // Ordinary one-use moves keep the strict no-source-
                        // write rule, which rejects both halves of XCHG. The
                        // one multi-use exception is a level-2 source copied
                        // into a W55 callee-saved pin, with every capture use
                        // before the source changes (the CRC EDI->EDX chain).
                        const bool safe_level2_capture =
                                source_uses > 1 && source_host <= 9 &&
                                (target == 22 || target == 23 || target == 29) &&
                                !source_used_after_write;
                        if (source_written && !safe_level2_capture) {
                            continue;
                        }
                        break;
                    }
                    case OpCode::Add:
                    case OpCode::Sub:
                    case OpCode::And:
                    case OpCode::Or:
                    case OpCode::Xor:
                        break;
                    default:
                        continue;
                }
                for (auto scan = std::next(producer); scan != zext; ++scan) {
                    target_observed_before_store |= observes_host(*scan, target);
                }
                if (!target_observed_before_store) {
                    reg_alloc->MapRegister(
                            producer->Id(), HostGPR{static_cast<u16>(target)});
                }
            }
        };
        if (function) {
            // Remap only chains whose global use counts prove that every
            // producer/value consumer is inside the block being scanned.
            for (auto* hir_block : function->GetHIRBlocks()) {
                fuse_block(hir_block->GetBlock());
            }
        } else {
            fuse_block(block);
        }
    }

    static bool IsPshufdDirectMask(const Inst& inst) {
        if (inst.GetOp() != OpCode::VecLoadConst) {
            return false;
        }
        const u64 low = inst.GetArg<Imm>(0).Get();
        const u64 high = inst.GetArg<Imm>(1).Get();
        return backend::arm64::DecodePshufdDirectControl(low, high).has_value();
    }

    void CacheConstantAddresses() {
        if (!features.const_addr_cache) {
            return;
        }

        const auto& config = GetSvmConfig();
        const bool audit = config.ra_shape_prof_is_set &&
                           config.ra_shape_prof != "0";
        const u64 unit_pc = function
                ? function->GetFunction()->GetStartLocation().Value()
                : block->GetStartLocation().Value();

        RegisterAllocFamilyCallbacks callbacks{
                this,
                [](void* context, Inst* inst, u32 extra_gpr, u32 extra_fpr) {
                    return static_cast<LinearScanAllocator*>(context)->CheckInstr(
                            inst, extra_gpr, extra_fpr);
                },
                [](void* context, Inst* inst) {
                    return static_cast<LinearScanAllocator*>(context)->DirectlyFeedsMemory(inst);
                }};
        auto cache_block = [&](Block* lir_block) {
            CacheConstantAddressesForBlock(lir_block, reg_alloc, unit_pc, audit, callbacks);
        };

        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                cache_block(hir_block->GetBlock());
            }
        } else {
            cache_block(block);
        }
    }

    void CoalesceWidthChains() {
        if (!features.ra_width_chain && !features.ra_width_chain_long) {
            return;
        }

        RegisterAllocFamilyCallbacks callbacks{
                this,
                [](void* context, Inst* inst, u32 extra_gpr, u32 extra_fpr) {
                    return static_cast<LinearScanAllocator*>(context)->CheckInstr(
                            inst, extra_gpr, extra_fpr);
                },
                nullptr,
                [](void* context, Block* lir_block, Value value) {
                    return static_cast<LinearScanAllocator*>(context)
                            ->ValueUsesStayInBlock(lir_block, value);
                }};
        auto coalesce_block = [&](Block* lir_block) {
            auto use_end = CollectWidthChainUseEnds(lir_block, InstrCount());
            auto long_bridge = CollectLongWidthChainBridges(
                    lir_block, reg_alloc, features, InstrCount(), use_end);

            CoalesceWidthChainBridges(
                    lir_block, reg_alloc, features, use_end, long_bridge,
                    fixed_gpr_clobbers, callbacks);
        };

        if (function) {
            for (auto& hir_block : function->GetHIRBlocksRPO()) {
                coalesce_block(hir_block.GetBlock());
            }
        } else {
            coalesce_block(block);
        }
    }

    void CoalesceLow32Copies() {
        if (!features.ra_coalesce ||
            !backend::X86PinExtLevel2Enabled(reg_alloc->GetGprs())) {
            return;
        }
        RegisterAllocFamilyCallbacks callbacks{
                this,
                [](void* context, Inst* inst, u32 extra_gpr, u32 extra_fpr) {
                    return static_cast<LinearScanAllocator*>(context)->CheckInstr(
                            inst, extra_gpr, extra_fpr);
                },
                nullptr,
                [](void* context, Block* lir_block, Value value) {
                    return static_cast<LinearScanAllocator*>(context)
                            ->ValueUsesStayInBlock(lir_block, value);
                }};
        auto coalesce_block = [&](Block* lir_block) {
            auto use_end = CollectGuestGPRUseEnds(lir_block, InstrCount());
            CoalesceLow32CopyChains(lir_block, reg_alloc, use_end, callbacks);
        };
        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                coalesce_block(hir_block->GetBlock());
            }
        } else {
            coalesce_block(block);
        }
    }

    void CoalesceGuestRegisterAccesses() {
        if (!features.ra_coalesce ||
            !backend::X86PinExtLevel2Enabled(reg_alloc->GetGprs())) {
            return;
        }

        RegisterAllocFamilyCallbacks callbacks{
                this,
                [](void* context, Inst* inst, u32 extra_gpr, u32 extra_fpr) {
                    return static_cast<LinearScanAllocator*>(context)->CheckInstr(
                            inst, extra_gpr, extra_fpr);
                },
                nullptr,
                [](void* context, Block* lir_block, Value value) {
                    return static_cast<LinearScanAllocator*>(context)
                            ->ValueUsesStayInBlock(lir_block, value);
                }};
        auto coalesce_block = [&](Block* lir_block) {
            auto use_end = CollectGuestGPRUseEnds(lir_block, InstrCount());

            CoalesceGuestGPRWrites(
                    lir_block, reg_alloc, features, use_end, fixed_gpr_clobbers,
                    callbacks);
            CoalescePinnedWViewInputs(lir_block, reg_alloc, use_end, callbacks);
            CoalesceGuestGPRReads(lir_block, reg_alloc, use_end, callbacks);
            CensusPinnedHostResidual(lir_block, reg_alloc, use_end);
        };

        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                coalesce_block(hir_block->GetBlock());
            }
        } else {
            coalesce_block(block);
        }
    }

    void CoalesceGuestRegisterAccessesAndWidthChains() {
        RegisterAllocFamilyCallbacks callbacks{
                this,
                [](void* context, Inst* inst, u32 extra_gpr, u32 extra_fpr) {
                    return static_cast<LinearScanAllocator*>(context)->CheckInstr(
                            inst, extra_gpr, extra_fpr);
                },
                nullptr,
                [](void* context, Block* lir_block, Value value) {
                    return static_cast<LinearScanAllocator*>(context)
                            ->ValueUsesStayInBlock(lir_block, value);
                }};
        const bool host_accesses = features.ra_coalesce &&
                backend::X86PinExtLevel2Enabled(reg_alloc->GetGprs());
        auto transact_block = [&](Block* lir_block) {
            auto use_end = CollectGuestGPRUseEnds(lir_block, InstrCount());

            // Establish the exact gate-OFF result first.  Width components are
            // an extension of the already-shipped W-alpha/LONG decisions, not
            // competitors for the same fixed home: choosing a new component
            // must never evict a write/read tie that the baseline already
            // proved.  The staged v2 transaction therefore starts from this
            // immutable baseline and rolls back to it, rather than to the raw
            // linear-scan allocation.
            auto baseline_features = features;
            baseline_features.ra_width_chain = false;
            if (host_accesses) {
                CoalesceGuestGPRWrites(
                        lir_block, reg_alloc, baseline_features, use_end,
                        fixed_gpr_clobbers, callbacks);
                CoalescePinnedWViewInputs(lir_block, reg_alloc, use_end, callbacks);
                CoalesceGuestGPRReads(lir_block, reg_alloc, use_end, callbacks);
            }
            auto baseline_long = CollectLongWidthChainBridges(
                    lir_block, reg_alloc, baseline_features, InstrCount(), use_end);
            CoalesceWidthChainBridges(
                    lir_block, reg_alloc, baseline_features, use_end, baseline_long,
                    fixed_gpr_clobbers, callbacks);
            auto baseline = reg_alloc->SaveGPRCoalesceState();

            // Freeze every multi-publication owner before either half can
            // alter a baseline mapping.  All subsequent decisions run against
            // one staged allocation and become visible only if the final owner
            // validation succeeds.
            RecipeWidthComponentOwners(
                    lir_block, reg_alloc, features, use_end);
            if (host_accesses) {
                CoalesceGuestGPRWrites(
                        lir_block, reg_alloc, features, use_end,
                        fixed_gpr_clobbers, callbacks);
                CoalescePinnedWViewInputs(lir_block, reg_alloc, use_end, callbacks);
            }

            auto long_bridge = CollectLongWidthChainBridges(
                    lir_block, reg_alloc, features, InstrCount(), use_end);
            CoalesceWidthChainBridges(
                    lir_block, reg_alloc, features, use_end, long_bridge,
                    fixed_gpr_clobbers, callbacks);
            u32 new_zero_emission_sites = 0;
            for (auto& inst : lir_block->GetInstList()) {
                if (reg_alloc->IsHostWriteCoalesced(inst.Id()) &&
                    !baseline.host_writes[inst.Id()]) {
                    ++new_zero_emission_sites;
                }
                if (reg_alloc->IsWidthChainCoalesced(inst.Id()) &&
                    baseline.width_anchors[inst.Id()] == UINT32_MAX &&
                    (inst.GetOp() == OpCode::BitExtract ||
                     inst.GetOp() == OpCode::ZeroExtend32To64)) {
                    ++new_zero_emission_sites;
                }
            }
            // In a region, v2 exists for the multi-node matrix/CRC components,
            // not the short scalar chains that made the original general gate
            // move otherwise unrelated STREAM units.  An audit over the
            // complete STREAM image found a maximum of nine new zero-emission
            // sites in any LIR block, while the seven CoreMark matrix blocks
            // have 11..17.  Single-block compilation keeps the proof-complete
            // small components covered by the focused allocator/emitter tests;
            // they cannot perturb a neighbouring region member's layout.
            // Keep either decision transactional: a rejected component
            // contributes no partial owner, mapping or active-mask state.
            constexpr u32 kMinRegionWidthComponentSites = 10;
            const u32 min_width_component_sites =
                    function ? kMinRegionWidthComponentSites : 1;
            if (new_zero_emission_sites < min_width_component_sites ||
                !ValidateWidthComponentTransaction(lir_block, reg_alloc)) {
                reg_alloc->RestoreGPRCoalesceState(std::move(baseline));
            }
            CensusPinnedHostResidual(lir_block, reg_alloc, use_end);
        };

        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                transact_block(hir_block->GetBlock());
            }
        } else {
            transact_block(block);
        }
    }

    void CoalesceGuestFPRAccesses(bool enabled = GetSvmConfig().xmm_resident) {
        if (!enabled) {
            return;
        }

        auto coalesce_block = [&](Block* lir_block) {
            CoalesceGuestFPRWrites(
                    lir_block, reg_alloc, features, InstrCount(), scalar_insert);
        };

        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                coalesce_block(hir_block->GetBlock());
            }
        } else {
            coalesce_block(block);
        }
    }

    void RecognizePshufdDirect() {
        auto recognize_block = [&](Block* lir_block) {
            auto& list = lir_block->GetInstList();
            for (auto& constant : list) {
                if (!IsPshufdDirectMask(constant)) {
                    continue;
                }
                Vector<Inst*> shuffles;
                u32 local_uses = 0;
                bool exact = true;
                for (auto& consumer : list) {
                    for (auto value : consumer.GetValues()) {
                        if (value.Def() != &constant) {
                            continue;
                        }
                        ++local_uses;
                        if (consumer.GetOp() != OpCode::VecShuffle32Indexed ||
                            consumer.GetArg<Value>(1).Def() != &constant ||
                            consumer.GetArg<Value>(0).Def() == &constant) {
                            exact = false;
                            continue;
                        }
                        if (std::find(shuffles.begin(), shuffles.end(), &consumer) ==
                            shuffles.end()) {
                            shuffles.push_back(&consumer);
                        }
                    }
                }
                // GetUses also sees consumers outside this LIR block and
                // terminal operands. Exact equality therefore makes mixed or
                // cross-block use fail closed as one indivisible component.
                if (!exact || shuffles.empty() || local_uses != constant.GetUses()) {
                    continue;
                }
                reg_alloc->MarkPshufdDirect(constant.Id());
                for (auto* shuffle : shuffles) {
                    reg_alloc->MarkPshufdDirect(shuffle->Id());
                }
            }
        };

        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                recognize_block(hir_block->GetBlock());
            }
        } else {
            recognize_block(block);
        }
    }

    void InitializeFixedClobbers() {
        fixed_gpr_clobbers.resize(InstrCount());
        const bool scratch_only =
                backend::X86PinExtScratchOnlyEnabled(reg_alloc->GetGprs(), features);
        auto add_block = [&](Block* lir_block) {
            bool has_inst = false;
            u32 last_id = 0;
            for (auto& inst : lir_block->GetInstList()) {
                if (inst.Id() < fixed_gpr_clobbers.size()) {
                    fixed_gpr_clobbers[inst.Id()] |=
                            backend::FixedGPRClobbers(inst, features, scratch_only);
                }
                has_inst = true;
                last_id = std::max<u32>(last_id, inst.Id());
            }
            // Every block terminal/link sequence owns x11. Recording that
            // clobber at the block's final instruction extends the exclusion
            // to values consumed by terminal::If/Switch as well.
            if (backend::ScratchXPoolEnabled(features) && has_inst &&
                last_id < fixed_gpr_clobbers.size()) {
                fixed_gpr_clobbers[last_id] |=
                        backend::kTerminalFixedGPRClobbers;
            }
        };
        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                add_block(hir_block->GetBlock());
            }
        } else {
            add_block(block);
        }
        for (u32 id = 0; id < fixed_gpr_clobbers.size(); ++id) {
            const u32 fixed = fixed_gpr_clobbers[id];
            for (u32 code = 0; code < 32; ++code) {
                if (fixed & (1u << code)) {
                    fixed_gpr_clobber_points[code].push_back(id);
                }
            }
        }
    }

    void RecordActiveRegs(u32 id) {
        auto base_gprs = reg_alloc->GetGprs();
        auto base_fprs = reg_alloc->GetFprs();
        const u32 allocated_gprs =
                active_gprs.GetMarkedCount() - base_gprs.GetMarkedCount();
        const u32 allocated_fprs =
                active_fprs.GetMarkedCount() - base_fprs.GetMarkedCount();
        max_live_gpr = std::max(max_live_gpr, allocated_gprs + active_spill_gpr);
        max_live_fpr = std::max(max_live_fpr, allocated_fprs + active_spill_fpr);
        auto gprs = active_gprs;
        if (id < fixed_gpr_clobbers.size()) {
            const u32 fixed = fixed_gpr_clobbers[id];
            for (u32 code = 0; code < 32; ++code) {
                if (fixed & (1u << code)) {
                    gprs.Mark(code);
                }
            }
        }
        reg_alloc->SetActiveRegs(id, gprs, active_fprs);
    }

    [[nodiscard]] u32 IntervalFixedClobbers(const LiveInterval& interval) const {
        u32 result = 0;
        for (u32 code = 0; code < fixed_gpr_clobber_points.size(); ++code) {
            const auto& points = fixed_gpr_clobber_points[code];
            const auto it =
                    std::lower_bound(points.begin(), points.end(), interval.start);
            if (it != points.end() && *it <= interval.end) {
                result |= 1u << code;
            }
        }
        return result;
    }

    bool DirectlyFeedsImmediateShift(Inst* inst) const {
        auto in_block = [&](Block* candidate) {
            bool found = false;
            for (auto& next : candidate->GetInstList()) {
                if (found) {
                    return (next.GetOp() == OpCode::LslImm ||
                            next.GetOp() == OpCode::LsrImm ||
                            next.GetOp() == OpCode::AsrImm) &&
                           next.GetArg<Value>(0).Def() == inst;
                }
                found = &next == inst;
            }
            return false;
        };
        if (block) {
            return in_block(block);
        }
        for (auto* hir_block : function->GetHIRBlocks()) {
            if (in_block(hir_block->GetBlock())) {
                return true;
            }
        }
        return false;
    }

    bool DirectlyFeedsLogicalFlagDirect(Inst* inst) const {
        auto in_block = [&](Block* candidate) {
            bool found = false;
            for (auto& next : candidate->GetInstList()) {
                if (found) {
                    if (next.GetOp() != OpCode::Or || next.GetUses() != 0 ||
                        next.GetArg<Value>(0).Def() != inst ||
                        !next.HasFlagsSavePseudo()) {
                        return false;
                    }
                    auto operand = next.GetArg<Operand>(1);
                    return operand.IsImm() &&
                           operand.GetLeft().imm.Get() == 0;
                }
                found = &next == inst;
            }
            return false;
        };
        if (block) {
            return in_block(block);
        }
        for (auto* hir_block : function->GetHIRBlocks()) {
            if (in_block(hir_block->GetBlock())) {
                return true;
            }
        }
        return false;
    }

    Value NarrowLoadTieSource(Inst* inst) const {
        if (shift_imm_fast) {
            if (inst->GetOp() == OpCode::BitExtract &&
                inst->GetArg<Imm>(1).Get() == 0 &&
                inst->GetArg<Imm>(2).Get() == GetValueSizeByte(inst->ReturnType()) * 8 &&
                (DirectlyFeedsImmediateShift(inst) ||
                 DirectlyFeedsLogicalFlagDirect(inst))) {
                return ResolveBitCastSource(inst->GetArg<Value>(0));
            } else if (inst->GetOp() == OpCode::ZeroExtend32) {
                auto source = ResolveBitCastSource(inst->GetArg<Value>(0));
                if (source.Defined() && source.Def()->GetOp() == OpCode::LoadUniform &&
                    GetValueSizeByte(source.Type()) <= 2 &&
                    DirectlyFeedsImmediateShift(inst)) {
                    return source;
                }
                if (source.Defined() && source.Def()->GetOp() == OpCode::BitExtract &&
                    source.Def()->GetArg<Imm>(1).Get() == 0 &&
                    source.Def()->GetArg<Imm>(2).Get() ==
                            GetValueSizeByte(source.Type()) * 8 &&
                    DirectlyFeedsImmediateShift(inst)) {
                    auto input = source.Def()->GetArg<Value>(0);
                    if (input.Def() && input.Def()->GetOp() == OpCode::GetHostGPR &&
                        input.Def()->GetArg<Imm>(0).Get() <= 9) {
                        return source;
                    }
                }
            } else if (inst->GetOp() == OpCode::ZeroExtend32To64) {
                auto source = ResolveBitCastSource(inst->GetArg<Value>(0));
                // Only a 32-bit (W-write) shift makes the extension a no-op:
                // a 64-bit shift leaves live upper bits (e.g. the sign
                // extension of imul/cdq's high half) that the ZExt must clear.
                if (source.Defined() &&
                    (source.Def()->GetOp() == OpCode::LslImm ||
                     source.Def()->GetOp() == OpCode::LsrImm ||
                     source.Def()->GetOp() == OpCode::AsrImm) &&
                    GetValueSizeByte(source.Def()->ReturnType()) == 4) {
                    return source;
                }
            }
        }
        if (!features.mem_narrow_fuse) {
            return {};
        }
        switch (inst->GetOp()) {
            case OpCode::SignExtend:
            case OpCode::ZeroExtend32: {
                auto source = ResolveBitCastSource(inst->GetArg<Value>(0));
                if (source.Defined() && source.Def()->GetOp() == OpCode::LoadMemory &&
                    GetValueSizeByte(source.Type()) <= 2) {
                    return source;
                }
                break;
            }
            case OpCode::ZeroExtend32To64: {
                auto source = ResolveBitCastSource(inst->GetArg<Value>(0));
                if (!source.Defined() ||
                    (source.Def()->GetOp() != OpCode::SignExtend &&
                     source.Def()->GetOp() != OpCode::ZeroExtend32)) {
                    break;
                }
                auto load = ResolveBitCastSource(source.Def()->GetArg<Value>(0));
                if (load.Defined() && load.Def()->GetOp() == OpCode::LoadMemory &&
                    GetValueSizeByte(load.Type()) <= 2) {
                    return source;
                }
                break;
            }
            default:
                break;
        }
        return {};
    }

    bool TryTieNarrowLoad(LiveInterval& current) {
        return TryTieGPR(current, NarrowLoadTieSource(current.inst));
    }

    bool DirectlyFeedsMemory(Inst* inst) const {
        auto in_block = [&](Block* candidate) {
            bool found = false;
            for (auto& next : candidate->GetInstList()) {
                if (found) {
                    bool uses_value = false;
                    for (auto used : next.GetValues()) {
                        uses_value |= used.Def() == inst;
                    }
                    if (!uses_value) {
                        continue;
                    }
                    return next.GetOp() == OpCode::LoadMemory ||
                           next.GetOp() == OpCode::StoreMemory ||
                           next.GetOp() == OpCode::LoadMemoryTSO ||
                           next.GetOp() == OpCode::StoreMemoryTSO;
                }
                found = &next == inst;
            }
            return false;
        };
        if (block) {
            return in_block(block);
        }
        for (auto* hir_block : function->GetHIRBlocks()) {
            if (in_block(hir_block->GetBlock())) {
                return true;
            }
        }
        return false;
    }

    Value MemoryOperandTieSource(Inst* inst) const {
        if ((!features.mem_narrow_fuse && !features.addr_ea_tie) ||
            inst->GetOp() != OpCode::GetOperand ||
            inst->GetUses() != 1 || !DirectlyFeedsMemory(inst)) {
            return {};
        }
        const auto operand = inst->GetArg<Operand>(0);
        if (!operand.GetRight().Null() || !operand.GetLeft().IsValue()) {
            return {};
        }
        return operand.GetLeft().value;
    }

    bool TryTieMemoryOperand(LiveInterval& current) {
        // 新开关只放宽“固定别名在此处终止”的所有权转移证明；不会把普通
        // 计算结果提前发布到固定家，也不会越过后续 SetHostGPR。
        return TryTieGPR(current,
                         MemoryOperandTieSource(current.inst),
                         features.addr_ea_tie);
    }

    Value IntWidthTieSource(Inst* inst) const {
        Value source{};
        if (inst->GetOp() == OpCode::BitExtract &&
            GetValueSizeByte(inst->ReturnType()) == sizeof(u32) &&
            inst->GetArg<Imm>(1).Get() == 0 &&
            inst->GetArg<Imm>(2).Get() == 32) {
            source = inst->GetArg<Value>(0);
        } else if (inst->GetOp() == OpCode::ZeroExtend32To64 &&
                   GetValueSizeByte(inst->GetArg<Value>(0).Type()) == sizeof(u32)) {
            source = inst->GetArg<Value>(0);
        }
        return HasKnownWWrite(source) ? source : Value{};
    }

    bool TryTieIntWidth(LiveInterval& current) {
        return TryTieGPR(current, IntWidthTieSource(current.inst));
    }

    static bool IsAddImmediate(u64 value) {
        return value <= 0xfff ||
               ((value & 0xfff) == 0 && (value >> 12) <= 0xfff);
    }

    Value InductionTieSource(Inst* inst, u32 result_end) const {
        if (inst->GetOp() != OpCode::Add ||
            GetValueSizeByte(inst->ReturnType()) != sizeof(u64)) {
            return {};
        }
        const auto right = inst->GetArg<Operand>(1);
        if (!right.GetRight().Null()) {
            return {};
        }
        u64 immediate_value{};
        if (right.GetLeft().IsImm() && features.int_imm_fold) {
            immediate_value = right.GetLeft().imm.Get();
        } else if (right.GetLeft().IsValue()) {
            const auto immediate = right.GetLeft().value;
            if (!immediate.Def() || immediate.Def()->GetOp() != OpCode::LoadImm ||
                immediate.Def()->GetUses() == 0 ||
                GetValueSizeByte(immediate.Type()) != sizeof(u64)) {
                return {};
            }
            immediate_value = immediate.Def()->GetArg<Imm>(0).Get();
        } else {
            return {};
        }
        if (!IsAddImmediate(immediate_value)) {
            return {};
        }

        auto source = ResolveBitCastSource(inst->GetArg<Value>(0));
        if (!source.Def() || source.Def()->GetOp() != OpCode::GetHostGPR ||
            GetValueSizeByte(source.Type()) != sizeof(u64)) {
            return {};
        }
        const u64 source_host = source.Def()->GetArg<Imm>(0).Get();

        auto match_block = [&](Block* candidate) {
            bool after_add = false;
            bool published = false;
            for (auto& next : candidate->GetInstList()) {
                if (&next == inst) {
                    after_add = true;
                    continue;
                }
                if (!after_add) {
                    continue;
                }
                if (next.GetOp() == OpCode::SetHostGPR) {
                    const auto value = ResolveBitCastSource(next.GetArg<Value>(0));
                    if (!published) {
                        if (value.Def() != inst ||
                            next.GetArg<Imm>(1).Get() != source_host ||
                            next.GetArg<Imm>(2).Get() != 0 ||
                            GetValueSizeByte(next.GetArg<Value>(0).Type()) != sizeof(u64)) {
                            return false;
                        }
                        published = true;
                        continue;
                    }
                    // The result aliases the architectural pin after its first
                    // publication. A later write to that pin invalidates the
                    // SSA capture if the result is still live (for example
                    // RAX+=32; RCX=RAX; RAX+=32; load [RCX]).
                    if (next.Id() <= result_end &&
                        next.GetArg<Imm>(1).Get() == source_host) {
                        return false;
                    }
                    continue;
                }
                if (published) {
                    continue;
                }
                // These are non-faulting metadata/flag consumers in the
                // measured induction lowering. Any architectural observer,
                // host-register access or real operation rejects the early
                // pinned ownership transfer.
                if (next.GetOp() == OpCode::SaveFlags) {
                    if (next.GetArg<Flags>(1) != Flags::None) {
                        return false;
                    }
                    continue;
                }
                if (next.GetOp() != OpCode::LoadImm && !next.IsBitCastOperation()) {
                    return false;
                }
            }
            return published;
        };
        if (block) {
            return match_block(block) ? source : Value{};
        }
        for (auto* hir_block : function->GetHIRBlocks()) {
            if (match_block(hir_block->GetBlock())) {
                return source;
            }
        }
        return {};
    }

    bool TryTieInduction(LiveInterval& current) {
        return TryTieGPR(
                current, InductionTieSource(current.inst, current.end), true);
    }

    bool TryTieGPR(LiveInterval& current, Value source, bool allow_fixed_owner = false) {
        source = ResolveBitCastSource(source);
        if (!source.Defined() || source.Id() == current.inst->Id() ||
            reg_alloc->ValueType(source) != backend::RegAlloc::GPR) {
            return false;
        }

        auto matches = [&](const LiveInterval& live) {
            return live.inst->Id() == source.Id() && live.end == current.start;
        };
        bool removed = false;
        if (single_block_fast_path) {
            auto it = std::find_if(fast_active_lives.begin(), fast_active_lives.end(), matches);
            if (it != fast_active_lives.end()) {
                fast_active_lives.erase(it);
                std::make_heap(fast_active_lives.begin(), fast_active_lives.end(),
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
            if (!allow_fixed_owner || source.Id() >= fixed_gpr_alias_end.size() ||
                fixed_gpr_alias_end[source.Id()] != current.start) {
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

    Value ScalarInsertTieSource(Inst* inst) const {
        // FEX-style AES rounds are destructive on the state operand.  When
        // that SSA value dies at this instruction, give the result its host
        // register and eliminate the otherwise mandatory vector copy.  The
        // fast opcodes are emitted only under SVM_AES_ZERO_REUSE, so =0
        // restores both the old IR and the old allocation.
        switch (inst->GetOp()) {
            case OpCode::VecAesEncFast:
            case OpCode::VecAesEncLastFast:
            case OpCode::VecAesDecFast:
            case OpCode::VecAesDecLastFast:
                return inst->GetArg<Value>(0);
            default:
                break;
        }
        if (features.sse_shufps_imm &&
            inst->GetOp() == OpCode::VecShuffle32TwoSrc) {
            const u32 control = inst->GetArg<Imm>(2).Get() & 0xffu;
            if (control == 0xe4 ||
                ((control == 0xe0 || control == 0xe5) &&
                 inst->GetArg<Value>(0).Id() == inst->GetArg<Value>(1).Id())) {
                return inst->GetArg<Value>(0);
            }
        }
        if (!scalar_insert) {
            return {};
        }
        switch (inst->GetOp()) {
            case OpCode::VecFAddScalar32:
            case OpCode::VecFSubScalar32:
            case OpCode::VecFMulScalar32:
            case OpCode::VecFDivScalar32:
            case OpCode::VecFAddScalar64:
            case OpCode::VecFSubScalar64:
            case OpCode::VecFMulScalar64:
            case OpCode::VecFDivScalar64:
                return inst->GetArg<Value>(0);
            case OpCode::VecFMinMax:
                if (inst->GetArg<Imm>(4).Get() != 0) {
                    return inst->GetArg<Value>(0);
                }
                break;
            case OpCode::VecFUnary:
                if (inst->GetArg<Imm>(4).Get() != 0) {
                    return inst->GetArg<Value>(1);
                }
                break;
            default:
                break;
        }
        return {};
    }

    void CollectResidentFPRWrites() {
        if (scalar_tie_fixed_read_end.empty()) {
            return;
        }
        scalar_tie_future_writes.resize(InstrCount(), 0);
        auto collect = [&](Block* lir_block) {
            u32 writes = 0;
            auto& list = lir_block->GetInstList();
            for (auto it = list.rbegin(); it != list.rend(); ++it) {
                if (IsPinnedCoalesceObserver(it->GetOp())) {
                    writes = 0;
                } else if (it->GetOp() == OpCode::SetHostFPR &&
                           it->GetArg<Imm>(2).Get() == 0) {
                    const auto target = it->GetArg<Imm>(1).Get();
                    if (target < 32) writes |= u32{1} << target;
                }
                scalar_tie_future_writes[it->Id()] = writes;
            }
        };
        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                collect(hir_block->GetBlock());
            }
        } else {
            collect(block);
        }
    }

    bool TryTieScalarInsert(LiveInterval& current) {
        auto source = ScalarInsertTieSource(current.inst);
        if (!source.Defined()) {
            return false;
        }
        source = ResolveBitCastSource(source);
        if (!source.Defined() || source.Id() == current.inst->Id() ||
            reg_alloc->ValueType(source) != backend::RegAlloc::FPR) {
            return false;
        }
        const auto source_fpr = reg_alloc->ValueFPR(source);
        if (reg_alloc->GetFprs().Get(source_fpr.id)) {
            // The guest register stays live at exits and faults even if this
            // SSA read dies here. Reuse it only when the same block replaces
            // its guest value before any operation can observe the old state.
            if (current.start >= scalar_tie_future_writes.size() ||
                !(scalar_tie_future_writes[current.start] & (u32{1} << source_fpr.id))) {
                return false;
            }
            // Other reads can still require the guest value before this
            // result is published, even when this particular source dies now.
            for (const auto& [read_id, read_end] : scalar_tie_fixed_read_end) {
                if (read_id != source.Id() && read_id < current.end &&
                    read_end > current.start &&
                    reg_alloc->ValueFPR(read_id).id == source_fpr.id) {
                    return false;
                }
            }
            auto writes = scalar_tie_fpr_writes.find(source_fpr.id);
            if (writes != scalar_tie_fpr_writes.end() &&
                std::any_of(writes->second.begin(), writes->second.end(), [&current](u32 write_id) {
                    return current.start < write_id && write_id < current.end;
                })) {
                return false;
            }
        }
        const auto fixed_source = scalar_tie_fixed_read_end.find(source.Id());
        const bool transfer_fixed_source = fixed_source != scalar_tie_fixed_read_end.end() &&
                                           fixed_source->second == current.start &&
                                           reg_alloc->IsHostReadCoalesced(source.Id());

        auto matches = [&](const LiveInterval& live) {
            return live.inst->Id() == source.Id() && live.end == current.start;
        };
        bool removed = false;
        if (single_block_fast_path) {
            auto it = std::find_if(fast_active_lives.begin(), fast_active_lives.end(), matches);
            if (it != fast_active_lives.end()) {
                fast_active_lives.erase(it);
                std::make_heap(fast_active_lives.begin(), fast_active_lives.end(),
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

    // True when a value ends up in State::spill_area, so every access to it
    // costs a scratch register.
    bool IsSpilled(const Value& value) {
        if (!value.Defined()) {
            return false;
        }
        auto source = ResolveBitCastSource(value);
        if (source.Defined() && source.Id() >= reg_alloc->MapCount()) {
            auto* d = source.Def();
            HIRBlock* db = nullptr;
            if (function) {
                for (auto* hb : function->GetHIRBlocks()) {
                    for (auto& i : hb->GetBlock()->GetInstList()) {
                        if (&i == d) { db = hb; break; }
                    }
                    if (db) break;
                }
            }
            bool inrpo = false;
            if (function && db) {
                for (auto& hb : function->GetHIRBlocksRPO()) { if (&hb == db) { inrpo = true; break; } }
            }
            SVM_DIAG_PRINT(RegisterAllocation,
                         "[OOB-IsSpilled] vid=%u def_op=%u db=%p oid=%u in_rpo=%d in_edges=%u size=%u\n",
                         (unsigned)source.Id(), d ? (unsigned)d->GetOp() : 0xffffu,
                         (void*)db, db ? (unsigned)db->GetOrderId() : 0xffffu,
                         (int)inrpo,
                         db ? (unsigned)db->GetIncomingEdges().size() : 0xffffu,
                         (unsigned)reg_alloc->MapCount());
        }
        return reg_alloc->ValueType(source) == backend::RegAlloc::MEM;
    }

    void CountTerminalReloads(const Terminal& term, u32& gpr, u32& fpr) {
        auto count = [&](const Value& value) {
            if (!IsSpilled(value)) {
                return;
            }
            (IsFloatValue(ResolveBitCastSource(value).Def()) ? fpr : gpr)++;
        };
        std::function<void(const Terminal&)> walk = [&](const Terminal& t) {
            VisitVariant<void>(t, [&](auto term_case) {
                using T = std::decay_t<decltype(term_case)>;
                if constexpr (std::is_same_v<T, terminal::If>) {
                    count(term_case.cond);
                    walk(term_case.then_);
                    walk(term_case.else_);
                } else if constexpr (std::is_same_v<T, terminal::Switch>) {
                    count(term_case.value);
                    for (auto& c : term_case.cases) {
                        walk(c.then);
                    }
                } else if constexpr (std::is_same_v<T, terminal::Condition>) {
                    walk(term_case.then_);
                    walk(term_case.else_);
                } else if constexpr (std::is_same_v<T, terminal::CheckHalt>) {
                    walk(term_case.else_);
                }
            });
        };
        walk(term);
    }

    bool CheckInstr(Inst* inst, u32 extra_gpr, u32 extra_fpr) {
        const u32 id = inst->Id();
        if (id >= reg_alloc->MapCount()) {
            return true;  // no mask was recorded for this id
        }
        auto need = backend::ScratchBudget(*inst, features);
        if (fixed_class) {
            const auto contract = backend::ClassifyGPRContract(
                    reg_alloc->GetGprs(), *inst, features);
            ASSERT(contract.hot_instruction_scratch == need.gpr);
            ASSERT((contract.fixed_clobber_mask & backend::kX86FixedGPRHomes) == 0);
        }
        u32 reload_gpr = 0;
        u32 reload_fpr = 0;
        // One reload register per DISTINCT spilled value the instruction
        // names -- distinct because JitContext memoizes reloads per
        // (instruction, value).
        StackVector<u32, 8> counted{};
        auto add = [&](const Value& value) {
            if (!IsSpilled(value)) {
                return;
            }
            auto source = ResolveBitCastSource(value);
            if (std::find(counted.begin(), counted.end(), source.Id()) != counted.end()) {
                return;
            }
            counted.push_back(source.Id());
            if (IsFloatValue(source.Def())) {
                ++reload_fpr;
            } else if (!reg_alloc->HasSpillReload(source.Id(), id)) {
                ++reload_gpr;
            }
        };
        for (auto& value : inst->GetValues()) {
            add(value);
        }
        if (inst->HasValue()) {
            add(Value{inst});
        }
        auto gprs = reg_alloc->DirtyGPR(id);
        auto fprs = reg_alloc->DirtyFPR(id);
        const u32 scratch_only_gprs =
                ScratchOnlyGPRs(inst->GetOp(), reg_alloc->GetGprs(), features);
        u32 need_gpr = need.gpr + reload_gpr + extra_gpr;
        // Legacy Add/Sub's five-register declaration is the no-spill worst case:
        // tied inputs must be preserved for AF/PF after the destination is
        // overwritten. A spilled operand/result cannot be tied, and its reload
        // register replaces that preservation temporary. Charge the larger of
        // the no-spill peak and the ordinary three-register shape plus reloads
        // instead of adding both mutually-exclusive peaks. Precise pricing
        // excludes those dead preservation arms, so ordinary scratch and
        // reload registers are independent and use the direct sum above.
        if (!backend::ScratchPreciseRequested(features) && scratch_only_gprs &&
            (inst->GetOp() == OpCode::Add || inst->GetOp() == OpCode::Sub)) {
            need_gpr = std::max<u32>(
                    need.gpr, backend::kDefaultScratchGPR + reload_gpr) + extra_gpr;
        }
        const u32 need_fpr = need.fpr + reload_fpr + extra_fpr;
        return static_cast<u32>(gprs.GetClearCount()) + scratch_only_gprs >= need_gpr &&
               static_cast<u32>(fprs.GetClearCount()) >= need_fpr;
    }

    // Number of RegAlloc map entries (matches how the caller sized it).
    u32 InstrCount() {
        return function ? static_cast<u32>(function->MaxInstrCount())
                        : block->MaxInstrId();
    }

    void CollectLiveIntervals(HIRFunction* hir_function) {
        PerfScope2 perf_scan{GetPerfStats2().regalloc_live_scan};
        scalar_tie_fpr_writes.clear();
        scalar_tie_fixed_read_end.clear();
        // A value referenced ONLY by a block terminal (terminal::If.cond, a
        // Switch dispatch value, ...) never appears in the HIRValue use list:
        // HIRFunction::UseInst walks instruction arguments only, and EndBlock
        // stores the terminal without registering uses for the values it reads.
        // Left unaccounted, such a value's live interval collapses to [def, def]
        // and the linear scan frees its host register while the terminal (emitted
        // at block exit, after every instruction) still reads it — the register
        // gets handed to a later, overlapping interval and the terminal branches
        // on a clobbered value (SIGSEGV). Mirror the block-level walk_terminal
        // below: extend each terminal-used value to the last instruction id of
        // the block that owns the terminal (function-global id).
        Map<u32, u32> terminal_end{};
        Map<u32, u32> actual_use_end{};
        Map<u32, u32> definition_block_end{};
        HostRegWriteMap host_gpr_writes{};
        HostRegWriteMap host_fpr_writes{};
        // Reachable blocks only: a block dropped from the RPO keeps stale
        // instruction ids (IdByRPO numbers unreachable instructions past the
        // value table), and folding their operand uses into the live-end
        // accumulators below would stretch reachable defs' intervals to dead
        // positions.
        for (auto& hir_block : hir_function->GetHIRBlocksRPO()) {
            auto* lir_block = hir_block.GetBlock();
            u32 block_end = 0;
            for (auto& inst : lir_block->GetInstList()) {
                block_end = std::max<u32>(block_end, inst.Id());
                if (inst.GetOp() == OpCode::SetHostGPR) {
                    const auto host_reg = static_cast<u16>(inst.GetArg<Imm>(1).Get());
                    host_gpr_writes[host_reg].push_back(inst.Id());
                } else if (inst.GetOp() == OpCode::SetHostFPR) {
                    const auto host_reg = static_cast<u16>(inst.GetArg<Imm>(1).Get());
                    host_fpr_writes[host_reg].push_back(inst.Id());
                    scalar_tie_fpr_writes[host_reg].push_back(inst.Id());
                }
                for (auto value : inst.GetValues()) {
                    auto source = ResolveBitCastSource(value);
                    auto& end = actual_use_end[source.Id()];
                    end = std::max<u32>(end, inst.Id());
                }
            }
            if (block_end == 0) {
                continue;  // empty block — nothing can be live out of it
            }
            for (auto& inst : lir_block->GetInstList()) {
                definition_block_end[inst.Id()] = block_end;
            }
            // 提升值只在普通入口初始化，自回边直接复用物理寄存器；把其
            // 区间延到 terminal，禁止线性扫描在循环体内回收该寄存器。
            for (auto* anchor : lir_block->GetLoopHoistMetadata().anchors) {
                auto& end = actual_use_end[anchor->Id()];
                end = std::max(end, block_end);
            }
            auto extend_use = [&terminal_end, block_end](const Value& value) {
                if (!value.Defined()) {
                    return;
                }
                auto id = ResolveBitCastSource(value).Id();
                auto& end = terminal_end[id];
                end = std::max(end, block_end);
            };
            std::function<void(const Terminal&)> walk_terminal =
                    [&walk_terminal, &extend_use](const Terminal& term) {
                        VisitVariant<void>(term, [&walk_terminal, &extend_use](auto t) {
                            using T = std::decay_t<decltype(t)>;
                            if constexpr (std::is_same_v<T, terminal::If>) {
                                extend_use(t.cond);
                                walk_terminal(t.then_);
                                walk_terminal(t.else_);
                            } else if constexpr (std::is_same_v<T, terminal::Switch>) {
                                extend_use(t.value);
                                for (auto& c : t.cases) {
                                    walk_terminal(c.then);
                                }
                            } else if constexpr (std::is_same_v<T, terminal::Condition>) {
                                walk_terminal(t.then_);
                                walk_terminal(t.else_);
                            } else if constexpr (std::is_same_v<T, terminal::CheckHalt>) {
                                walk_terminal(t.else_);
                            }
                        });
                    };
            walk_terminal(lir_block->GetTerminal());
        }
        perf_scan.Stop();
        PerfScope2 perf_values{GetPerfStats2().regalloc_live_values};
        // GetHIRValues() is indexed by instruction id, so this visits values in
        // ascending id -- the order the linear scan below requires -- and holds a
        // null for every instruction that defines no value.
        for (auto* hir_value_ptr : hir_function->GetHIRValues()) {
            if (!hir_value_ptr) {
                continue;
            }
            auto& hir_value = *hir_value_ptr;
            auto instr = hir_value.value.Def();
            auto start = hir_value.GetOrderId();
            u32 current_end{hir_value.GetOrderId()};
            if (auto it = actual_use_end.find(instr->Id()); it != actual_use_end.end()) {
                current_end = std::max(current_end, it->second);
            }
            if (auto it = terminal_end.find(instr->Id()); it != terminal_end.end()) {
                current_end = std::max(current_end, it->second);
            }
            u32 end{current_end};
            // Reachable-use bound: IdByRPO numbers instructions in blocks
            // dropped from the RPO past the value table, so a dead use in one
            // of those blocks carries an id >= GetHIRValues().size(). It must
            // not stretch a reachable def's interval.
            const u32 live_id_bound =
                    static_cast<u32>(hir_function->GetHIRValues().size());
            std::for_each(hir_value.uses.begin(), hir_value.uses.end(), [&](auto& use) {
                if (use.inst->Id() < live_id_bound) {
                    end = std::max(end, (u32)use.inst->Id());
                }
            });
            const bool host_reg_alias = instr->IsGetHostRegOperation();
            const auto host_index =
                    host_reg_alias ? static_cast<u16>(instr->GetArg<Imm>(0).Get()) : u16{};
            const bool full_gpr_get = host_reg_alias && instr->GetOp() == OpCode::GetHostGPR &&
                                      GetValueSizeByte(instr->ReturnType()) == sizeof(u64);
            const bool fixed_fpr_get =
                    host_reg_alias && instr->GetOp() == OpCode::GetHostFPR &&
                    instr->GetArg<Imm>(1).Get() == 0 &&
                    IsFloatValueType(instr->ReturnType());
            // A fixed mapping aliases this SSA value directly to the pinned
            // register. Preserve capture semantics by forcing a copy when a
            // SetHostGPR can overwrite that register before the value's last use.
            const bool fixed_gpr_get =
                    full_gpr_get &&
                    !LiveRangeCrossesHostRegWrite(host_gpr_writes, host_index, instr->Id(), end);
            const bool fixed_fpr_alias =
                    fixed_fpr_get && end <= definition_block_end[instr->Id()] &&
                    !LiveRangeCrossesHostRegWrite(host_fpr_writes, host_index, instr->Id(), end);
            if (fixed_fpr_alias) {
                reg_alloc->MapRegister(hir_value.GetOrderId(), HostFPR{host_index});
                reg_alloc->MarkHostReadCoalesced(hir_value.GetOrderId());
                if (scalar_insert && features.sse_scalar_tie) {
                    scalar_tie_fixed_read_end.emplace(instr->Id(), current_end);
                }
                continue;
            }
            if (fixed_gpr_get) {
                MapFixedRead(hir_value.GetOrderId(), host_index);
                fixed_gpr_alias_end[hir_value.GetOrderId()] = end;
                continue;
            }
            if (instr->IsBitCastOperation()) {
                auto from = instr->GetArg<Value>(0);
                reg_alloc->MapReference(from.Id(), instr->Id());
                continue;
            }
            if (auto inst = hir_value.value.Def(); inst->IsPseudoOperation()) {
                start = inst->GetArg<Value>(0).Id();
            }
            live_interval.push_back({hir_value.value.Def(), start, end});
        }
        perf_values.Stop();
    }

    void CollectLiveIntervalsSingleBlock(HIRFunction* hir_function) {
        auto& rpo = hir_function->GetHIRBlocksRPO();
        ASSERT_MSG(rpo.size() == 1, "single-block register allocator received {} blocks",
                   rpo.size());
        auto* lir_block = rpo.front().GetBlock();
        scalar_tie_fpr_writes.clear();
        scalar_tie_fixed_read_end.clear();
        const auto instr_count = static_cast<u32>(hir_function->MaxInstrCount());

        // Exact specialization of the function collector above: instruction ids
        // are dense in a one-block function, so two id-indexed arrays and a tiny
        // write list replace three ordered maps. Interval construction order and
        // every lifetime rule remain unchanged.
        StackVector<u16, 64> actual_use_end{};
        actual_use_end.resize(instr_count);
        StackVector<std::pair<u16, u16>, 8> host_gpr_writes{};
        StackVector<std::pair<u16, u16>, 8> host_fpr_writes{};

        PerfScope2 perf_scan{GetPerfStats2().regalloc_live_scan};
        u16 block_end = 0;
        for (auto& inst : lir_block->GetInstList()) {
            block_end = std::max<u16>(block_end, inst.Id());
            if (inst.GetOp() == OpCode::SetHostGPR) {
                const auto host_reg = static_cast<u16>(inst.GetArg<Imm>(1).Get());
                host_gpr_writes.emplace_back(host_reg, inst.Id());
            } else if (inst.GetOp() == OpCode::SetHostFPR) {
                const auto host_reg = static_cast<u16>(inst.GetArg<Imm>(1).Get());
                host_fpr_writes.emplace_back(host_reg, inst.Id());
                scalar_tie_fpr_writes[host_reg].push_back(inst.Id());
            }
            auto record_use = [&actual_use_end, &inst](Value value) {
                auto source = ResolveBitCastSource(value);
                auto& end = actual_use_end[source.Id()];
                end = std::max<u16>(end, inst.Id());
            };
            // Inline Inst::GetValues so the one-block collector does not build
            // and destroy a temporary small_vector for every instruction.
            // The slot walk and the Value/Lambda/Params cases are identical.
            for (u8 i = 0; i < Inst::max_args; ++i) {
                auto& arg = inst.ArgAt(i);
                if (arg.IsValue()) {
                    record_use(arg.Get<Value>());
                } else if (arg.IsLambda() && arg.Get<Lambda>().IsValue()) {
                    record_use(arg.Get<Lambda>().GetValue());
                } else if (arg.IsParams()) {
                    for (auto param : arg.Get<Params>()) {
                        if (auto data = param.data; data.IsValue()) {
                            record_use(data.value);
                        }
                    }
                }
            }
        }
        if (block_end != 0) {
            for (auto* anchor : lir_block->GetLoopHoistMetadata().anchors) {
                auto& end = actual_use_end[anchor->Id()];
                end = std::max<u16>(end, block_end);
            }
            auto extend_use = [&actual_use_end, block_end](const Value& value) {
                if (!value.Defined()) {
                    return;
                }
                auto id = ResolveBitCastSource(value).Id();
                auto& end = actual_use_end[id];
                end = std::max(end, block_end);
            };
            std::function<void(const Terminal&)> walk_terminal =
                    [&walk_terminal, &extend_use](const Terminal& term) {
                        VisitVariant<void>(term, [&walk_terminal, &extend_use](auto t) {
                            using T = std::decay_t<decltype(t)>;
                            if constexpr (std::is_same_v<T, terminal::If>) {
                                extend_use(t.cond);
                                walk_terminal(t.then_);
                                walk_terminal(t.else_);
                            } else if constexpr (std::is_same_v<T, terminal::Switch>) {
                                extend_use(t.value);
                                for (auto& c : t.cases) {
                                    walk_terminal(c.then);
                                }
                            } else if constexpr (std::is_same_v<T, terminal::Condition>) {
                                walk_terminal(t.then_);
                                walk_terminal(t.else_);
                            } else if constexpr (std::is_same_v<T, terminal::CheckHalt>) {
                                walk_terminal(t.else_);
                            }
                        });
                    };
            walk_terminal(lir_block->GetTerminal());
        }
        perf_scan.Stop();

        PerfScope2 perf_values{GetPerfStats2().regalloc_live_values};
        for (auto* hir_value_ptr : hir_function->GetHIRValues()) {
            if (!hir_value_ptr) {
                continue;
            }
            auto& hir_value = *hir_value_ptr;
            auto* instr = hir_value.value.Def();
            auto start = hir_value.GetOrderId();
            const u32 current_end =
                    std::max<u32>(hir_value.GetOrderId(), actual_use_end[instr->Id()]);
            u32 end{current_end};
            // Preserve the generic collector's use-list maximum even when an
            // optimization has left a stale use behind. The current-argument
            // scan below is the matching second maximum: it catches rewritten
            // BitCast roots whose new use is absent from the old list. A use in
            // a block dropped from the RPO carries an id past the value table,
            // though, and must not stretch a reachable def's interval.
            const u32 live_id_bound =
                    static_cast<u32>(hir_function->GetHIRValues().size());
            std::for_each(hir_value.uses.begin(), hir_value.uses.end(), [&](auto& use) {
                if (use.inst->Id() < live_id_bound) {
                    end = std::max(end, static_cast<u32>(use.inst->Id()));
                }
            });
            end = std::max<u32>(end, actual_use_end[instr->Id()]);

            const bool host_reg_alias = instr->IsGetHostRegOperation();
            const auto host_index =
                    host_reg_alias ? static_cast<u16>(instr->GetArg<Imm>(0).Get()) : u16{};
            const bool full_gpr_get = host_reg_alias && instr->GetOp() == OpCode::GetHostGPR &&
                                      GetValueSizeByte(instr->ReturnType()) == sizeof(u64);
            const bool fixed_fpr_get =
                    host_reg_alias && instr->GetOp() == OpCode::GetHostFPR &&
                    instr->GetArg<Imm>(1).Get() == 0 &&
                    IsFloatValueType(instr->ReturnType());
            const bool crosses_host_write =
                    full_gpr_get &&
                    std::any_of(host_gpr_writes.begin(), host_gpr_writes.end(),
                                [host_index, instr, end](const auto& write) {
                                    return write.first == host_index && instr->Id() < write.second &&
                                           write.second <= end;
                                });
            const bool fixed_gpr_get = full_gpr_get && !crosses_host_write;
            const bool crosses_host_fpr_write =
                    fixed_fpr_get && std::any_of(host_fpr_writes.begin(),
                                                 host_fpr_writes.end(),
                                                 [host_index, instr, end](const auto& write) {
                                                     return write.first == host_index &&
                                                            instr->Id() < write.second &&
                                                            write.second <= end;
                                                 });
            const bool fixed_fpr_alias = fixed_fpr_get && !crosses_host_fpr_write;
            if (fixed_fpr_alias) {
                reg_alloc->MapRegister(hir_value.GetOrderId(), HostFPR{host_index});
                reg_alloc->MarkHostReadCoalesced(hir_value.GetOrderId());
                if (scalar_insert && features.sse_scalar_tie) {
                    scalar_tie_fixed_read_end.emplace(instr->Id(), current_end);
                }
                continue;
            }
            if (fixed_gpr_get) {
                MapFixedRead(hir_value.GetOrderId(), host_index);
                fixed_gpr_alias_end[hir_value.GetOrderId()] = end;
                continue;
            }
            if (instr->IsBitCastOperation()) {
                auto from = instr->GetArg<Value>(0);
                reg_alloc->MapReference(from.Id(), instr->Id());
                continue;
            }
            if (instr->IsPseudoOperation()) {
                start = instr->GetArg<Value>(0).Id();
            }
            live_interval.push_back({instr, start, end});
        }
        perf_values.Stop();
    }

    void CollectLiveIntervals(Block* lir_block) {
        PerfScope2 perf_scan{GetPerfStats2().regalloc_live_scan};
        scalar_tie_fpr_writes.clear();
        scalar_tie_fixed_read_end.clear();
        ASSERT_MSG(lir_block, "block == null");
        ASSERT_MSG(!lir_block->IsEmptyBlock(), "block is empty");
        StackVector<u16, 64> use_end{};
        use_end.resize(std::max<u32>(lir_block->MaxInstrId(), lir_block->GetInstList().size()));
        HostRegWriteMap host_gpr_writes{};
        HostRegWriteMap host_fpr_writes{};
        for (auto& instr : lir_block->GetInstList()) {
            if (instr.GetOp() == OpCode::SetHostGPR) {
                const auto host_reg = static_cast<u16>(instr.GetArg<Imm>(1).Get());
                host_gpr_writes[host_reg].push_back(instr.Id());
            } else if (instr.GetOp() == OpCode::SetHostFPR) {
                const auto host_reg = static_cast<u16>(instr.GetArg<Imm>(1).Get());
                host_fpr_writes[host_reg].push_back(instr.Id());
                scalar_tie_fpr_writes[host_reg].push_back(instr.Id());
            }
            if (!instr.IsGetHostRegOperation() && !instr.IsBitCastOperation()) {
                // SetHost* is a normal use. Pinned registers are reserved from
                // linear scan, and the emitter performs the move/bit insert at
                // the SetHost* instruction. Coalescing the source into the
                // pinned register would update guest state at the source
                // definition, potentially before intervening uses.
                auto values = instr.GetValues();
                for (auto& value : values) {
                    auto source = ResolveBitCastSource(value);
                    auto& end = use_end[source.Id()];
                    end = std::max(end, instr.Id());
                }
            }
        }
        // Values referenced only by the block terminal (e.g. the condition of
        // terminal::If produced for a jcc, or a Switch dispatch value) are
        // used at the end of the block and must stay live until then.
        if (lir_block->MaxInstrId() > 0) {
            auto block_end = static_cast<u16>(lir_block->MaxInstrId() - 1);
            for (auto* anchor : lir_block->GetLoopHoistMetadata().anchors) {
                auto& end = use_end[anchor->Id()];
                end = std::max(end, block_end);
            }
            auto extend_use = [&use_end, block_end](const Value& value) {
                if (!value.Defined()) {
                    return;
                }
                auto id = ResolveBitCastSource(value).Id();
                if (id < use_end.size()) {
                    auto& end = use_end[id];
                    end = std::max(end, block_end);
                }
            };
            std::function<void(const Terminal&)> walk_terminal =
                    [&walk_terminal, &extend_use](const Terminal& term) {
                        VisitVariant<void>(term, [&walk_terminal, &extend_use](auto t) {
                            using T = std::decay_t<decltype(t)>;
                            if constexpr (std::is_same_v<T, terminal::If>) {
                                extend_use(t.cond);
                                walk_terminal(t.then_);
                                walk_terminal(t.else_);
                            } else if constexpr (std::is_same_v<T, terminal::Switch>) {
                                extend_use(t.value);
                                for (auto& c : t.cases) {
                                    walk_terminal(c.then);
                                }
                            } else if constexpr (std::is_same_v<T, terminal::Condition>) {
                                walk_terminal(t.then_);
                                walk_terminal(t.else_);
                            } else if constexpr (std::is_same_v<T, terminal::CheckHalt>) {
                                walk_terminal(t.else_);
                            }
                        });
                    };
            walk_terminal(lir_block->GetTerminal());
        }
        perf_scan.Stop();
        PerfScope2 perf_values{GetPerfStats2().regalloc_live_values};
        for (auto& instr : lir_block->GetInstList()) {
            const bool host_reg_alias = instr.IsGetHostRegOperation();
            const auto host_index =
                    host_reg_alias ? static_cast<u16>(instr.GetArg<Imm>(0).Get()) : u16{};
            const bool full_gpr_get = host_reg_alias && instr.GetOp() == OpCode::GetHostGPR &&
                                      GetValueSizeByte(instr.ReturnType()) == sizeof(u64);
            const bool fixed_fpr_get =
                    host_reg_alias && instr.GetOp() == OpCode::GetHostFPR &&
                    instr.GetArg<Imm>(1).Get() == 0 &&
                    IsFloatValueType(instr.ReturnType());
            // See the function-level collector above: a crossing write makes
            // the GetHostGPR result a capture, not a zero-cost alias.
            const bool fixed_gpr_get =
                    full_gpr_get &&
                    !LiveRangeCrossesHostRegWrite(
                            host_gpr_writes, host_index, instr.Id(), use_end[instr.Id()]);
            const bool fixed_fpr_alias =
                    fixed_fpr_get &&
                    !LiveRangeCrossesHostRegWrite(
                            host_fpr_writes, host_index, instr.Id(), use_end[instr.Id()]);
            if (fixed_fpr_alias) {
                reg_alloc->MapRegister(instr.Id(), HostFPR{host_index});
                reg_alloc->MarkHostReadCoalesced(instr.Id());
                if (scalar_insert && features.sse_scalar_tie) {
                    scalar_tie_fixed_read_end.emplace(
                            instr.Id(), std::max<u32>(instr.Id(), use_end[instr.Id()]));
                }
                continue;
            }
            if (fixed_gpr_get) {
                MapFixedRead(instr.Id(), host_index);
                fixed_gpr_alias_end[instr.Id()] = use_end[instr.Id()];
                continue;
            }
            if (instr.IsBitCastOperation()) {
                auto from = instr.GetArg<Value>(0);
                reg_alloc->MapReference(from.Id(), instr.Id());
                continue;
            }
            if (instr.HasValue()) {
                auto start = instr.Id();
                auto end = use_end[start];
                if (!end) {
                    // A value nobody reads still needs a register: the emitter
                    // writes its destination unconditionally, and asking
                    // RegAlloc for an unallocated value asserts
                    // (alloc_result[id].type == GPR).
                    //
                    // This was unreachable until uniform dead-store
                    // elimination landed: the front end always stored a
                    // result into guest state, and that store was the use.
                    // DSE can now remove the store while the producer survives
                    // on its side effects -- AtomicExchange is the measured
                    // case, and anything DCE keeps for a reason other than its
                    // result has the same shape.
                    //
                    // A degenerate [start, start] interval is exactly right:
                    // the register is needed for this one instruction and free
                    // immediately after. The function-level collector already
                    // behaves this way (its `end` starts at the def's order
                    // id), which is why only block mode failed.
                    end = start;
                }
                live_interval.push_back({&instr, start, end});
            }
        }
        perf_values.Stop();
    }

    void ExpireOldIntervals(LiveInterval& current) {
        if (single_block_fast_path) {
            const auto later_end = [](const LiveInterval& left, const LiveInterval& right) {
                return left.end > right.end;
            };
            while (!fast_active_lives.empty() &&
                   fast_active_lives.front().end < current.start) {
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

    void ReleaseInterval(const LiveInterval& interval) {
        auto value_type = reg_alloc->ValueType(ir::Value(interval.inst));
        if (value_type == backend::RegAlloc::GPR) {
            FreeGPR(reg_alloc->ValueGPR(interval.inst->Id()).id);
        } else if (value_type == backend::RegAlloc::FPR) {
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

    // Appends new_item_size free slots. Callers must take the new index from
    // spill_slots.size() *before* calling this, never from a cached cursor.
    void GrowSpillStack(u32 new_item_size) {
        spill_slots.resize(spill_slots.size() + new_item_size);
    }

    static bool IsFloatValue(Inst* inst) {
        auto value_type = inst->ReturnType();
        return value_type >= ValueType::V8 && value_type <= ValueType::V256;
    }

    // The mask this pass records for an instruction is exactly `active_*` at
    // the moment that instruction is reached, so refusing to allocate below
    // the reserve here is what guarantees GetTmpX/GetTmpV find a register at
    // every instruction of the unit.
    int AllocGPR(const LiveInterval& interval) {
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

    int AllocFPR() {
        if (static_cast<u32>(active_fprs.GetClearCount()) <= fpr_reserve) {
            return -1;
        }
        if (auto alloc = active_fprs.GetFirstClear(); alloc >= 0) {
            active_fprs.Mark(alloc);
            return alloc;
        }
        return -1;
    }

    void SpillAtInterval(LiveInterval& interval) {
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
        ASSERT_MSG(slot + slot_size <= backend::kMaxSpillSlots,
                   "spill area exhausted: slot {} (+{}) >= {} reserved slots",
                   slot, slot_size, backend::kMaxSpillSlots);
        spill_count++;
        if (is_float) {
            ++active_spill_fpr;
        } else {
            ++active_spill_gpr;
        }
        max_spill_slot = std::max(max_spill_slot, slot + slot_size - 1);
    }

    void FreeGPR(u32 id) {
        ASSERT(active_gprs.Get(id));
        // A tied induction result may temporarily own a statically reserved
        // guest register. Expiry ends the SSA interval but must never return
        // that architectural register to the scratch/value pool.
        if (!reg_alloc->GetGprs().Get(id)) {
            active_gprs.Clear(id);
        }
    }

    void FreeFPR(u32 id) {
        ASSERT(active_fprs.Get(id));
        if (!reg_alloc->GetFprs().Get(id)) {
            active_fprs.Clear(id);
        }
    }

    void FreeSpill(u32 slot) {
        ASSERT(spill_slots[slot]);
        spill_slots[slot] = false;
    }

    HIRFunction* function;
    Block* block;
    backend::RegAlloc* reg_alloc;
    Vector<LiveInterval> live_interval;
    List<LiveInterval> active_lives;
    Vector<LiveInterval> fast_active_lives;
    backend::GPRSMask active_gprs;
    backend::FPRSMask active_fprs;
    HostRegWriteMap scalar_tie_fpr_writes;
    Map<u32, u32> scalar_tie_fixed_read_end;
    Vector<u32> scalar_tie_future_writes;
    const u32 gpr_reserve{0};
    const u32 fpr_reserve{0};
    const bool single_block_fast_path{false};
    const bool scalar_insert{false};
    const bool intwidth_tie{false};
    const bool induct_tie{false};
    const Vector<u32>* forced_spills{};
    const bool select_eviction{false};
    const bool shift_imm_fast{false};
    const FeatureSet features;
    const bool xmm_resident{false};
    bool fixed_class{false};
    bool has_eviction_candidate{false};
    u32 eviction_candidate{0};
    Vector<u32> fixed_gpr_alias_end{};
    Vector<u16> fixed_gpr_affinity{};
    Vector<u32> fixed_gpr_affinity_store{};
    Vector<u16> call_abi_affinity{};
    Vector<u8> block_local_values{};
    std::array<Vector<std::pair<u32, u32>>, 32> fixed_ranges{};
    Vector<bool> spill_slots{};
    Vector<u32> fixed_gpr_clobbers{};
    std::array<Vector<u32>, 32> fixed_gpr_clobber_points{};
    // Spill telemetry (reported at the end of AllocateRegisters): spilling
    // has never triggered on current workloads, so any hit is worth a log
    // line — it means the JIT's defensive MEM path is being exercised.
    u32 spill_count{0};
    u32 max_spill_slot{0};
    u32 active_spill_gpr{0};
    u32 active_spill_fpr{0};
    u32 max_live_gpr{0};
    u32 max_live_fpr{0};
    u32 fixed_affinity_reads{0};
    u32 fixed_affinity_writes{0};
    u32 fixed_hazards{0};
    u32 fixed_evictions{0};
    u32 fixed_copies_elided{0};
};
} // namespace
} // namespace swift::runtime::ir
