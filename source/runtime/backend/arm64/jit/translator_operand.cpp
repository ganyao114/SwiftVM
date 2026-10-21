#include "translator.h"
#include "runtime/backend/arm64/alloc/target_analysis_traits.h"
#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

std::optional<MemOperand> JitTranslator::TryEmitSpilledMemoryOperand(
        ir::Inst* address,
        ir::ValueType type,
        bool pair,
        bool atomic,
        ir::Inst* memory_inst) {
    const auto recipe = memory_state.spilled_memory_operands.find(address);
    if (recipe == memory_state.spilled_memory_operands.end() ||
        recipe->second.consumer != memory_inst) {
        return std::nullopt;
    }
    if (memory_state.use_memory_base) {
        return BiasMem(recipe->second.base, recipe->second.offset, atomic);
    }
    const u32 access_size = ir::GetValueSizeByte(type);
    const bool encodable = pair
            ? __ IsImmLSPair(recipe->second.offset, access_size)
            : __ IsImmLSUnscaled(recipe->second.offset) ||
                      __ IsImmLSScaled(recipe->second.offset, access_size);
    if (encodable) {
        return MemOperand{recipe->second.base, recipe->second.offset};
    }
    const auto address_reg = context.GetTmpX();
    if (recipe->second.offset > 0 && __ IsImmAddSub(recipe->second.offset)) {
        __ Add(address_reg, recipe->second.base, recipe->second.offset);
    } else if (recipe->second.offset < 0 &&
               __ IsImmAddSub(-recipe->second.offset)) {
        __ Sub(address_reg, recipe->second.base, -recipe->second.offset);
    } else {
        __ Mov(address_reg, recipe->second.offset);
        __ Add(address_reg, recipe->second.base, address_reg);
    }
    return MemOperand{address_reg};
}

bool JitTranslator::CanUseZeroStoreRegister(ir::Value value) {
    return TargetCanUseZeroStoreRegister(value, context.GetAllocation(),
                                        context.GetFeatures(), gpr_plan->consumers.get());
}



std::optional<JitTranslator::MemoryUpdate>
JitTranslator::MatchPreIndexMemoryUpdate(ir::Inst* update) const {
    const auto found = gpr_plan->preindex_updates.find(update);
    return found == gpr_plan->preindex_updates.end() ? std::nullopt : std::optional{found->second};
}

std::optional<JitTranslator::MemoryUpdate>
JitTranslator::MatchBiasedMemoryUpdate(ir::Inst* update) const {
    const auto found = gpr_plan->biased_updates.find(update);
    return found == gpr_plan->biased_updates.end() ? std::nullopt : std::optional{found->second};
}



MemOperand JitTranslator::EmitMemOperand(ir::Operand& ir_op,
                                         ir::ValueType type,
                                         bool pair,
                                         bool atomic,
                                         bool allow_writeback,
                                         bool structured_guest_ea,
                                         ir::Inst* memory_inst) {
    auto access_size = ir::GetValueSizeByte(type);
    if (ir_op.GetRight().Null()) {
        if (ir_op.GetLeft().IsImm()) {
            auto imm = ir_op.GetLeft().imm.Get();
            auto imm_signed = ir_op.GetLeft().imm.GetSigned();
            if (memory_state.use_memory_base) {
                // Absolute guest address: materialize it, then apply the pt
                // bias (guest addr + pt = host addr). With a bounded guest
                // window the truncation happens at translation time — the
                // immediate is a compile-time constant, so it is free.
                __ Mov(mem_scratch, memory_state.guest_addr_mask ? (imm & memory_state.guest_addr_mask) : imm);
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
            if (memory_inst && gpr_plan->memory) {
                const auto found = gpr_plan->memory->offsets.find(memory_inst);
                if (found != gpr_plan->memory->offsets.end()) {
                    const auto& action = found->second;
                    auto pinned = context.PlannedGPR<XRegister>(action.base, cur_instr);
                    const Register base = pinned ? Register{*pinned}
                                                 : context.R(action.base);
                    if (memory_state.use_memory_base) {
                        return BiasMem(base, action.offset, atomic);
                    }
                    return MemOperand{base, action.offset};
                }
            }
            if (addr_value.Def()) {
                if (auto recomputed = TryEmitSpilledMemoryOperand(
                            addr_value.Def(), type, pair, atomic, memory_inst)) {
                    return *recomputed;
                }
            }
            if (allow_writeback && !atomic) {
                if (memory_state.use_memory_base) {
                    auto update = MatchBiasedMemoryUpdate(addr_value.Def());
                    if (update && update->memory == memory_inst) {
                        return BiasMem(update->base, update->offset, false);
                    }
                } else {
                    auto update = MatchPreIndexMemoryUpdate(addr_value.Def());
                    if (update && update->memory == memory_inst) {
                        return MemOperand{update->base, update->offset, PreIndex};
                    }
                }
            }
            if (allow_writeback && !atomic && gpr_plan->memory) {
                const auto found = gpr_plan->memory->writebacks.find(memory_inst);
                if (found != gpr_plan->memory->writebacks.end()) {
                    const auto& action = found->second;
                    ASSERT(!pair && action.access_bytes == access_size);
                    return MemOperand{XRegister(action.base), action.offset, PostIndex};
                }
            }
            if (memory_state.use_memory_base) {
                auto pinned = context.PlannedGPR<XRegister>(addr_value, cur_instr);
                return BiasMem(pinned ? Register{*pinned}
                                      : context.R(addr_value),
                               atomic);
            }
            auto pinned = context.PlannedGPR<XRegister>(addr_value, cur_instr);
            return MemOperand{pinned ? Register{*pinned}
                                     : context.R(addr_value)};
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
            auto left = ir_op.GetLeft().value;
            auto pinned = context.PlannedGPR<XRegister>(left, cur_instr);
            left_reg = pinned ? Register{*pinned} : context.R(left, true);
        }
        auto right = ir_op.GetRight();
        if (right.IsImm()) {
            auto imm = right.imm.GetSigned();
            bool can_imm = pair ? __ IsImmLSPair(imm, access_size)
                                : __ IsImmLSUnscaled(imm);
            if (!memory_state.use_memory_base && !pair && ir_op.GetOp() == ir::OperandOp::Plus) {
                can_imm |= __ IsImmLSScaled(imm, access_size);
            }
            if (can_imm) {
                if (ir_op.GetOp() == ir::OperandOp::Plus) {
                    if (memory_state.use_memory_base) {
                        return BiasMem(left_reg, imm, atomic);
                    }
                    return MemOperand{left_reg, imm};
                } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                    if (memory_state.use_memory_base) {
                        __ Lsl(mem_scratch, left_reg, imm);
                        return BiasMem(mem_scratch, atomic);
                    }
                    auto tmp = context.GetTmpX();
                    __ Lsl(tmp, left_reg, imm);
                    return MemOperand{tmp};
                } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                    if (memory_state.use_memory_base) {
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
                if (memory_state.use_memory_base) {
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
            auto pinned = context.PlannedGPR<XRegister>(right.value, cur_instr);
            auto right_reg = pinned ? Register{*pinned}
                                    : context.R(right.value, true);
            if (ir_op.GetOp() == ir::OperandOp::Plus) {
                if (memory_state.use_memory_base) {
                    if (structured_guest_ea && memory_state.window_uxtw) {
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
                if (memory_state.use_memory_base) {
                    __ Lsl(mem_scratch, left_reg, right_reg);
                    return BiasMem(mem_scratch, atomic);
                }
                return MemOperand{left_reg, right_reg, LSL};
            } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                if (memory_state.use_memory_base) {
                    __ Lsr(mem_scratch, left_reg, right_reg);
                    return BiasMem(mem_scratch, atomic);
                }
                return MemOperand{left_reg, right_reg, LSR};
            } else if (ir_op.GetOp() == ir::OperandOp::PlusExt) {
                auto shift_amount = ir_op.GetOp().shift_ext;
                if (structured_guest_ea && memory_state.use_memory_base) {
                    if (memory_state.window_uxtw) {
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
                const bool ea_scaled_offset =
                        memory_state.addr_ea_tie && !memory_state.use_memory_base && shift_amount < 4 &&
                        (u64{1} << shift_amount) == access_size;
                if (ea_scaled_offset ||
                    ir::GetValueSizeByte(right.value.Type()) == shift_amount) {
                    if (memory_state.use_memory_base) {
                        __ Add(mem_scratch,
                               left_reg,
                               Operand{right_reg, LSL, shift_amount});
                        return BiasMem(mem_scratch, atomic);
                    }
                    return MemOperand{left_reg, right_reg, LSL, shift_amount};
                } else {
                    if (memory_state.use_memory_base) {
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

#undef __

}  // namespace swift::runtime::backend::arm64
