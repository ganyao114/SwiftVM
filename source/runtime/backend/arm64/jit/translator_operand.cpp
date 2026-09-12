#include "translator.h"
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
    auto* definition = value.Def();
    if (!context.GetFeatures().zero_store_zr || context.IsSpilled(value) ||
        !definition || !IsZeroStoreValue(value) ||
        ir::GetValueSizeByte(value.Type()) > sizeof(u64)) {
        return false;
    }
    return HasOnlyZeroStoreUses(definition);
}

bool JitTranslator::IsZeroStoreValue(ir::Value value) {
    if (!value.Defined() || ir::IsFloatValueType(value.Type())) {
        return false;
    }
    auto* definition = value.Def();
    if (definition->GetOp() == ir::OpCode::LoadImm) {
        return definition->GetArg<ir::Imm>(0).Get() == 0;
    }
    if (definition->GetOp() != ir::OpCode::ZeroExtend32 &&
        definition->GetOp() != ir::OpCode::ZeroExtend32To64) {
        return false;
    }
    return IsZeroStoreValue(definition->GetArg<ir::Value>(0));
}

bool JitTranslator::HasOnlyZeroStoreUses(ir::Inst* definition) {
    u32 compatible_uses = 0;
    for (auto& use : cur_block->GetInstList()) {
        if ((use.GetOp() == ir::OpCode::StoreUniform ||
             use.GetOp() == ir::OpCode::StoreMemory) &&
            use.GetArg<ir::Value>(1).Def() == definition) {
            ++compatible_uses;
        } else if (use.GetOp() == ir::OpCode::SetHostFPR &&
                   use.GetArg<ir::Value>(0).Def() == definition) {
            ++compatible_uses;
        } else if ((use.GetOp() == ir::OpCode::ZeroExtend32 ||
                    use.GetOp() == ir::OpCode::ZeroExtend32To64) &&
                   use.GetArg<ir::Value>(0).Def() == definition &&
                   !context.IsSpilled(ir::Value{&use}) &&
                   IsZeroStoreValue(ir::Value{&use}) &&
                   HasOnlyZeroStoreUses(&use)) {
            ++compatible_uses;
        }
    }
    return compatible_uses != 0 &&
           compatible_uses == definition->GetUses(false);
}

std::optional<JitTranslator::MemoryUpdate>
JitTranslator::MatchMemoryUpdate(ir::Inst* update) const {
    if (!update || update->GetOp() != ir::OpCode::Sub ||
        ir::GetValueSizeByte(update->ReturnType()) != sizeof(u64) ||
        update->GetUses(false) != 2) {
        return std::nullopt;
    }

    const auto right = update->GetArg<ir::Operand>(1);
    if (!right.GetRight().Null() || !right.GetLeft().IsImm()) {
        return std::nullopt;
    }
    const u64 decrement = right.GetLeft().imm.Get();
    if (decrement == 0 || decrement > 256) {
        return std::nullopt;
    }

    const auto source = update->GetArg<ir::Value>(0);
    auto* source_def = source.Def();
    if (!source_def || source_def->GetOp() != ir::OpCode::GetHostGPR ||
        source_def->GetArg<ir::Imm>(1).Get() != 0 ||
        ir::GetValueSizeByte(source.Type()) != sizeof(u64)) {
        return std::nullopt;
    }
    const u32 target = source_def->GetArg<ir::Imm>(0).Get();
    const auto base = XRegister(target);
    if (context.X(source) != base) {
        return std::nullopt;
    }

    auto& instructions = cur_block->GetInstList();
    auto memory_it = std::next(instructions.iterator_to(*update));
    if (memory_it == instructions.end() ||
        memory_it->GetOp() != ir::OpCode::StoreMemory) {
        return std::nullopt;
    }
    const auto memory_operand = memory_it->GetArg<ir::Operand>(0);
    const auto stored = memory_it->GetArg<ir::Value>(1);
    if (!memory_operand.GetRight().Null() ||
        !memory_operand.GetLeft().IsValue() ||
        memory_operand.GetLeft().value.Def() != update ||
        ir::IsFloatValueType(stored.Type()) ||
        ir::GetValueSizeByte(stored.Type()) != decrement) {
        return std::nullopt;
    }
    if (!context.IsSpilled(stored) && context.X(stored) == base) {
        return std::nullopt;
    }

    auto publication_it = std::next(memory_it);
    if (publication_it == instructions.end() ||
        publication_it->GetOp() != ir::OpCode::SetHostGPR ||
        publication_it->GetArg<ir::Value>(0).Def() != update ||
        publication_it->GetArg<ir::Imm>(1).Get() != target ||
        publication_it->GetArg<ir::Imm>(2).Get() != 0) {
        return std::nullopt;
    }

    return MemoryUpdate{
            .memory = memory_it.operator->(),
            .publication = publication_it.operator->(),
            .base = base,
            .offset = -static_cast<s64>(decrement),
    };
}

std::optional<JitTranslator::MemoryUpdate>
JitTranslator::MatchPreIndexMemoryUpdate(ir::Inst* update) const {
    if (memory_state.use_memory_base) {
        return std::nullopt;
    }
    return MatchMemoryUpdate(update);
}

std::optional<JitTranslator::MemoryUpdate>
JitTranslator::MatchBiasedMemoryUpdate(ir::Inst* update) const {
    if (!memory_state.use_memory_base) {
        return std::nullopt;
    }
    auto match = MatchMemoryUpdate(update);
    if (!match || !context.IsSpilled(ir::Value{update})) {
        return std::nullopt;
    }
    return match;
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
            if (addr_value.Def()) {
                if (auto recomputed = TryEmitSpilledMemoryOperand(
                            addr_value.Def(), type, pair, atomic, memory_inst)) {
                    return *recomputed;
                }
            }
            if (!memory_state.use_memory_base &&
                context.IsConstAddressCached(addr_value.Id())) {
                const auto offset = CachedConstAddressOffset(addr_value.Def());
                ASSERT_MSG(offset,
                           "constant-address offset proof failed at IR {}",
                           addr_value.Id());
                return MemOperand{context.R(addr_value), static_cast<s64>(*offset)};
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
            if ((memory_state.mem_narrow_fuse || memory_state.addr_ea_tie) &&
                addr_value.Def()->GetOp() == ir::OpCode::GetOperand &&
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
                    if (memory_state.use_memory_base) {
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
            if (allow_writeback && !memory_state.use_memory_base && addr_value.Def()->GetUses() == 2) {
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
            if (memory_state.use_memory_base) {
                auto pinned = ResolvePinnedGPRValue(addr_value);
                return BiasMem(pinned ? Register{*pinned}
                                      : context.R(addr_value),
                               atomic);
            }
            auto pinned = ResolvePinnedGPRValue(addr_value);
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
            auto pinned = ResolvePinnedGPRValue(left);
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
            auto pinned = ResolvePinnedGPRValue(right.value);
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
