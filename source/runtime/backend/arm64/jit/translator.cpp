#pragma once

#include "translator.h"

#include <cstring>
#include <functional>
#include <iterator>
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.


JitTranslator::JitTranslator(JitContext& ctx) : context(ctx), masm(ctx.GetMasm()) {
    auto& config = ctx.GetConfig();
    use_memory_base = config.memory_base != nullptr || config.page_table != nullptr;
    guest_addr_mask = config.guest_addr_mask;
    window_uxtw = guest_addr_mask == 0xFFFFFFFFull;
    sse_scalar_insert = True(config.arm64_features & Arm64Features::AFP);
    if (const char* shift_fast = PerfGetenv("SVM_SHIFT_IMM_FAST")) {
        shift_imm_fast = std::strcmp(shift_fast, "0") != 0;
    }
    if (const char* mem_fuse = PerfGetenv("SVM_MEM_NARROW_FUSE")) {
        mem_narrow_fuse = std::strcmp(mem_fuse, "0") != 0;
    }
    if (const char* nan_fast = PerfGetenv("SVM_SSE_NAN_FAST")) {
        sse_nan_fast = std::strcmp(nan_fast, "0") != 0;
    }
    if (const char* nan_coldpath = PerfGetenv("SVM_SSE_NAN_COLDPATH")) {
        sse_nan_coldpath = std::strcmp(nan_coldpath, "0") != 0;
    }
}



void JitTranslator::Translate(ir::Block* block) {
    vixl::svm_vixl_prof::JitScope vixl_prof;
    ASSERT(vec_nan_cold_sites.empty());
    PerfScope2 perf_prologue{GetPerfStats2().codegen_prologue};
    cur_block = block;
    cur_block_is_call = false;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() == ir::OpCode::PushRSB) {
            cur_block_is_call = true;
            break;
        }
    }
    static_next_loc.reset();
    context.SetCurrent(block);
    // Count the remaining x86 GPR uniform-buffer traffic dynamically without
    // instrumenting each access: add the block's static emitted access count
    // once on entry. ThreadContext64 begins with the 16 8-byte GPRs, so the
    // first 128 uniform bytes are exactly the register-residency region.
    u32 gpr_uniform_accesses = 0;
    u32 xmm_uniform_accesses = 0;
    constexpr u32 kXmmBegin = offsetof(swift::x86::ThreadContext64, xmms);
    constexpr u32 kXmmEnd = kXmmBegin + sizeof(swift::x86::ThreadContext64::xmms);
    for (auto& inst : block->GetInstList()) {
        if ((inst.GetOp() == ir::OpCode::LoadUniform ||
             inst.GetOp() == ir::OpCode::StoreUniform)) {
            const u32 offset = inst.GetArg<ir::Uniform>(0).GetOffset();
            if (offset < 16 * sizeof(u64)) {
                ++gpr_uniform_accesses;
            } else if (offset >= kXmmBegin && offset < kXmmEnd) {
                ++xmm_uniform_accesses;
            }
        }
    }
    context.RecordExecCounter(exec_offset_gpr_uniform_accesses,
                              gpr_uniform_accesses);
    context.RecordExecCounter(exec_offset_xmm_uniform_accesses,
                              xmm_uniform_accesses);
    perf_prologue.Stop();
    // Function-mode IdByRPO assigns global instruction ids, while
    // Block::MaxInstrId remains the block-local count established at decode.
    // Never shrink a function-sized bitmap back to that local count: memory
    // operand peepholes index it with the global ids of instructions they
    // suppress.
    disable_instructions.resize(
            std::max<size_t>(disable_instructions.size(), block->MaxInstrId()));
    PerfScope2 perf_body{GetPerfStats2().codegen_body};
    for (auto& inst : block->GetInstList()) {
        cur_instr = &inst;
        if (inst.Id() < disable_instructions.size() && disable_instructions.test(inst.Id())) {
            continue;
        }
        Translate(&inst);
    }
    perf_body.Stop();

    PerfScope2 perf_terminal{GetPerfStats2().codegen_terminal};
    context.BeginTerminalScratch();
    FlushFlags();
    EmitTerminal(block->GetTerminal());
    context.EndTerminalScratch();
    // Close the W71 accounting window before out-of-line NaN repair stubs.
    // The hot guard remains in the block; cold handlers are not executed on
    // the normal path and therefore do not belong in static x entry counts.
    context.FinishHotCoalesceBlock();
    context.BeginColdScratch();
    EmitVecNaNColdPaths();
    context.EndColdScratch();
}

void JitTranslator::Translate(ir::HIRFunction* function) {
    vixl::svm_vixl_prof::JitScope vixl_prof;
    ASSERT(function);
    context.SetCurrent(function->GetFunction());
    disable_instructions.resize(function->MaxInstrCount());
    for (auto& hir_block : function->GetHIRBlocksRPO()) {
        // Undecoded successor left behind by lazy region compilation (and by
        // the pre-existing 128-block cap): no instructions and no terminal.
        // Emitting it would bind a label nobody branches to and then fall into
        // terminal::Invalid -> Ret without setting current_loc, which is a
        // dispatcher loop if it were ever entered.  Its guest address is
        // deliberately never published (TranslateIR skips empty blocks), so the
        // only way in is JitContext::Forward, which routes to it through the L2
        // dispatch slot instead.
        auto* block = hir_block.GetBlock();
        if (block->GetInstList().empty() && !block->HasTerminal()) {
            continue;
        }
        Translate(block);
    }
}

void JitTranslator::EmitTerminal(const ir::Terminal& terminal) {
    VisitVariant<void>(terminal, [this](auto term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::Invalid>) {
            // Flat decoded blocks have no explicit terminal: the next location was
            // already written to state->current_loc by a SetLocation instruction.
            MergeNZCV();
            context.RecordExecCounter(static_next_loc ? exec_offset_exit_direct
                                                      : exec_offset_exit_indirect);
            if (!EmitStaticForward()) {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToDispatch>) {
            MergeNZCV();
            context.RecordExecCounter(
                    cur_block_is_call ? exec_offset_exit_call
                                      : (static_next_loc ? exec_offset_exit_direct
                                                         : exec_offset_exit_indirect));
            if (!EmitStaticForward()) {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost>) {
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_syscall);
            __ Mov(ipw, static_cast<u32>(HaltReason::CallHost));
            __ Str(ipw, MemOperand(state, state_offset_halt_reason));
            __ Ret();
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlock>) {
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_direct);
            context.Forward(term.next);
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_direct);
            context.Forward(term.next);
        } else if constexpr (std::is_same_v<T, ir::terminal::PopRSBHint>) {
            // Return Stack Buffer: this is the real pop+predict site. It must
            // run here (not at the PopRSB instruction) because guest flags have
            // just been committed by FlushFlags/MergeNZCV — the hit path
            // branches directly to the return target, which expects the flags
            // register to be current. EmitRSBPop ends in Br (hit) or Ret (miss/
            // underflow), so it fully terminates the block.
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_ret);
            if (True(context.GetConfig().global_opts & Optimizations::ReturnStackBuffer)) {
                context.EmitRSBPop();
            } else {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
            Label else_label;
            if (auto local = LocalConditionFor(term.cond)) {
                __ B(&else_label,
                     static_cast<Condition>(static_cast<u8>(*local) ^ 1));
            } else {
                __ Cbz(context.W(term.cond), &else_label);
            }
            EmitTerminal(term.then_);
            __ Bind(&else_label);
            EmitTerminal(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            Label else_label;
            auto host_cond = MapCond(term.cond);
            if (!(save_in_nzcv && nzcv_dirty)) {
                LoadNZCVFromFlags();
            }
            __ B(&else_label, static_cast<Condition>(static_cast<u8>(host_cond) ^ 1));
            EmitTerminal(term.then_);
            __ Bind(&else_label);
            EmitTerminal(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            // Linear compare chain; each arm ends with its own terminal.
            MergeNZCV();
            auto value = context.R(term.value);
            for (auto& case_ : term.cases) {
                Label next_case;
                __ Mov(ip, case_.case_value.Get());
                __ Cmp(value, ip);
                __ B(&next_case, ne);
                EmitTerminal(case_.then);
                __ Bind(&next_case);
            }
            // No case matched: bail out to the dispatcher.
            context.RecordExecCounter(exec_offset_exit_indirect);
            __ Ret();
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            Label no_halt;
            __ Ldr(ipw, MemOperand(state, state_offset_halt_reason));
            __ Cbz(ipw, &no_halt);
            MergeNZCV();
            __ Ret();
            __ Bind(&no_halt);
            EmitTerminal(term.else_);
        } else {
            PANIC("Unknown terminal!");
        }
    });
}

std::optional<Condition> JitTranslator::LocalConditionFor(ir::Value value) const {
    if (!value.Def()) {
        return std::nullopt;
    }
    if (auto it = local_conditions.find(value.Def()); it != local_conditions.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool JitTranslator::IsCompactFCmp(ir::Value value) {
    return value.Def() && value.Def()->GetOp() == ir::OpCode::VecFCmp &&
           value.Def()->GetArg<ir::Imm>(3).Get() != 0;
}

bool JitTranslator::RecordLocalCondition(ir::Inst* inst, ir::Cond cond) {
    if (inst->GetUses() != 1) {
        return false;
    }
    auto& list = cur_block->GetInstList();
    for (auto it = std::next(list.iterator_to(*inst)); it != list.end(); ++it) {
        bool names = false;
        for (auto value : it->GetValues()) {
            names = names || value.Def() == inst;
        }
        if (!names) {
            continue;
        }
        const bool supported =
                (it->GetOp() == ir::OpCode::Goto ||
                 it->GetOp() == ir::OpCode::NotGoto) &&
                        it->GetArg<ir::Value>(0).Def() == inst ||
                it->GetOp() == ir::OpCode::Select &&
                        it->GetArg<ir::Value>(0).Def() == inst;
        if (!supported) {
            return false;
        }
        local_conditions.emplace(inst, MapCond(cond));
        return true;
    }

    bool terminal_use = false;
    std::function<void(const ir::Terminal&)> visit = [&](const ir::Terminal& terminal) {
        VisitVariant<void>(terminal, [&](auto term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) {
                if (term.cond.Def() == inst) {
                    terminal_use = true;
                }
                visit(term.then_);
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                visit(term.then_);
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                for (const auto& arm : term.cases) {
                    visit(arm.then);
                }
            }
        });
    };
    visit(cur_block->GetTerminal());
    if (terminal_use) {
        local_conditions.emplace(inst, MapCond(cond));
    }
    return terminal_use;
}

// A direct jmp/call decodes to SetLocation(imm) + ReturnToDispatcher, and the
// trampoline then re-reads state->current_loc and walks the L1 hash chain for
// a target that was already known when the code was emitted. The dispatch
// table indexed here is the same one the RSB pop and JitContext::Forward's
// BlockLink path already branch through, with the same safety property: SMC
// invalidation (SmcTracker::ClearDispatchSlots) zeroes the slot, so a stale
// translation degrades to the Cbz fallback rather than to a wild branch.
bool JitTranslator::EmitStaticForward() {
    if (!static_next_loc) {
        return false;
    }
    const u64 target = *static_next_loc;
    static_next_loc.reset();
    return context.ForwardStatic(ir::Location{target});
}

Label* JitTranslator::GetLocalLabel(ir::Inst* inst) {
    if (auto itr = local_labels.find(inst); itr != local_labels.end()) {
        return &itr->second;
    }
    return &local_labels.try_emplace(inst).first->second;
}

HostFlags JitTranslator::GuestNZCVToHost(ir::Flags guest) {
    HostFlags host{};
    if (True(guest & ir::Flags::Negate)) {
        host |= HostFlags::N;
    }
    if (True(guest & ir::Flags::Zero)) {
        host |= HostFlags::Z;
    }
    if (True(guest & ir::Flags::Carry)) {
        host |= HostFlags::C;
    }
    if (True(guest & ir::Flags::Overflow)) {
        host |= HostFlags::V;
    }
    return host;
}

Condition JitTranslator::MapCond(ir::Cond cond) {
    // ir::Cond values match the ARM condition encoding.
    return static_cast<Condition>(static_cast<u8>(cond) & 0xF);
}





Register JitTranslator::MaterializeOperand(const Operand& operand, ir::ValueType type) {
    auto tmp = context.GetTmpGPR(type);
    __ Mov(tmp, operand);
    return tmp;
}

void JitTranslator::Translate(ir::Inst* inst) {
    ASSERT(inst);
    context.TickIR(inst);
    if (inst->GetOp() != ir::OpCode::SetLocation) {
        static_next_loc.reset();
    }

#define INST(name, ...)                                                                            \
    case ir::OpCode::name:                                                                         \
        Emit##name(inst);                                                                          \
        break;

    switch (inst->GetOp()) {
#include "runtime/ir/ir.inc"
        default:
            ASSERT_MSG(false, "Instr unk op: {}", inst->GetOp());
    }

#undef INST
    context.EndInstructionScratch();
}

bool JitTranslator::MatchMemoryOffsetCase(ir::Inst* inst) { return false; }
































Operand JitTranslator::EmitOperand(ir::Operand& ir_op) {
    if (ir_op.GetRight().Null()) {
        if (ir_op.GetLeft().IsImm()) {
            auto imm = ir_op.GetLeft().imm.Get();
            auto imm_signed = ir_op.GetLeft().imm.GetSigned();
            bool can_imm = __ IsImmAddSub(imm_signed);
            if (can_imm) {
                return Operand{imm_signed};
            } else {
                auto tmp = context.GetTmpX();
                __ Mov(tmp, imm);
                return Operand{tmp};
            }
        } else {
            return Operand{context.R(ir_op.GetLeft().value, true)};
        }
    } else {
        Register left_reg;
        ir::ValueType left_type{ir::ValueType::U64};
        if (ir_op.GetLeft().IsImm()) {
            // Materialize an immediate left side (constant-based composite
            // operand) into a scratch register first.
            auto tmp = context.GetTmpX();
            __ Mov(tmp, ir_op.GetLeft().imm.Get());
            left_reg = tmp;
        } else {
            auto left_value = ir_op.GetLeft().value;
            left_type = left_value.Type();
            left_reg = context.R(left_value, true);
        }
        auto right = ir_op.GetRight();
        if (right.IsImm()) {
            auto imm = right.imm.GetSigned();
            auto is_lsl = ir_op.GetOp() == ir::OperandOp::LSL;
            auto is_lsr = ir_op.GetOp() == ir::OperandOp::LSR;
            if (is_lsl || is_lsr) {
                if ((left_reg.Is64Bits() || (imm < kWRegSize)) || (left_reg.Is32Bits() || (imm < kXRegSize))) {
                    return Operand{left_reg, is_lsl ? LSL : LSR, static_cast<u8>(imm)};
                } else {
                    PANIC();
                }
            } else if (ir_op.GetOp() == ir::OperandOp::Plus) {
                auto tmp = context.GetTmpGPR(left_type);
                bool can_imm = __ IsImmAddSub(imm);
                if (can_imm) {
                    __ Add(tmp, left_reg, imm);
                } else {
                    __ Mov(tmp, imm);
                    __ Add(tmp, left_reg, tmp);
                }
                return Operand{tmp};
            } else {
                PANIC();
            }
        } else {
            auto right_reg = context.R(right.value, true);
            auto tmp = context.GetTmpGPR(left_type);
            if (ir_op.GetOp() == ir::OperandOp::Plus) {
                __ Add(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                __ Lsl(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                __ Lsr(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::PlusExt) {
                auto shift_amount = ir_op.GetOp().shift_ext;
                ASSERT(right_reg.Is64Bits() || (shift_amount < kWRegSize));
                ASSERT(right_reg.Is32Bits() || (shift_amount < kXRegSize));
                __ Add(tmp, left_reg, Operand{right_reg, LSL, shift_amount});
                return Operand{tmp};
            } else {
                PANIC();
            }
        }
        return {};
    }
}

MemOperand JitTranslator::EmitMemOperand(ir::Operand& ir_op,
                                         ir::ValueType type,
                                         bool pair,
                                         bool atomic,
                                         bool allow_writeback,
                                         bool structured_guest_ea) {
    auto access_size = ir::GetValueSizeByte(type);
    if (ir_op.GetRight().Null()) {
        if (ir_op.GetLeft().IsImm()) {
            auto imm = ir_op.GetLeft().imm.Get();
            auto imm_signed = ir_op.GetLeft().imm.GetSigned();
            if (use_memory_base) {
                // Absolute guest address: materialize it, then apply the pt
                // bias (guest addr + pt = host addr). With a bounded guest
                // window the truncation happens at translation time — the
                // immediate is a compile-time constant, so it is free.
                __ Mov(mem_scratch, guest_addr_mask ? (imm & guest_addr_mask) : imm);
                if (atomic) {
                    __ Add(mem_scratch, mem_scratch, pt);
                    return MemOperand{mem_scratch};
                }
                return MemOperand{mem_scratch, pt};
            }
            bool can_imm = pair ? __ IsImmLSPair(imm_signed, access_size) : __ IsImmLSUnscaled(imm_signed);
            if (can_imm) {
                return MemOperand{xzr, imm_signed};
            } else {
                auto tmp = context.GetTmpX();
                __ Mov(tmp, imm);
                return MemOperand{tmp};
            }
        } else {
            // Match Case: load store post/index & push/pop
            auto addr_value = ir_op.GetLeft().value;
            if (mem_narrow_fuse && addr_value.Def()->GetOp() == ir::OpCode::GetOperand &&
                addr_value.Def()->GetUses() == 1) {
                auto source_operand = addr_value.Def()->GetArg<ir::Operand>(0);
                auto source_left = source_operand.GetLeft();
                if (source_operand.GetRight().Null() && source_left.IsValue() &&
                    context.SharesGPR(addr_value, source_left.value)) {
                    // A simple EA does not need to be materialised in the
                    // GetOperand result register. The RA tie makes the result
                    // own that same register through the memory use, so
                    // consume the live result allocation here rather than
                    // extending the source SSA's lifetime in the emitter.
                    disable_instructions.set(addr_value.Def()->Id());
                    auto address_reg = context.R(addr_value, true);
                    if (use_memory_base) {
                        return BiasMem(address_reg, atomic);
                    }
                    return MemOperand{address_reg};
                }
            }
            auto& instr_list = cur_block->GetInstList();
            auto instr = addr_value.Def();
            // With the pt bias active, post-index forms cannot express
            // [base + pt] (+writeback), so the folding is disabled and the
            // address update executes as a normal Add/Sub.
            if (allow_writeback && !use_memory_base && addr_value.Def()->GetUses() == 2) {
                int search_times{0};
                for (auto itr = instr_list.iterator_to(*instr);
                     itr != instr_list.end() && search_times < 3;
                     itr++, search_times++) {
                    auto add_sub =
                            itr->GetOp() == ir::OpCode::Add || itr->GetOp() == ir::OpCode::Sub;
                    if (!add_sub) {
                        continue;
                    }
                    auto same_value = itr->GetArg<ir::Value>(0) == addr_value;
                    if (!same_value) {
                        continue;
                    }
                    auto operand = itr->GetArg<ir::Operand>(1);
                    auto no_right = operand.GetRight().Null();
                    if (!no_right) {
                        continue;
                    }
                    auto same_register = context.R(addr_value) == context.R(itr.operator->());
                    if (!same_register) {
                        continue;
                    }
                    auto left = operand.GetLeft();
                    if (left.IsImm()) {
                        auto imm = left.imm.GetSigned();
                        if (!pair && !__ IsImmLSUnscaled(imm)) {
                            continue;
                        }
                        if (pair && !__ IsImmLSPair(imm, access_size)) {
                            continue;
                        }
                        if (itr->GetOp() == ir::OpCode::Add) {
                            disable_instructions.set(itr->Id());
                            return MemOperand{context.R(addr_value), imm, PostIndex};
                        } else {
                            disable_instructions.set(itr->Id());
                            return MemOperand{context.R(addr_value), -imm, PostIndex};
                        }
                    } else {
                        if (itr->GetOp() == ir::OpCode::Add) {
                            disable_instructions.set(itr->Id());
                            return MemOperand{
                                    context.R(addr_value), context.R(left.value), PostIndex};
                        }
                    }
                }
            }
            if (use_memory_base) {
                return BiasMem(context.R(addr_value), atomic);
            }
            return MemOperand{context.R(addr_value)};
        }
    } else {
        Register left_reg;
        if (ir_op.GetLeft().IsImm()) {
            // Materialize an immediate left side (absolute address + offset
            // forms) into a scratch register first.
            auto tmp = context.GetTmpX();
            __ Mov(tmp, ir_op.GetLeft().imm.Get());
            left_reg = tmp;
        } else {
            left_reg = context.R(ir_op.GetLeft().value, true);
        }
        auto right = ir_op.GetRight();
        if (right.IsImm()) {
            auto imm = right.imm.GetSigned();
            bool can_imm = pair ? __ IsImmLSPair(imm, access_size) : __ IsImmLSUnscaled(imm);
            if (can_imm) {
                if (ir_op.GetOp() == ir::OperandOp::Plus) {
                    if (use_memory_base) {
                        return BiasMem(left_reg, imm, atomic);
                    }
                    return MemOperand{left_reg, imm};
                } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                    if (use_memory_base) {
                        __ Lsl(mem_scratch, left_reg, imm);
                        return BiasMem(mem_scratch, atomic);
                    }
                    auto tmp = context.GetTmpX();
                    __ Lsl(tmp, left_reg, imm);
                    return MemOperand{tmp};
                } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                    if (use_memory_base) {
                        __ Lsr(mem_scratch, left_reg, imm);
                        return BiasMem(mem_scratch, atomic);
                    }
                    auto tmp = context.GetTmpX();
                    __ Lsr(tmp, left_reg, imm);
                    return MemOperand{tmp};
                } else {
                    PANIC();
                }
            } else {
                if (use_memory_base) {
                    __ Mov(mem_scratch, imm);
                    if (ir_op.GetOp() == ir::OperandOp::Plus) {
                        __ Add(mem_scratch, left_reg, mem_scratch);
                    } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                        __ Lsl(mem_scratch, left_reg, mem_scratch);
                    } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                        __ Lsr(mem_scratch, left_reg, mem_scratch);
                    } else {
                        PANIC();
                    }
                    return BiasMem(mem_scratch, atomic);
                }
                auto tmp = context.GetTmpX();
                __ Mov(tmp, imm);
                if (ir_op.GetOp() == ir::OperandOp::Plus) {
                    return MemOperand{left_reg, tmp};
                } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                    return MemOperand{left_reg, tmp, LSL};
                } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                    return MemOperand{left_reg, tmp, LSR};
                } else {
                    PANIC();
                }
            }
        } else {
            auto right_reg = context.R(right.value, true);
            if (ir_op.GetOp() == ir::OperandOp::Plus) {
                if (use_memory_base) {
                    if (structured_guest_ea && window_uxtw) {
                        // Compute the guest EA in W form before applying the
                        // host bias: pt + ((base + index) mod 2^32).
                        // This deliberately is not a prebiased base.
                        __ Add(mem_scratch.W(), left_reg.W(), right_reg.W());
                    } else {
                        __ Add(mem_scratch, left_reg, right_reg);
                    }
                    return BiasMem(mem_scratch, atomic);
                }
                return MemOperand{left_reg, right_reg};
            } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                if (use_memory_base) {
                    __ Lsl(mem_scratch, left_reg, right_reg);
                    return BiasMem(mem_scratch, atomic);
                }
                return MemOperand{left_reg, right_reg, LSL};
            } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                if (use_memory_base) {
                    __ Lsr(mem_scratch, left_reg, right_reg);
                    return BiasMem(mem_scratch, atomic);
                }
                return MemOperand{left_reg, right_reg, LSR};
            } else if (ir_op.GetOp() == ir::OperandOp::PlusExt) {
                auto shift_amount = ir_op.GetOp().shift_ext;
                if (structured_guest_ea && use_memory_base) {
                    if (window_uxtw) {
                        // Keep base/index/scale in one wrapping W add.
                        // BiasMem supplies the final pt + Wguest, UXTW step.
                        __ Add(mem_scratch.W(),
                               left_reg.W(),
                               Operand{right_reg.W(), LSL, shift_amount});
                    } else {
                        __ Add(mem_scratch, left_reg, Operand{right_reg, LSL, shift_amount});
                    }
                    return BiasMem(mem_scratch, atomic);
                }
                if (ir::GetValueSizeByte(right.value.Type()) == shift_amount) {
                    if (use_memory_base) {
                        __ Add(mem_scratch,
                               left_reg,
                               Operand{right_reg, LSL, shift_amount});
                        return BiasMem(mem_scratch, atomic);
                    }
                    return MemOperand{left_reg, right_reg, LSL, shift_amount};
                } else {
                    if (use_memory_base) {
                        __ Lsl(mem_scratch, right_reg, shift_amount);
                        __ Add(mem_scratch, left_reg, mem_scratch);
                        return BiasMem(mem_scratch, atomic);
                    }
                    auto tmp = context.GetTmpX();
                    __ Lsl(tmp, right_reg, shift_amount);
                    return MemOperand{left_reg, tmp};
                }
            } else {
                PANIC();
            }
        }
        return {};
    }
}




























































#undef masm

}  // namespace swift::runtime::backend::arm64
