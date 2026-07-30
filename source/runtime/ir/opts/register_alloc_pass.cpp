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
    }

    [[nodiscard]] u32 SpillCount() const { return spill_count; }

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
                    ok &= static_cast<u32>(reg_alloc->DirtyGPR(inst.Id()).GetClearCount()) >=
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
                reg_alloc->SetActiveRegs(next_id, active_gprs, active_fprs);
            }
        };
        for (auto& interval : live_interval) {
            fill_gap(interval.start);

            ExpireOldIntervals(interval);

            if (!IsFloatValue(interval.inst)) {
                if (TryTieNarrowLoad(interval)) {
                    // A plain narrow load and its extension chain share one W/X
                    // register. The ARM64 emitter can then replace
                    // LDRH+SXTH (or LDRB+UXTB) with the extending load itself,
                    // and a following 32->64 W write becomes a true no-op.
                } else if (auto alloc = AllocGPR(); alloc >= 0) {
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
            reg_alloc->SetActiveRegs(interval.inst->Id(), active_gprs, active_fprs);
            next_id = std::max(next_id, interval.start + 1);
        }
        // Fill any remaining instructions after the last interval start.
        fill_gap(instr_count);
        perf_assign.Stop();

        if (spill_count) {
            LOG_WARNING("RegisterAllocPass: {} value(s) spilled to stack slots (highest slot {})",
                        spill_count, max_spill_slot);
        }
    }

private:
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
        auto source = NarrowLoadTieSource(current.inst);
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
        u32 need_gpr = need.gpr + extra_gpr;
        u32 need_fpr = need.fpr + extra_fpr;
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
            (IsFloatValue(source.Def()) ? need_fpr : need_gpr)++;
        };
        for (auto& value : inst->GetValues()) {
            add(value);
        }
        if (inst->HasValue()) {
            add(Value{inst});
        }
        auto gprs = reg_alloc->DirtyGPR(id);
        auto fprs = reg_alloc->DirtyFPR(id);
        return static_cast<u32>(gprs.GetClearCount()) >= need_gpr &&
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
            const bool full_gpr_get = host_reg_alias && instr->GetOp() == OpCode::GetHostGPR &&
                                      GetValueSizeByte(instr->ReturnType()) == sizeof(u64);
            // Scalar views of a pinned SIMD register (XmmLo/XmmHi) still
            // produce GPR values: EmitGetHostFPR materializes them with UMOV.
            // Only vector-typed reads can alias the fixed FPR directly.
            const bool fixed_fpr_get =
                    host_reg_alias && instr->GetOp() == OpCode::GetHostFPR &&
                    IsFloatValueType(instr->ReturnType());
            const auto host_index =
                    host_reg_alias ? static_cast<u16>(instr->GetArg<Imm>(0).Get()) : u16{};
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
            const bool full_gpr_get = host_reg_alias && instr->GetOp() == OpCode::GetHostGPR &&
                                      GetValueSizeByte(instr->ReturnType()) == sizeof(u64);
            const bool fixed_fpr_get =
                    host_reg_alias && instr->GetOp() == OpCode::GetHostFPR &&
                    IsFloatValueType(instr->ReturnType());
            const auto host_index =
                    host_reg_alias ? static_cast<u16>(instr->GetArg<Imm>(0).Get()) : u16{};
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
            const bool full_gpr_get = host_reg_alias && instr.GetOp() == OpCode::GetHostGPR &&
                                      GetValueSizeByte(instr.ReturnType()) == sizeof(u64);
            const bool fixed_fpr_get =
                    host_reg_alias && instr.GetOp() == OpCode::GetHostFPR &&
                    IsFloatValueType(instr.ReturnType());
            const auto host_index =
                    host_reg_alias ? static_cast<u16>(instr.GetArg<Imm>(0).Get()) : u16{};
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
    int AllocGPR() {
        if (static_cast<u32>(active_gprs.GetClearCount()) <= gpr_reserve) {
            return -1;
        }
        if (auto alloc = active_gprs.GetFirstClear(); alloc >= 0) {
            active_gprs.Mark(alloc);
            return alloc;
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
static void CollectUnitBudget(Block* block, u32& gpr, u32& fpr, u32& reload_bound) {
    for (auto& inst : block->GetInstList()) {
        auto need = backend::ScratchBudget(inst.GetOp());
        gpr = std::max<u32>(gpr, need.gpr);
        fpr = std::max<u32>(fpr, need.fpr);
        reload_bound = std::max<u32>(reload_bound, static_cast<u32>(inst.GetValues().size()) + 1);
    }
}

static void CollectUnitBudget(HIRFunction* function, u32& gpr, u32& fpr, u32& reload_bound) {
    for (auto* hir_block : function->GetHIRBlocks()) {
        CollectUnitBudget(hir_block->GetBlock(), gpr, fpr, reload_bound);
    }
}

template <typename Unit>
static void RunVerified(Unit* unit,
                        backend::RegAlloc* reg_alloc,
                        bool single_block_fast_path = false,
                        bool scalar_insert = false) {
    // Attempt one: the ordinary emitter shape. Every unit in the corpus stops
    // here, so nothing above this point may walk the instruction list -- the
    // whole-unit budget scan below is deliberately deferred until an escalation
    // is known to be necessary (func_tests is dominated by translation cost and
    // notices a redundant pass).
    {
        LinearScanAllocator scan{unit, reg_alloc, backend::kDefaultScratchGPR,
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
    CollectUnitBudget(unit, unit_gpr, unit_fpr, reload_bound);

    // A reserve at or above the pool size would leave the scan nothing to
    // allocate, so each rung is clamped to leave one register allocatable.
    // On the ARM64 pools this never binds (the pool is far larger than any
    // opcode budget); it only matters for the artificially small masks unit
    // tests use to force the spill path.
    const auto pool_gpr = static_cast<u32>(backend::GPRSMask{reg_alloc->GetGprs()}.GetClearCount());
    const auto pool_fpr = static_cast<u32>(backend::FPRSMask{reg_alloc->GetFprs()}.GetClearCount());
    const auto clamp = [](u32 want, u32 pool) { return pool ? std::min(want, pool - 1) : 0u; };
    const u32 base_gpr = std::max<u32>(unit_gpr, backend::kDefaultScratchGPR);
    const u32 base_fpr = std::max<u32>(unit_fpr, backend::kDefaultScratchFPR);
    const struct {
        u32 gpr;
        u32 fpr;
    } ladder[] = {
            {clamp(base_gpr, pool_gpr), clamp(base_fpr, pool_fpr)},
            {clamp(base_gpr + reload_bound, pool_gpr), clamp(base_fpr + reload_bound, pool_fpr)},
    };
    for (auto& rung : ladder) {
        reg_alloc->ResetAllocations();
        LinearScanAllocator scan{unit, reg_alloc, rung.gpr, rung.fpr, single_block_fast_path,
                                 scalar_insert};
        scan.AllocateRegisters();
        PerfScope2 perf_verify{GetPerfStats2().regalloc_verify};
        const bool verified = scan.Verify();
        perf_verify.Stop();
        if (verified) {
            LOG_WARNING("RegisterAllocPass: scratch reserve escalated to {}/{}", rung.gpr,
                        rung.fpr);
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
