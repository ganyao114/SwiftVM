//
// Created by 甘尧 on 2023/12/6.
//

#include "register_alloc_pass.h"
#include "base/logging.h"
#include "runtime/common/perf_stats.h"

namespace swift::runtime::ir {

// Scratch headroom the linear scan must never hand out to a value.
//
// JitContext::GetTmpX takes a scratch register by picking the first register
// NOT set in this pass's per-instruction active mask, and there is no release
// mechanism -- scratch is recycled only at the next TickIR. If the scan fills
// every allocatable GPR, the active mask is all ones, GetTmpX has nothing to
// return and panics ("No free temporary GPR"). That makes spilling
// self-defeating: the very reload the spill requires (JitContext::SpillGPR)
// asks for scratch, so the first spilled value guarantees the panic.
// SVM_FUNC_BASE=0 on x87_bench_x86_64 and x87_topvirt_stress_x86_64 hit
// exactly this and aborted the guest.
//
// The headroom is therefore not a tuning knob but a correctness obligation.
// A previous fixed reserve of 4 was below the real peak of 8 (the vector
// NaN-fixup path behind VecFAdd/VecFMul/X87Op) and survived only because those
// opcodes happened never to appear in a saturated unit -- luck, not a
// guarantee.
//
// It is now derived and *checked* instead of tuned:
//
//   backend::ScratchBudget declares, per opcode, how much scratch its emitter
//   may hold at once (JitContext asserts nobody exceeds their declaration), and
//   after allocating, this pass VERIFIES its own output: for every instruction
//   it re-reads the mask it recorded and confirms the free-register count
//   covers that instruction's budget plus one reload register for each spilled
//   value the instruction names. A unit that fails is re-allocated with a
//   larger reserve.
//
// Verifying beats reserving up front because the reserve is a whole-unit
// property while the demand is per-instruction. Reserving the unit maximum
// would charge every instruction of a function for the one VecFAdd inside it:
// measured on this corpus that turned basic_coverage_smoke from zero spills
// into 777. Verification only escalates the units where high demand actually
// coincides with high pressure -- which, on the whole corpus, is none.

struct LiveInterval {
    Inst* inst{};
    u32 start{};
    u32 end{};

    bool operator<(const LiveInterval& other) const {
        if (start == other.start) {
            return end < other.end;
        } else {
            return start < other.start;
        }
    }
};

static Value ResolveBitCastSource(Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<Value>(0);
    }
    return value;
}

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

class LinearScanAllocator {
public:
    explicit LinearScanAllocator(HIRFunction* function,
                                 backend::RegAlloc* alloc,
                                 u32 gpr_res,
                                 u32 fpr_res,
                                 bool single_block_fast_path = false,
                                 bool scalar_insert = false,
                                 bool intwidth_tie = false,
                                 bool induct_tie = false,
                                 const Vector<u32>* forced_spills = nullptr,
                                 bool select_eviction = false,
                                 const FeatureSet& features = FeatureSet{},
                                 int xmm_resident_override = -1)
            : function(function), block(), reg_alloc(alloc), live_interval(), active_lives(),
              gpr_reserve(gpr_res), fpr_reserve(fpr_res),
              single_block_fast_path(single_block_fast_path),
              scalar_insert(scalar_insert),
              intwidth_tie(intwidth_tie),
              induct_tie(induct_tie),
              forced_spills(forced_spills),
              select_eviction(select_eviction),
              shift_imm_fast(features.shift_imm_fast), features(features),
              xmm_resident(xmm_resident_override >= 0
                                   ? xmm_resident_override != 0
                                   : GetSvmConfig().xmm_resident) {
        active_gprs = alloc->GetGprs();
        active_fprs = alloc->GetFprs();
        live_interval.reserve(function->MaxInstrCount());
        fixed_gpr_alias_end.resize(function->MaxInstrCount());
        InitializeFixedClobbers();
        if (single_block_fast_path) {
            fast_active_lives.reserve(function->MaxInstrCount());
        }
    }

    explicit LinearScanAllocator(Block* block,
                                 backend::RegAlloc* alloc,
                                 u32 gpr_res,
                                 u32 fpr_res,
                                 bool = false,
                                 bool scalar_insert = false,
                                 bool intwidth_tie = false,
                                 bool induct_tie = false,
                                 const Vector<u32>* forced_spills = nullptr,
                                 bool select_eviction = false,
                                 const FeatureSet& features = FeatureSet{},
                                 int xmm_resident_override = -1)
            : function(), block(block), reg_alloc(alloc), live_interval(), active_lives(),
              gpr_reserve(gpr_res), fpr_reserve(fpr_res),
              scalar_insert(scalar_insert),
              intwidth_tie(intwidth_tie),
              induct_tie(induct_tie),
              forced_spills(forced_spills),
              select_eviction(select_eviction),
              shift_imm_fast(features.shift_imm_fast), features(features),
              xmm_resident(xmm_resident_override >= 0
                                   ? xmm_resident_override != 0
                                   : GetSvmConfig().xmm_resident) {
        active_gprs = alloc->GetGprs();
        active_fprs = alloc->GetFprs();
        live_interval.reserve(block->GetInstList().size());
        fixed_gpr_alias_end.resize(block->MaxInstrId());
        InitializeFixedClobbers();
    }

    [[nodiscard]] u32 SpillCount() const { return spill_count; }
    [[nodiscard]] u32 SpillHighWater() const {
        return spill_count ? max_spill_slot + 1 : 0;
    }
    [[nodiscard]] u32 MaxLiveGPR() const { return max_live_gpr; }
    [[nodiscard]] u32 MaxLiveFPR() const { return max_live_fpr; }
    [[nodiscard]] u32 GPRReserve() const { return gpr_reserve; }
    [[nodiscard]] u32 FPRReserve() const { return fpr_reserve; }
    [[nodiscard]] bool HasEvictionCandidate() const { return has_eviction_candidate; }
    [[nodiscard]] u32 EvictionCandidate() const {
        ASSERT(has_eviction_candidate);
        return eviction_candidate;
    }
    void CoalesceGuestRegisterAccessesForTest() {
        CoalesceGuestRegisterAccesses();
    }
    void CoalesceGuestFPRAccessesForTest() {
        CoalesceGuestFPRAccesses(true);
    }

    static u32 ScratchOnlyGPRs(OpCode op, const backend::GPRSMask& pool,
                               const FeatureSet& features) {
        u32 count = backend::X86PinExtLevel3AluScratchEnabled(pool, op) ? 1u : 0u;
        const bool level2_scratch =
                backend::X86PinExtScratchOnlyEnabled(pool, features);
        if (level2_scratch) {
            const u32 fixed = backend::FixedGPRClobbers(op, features, true);
            count += ((fixed & (1u << 12)) ? 0u : 1u) +
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
            fill_gap(interval.start);

            ExpireOldIntervals(interval);

            if (IsForcedSpill(interval)) {
                SpillAtInterval(interval);
            } else if (!IsFloatValue(interval.inst)) {
                if (TryTieMemoryOperand(interval)) {
                    // A single-use identity GetOperand and its source share a
                    // register. The GetOperand result then owns that register
                    // until the memory operation, so the emitter can remove
                    // the identity copy without extending the source SSA's
                    // live range.
                } else if (TryTieNarrowLoad(interval)) {
                    // A plain narrow load and its extension chain share one W/X
                    // register. The ARM64 emitter can then replace
                    // LDRH+SXTH (or LDRB+UXTB) with the extending load itself,
                    // and a following 32->64 W write becomes a true no-op.
                } else if (intwidth_tie && TryTieIntWidth(interval)) {
                    // A proven W-clean source dies exactly at an identity
                    // 32-bit bridge. Transfer its physical register to the
                    // result so the existing SharesGPR emitter path removes
                    // the bridge without extending the source interval.
                } else if (induct_tie && TryTieInduction(interval)) {
                    // A fixed guest GPR snapshot dies at this U64 induction
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
        perf_assign.Stop();

        FusePinnedWriteChains();
        CoalesceGuestRegisterAccesses();
        CoalesceGuestFPRAccesses(xmm_resident);
        CacheConstantAddresses();
        if (spill_count && RaDiagEnabled()) {
            LOG_WARNING("RegisterAllocPass: {} value(s) spilled to stack slots (highest slot {})",
                        spill_count, max_spill_slot);
        }
    }

private:
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
            if (!is_float) {
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
        if (!backend::X86PinExtLevel2Enabled(reg_alloc->GetGprs())) {
            return;
        }
        auto fuse_block = [&](Block* lir_block) {
            auto& list = lir_block->GetInstList();
            // Full-width exchanges are represented as mutually dependent
            // GetHost/SetHost pairs. Moving any neighbouring 32-bit producer
            // into a static destination before those pairs have consumed
            // their snapshots can create an implicit physical-register cycle.
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
                // valid snapshot for later SSA users because the scan above
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
                // snapshot. A fixed-register read is the useful exception:
                // after copying source->target, later SSA users may read the
                // target until the next architectural target write. CRC's
                // initial EDI snapshot has exactly this shape.
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
                        // into a W55 callee-saved pin, with every snapshot use
                        // before the source changes (the CRC EDI->EDX chain).
                        const bool safe_level2_snapshot =
                                source_uses > 1 && source_host <= 9 &&
                                (target == 22 || target == 23 || target == 29) &&
                                !source_used_after_write;
                        if (source_written && !safe_level2_snapshot) {
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

    static bool IsPinnedCoalesceTarget(u32 reg) {
        return reg <= 9 || reg == 22 || reg == 23 || reg == 29;
    }

    static bool IsPinnedCoalesceProducer(OpCode op) {
        switch (op) {
            case OpCode::LoadImm:
            case OpCode::LoadUniform:
            case OpCode::Zero:
            case OpCode::Add:
            case OpCode::Sub:
            case OpCode::And:
            case OpCode::AndNot:
            case OpCode::Or:
            case OpCode::Xor:
            case OpCode::Adc:
            case OpCode::Sbb:
            case OpCode::Mul:
            case OpCode::Div:
            case OpCode::Not:
            case OpCode::LslImm:
            case OpCode::LslValue:
            case OpCode::LsrImm:
            case OpCode::LsrValue:
            case OpCode::AsrImm:
            case OpCode::AsrValue:
            case OpCode::RorImm:
            case OpCode::RorValue:
            case OpCode::ByteSwap:
            case OpCode::BitExtract:
            case OpCode::BitClear:
            case OpCode::Select:
            case OpCode::CondSelect:
            case OpCode::MulHigh:
                return true;
            default:
                return false;
        }
    }

    static bool IsPinnedCoalesceObserver(OpCode op) {
        switch (op) {
            case OpCode::SaveFlags:
            case OpCode::LoadMemory:
            case OpCode::StoreMemory:
            case OpCode::LoadMemoryTSO:
            case OpCode::StoreMemoryTSO:
            case OpCode::MemoryCopy:
            case OpCode::MemoryCopyTSO:
            case OpCode::CompareAndSwap:
            case OpCode::CompareAndSwap128:
            case OpCode::CheckMemoryAlignment:
            case OpCode::AtomicExchange:
            case OpCode::AtomicFetchAdd:
            case OpCode::AtomicRMW:
            case OpCode::CallLambda:
            case OpCode::CallLocation:
            case OpCode::CallDynamic:
            case OpCode::X87Op:
            case OpCode::Sse42Str:
            case OpCode::GetUniformAddress:
            case OpCode::UniformBarrier:
                return true;
            default:
                return false;
        }
    }

    static bool IsResidentFPRTarget(u32 reg) {
        // The actual enabled subset is determined by the allocator's reserved
        // FPR mask below.  Keeping one architectural range avoids a parallel
        // high-XMM coalescer and naturally supports the P1 partial map.
        return reg >= 16 && reg <= 31;
    }

    static bool IsResidentFPRProducer(OpCode op) {
        switch (op) {
            // These emitters either write a fresh destination or use a single
            // A64 three-register SIMD instruction. Their source/destination
            // aliasing is architectural; the input-liveness guard below still
            // rejects an input that must survive the publication point.
            case OpCode::LoadUniform:
            case OpCode::LoadMemory:
            case OpCode::VecAnd:
            case OpCode::VecOr:
            case OpCode::VecXor:
            case OpCode::VecAdd:
            case OpCode::VecSub:
            case OpCode::VecMul:
            case OpCode::VecFAdd:
            case OpCode::VecFSub:
            case OpCode::VecFMul:
            case OpCode::VecFDiv:
                return true;
            default:
                return false;
        }
    }

    struct ConstAddressCandidate {
        Inst* inst{};
        u32 use_id{};
    };

    static std::optional<u64> ConstAddressValue(Inst* inst) {
        if (!inst || inst->GetOp() != OpCode::GetOperand ||
            inst->ReturnType() != ValueType::U64 || inst->GetUses() != 1) {
            return std::nullopt;
        }
        const auto operand = inst->GetArg<Operand>(0);
        if (!operand.GetLeft().IsImm()) {
            return std::nullopt;
        }
        if (operand.GetRight().Null()) {
            return operand.GetLeft().imm.Get();
        }
        if (operand.GetOp() == OperandOp::Plus && operand.GetRight().IsImm() &&
            operand.GetRight().imm.Get() == 0) {
            return operand.GetLeft().imm.Get();
        }
        return std::nullopt;
    }

    static bool IsConstAddressBarrier(OpCode op) {
        return op == OpCode::Goto || op == OpCode::NotGoto ||
               op == OpCode::BindLabel;
    }

    void CacheConstantAddresses() {
        if (!features.const_addr_cache) {
            return;
        }

        auto cache_block = [&](Block* lir_block) {
            struct Group {
                u64 address{};
                Vector<ConstAddressCandidate> candidates{};
            };
            Vector<Group> groups{};
            auto& list = lir_block->GetInstList();

            auto process_groups = [&] {
                for (auto& group : groups) {
                    if (group.candidates.size() < 2) {
                        continue;
                    }
                    std::size_t first = 0;
                    while (first + 1 < group.candidates.size()) {
                        bool cached = false;
                        for (std::size_t last = group.candidates.size() - 1;
                             last > first && !cached; --last) {
                            const u32 range_begin = group.candidates[first].inst->Id();
                            const u32 range_end = group.candidates[last].use_id;
                            for (u32 target = 0; target < 32 && !cached; ++target) {
                                if (reg_alloc->GetGprs().Get(target)) {
                                    continue;
                                }
                                bool free = true;
                                for (auto& scan : list) {
                                    if (scan.Id() < range_begin || scan.Id() > range_end) {
                                        continue;
                                    }
                                    if (reg_alloc->DirtyGPR(scan.Id()).Get(target)) {
                                        free = false;
                                        break;
                                    }
                                }
                                if (!free) {
                                    continue;
                                }

                                // 缓存所有者记入每条指令的 active mask。这样
                                // emitter 瞬时 scratch 与 spill reload 仍由既有硬门
                                // 计费；少一条空闲 GPR 后无法容纳就原样回退。
                                struct SavedMask {
                                    u32 id;
                                    backend::GPRSMask gprs;
                                    backend::FPRSMask fprs;
                                };
                                Vector<SavedMask> saved{};
                                for (auto& scan : list) {
                                    if (scan.Id() < range_begin || scan.Id() > range_end) {
                                        continue;
                                    }
                                    auto gprs = reg_alloc->DirtyGPR(scan.Id());
                                    auto fprs = reg_alloc->DirtyFPR(scan.Id());
                                    saved.push_back({scan.Id(), gprs, fprs});
                                    gprs.Mark(target);
                                    reg_alloc->SetActiveRegs(scan.Id(), gprs, fprs);
                                }
                                bool verified = true;
                                for (auto& scan : list) {
                                    if (scan.Id() >= range_begin && scan.Id() <= range_end &&
                                        !CheckInstr(&scan, 0, 0)) {
                                        verified = false;
                                        break;
                                    }
                                }
                                if (!verified) {
                                    for (auto& old : saved) {
                                        reg_alloc->SetActiveRegs(old.id, old.gprs, old.fprs);
                                    }
                                    continue;
                                }

                                const u32 anchor = group.candidates[first].inst->Id();
                                for (std::size_t i = first; i <= last; ++i) {
                                    auto* candidate = group.candidates[i].inst;
                                    reg_alloc->MapRegister(
                                            candidate->Id(),
                                            HostGPR{static_cast<u16>(target)});
                                    reg_alloc->MarkConstAddressCached(candidate->Id(), anchor);
                                }
                                first = last + 1;
                                cached = true;
                            }
                        }
                        if (!cached) {
                            ++first;
                        }
                    }
                }
                groups.clear();
            };

            for (auto& inst : list) {
                if (IsConstAddressBarrier(inst.GetOp())) {
                    process_groups();
                    continue;
                }
                const auto address = ConstAddressValue(&inst);
                if (!address ||
                    reg_alloc->ValueType(Value{&inst}) != backend::RegAlloc::GPR ||
                    !DirectlyFeedsMemory(&inst)) {
                    continue;
                }
                u32 use_id = inst.Id();
                bool valid_use = false;
                for (auto& use : list) {
                    if (use.Id() <= inst.Id()) {
                        continue;
                    }
                    if (IsConstAddressBarrier(use.GetOp())) {
                        break;
                    }
                    bool names = false;
                    for (auto value : use.GetValues()) {
                        names |= value.Def() == &inst;
                    }
                    if (names) {
                        use_id = use.Id();
                        valid_use = true;
                        break;
                    }
                }
                if (!valid_use) {
                    continue;
                }
                auto group = std::find_if(groups.begin(), groups.end(),
                                          [&](const Group& item) {
                                              return item.address == *address;
                                          });
                if (group == groups.end()) {
                    groups.push_back(Group{*address, {}});
                    group = std::prev(groups.end());
                }
                group->candidates.push_back({&inst, use_id});
            }
            process_groups();
        };

        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                cache_block(hir_block->GetBlock());
            }
        } else {
            cache_block(block);
        }
    }

    void CoalesceGuestRegisterAccesses() {
        if (!features.ra_coalesce ||
            !backend::X86PinExtLevel2Enabled(reg_alloc->GetGprs())) {
            return;
        }

        auto coalesce_block = [&](Block* lir_block) {
            auto& list = lir_block->GetInstList();
            Vector<u32> use_end(InstrCount());
            for (auto& inst : list) {
                for (auto value : inst.GetValues()) {
                    value = ResolveBitCastSource(value);
                    if (value.Defined() && value.Id() < use_end.size()) {
                        use_end[value.Id()] = std::max<u32>(use_end[value.Id()], inst.Id());
                    }
                }
            }
            if (list.begin() != list.end()) {
                const u32 block_end = std::prev(list.end())->Id();
                auto extend_terminal = [&](const Value& value) {
                    auto root = ResolveBitCastSource(value);
                    if (root.Defined() && root.Id() < use_end.size()) {
                        use_end[root.Id()] = std::max(use_end[root.Id()], block_end);
                    }
                };
                std::function<void(const Terminal&)> walk_terminal =
                        [&](const Terminal& terminal_value) {
                            VisitVariant<void>(terminal_value, [&](const auto& edge) {
                                using T = std::decay_t<decltype(edge)>;
                                if constexpr (std::is_same_v<T, terminal::If>) {
                                    extend_terminal(edge.cond);
                                    walk_terminal(edge.then_);
                                    walk_terminal(edge.else_);
                                } else if constexpr (std::is_same_v<T, terminal::Switch>) {
                                    extend_terminal(edge.value);
                                    for (const auto& item : edge.cases) {
                                        walk_terminal(item.then);
                                    }
                                } else if constexpr (std::is_same_v<T, terminal::Condition>) {
                                    walk_terminal(edge.then_);
                                    walk_terminal(edge.else_);
                                } else if constexpr (std::is_same_v<T, terminal::CheckHalt>) {
                                    walk_terminal(edge.else_);
                                }
                            });
                        };
                walk_terminal(lir_block->GetTerminal());
            }

            auto mapped_to = [&](Value value, u32 target) {
                value = ResolveBitCastSource(value);
                return value.Defined() &&
                       reg_alloc->ValueType(value) == backend::RegAlloc::GPR &&
                       reg_alloc->ValueGPR(value).id == target;
            };
            auto has_target_conflict = [&](Inst* producer, Inst* wrapper,
                                           Inst* store, u32 target) {
                for (auto& other : list) {
                    if (&other == producer || &other == wrapper || &other == store ||
                        !other.HasValue() || other.IsBitCastOperation() ||
                        other.Id() >= store->Id()) {
                        continue;
                    }
                    Value value{&other};
                    if (!mapped_to(value, target)) {
                        continue;
                    }
                    const u32 end = value.Id() < use_end.size() ? use_end[value.Id()] : value.Id();
                    // A pre-existing tie can define a new value in the
                    // producer->publication window. Its ordinary emitter then
                    // writes the fixed home even though no Get/SetHostGPR is
                    // present for the observer scan to see. Reject every
                    // third-party target interval intersecting that window,
                    // from either side of the producer definition.
                    if (end > producer->Id()) {
                        return true;
                    }
                }
                return false;
            };

            for (auto& store : list) {
                if (store.GetOp() != OpCode::SetHostGPR ||
                    store.GetArg<Imm>(2).Get() != 0) {
                    continue;
                }
                const u32 target = store.GetArg<Imm>(1).Get();
                if (!IsPinnedCoalesceTarget(target) ||
                    !reg_alloc->GetGprs().Get(target)) {
                    continue;
                }

                Value stored = ResolveBitCastSource(store.GetArg<Value>(0));
                auto* wrapper = stored.Def();
                if (!wrapper || stored.Id() >= use_end.size() ||
                    wrapper->GetUses() != 1 || use_end[stored.Id()] != store.Id()) {
                    continue;
                }

                Value produced = stored;
                bool zero_extend_chain = false;
                if (wrapper->GetOp() == OpCode::ZeroExtend32To64) {
                    produced = ResolveBitCastSource(wrapper->GetArg<Value>(0));
                    zero_extend_chain = true;
                    if (!HasKnownWWrite(produced) || !produced.Def() ||
                        produced.Id() >= use_end.size() || produced.Def()->GetUses() != 1 ||
                        use_end[produced.Id()] != wrapper->Id()) {
                        continue;
                    }
                }
                auto* producer = produced.Def();
                if (!producer || !IsPinnedCoalesceProducer(producer->GetOp()) ||
                    reg_alloc->ValueType(produced) != backend::RegAlloc::GPR) {
                    continue;
                }
                const u32 width = GetValueSizeByte(produced.Type());
                if ((width != sizeof(u32) && width != sizeof(u64)) ||
                    (width == sizeof(u32) && !HasKnownWWrite(produced))) {
                    continue;
                }
                if (mapped_to(produced, target) &&
                    (!zero_extend_chain || mapped_to(stored, target))) {
                    continue;
                }
                if (has_target_conflict(producer, wrapper, &store, target)) {
                    continue;
                }

                bool after_producer = false;
                bool blocked = false;
                for (auto& scan : list) {
                    if (&scan == producer) {
                        after_producer = true;
                        continue;
                    }
                    if (!after_producer || &scan == wrapper) {
                        continue;
                    }
                    if (&scan == &store) {
                        break;
                    }
                    if (IsPinnedCoalesceObserver(scan.GetOp()) ||
                        (scan.GetOp() == OpCode::GetHostGPR &&
                         scan.GetArg<Imm>(0).Get() == target) ||
                        (scan.GetOp() == OpCode::SetHostGPR &&
                         scan.GetArg<Imm>(1).Get() == target)) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) {
                    continue;
                }
                for (auto input : producer->GetValues()) {
                    auto root = ResolveBitCastSource(input);
                    if (mapped_to(root, target) && root.Id() < use_end.size() &&
                        use_end[root.Id()] > producer->Id()) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) {
                    continue;
                }

                reg_alloc->MapRegister(producer->Id(), HostGPR{static_cast<u16>(target)});
                if (zero_extend_chain) {
                    reg_alloc->MapRegister(wrapper->Id(), HostGPR{static_cast<u16>(target)});
                }
                reg_alloc->MarkHostWriteCoalesced(store.Id());
            }

            for (auto& read : list) {
                if (read.GetOp() != OpCode::GetHostGPR ||
                    read.GetArg<Imm>(1).Get() != 0 ||
                    GetValueSizeByte(read.ReturnType()) != sizeof(u32)) {
                    continue;
                }
                const u32 target = read.GetArg<Imm>(0).Get();
                if (!IsPinnedCoalesceTarget(target) ||
                    !reg_alloc->GetGprs().Get(target) || read.Id() >= use_end.size()) {
                    continue;
                }
                Inst* latest_store = nullptr;
                bool blocked = false;
                for (auto& scan : list) {
                    if (&scan == &read) {
                        break;
                    }
                    if (scan.GetOp() == OpCode::SetHostGPR &&
                        scan.GetArg<Imm>(1).Get() == target) {
                        latest_store = &scan;
                        blocked = false;
                        continue;
                    }
                    if (latest_store && IsPinnedCoalesceObserver(scan.GetOp())) {
                        blocked = true;
                    }
                }
                if (!latest_store || blocked) {
                    continue;
                }
                auto published = ResolveBitCastSource(latest_store->GetArg<Value>(0));
                const bool high_zero = published.Def() &&
                        ((published.Def()->GetOp() == OpCode::ZeroExtend32To64 &&
                          GetValueSizeByte(published.Def()->GetArg<Value>(0).Type()) == sizeof(u32)) ||
                         (GetValueSizeByte(published.Type()) == sizeof(u32) &&
                          HasKnownWWrite(published)));
                if (!high_zero) {
                    continue;
                }
                for (auto& scan : list) {
                    if (scan.Id() <= read.Id() || scan.Id() > use_end[read.Id()]) {
                        continue;
                    }
                    if (scan.GetOp() == OpCode::SetHostGPR &&
                        scan.GetArg<Imm>(1).Get() == target) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) {
                    continue;
                }
                reg_alloc->MapRegister(read.Id(), HostGPR{static_cast<u16>(target)});
                reg_alloc->MarkHostReadCoalesced(read.Id());
            }
        };

        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                coalesce_block(hir_block->GetBlock());
            }
        } else {
            coalesce_block(block);
        }
    }

    void CoalesceGuestFPRAccesses(bool enabled = GetSvmConfig().xmm_resident) {
        if (!enabled) {
            return;
        }

        auto coalesce_block = [&](Block* lir_block) {
            auto& list = lir_block->GetInstList();
            Vector<u32> use_end(InstrCount());
            for (auto& inst : list) {
                for (auto value : inst.GetValues()) {
                    value = ResolveBitCastSource(value);
                    if (value.Defined() && value.Id() < use_end.size()) {
                        use_end[value.Id()] = std::max<u32>(use_end[value.Id()], inst.Id());
                    }
                }
            }

            auto mapped_to = [&](Value value, u32 target) {
                value = ResolveBitCastSource(value);
                return value.Defined() &&
                       reg_alloc->ValueType(value) == backend::RegAlloc::FPR &&
                       reg_alloc->ValueFPR(value).id == target;
            };

            for (auto& store : list) {
                if (store.GetOp() != OpCode::SetHostFPR ||
                    store.GetArg<Imm>(2).Get() != 0) {
                    continue;
                }
                const u32 target = store.GetArg<Imm>(1).Get();
                if (!IsResidentFPRTarget(target) || !reg_alloc->GetFprs().Get(target)) {
                    continue;
                }
                Value produced = ResolveBitCastSource(store.GetArg<Value>(0));
                auto* producer = produced.Def();
                if (!producer || produced.Id() >= use_end.size() ||
                    produced.Type() != ValueType::V128 ||
                    use_end[produced.Id()] != store.Id() ||
                    !IsResidentFPRProducer(producer->GetOp()) ||
                    reg_alloc->ValueType(produced) != backend::RegAlloc::FPR) {
                    continue;
                }
                if (mapped_to(produced, target)) {
                    reg_alloc->MarkHostWriteCoalesced(store.Id());
                    continue;
                }

                bool blocked = false;
                for (auto& other : list) {
                    if (&other == producer || &other == &store || !other.HasValue() ||
                        other.IsBitCastOperation() || other.Id() >= store.Id()) {
                        continue;
                    }
                    Value value{&other};
                    if (mapped_to(value, target) && value.Id() < use_end.size() &&
                        use_end[value.Id()] > producer->Id()) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) continue;

                bool after_producer = false;
                for (auto& scan : list) {
                    if (&scan == producer) {
                        after_producer = true;
                        continue;
                    }
                    if (!after_producer) continue;
                    if (&scan == &store) break;
                    if (IsPinnedCoalesceObserver(scan.GetOp()) ||
                        (scan.GetOp() == OpCode::GetHostFPR &&
                         scan.GetArg<Imm>(0).Get() == target) ||
                        (scan.GetOp() == OpCode::SetHostFPR &&
                         scan.GetArg<Imm>(1).Get() == target)) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) continue;

                for (auto input : producer->GetValues()) {
                    auto root = ResolveBitCastSource(input);
                    if (mapped_to(root, target) && root.Id() < use_end.size() &&
                        use_end[root.Id()] > producer->Id()) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) continue;

                reg_alloc->MapRegister(producer->Id(), HostFPR{static_cast<u16>(target)});
                reg_alloc->MarkHostWriteCoalesced(store.Id());
            }
        };

        if (function) {
            for (auto* hir_block : function->GetHIRBlocks()) {
                coalesce_block(hir_block->GetBlock());
            }
        } else {
            coalesce_block(block);
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

    Value NarrowLoadTieSource(Inst* inst) const {
        if (shift_imm_fast) {
            if (inst->GetOp() == OpCode::BitExtract &&
                inst->GetArg<Imm>(1).Get() == 0 &&
                inst->GetArg<Imm>(2).Get() == GetValueSizeByte(inst->ReturnType()) * 8 &&
                DirectlyFeedsImmediateShift(inst)) {
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

    bool HasKnownWWrite(Value value) const {
        if (!value.Defined()) {
            return false;
        }
        auto* def = value.Def();
        // These are aliases/reads, not physical W writes. In particular a
        // U32 GetHostGPR only selects the W view of a pinned X register; it
        // does not prove that X[63:32] is zero.
        if (def->GetOp() == OpCode::GetHostGPR || def->IsBitCastOperation()) {
            return false;
        }
        if (def->GetOp() == OpCode::BitExtract &&
            GetValueSizeByte(def->ReturnType()) == sizeof(u32) &&
            def->GetArg<Imm>(1).Get() == 0 &&
            def->GetArg<Imm>(2).Get() == 32) {
            return HasKnownWWrite(def->GetArg<Value>(0));
        }
        if (def->GetOp() == OpCode::ZeroExtend32To64 &&
            GetValueSizeByte(def->GetArg<Value>(0).Type()) == sizeof(u32)) {
            return HasKnownWWrite(def->GetArg<Value>(0));
        }
        return GetValueSizeByte(def->ReturnType()) == sizeof(u32);
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
        if (!right.GetRight().Null() || !right.GetLeft().IsValue()) {
            return {};
        }
        const auto immediate = right.GetLeft().value;
        if (!immediate.Def() || immediate.Def()->GetOp() != OpCode::LoadImm ||
            immediate.Def()->GetUses() == 0 ||
            GetValueSizeByte(immediate.Type()) != sizeof(u64) ||
            !IsAddImmediate(immediate.Def()->GetArg<Imm>(0).Get())) {
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
                    // SSA snapshot if the result is still live (for example
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
            return false;
        }
        reg_alloc->MapRegister(current.inst->Id(), reg_alloc->ValueFPR(source));
        return true;
    }

    // True when a value ends up in State::spill_area, so every access to it
    // costs a scratch register.
    bool IsSpilled(const Value& value) {
        return value.Defined() &&
               reg_alloc->ValueType(ResolveBitCastSource(value)) == backend::RegAlloc::MEM;
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
            (IsFloatValue(source.Def()) ? reload_fpr : reload_gpr)++;
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
        for (auto* hir_block : hir_function->GetHIRBlocks()) {
            auto* lir_block = hir_block->GetBlock();
            u32 block_end = 0;
            for (auto& inst : lir_block->GetInstList()) {
                block_end = std::max<u32>(block_end, inst.Id());
                if (inst.GetOp() == OpCode::SetHostGPR) {
                    const auto host_reg = static_cast<u16>(inst.GetArg<Imm>(1).Get());
                    host_gpr_writes[host_reg].push_back(inst.Id());
                } else if (inst.GetOp() == OpCode::SetHostFPR) {
                    const auto host_reg = static_cast<u16>(inst.GetArg<Imm>(1).Get());
                    host_fpr_writes[host_reg].push_back(inst.Id());
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
            u32 end{hir_value.GetOrderId()};
            std::for_each(hir_value.uses.begin(), hir_value.uses.end(), [&end](auto& use) {
                end = std::max(end, (u32) use.inst->Id());
            });
            // Block optimization passes can turn a LoadUniform into a BitCast
            // after HIR use lists were built. Scan the current instructions
            // above so alias roots remain live even when those lists are stale.
            if (auto it = actual_use_end.find(instr->Id()); it != actual_use_end.end()) {
                end = std::max(end, it->second);
            }
            // Extend for terminal uses (see above): the value must stay live
            // until the end of the block whose terminal reads it.
            if (auto it = terminal_end.find(instr->Id()); it != terminal_end.end()) {
                end = std::max(end, it->second);
            }
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
            // register. Preserve snapshot semantics by forcing a copy when a
            // SetHostGPR can overwrite that register before the value's last use.
            const bool fixed_gpr_get =
                    full_gpr_get &&
                    !LiveRangeCrossesHostRegWrite(host_gpr_writes, host_index, instr->Id(), end);
            const bool fixed_fpr_alias =
                    fixed_fpr_get &&
                    end <= definition_block_end[instr->Id()] &&
                    !LiveRangeCrossesHostRegWrite(host_fpr_writes, host_index, instr->Id(), end);
            if (fixed_fpr_alias || fixed_gpr_get) {
                if (fixed_fpr_alias) {
                    reg_alloc->MapRegister(hir_value.GetOrderId(), HostFPR{host_index});
                    reg_alloc->MarkHostReadCoalesced(hir_value.GetOrderId());
                } else {
                    reg_alloc->MapRegister(hir_value.GetOrderId(), HostGPR{host_index});
                    fixed_gpr_alias_end[hir_value.GetOrderId()] = end;
                }
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
            u32 end{hir_value.GetOrderId()};
            // Preserve the generic collector's use-list maximum even when an
            // optimization has left a stale use behind. The current-argument
            // scan below is the matching second maximum: it catches rewritten
            // BitCast roots whose new use is absent from the old list.
            std::for_each(hir_value.uses.begin(), hir_value.uses.end(), [&end](auto& use) {
                end = std::max(end, static_cast<u32>(use.inst->Id()));
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
                    fixed_fpr_get &&
                    std::any_of(host_fpr_writes.begin(), host_fpr_writes.end(),
                                [host_index, instr, end](const auto& write) {
                                    return write.first == host_index && instr->Id() < write.second &&
                                           write.second <= end;
                                });
            const bool fixed_fpr_alias = fixed_fpr_get && !crosses_host_fpr_write;
            if (fixed_fpr_alias || fixed_gpr_get) {
                if (fixed_fpr_alias) {
                    reg_alloc->MapRegister(hir_value.GetOrderId(), HostFPR{host_index});
                    reg_alloc->MarkHostReadCoalesced(hir_value.GetOrderId());
                } else {
                    reg_alloc->MapRegister(hir_value.GetOrderId(), HostGPR{host_index});
                    fixed_gpr_alias_end[hir_value.GetOrderId()] = end;
                }
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
            // the GetHostGPR result a snapshot, not a zero-cost alias.
            const bool fixed_gpr_get =
                    full_gpr_get &&
                    !LiveRangeCrossesHostRegWrite(
                            host_gpr_writes, host_index, instr.Id(), use_end[instr.Id()]);
            const bool fixed_fpr_alias =
                    fixed_fpr_get &&
                    !LiveRangeCrossesHostRegWrite(
                            host_fpr_writes, host_index, instr.Id(), use_end[instr.Id()]);
            if (fixed_fpr_alias || fixed_gpr_get) {
                if (fixed_fpr_alias) {
                    reg_alloc->MapRegister(instr.Id(), HostFPR{host_index});
                    reg_alloc->MarkHostReadCoalesced(instr.Id());
                } else {
                    reg_alloc->MapRegister(instr.Id(), HostGPR{host_index});
                    fixed_gpr_alias_end[instr.Id()] = use_end[instr.Id()];
                }
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
        active_fprs.Clear(id);
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
    bool has_eviction_candidate{false};
    u32 eviction_candidate{0};
    Vector<u32> fixed_gpr_alias_end{};
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
};

class VRegisterAllocator {
public:
    explicit VRegisterAllocator(Block* block)
            : block(block), live_interval(), active_lives() {
    }

    void AllocateRegisters() {
        CollectLiveIntervals();
        // Step 2: Sort live intervals
        std::sort(live_interval.begin(), live_interval.end());

        // Step 3: Alloc Registers
        for (auto& interval : live_interval) {
            ExpireOldIntervals(interval);

            active_lives.push_back(interval);
            AllocVReg(interval);
        }
    }

    void ExpireOldIntervals(LiveInterval& current) {
        for (auto it = active_lives.begin(); it != active_lives.end();) {
            if (it->end < current.start) {
                active_v_regs[it->inst->VirRegID()] = false;
                if (IsFloatValue(it->inst)) {
                    active_v_regs[it->inst->VirRegID() + 1] = false;
                }
                it = active_lives.erase(it);  // Remove expired intervals
            } else {
                ++it;
            }
        }
    }

    bool IsFloatValue(Inst* inst) {
        auto value_type = inst->ReturnType();
        return value_type >= ValueType::V8 && value_type <= ValueType::V256;
    }

    void GrowVRegs(u32 new_item_size) {
        active_v_regs_cursor = active_v_regs.size();
        active_v_regs.resize(active_v_regs_cursor + new_item_size);
    }

    void AllocVReg(LiveInterval& interval) {
        auto is_float = IsFloatValue(interval.inst);
        auto slot_size = is_float ? 2 : 1;
        if (is_float) {
            s32 slot{-1};
            for (int i = 0; i + 1 < active_v_regs.size(); i += 2) {
                if (!active_v_regs[i] && !active_v_regs[i + 1]) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                slot = active_v_regs.size();
                GrowVRegs(slot_size);
            }
            interval.inst->SetVirReg(slot);
            active_v_regs[slot] = true;
        } else {
            auto itr = std::find(active_v_regs.begin(), active_v_regs.end(), false);
            if (itr != active_v_regs.end()) {
                u16 slot = std::distance(active_v_regs.begin(), itr);
                active_v_regs[slot] = true;
                interval.inst->SetVirReg(slot);
            } else {
                // grow stack
                u16 slot = active_v_regs_cursor;
                GrowVRegs(slot_size);
                active_v_regs[slot] = true;
                interval.inst->SetVirReg(slot);
            }
        }
    }

private:

    void CollectLiveIntervals() {
        StackVector<u16, 64> use_end{};
        use_end.resize(block->GetInstList().size());
        for (auto& instr : block->GetInstList()) {
            auto values = instr.GetValues();
            for (auto &value : values) {
                auto &end = use_end[value.Id()];
                end = std::max(end, instr.Id());
            }
        }
        for (auto& instr : block->GetInstList()) {
            if (instr.HasValue()) {
                auto start = instr.Id();
                auto end = use_end[start];
                live_interval.push_back({&instr, start, end});
            }
        }
    }

    Block* block;
    Vector<LiveInterval> live_interval;
    List<LiveInterval> active_lives;
    Vector<bool> active_v_regs{};
    u16 active_v_regs_cursor{0};
};

void RegisterAllocPass::Run(HIRBuilder* hir_builder, backend::RegAlloc* reg_alloc,
                            const FeatureSet& features) {
    for (auto& hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func, reg_alloc, features);
    }
}

// Allocate, then check the result against what the emitters actually need
// (LinearScanAllocator::Verify). Escalating the reserve only on failure is
// what keeps the guarantee free: the first attempt reserves the ordinary
// emitter shape and, on this corpus, always passes.
//
// The escalation ladder is finite and its last rung is sufficient by
// construction -- reserving the unit's largest opcode budget plus its largest
// possible reload count leaves every instruction more free registers than it
// can ask for -- so the loop cannot spin.
static void CollectUnitBudget(Block* block,
                              u32& gpr,
                              u32& fpr,
                              u32& reload_bound,
                              const backend::GPRSMask& pool,
                              const FeatureSet& features) {
    for (auto& inst : block->GetInstList()) {
        auto need = backend::ScratchBudget(inst, features);
        const u32 scratch_only =
                LinearScanAllocator::ScratchOnlyGPRs(inst.GetOp(), pool, features);
        gpr = std::max<u32>(gpr, need.gpr > scratch_only ? need.gpr - scratch_only : 0);
        fpr = std::max<u32>(fpr, need.fpr);
        reload_bound = std::max<u32>(reload_bound, static_cast<u32>(inst.GetValues().size()) + 1);
    }
}

static void CollectUnitBudget(HIRFunction* function,
                              u32& gpr,
                              u32& fpr,
                              u32& reload_bound,
                              const backend::GPRSMask& pool,
                              const FeatureSet& features) {
    for (auto* hir_block : function->GetHIRBlocks()) {
        CollectUnitBudget(
                hir_block->GetBlock(), gpr, fpr, reload_bound, pool, features);
    }
}

template <typename Unit>
static void RunVerified(Unit* unit,
                        backend::RegAlloc* reg_alloc,
                        const FeatureSet& features,
                        bool single_block_fast_path = false,
                        bool scalar_insert = false,
                        bool intwidth_tie = false,
                        bool induct_tie = false,
                        bool spill_evict = true,
                        RegisterAllocPass::SpillEvictTestResult* test_result = nullptr) {
    auto rerun_with_conditional_spill_scratch = [&] {
#if defined(__linux__) && !defined(__ANDROID__)
        const u32 spills = reg_alloc->SpillCount();
        if (spills != 0 && !reg_alloc->GetGprs().Get(18)) {
            // The first pass answers the only question that justifies paying
            // for x18: does this unit spill at all? If yes, rerun with x18 in
            // this unit's pool baseline. Reusing the first allocation would
            // be unsafe because it may already have assigned a live value to
            // x18, which the spill reload path must be free to overwrite.
            reg_alloc->ReserveGPRForUnit(18);
            reg_alloc->ResetAllocations();
            RunVerified(unit, reg_alloc, features, single_block_fast_path, scalar_insert,
                        intwidth_tie, induct_tie, spill_evict, test_result);
            if (RaDiagEnabled()) {
                LOG_WARNING("RegisterAllocPass: {} initial spill(s); x18 reserved for this unit",
                            spills);
            }
            return true;
        }
#endif
        return false;
    };
    auto record_shape = [&](const LinearScanAllocator& scan) {
        if (!RAShapeProfEnabled()) return;
        auto& shape = reg_alloc->RAShape();
        shape = {};
        shape.ra_valid = true;
        auto gprs = reg_alloc->GetGprs();
        auto fprs = reg_alloc->GetFprs();
        shape.gpr_pool = gprs.GetClearCount();
        shape.fpr_pool = fprs.GetClearCount();
        shape.max_live_gpr = scan.MaxLiveGPR();
        shape.max_live_fpr = scan.MaxLiveFPR();
        shape.scratch_gpr = scan.GPRReserve();
        shape.scratch_fpr = scan.FPRReserve();
        shape.spill_defs = scan.SpillCount();
        shape.spill_high_water = scan.SpillHighWater();
        if constexpr (std::is_same_v<Unit, Block>) {
            RAShapeAnalyzeFlags(unit, shape);
        } else {
            for (auto* hir_block : unit->GetHIRBlocks()) {
                RAShapeAnalyzeFlags(hir_block->GetBlock(), shape);
            }
        }
    };
    auto record_test_result = [&](const LinearScanAllocator& scan,
                                  u32 eviction_restarts,
                                  bool fell_back_to_ladder) {
        if (!test_result) return;
        test_result->eviction_restarts = eviction_restarts;
        test_result->final_gpr_reserve = scan.GPRReserve();
        test_result->final_fpr_reserve = scan.FPRReserve();
        test_result->fell_back_to_ladder = fell_back_to_ladder;
    };
    const bool scratch_only_enabled =
            backend::X86PinExtScratchOnlyEnabled(reg_alloc->GetGprs(), features);
    const u32 scratch_only = scratch_only_enabled ? 2u : 0u;
    const u32 default_gpr_reserve = backend::kDefaultScratchGPR > scratch_only
            ? backend::kDefaultScratchGPR - scratch_only
            : 0u;
    // Attempt one: the ordinary emitter shape. Every unit in the corpus stops
    // here, so nothing above this point may walk the instruction list -- the
    // whole-unit budget scan below is deliberately deferred until an escalation
    // is known to be necessary (func_tests is dominated by translation cost and
    // notices a redundant pass).
    u32 eviction_restarts = 0;
    bool eviction_fallback = false;
    if (spill_evict) {
        Vector<u32> forced_spills{};
        while (true) {
            if (eviction_restarts) {
                reg_alloc->ResetAllocations();
            }
            LinearScanAllocator scan{unit, reg_alloc, default_gpr_reserve,
                                     backend::kDefaultScratchFPR,
                                     single_block_fast_path, scalar_insert,
                                     intwidth_tie, induct_tie, &forced_spills,
                                     true, features};
            scan.AllocateRegisters();
            if (scan.HasEvictionCandidate()) {
                forced_spills.push_back(scan.EvictionCandidate());
                ++eviction_restarts;
                continue;
            }
            PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
            const bool verified = scan.Verify();
            perf_verify.Stop();
            if (verified) {
                if (rerun_with_conditional_spill_scratch()) return;
                record_shape(scan);
                record_test_result(scan, eviction_restarts, false);
                return;
            }
            eviction_fallback = true;
            break;
        }
    } else {
        LinearScanAllocator scan{unit, reg_alloc, default_gpr_reserve,
                                 backend::kDefaultScratchFPR, single_block_fast_path,
                                 scalar_insert, intwidth_tie, induct_tie,
                                 nullptr, false, features};
        scan.AllocateRegisters();
        PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
        const bool verified = scan.Verify();
        perf_verify.Stop();
        if (verified) {
            if (rerun_with_conditional_spill_scratch()) return;
            record_shape(scan);
            record_test_result(scan, 0, false);
            return;
        }
    }
    u32 unit_gpr = 0, unit_fpr = 0, reload_bound = 0;
    CollectUnitBudget(unit,
                      unit_gpr,
                      unit_fpr,
                      reload_bound,
                      reg_alloc->GetGprs(),
                      features);

    // A reserve at or above the pool size would leave the scan nothing to
    // allocate, so each rung is clamped to leave one register allocatable.
    // On the ARM64 pools this never binds (the pool is far larger than any
    // opcode budget); it only matters for the artificially small masks unit
    // tests use to force the spill path.
    const auto pool_gpr = static_cast<u32>(backend::GPRSMask{reg_alloc->GetGprs()}.GetClearCount());
    const auto pool_fpr = static_cast<u32>(backend::FPRSMask{reg_alloc->GetFprs()}.GetClearCount());
    const bool level3 = backend::X86PinExtLevel3Enabled(reg_alloc->GetGprs());
    const auto clamp_gpr = [level3](u32 want, u32 pool) {
        if (!pool) {
            return 0u;
        }
        // Level 3 may need the all-spill terminal rung: spilled definitions
        // are emitted into reload scratch, so correctness does not require a
        // value register to remain allocatable.
        return std::min(want, level3 ? pool : pool - 1);
    };
    const auto clamp_fpr = [](u32 want, u32 pool) {
        return pool ? std::min(want, pool - 1) : 0u;
    };
    const u32 base_gpr = std::max<u32>(unit_gpr, default_gpr_reserve);
    const u32 base_fpr = std::max<u32>(unit_fpr, backend::kDefaultScratchFPR);
    struct ReserveRung {
        u32 gpr;
        u32 fpr;
    };
    Vector<ReserveRung> ladder{};
    auto add_rung = [&](u32 gpr, u32 fpr) {
        ReserveRung rung{clamp_gpr(gpr, pool_gpr), clamp_fpr(fpr, pool_fpr)};
        if (ladder.empty() || ladder.back().gpr != rung.gpr ||
            ladder.back().fpr != rung.fpr) {
            ladder.push_back(rung);
        }
    };
    add_rung(base_gpr, base_fpr);
    if (level3) {
        // With seven dynamic GPRs, jumping straight from the emitter reserve
        // to the all-spill rung is counterproductive: it turns ordinary
        // three-operand ops into four-reload shapes. Walk the two intermediate
        // balances and stop at the first verified allocation.
        add_rung(base_gpr + 1, base_fpr + 1);
        add_rung(base_gpr + 2, base_fpr + 2);
    }
    add_rung(base_gpr + reload_bound, base_fpr + reload_bound);
    for (auto& rung : ladder) {
        reg_alloc->ResetAllocations();
        LinearScanAllocator scan{unit, reg_alloc, rung.gpr, rung.fpr, single_block_fast_path,
                                 scalar_insert, intwidth_tie, induct_tie,
                                 nullptr, false, features};
        scan.AllocateRegisters();
        PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
        const bool verified = scan.Verify();
        perf_verify.Stop();
        if (verified) {
            if (RaDiagEnabled()) {
                LOG_WARNING("RegisterAllocPass: scratch reserve escalated to {}/{}", rung.gpr,
                            rung.fpr);
            }
            if (rerun_with_conditional_spill_scratch()) return;
            record_shape(scan);
            record_test_result(scan, eviction_restarts, eviction_fallback);
            return;
        }
    }
    // Only reachable when the register pool itself is smaller than one
    // instruction's scratch demand, which no ARM64 configuration produces. Say
    // so here, where the unit is still identifiable, instead of letting the
    // JIT walk into "No free temporary GPR" halfway through an instruction.
    LOG_WARNING("RegisterAllocPass: pool {}/{} cannot cover scratch demand {}/{} (+{} reloads); "
                "emitting with the largest workable reserve",
                pool_gpr, pool_fpr, base_gpr, base_fpr, reload_bound);
}

void RegisterAllocPass::Run(HIRFunction* hir_function,
                            backend::RegAlloc* reg_alloc,
                            const FeatureSet& features) {
    RunWithScalarInsert(hir_function, reg_alloc, false, features);
}

void RegisterAllocPass::RunWithScalarInsert(HIRFunction* hir_function,
                                            backend::RegAlloc* reg_alloc,
                                            bool scalar_insert,
                                            const FeatureSet& features) {
    const bool single_block_fast_path = features.ra_1blk;
    const bool use_fast_path =
            single_block_fast_path && hir_function->GetHIRBlocksRPO().size() == 1;
    RunVerified(hir_function, reg_alloc, features, use_fast_path, scalar_insert,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
}

void RegisterAllocPass::Run(HIRFunction* hir_function,
                            backend::RegAlloc* reg_alloc,
                            bool single_block_fast_path,
                            const FeatureSet& features) {
    const bool use_fast_path =
            single_block_fast_path && hir_function->GetHIRBlocksRPO().size() == 1;
    RunVerified(hir_function, reg_alloc, features, use_fast_path, false,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
}

void RegisterAllocPass::Run(ir::Block* block,
                            backend::RegAlloc* reg_alloc,
                            bool scalar_insert,
                            const FeatureSet& features) {
    RunVerified(block, reg_alloc, features, false, scalar_insert,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
}

void RegisterAllocPass::RunForIntWidthTieTest(ir::Block* block,
                                              backend::RegAlloc* reg_alloc,
                                              bool intwidth_tie) {
    auto features = FeatureSet{};
    features.ra_intwidth_tie = intwidth_tie;
    RunVerified(block, reg_alloc, features, false, false, intwidth_tie,
                features.induct_tie, features.ra_spill_evict);
}

void RegisterAllocPass::RunForInductTieTest(ir::Block* block,
                                            backend::RegAlloc* reg_alloc,
                                            bool induct_tie) {
    auto features = FeatureSet{};
    features.induct_tie = induct_tie;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, induct_tie, features.ra_spill_evict);
}

void RegisterAllocPass::RunForCoalesceTest(ir::Block* block,
                                           backend::RegAlloc* reg_alloc,
                                           bool coalesce) {
    auto features = FeatureSet{};
    features.ra_coalesce = coalesce;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
}

void RegisterAllocPass::RunForCoalesceConflictTest(ir::Block* block,
                                                   backend::RegAlloc* reg_alloc,
                                                   u32 tied_value_id,
                                                   u16 target) {
    auto features = FeatureSet{};
    // The conflict binding below must be installed before the coalescer's
    // only run; keep the initial verified pass from coalescing on its own
    // regardless of the FeatureSet default.
    features.ra_coalesce = false;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, features.induct_tie,
                features.ra_spill_evict);
    // Model the output of any earlier fixed-home tie independently of its
    // current opcode-specific matcher. The coalescer must treat that mapping
    // as authoritative and reject an intersecting publication window.
    reg_alloc->MapRegister(tied_value_id, HostGPR{target});
    features.ra_coalesce = true;
    LinearScanAllocator scan{block, reg_alloc, 0, 0, false, false, false, false,
                             nullptr, false, features};
    scan.CoalesceGuestRegisterAccessesForTest();
}

void RegisterAllocPass::RunForXmmResidentTest(ir::Block* block,
                                              backend::RegAlloc* reg_alloc,
                                              bool enabled) {
    auto features = FeatureSet{};
    reg_alloc->ResetAllocations();
    LinearScanAllocator scan{block, reg_alloc, 0, backend::kDefaultScratchFPR,
                             false, false, false, false, nullptr, false, features,
                             enabled ? 1 : 0};
    scan.AllocateRegisters();
    ASSERT(scan.Verify());
}

void RegisterAllocPass::RunForXmmResidentConflictTest(ir::Block* block,
                                                      backend::RegAlloc* reg_alloc,
                                                      u32 tied_value_id,
                                                      u16 target) {
    auto features = FeatureSet{};
    reg_alloc->ResetAllocations();
    LinearScanAllocator initial{block, reg_alloc, 0, backend::kDefaultScratchFPR,
                                false, false, false, false, nullptr, false, features, 0};
    initial.AllocateRegisters();
    ASSERT(initial.Verify());
    reg_alloc->MapRegister(tied_value_id, HostFPR{target});
    LinearScanAllocator scan{block, reg_alloc, 0, 0, false, false, false, false,
                             nullptr, false, features, 0};
    scan.CoalesceGuestFPRAccessesForTest();
}

RegisterAllocPass::SpillEvictTestResult RegisterAllocPass::RunForSpillEvictTest(
        ir::Block* block,
        backend::RegAlloc* reg_alloc,
        bool spill_evict) {
    SpillEvictTestResult result{};
    auto features = FeatureSet{};
    features.ra_spill_evict = spill_evict;
    RunVerified(block, reg_alloc, features, false, false,
                features.ra_intwidth_tie, features.induct_tie,
                spill_evict, &result);
    return result;
}

void VRegisterAllocPass::Run(ir::Block* block) {
    VRegisterAllocator allocator{block};
    allocator.AllocateRegisters();
}

}  // namespace swift::runtime::ir
