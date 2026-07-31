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

static bool MemNarrowFuseEnabled() {
    static const bool enabled = [] {
        const char* env = PerfGetenv("SVM_MEM_NARROW_FUSE");
        return !env || std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

static bool ShiftImmFastEnabled() {
    const char* env = PerfGetenv("SVM_SHIFT_IMM_FAST");
    return !env || std::strcmp(env, "0") != 0;
}

// Spill/escalation 诊断打印默认关闭：它们随编译线程异步产生，会混入 guest
// stdout（sqlite speedtest 一类边跑边输出的负载会被打断行），且非用户可行动项。
// 需要排查 regalloc 行为时设 SVM_RA_DIAG=1 打开。
static bool RaDiagEnabled() {
    static const bool enabled = [] {
        const char* env = PerfGetenv("SVM_RA_DIAG");
        return env && std::strcmp(env, "0") != 0;
    }();
    return enabled;
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
                                 bool scalar_insert = false)
            : function(function), block(), reg_alloc(alloc), live_interval(), active_lives(),
              gpr_reserve(gpr_res), fpr_reserve(fpr_res),
              single_block_fast_path(single_block_fast_path),
              scalar_insert(scalar_insert),
              shift_imm_fast(ShiftImmFastEnabled()) {
        active_gprs = alloc->GetGprs();
        active_fprs = alloc->GetFprs();
        live_interval.reserve(function->MaxInstrCount());
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
                                 bool scalar_insert = false)
            : function(), block(block), reg_alloc(alloc), live_interval(), active_lives(),
              gpr_reserve(gpr_res), fpr_reserve(fpr_res),
              scalar_insert(scalar_insert),
              shift_imm_fast(ShiftImmFastEnabled()) {
        active_gprs = alloc->GetGprs();
        active_fprs = alloc->GetFprs();
        live_interval.reserve(block->GetInstList().size());
        InitializeFixedClobbers();
    }

    [[nodiscard]] u32 SpillCount() const { return spill_count; }

    static u32 ScratchOnlyGPRs(OpCode op, const backend::GPRSMask& pool) {
        u32 count = backend::X86PinExtLevel3AluScratchEnabled(pool, op) ? 1u : 0u;
        const bool level2_scratch = backend::X86PinExtScratchOnlyEnabled(pool);
        if (level2_scratch) {
            const u32 fixed = backend::FixedGPRClobbers(op, true);
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
                    auto need = backend::ScratchBudget(inst.GetOp());
                    if (need.gpr <= gpr_reserve && need.fpr <= fpr_reserve) {
                        continue;
                    }
                    if (inst.Id() >= reg_alloc->MapCount()) {
                        continue;
                    }
                    ok &= static_cast<u32>(reg_alloc->DirtyGPR(inst.Id()).GetClearCount()) +
                                          ScratchOnlyGPRs(inst.GetOp(),
                                                          reg_alloc->GetGprs()) >=
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

            if (!IsFloatValue(interval.inst)) {
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
                } else if (auto alloc = AllocGPR(interval); alloc >= 0) {
                    reg_alloc->MapRegister(interval.inst->Id(), HostGPR{(u16)alloc});
                } else {
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
        if (spill_count && RaDiagEnabled()) {
            LOG_WARNING("RegisterAllocPass: {} value(s) spilled to stack slots (highest slot {})",
                        spill_count, max_spill_slot);
        }
    }

private:
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

    void InitializeFixedClobbers() {
        fixed_gpr_clobbers.resize(InstrCount());
        const bool scratch_only =
                backend::X86PinExtScratchOnlyEnabled(reg_alloc->GetGprs());
        auto add_block = [&](Block* lir_block) {
            bool has_inst = false;
            u32 last_id = 0;
            for (auto& inst : lir_block->GetInstList()) {
                if (inst.Id() < fixed_gpr_clobbers.size()) {
                    fixed_gpr_clobbers[inst.Id()] |=
                            backend::FixedGPRClobbers(inst.GetOp(), scratch_only);
                }
                has_inst = true;
                last_id = std::max<u32>(last_id, inst.Id());
            }
            // Every block terminal/link sequence owns x11. Recording that
            // clobber at the block's final instruction extends the exclusion
            // to values consumed by terminal::If/Switch as well.
            if (backend::ScratchXPoolEnabled() && has_inst &&
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
        if (!MemNarrowFuseEnabled()) {
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
        if (!MemNarrowFuseEnabled() || inst->GetOp() != OpCode::GetOperand ||
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
        return TryTieGPR(current, MemoryOperandTieSource(current.inst));
    }

    bool TryTieGPR(LiveInterval& current, Value source) {
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
            return false;
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
        auto need = backend::ScratchBudget(inst->GetOp());
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
                ScratchOnlyGPRs(inst->GetOp(), reg_alloc->GetGprs());
        u32 need_gpr = need.gpr + reload_gpr + extra_gpr;
        // Add/Sub's five-register declaration is the no-spill worst case:
        // tied inputs must be preserved for AF/PF after the destination is
        // overwritten. A spilled operand/result cannot be tied, and its reload
        // register replaces that preservation temporary. Charge the larger of
        // the no-spill peak and the ordinary three-register shape plus reloads
        // instead of adding both mutually-exclusive peaks.
        if (scratch_only_gprs &&
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
            // Scalar views of a pinned SIMD register (XmmLo/XmmHi) still
            // produce GPR values: EmitGetHostFPR materializes them with UMOV.
            // Only vector-typed reads can alias the fixed FPR directly.
            const bool fixed_fpr_get =
                    host_reg_alias && instr->GetOp() == OpCode::GetHostFPR &&
                    IsFloatValueType(instr->ReturnType());
            // A fixed mapping aliases this SSA value directly to the pinned
            // register. Preserve snapshot semantics by forcing a copy when a
            // SetHostGPR can overwrite that register before the value's last use.
            const bool fixed_gpr_get =
                    full_gpr_get &&
                    !LiveRangeCrossesHostRegWrite(host_gpr_writes, host_index, instr->Id(), end);
            const bool fixed_fpr_alias =
                    fixed_fpr_get &&
                    !LiveRangeCrossesHostRegWrite(host_fpr_writes, host_index, instr->Id(), end);
            if (fixed_fpr_alias || fixed_gpr_get) {
                if (fixed_fpr_alias) {
                    reg_alloc->MapRegister(hir_value.GetOrderId(), HostFPR{host_index});
                } else {
                    reg_alloc->MapRegister(hir_value.GetOrderId(), HostGPR{host_index});
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
                } else {
                    reg_alloc->MapRegister(hir_value.GetOrderId(), HostGPR{host_index});
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
                } else {
                    reg_alloc->MapRegister(instr.Id(), HostGPR{host_index});
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
        max_spill_slot = std::max(max_spill_slot, slot + slot_size - 1);
    }

    void FreeGPR(u32 id) {
        ASSERT(active_gprs.Get(id));
        active_gprs.Clear(id);
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
    const bool shift_imm_fast{false};
    Vector<bool> spill_slots{};
    Vector<u32> fixed_gpr_clobbers{};
    std::array<Vector<u32>, 32> fixed_gpr_clobber_points{};
    // Spill telemetry (reported at the end of AllocateRegisters): spilling
    // has never triggered on current workloads, so any hit is worth a log
    // line — it means the JIT's defensive MEM path is being exercised.
    u32 spill_count{0};
    u32 max_spill_slot{0};
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

void RegisterAllocPass::Run(HIRBuilder* hir_builder, backend::RegAlloc* reg_alloc) {
    for (auto& hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func, reg_alloc);
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
                              const backend::GPRSMask& pool) {
    for (auto& inst : block->GetInstList()) {
        auto need = backend::ScratchBudget(inst.GetOp());
        const u32 scratch_only = LinearScanAllocator::ScratchOnlyGPRs(inst.GetOp(), pool);
        gpr = std::max<u32>(gpr, need.gpr > scratch_only ? need.gpr - scratch_only : 0);
        fpr = std::max<u32>(fpr, need.fpr);
        reload_bound = std::max<u32>(reload_bound, static_cast<u32>(inst.GetValues().size()) + 1);
    }
}

static void CollectUnitBudget(HIRFunction* function,
                              u32& gpr,
                              u32& fpr,
                              u32& reload_bound,
                              const backend::GPRSMask& pool) {
    for (auto* hir_block : function->GetHIRBlocks()) {
        CollectUnitBudget(
                hir_block->GetBlock(), gpr, fpr, reload_bound, pool);
    }
}

template <typename Unit>
static void RunVerified(Unit* unit,
                        backend::RegAlloc* reg_alloc,
                        bool single_block_fast_path = false,
                        bool scalar_insert = false) {
    const bool scratch_only_enabled =
            backend::X86PinExtScratchOnlyEnabled(reg_alloc->GetGprs());
    const u32 scratch_only = scratch_only_enabled ? 2u : 0u;
    const u32 default_gpr_reserve = backend::kDefaultScratchGPR > scratch_only
            ? backend::kDefaultScratchGPR - scratch_only
            : 0u;
    // Attempt one: the ordinary emitter shape. Every unit in the corpus stops
    // here, so nothing above this point may walk the instruction list -- the
    // whole-unit budget scan below is deliberately deferred until an escalation
    // is known to be necessary (func_tests is dominated by translation cost and
    // notices a redundant pass).
    {
        LinearScanAllocator scan{unit, reg_alloc, default_gpr_reserve,
                                 backend::kDefaultScratchFPR, single_block_fast_path,
                                 scalar_insert};
        scan.AllocateRegisters();
        PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
        const bool verified = scan.Verify();
        perf_verify.Stop();
        if (verified) {
            return;
        }
    }
    u32 unit_gpr = 0, unit_fpr = 0, reload_bound = 0;
    CollectUnitBudget(unit,
                      unit_gpr,
                      unit_fpr,
                      reload_bound,
                      reg_alloc->GetGprs());

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
                                 scalar_insert};
        scan.AllocateRegisters();
        PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
        const bool verified = scan.Verify();
        perf_verify.Stop();
        if (verified) {
            if (RaDiagEnabled()) {
                LOG_WARNING("RegisterAllocPass: scratch reserve escalated to {}/{}", rung.gpr,
                            rung.fpr);
            }
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
                            backend::RegAlloc* reg_alloc) {
    RunWithScalarInsert(hir_function, reg_alloc, false);
}

void RegisterAllocPass::RunWithScalarInsert(HIRFunction* hir_function,
                                            backend::RegAlloc* reg_alloc,
                                            bool scalar_insert) {
    static const bool single_block_fast_path = [] {
        const char* env = PerfGetenv("SVM_RA_1BLK");
        return !env || std::strcmp(env, "0") != 0;
    }();
    const bool use_fast_path =
            single_block_fast_path && hir_function->GetHIRBlocksRPO().size() == 1;
    RunVerified(hir_function, reg_alloc, use_fast_path, scalar_insert);
}

void RegisterAllocPass::Run(HIRFunction* hir_function,
                            backend::RegAlloc* reg_alloc,
                            bool single_block_fast_path) {
    const bool use_fast_path =
            single_block_fast_path && hir_function->GetHIRBlocksRPO().size() == 1;
    RunVerified(hir_function, reg_alloc, use_fast_path, false);
}

void RegisterAllocPass::Run(ir::Block* block,
                            backend::RegAlloc* reg_alloc,
                            bool scalar_insert) {
    RunVerified(block, reg_alloc, false, scalar_insert);
}

void VRegisterAllocPass::Run(ir::Block* block) {
    VRegisterAllocator allocator{block};
    allocator.AllocateRegisters();
}

}  // namespace swift::runtime::ir
