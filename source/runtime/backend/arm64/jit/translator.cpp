#pragma once

#include "translator.h"

#include <cstring>
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.


JitTranslator::JitTranslator(JitContext& ctx) : context(ctx), masm(ctx.GetMasm()) {
    auto& config = ctx.GetConfig();
    use_memory_base = config.memory_base != nullptr || config.page_table != nullptr;
}



void JitTranslator::Translate(ir::Block* block) {
    cur_block = block;
    context.SetCurrent(block);
    disable_instructions.resize(block->MaxInstrId());
    for (auto& inst : block->GetInstList()) {
        cur_instr = &inst;
        if (inst.Id() < disable_instructions.size() && disable_instructions.test(inst.Id())) {
            continue;
        }
        Translate(&inst);
    }

    FlushFlags();
    EmitTerminal(block->GetTerminal());
}

void JitTranslator::Translate(ir::HIRFunction* function) {
    ASSERT(function);
    context.SetCurrent(function->GetFunction());
    for (auto& hir_block : function->GetHIRBlocksRPO()) {
        Translate(hir_block.GetBlock());
    }
}

void JitTranslator::EmitTerminal(const ir::Terminal& terminal) {
    VisitVariant<void>(terminal, [this](auto term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::Invalid>) {
            // Flat decoded blocks have no explicit terminal: the next location was
            // already written to state->current_loc by a SetLocation instruction.
            MergeNZCV();
            __ Ret();
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToDispatch>) {
            MergeNZCV();
            __ Ret();
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost>) {
            MergeNZCV();
            __ Mov(ipw, static_cast<u32>(HaltReason::CallHost));
            __ Str(ipw, MemOperand(state, state_offset_halt_reason));
            __ Ret();
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlock>) {
            MergeNZCV();
            context.Forward(term.next);
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            MergeNZCV();
            context.Forward(term.next);
        } else if constexpr (std::is_same_v<T, ir::terminal::PopRSBHint>) {
            // Return Stack Buffer: this is the real pop+predict site. It must
            // run here (not at the PopRSB instruction) because guest flags have
            // just been committed by FlushFlags/MergeNZCV — the hit path
            // branches directly to the return target, which expects the flags
            // register to be current. EmitRSBPop ends in Br (hit) or Ret (miss/
            // underflow), so it fully terminates the block.
            MergeNZCV();
            if (True(context.GetConfig().global_opts & Optimizations::ReturnStackBuffer)) {
                context.EmitRSBPop();
            } else {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
            Label else_label;
            __ Cbz(context.W(term.cond), &else_label);
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
                                         bool allow_writeback) {
    auto access_size = ir::GetValueSizeByte(type);
    if (ir_op.GetRight().Null()) {
        if (ir_op.GetLeft().IsImm()) {
            auto imm = ir_op.GetLeft().imm.Get();
            auto imm_signed = ir_op.GetLeft().imm.GetSigned();
            if (use_memory_base) {
                // Absolute guest address: materialize it, then apply the pt
                // bias (guest addr + pt = host addr).
                __ Mov(mem_scratch, imm);
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
                    __ Add(mem_scratch, left_reg, right_reg);
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
                if (ir::GetValueSizeByte(right.value.Type()) == shift_amount) {
                    if (use_memory_base) {
                        __ Add(mem_scratch, left_reg, Operand{right_reg, LSL, shift_amount});
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
