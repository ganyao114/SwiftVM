#pragma once

#include "translator.h"
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

JitTranslator::JitTranslator(JitContext& ctx) : context(ctx), masm(ctx.GetMasm()) {}

void JitTranslator::Translate(ir::Block* block) {
    cur_block = block;
    disable_instructions.resize(block->MaxInstrId());
    for (auto& inst : block->GetInstList()) {
        cur_instr = &inst;
        if (disable_instructions.test(inst.Id())) {
            continue;
        }
        Translate(&inst);
    }

    auto terminal = block->GetTerminal();
}

void JitTranslator::Translate(ir::HIRFunction* function) {}

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

void JitTranslator::SaveHostFlags(HostFlags host, ir::Flags guest) {
    // To arm64 host
    HostFlags host_need_saved{};
    if (True(guest & ir::Flags::Negate)) {
        host_need_saved |= HostFlags::N;
    }
    if (True(guest & ir::Flags::Zero)) {
        host_need_saved |= HostFlags::Z;
    }
    if (True(guest & ir::Flags::Carry)) {
        host_need_saved |= HostFlags::C;
    }
    if (True(guest & ir::Flags::Overflow)) {
        host_need_saved |= HostFlags::V;
    }
    if (save_in_nzcv) {
        if (host_need_saved != host) {
            PANIC();
        }
        nzcv_dirty = true;
    } else {
        __ Mrs(ip, NZCV);
        if (host_need_saved != host) {
            __ And(ip, ip, static_cast<u32>(host_need_saved));
        }
        __ Orr(flags, flags, ip);
    }
}

void JitTranslator::ClearFlags(ir::Flags guest) {
    if (!save_in_nzcv) {
        u64 mask{UINT64_MAX};
        if (True(guest & ir::Flags::Negate)) {
            mask &= ~(u64(1) << HostFlagsBit::N);
        }
        if (True(guest & ir::Flags::Zero)) {
            mask &= ~(u64(1) << HostFlagsBit::Z);
        }
        if (True(guest & ir::Flags::Carry)) {
            mask &= ~(u64(1) << HostFlagsBit::C);
        }
        if (True(guest & ir::Flags::Overflow)) {
            mask &= ~(u64(1) << HostFlagsBit::V);
        }
        __ And(flags, flags, ForceCast<s64>(mask));
    }
    auto af_cleared{false};
    if (True(guest & ir::Flags::Parity)) {
        // Clear Parity
        __ Mov(ip, 1);
        __ Bfi(flags, ip, HostFlagsBit::ParityByte, 8);
        if (True(guest & ir::Flags::AuxiliaryCarry)) {
            __ Bfi(flags, ip, HostFlagsBit::ParityByte, 16);
            af_cleared = true;
        } else {
            __ Bfi(flags, ip, HostFlagsBit::ParityByte, 8);
        }
    }
    if (True(guest & ir::Flags::AuxiliaryCarry) && !af_cleared) {
        __ Bfc(flags, HostFlagsBit::AFLeft, 8);
    }
}

void JitTranslator::SaveParity(Register& value) {
    __ Bfi(flags, value, HostFlagsBit::ParityByte, 8);
}

void JitTranslator::SaveNZ(Register& value, ir::ValueType type) {
    switch (type) {
        case ir::ValueType::U8:
            __ Sxtb(ip, value);
            break;
        case ir::ValueType::U16:
            __ Sxth(ip, value);
            break;
        case ir::ValueType::U32:
            __ Sxtw(ip, value);
            break;
        case ir::ValueType::U64:
            __ Mov(ip, value);
            break;
        default:
            PANIC();
    }
    __ Bics(ip, ip, 0);
    if (save_in_nzcv) {
        nzcv_dirty = true;
    } else {
        __ Mrs(ip, NZCV);
        __ Orr(flags, flags, ip);
    }
}

void JitTranslator::SaveCV(Register& value, ir::ValueType type) {
    if (type == ir::ValueType::U64) {
        return;
    }
    Label pass;
    __ Lsr(ip, value, ir::GetValueSizeByte(type) * 8);
    __ Cbz(ip, &pass);
    if (save_in_nzcv) {
        if (nzcv_dirty) {
            __ Mrs(ip, NZCV);
            __ Orr(ip, ip, 3u << HostFlagsBit::V);
            nzcv_dirty = false;
        } else {
            __ Orr(ip, xzr, 3u << HostFlagsBit::V);
        }
        __ Msr(NZCV, ip);
    } else {
        __ Orr(flags, flags, 3u << HostFlagsBit::V);
    }
    __ Bind(&pass);
}

void JitTranslator::SaveOF(Register& value, ir::ValueType type) {
    if (type == ir::ValueType::U64) {
        return;
    }
    Label pass;
    __ Lsr(ip, value, ir::GetValueSizeByte(type) * 8);
    __ Cbz(ip, &pass);
    if (save_in_nzcv) {
        if (nzcv_dirty) {
            __ Mrs(ip, NZCV);
            __ Orr(ip, ip, 1u << HostFlagsBit::V);
            nzcv_dirty = false;
        } else {
            __ Orr(ip, xzr, 1u << HostFlagsBit::V);
        }
        __ Msr(NZCV, ip);
    } else {
        __ Orr(flags, flags, 1u << HostFlagsBit::V);
    }
    __ Bind(&pass);
}

void JitTranslator::SaveAuxiliaryCarry(Register &left, Register &result) {
    __ Bfi(flags, left, HostFlagsBit::AFLeft, 4);
    __ Bfi(flags, result, HostFlagsBit::AFRight, 4);
}

void JitTranslator::EmitAdvancePC(ir::Inst* inst) {
    nzcv_dirty = false;
    FlushFlags();
}

void JitTranslator::GetParityFlag(Register& result) {
    __ Ubfx(result.W(), flags, HostFlagsBit::ParityByte, 8);
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 4});
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 2});
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 1});
}

void JitTranslator::TestParityFlag(Register& result) {

}

void JitTranslator::TestAuxiliaryCarry(Register& result) {

}

JitTranslator::PseudoFlags JitTranslator::GetPseudoFlags(ir::Inst* inst) {
    ir::Flags result_set{};
    ir::Flags result_clear{};
    if (auto pseudos = inst->GetPseudoOperations(); !pseudos.empty()) {
        for (auto& pseudo : pseudos) {
            if (pseudo->GetOp() == ir::OpCode::SaveFlags) {
                auto guest_flags = pseudo->GetArg<ir::Flags>(1);
                result_set |= guest_flags;
            } else if (pseudo->GetOp() == ir::OpCode::ClearFlags) {
                auto guest_flags = pseudo->GetArg<ir::Flags>(0);
                result_clear |= guest_flags;
            }
        }
    }
    return {result_set, result_clear};
}

void JitTranslator::EmitSaveFlags(ir::Inst* inst) {
    ASSERT(flags_set == ir::Flags::None);
    flags_set = inst->GetArg<ir::Flags>(1);
}

void JitTranslator::EmitClearFlags(ir::Inst* inst) {
    ASSERT(flags_clear == ir::Flags::None);
    flags_clear = inst->GetArg<ir::Flags>(1);
}

void JitTranslator::FlushFlags() {
    if (flags_set != ir::Flags::None) {

    }

    if (flags_clear != ir::Flags::None) {

    }

    flags_set = ir::Flags::None;
    flags_clear = ir::Flags::None;
}

void JitTranslator::EmitGetHostGPR(ir::Inst* inst) {
    auto offset = inst->GetArg<ir::Imm>(1).Get();
    if (!offset) {
        return;
    }
    auto reg_index = inst->GetArg<ir::Imm>(0).Get();
    auto host_reg = XRegister(reg_index);
    auto ret_reg = context.X(ir::Value{inst});
    if (host_reg != ret_reg) {
        __ Ubfx(ret_reg, host_reg, reg_index * 8, ir::GetValueSizeByte(inst->ReturnType()));
    }
}

void JitTranslator::EmitGetHostFPR(ir::Inst* inst) {
    auto offset = inst->GetArg<ir::Imm>(1).Get();
    if (!offset) {
        return;
    }
    auto reg_index = inst->GetArg<ir::Imm>(0).Get();
    auto host_reg = VRegister::GetQRegFromCode(reg_index);
    auto ret_reg = context.V(ir::Value{inst});
    if (host_reg != ret_reg) {
        PANIC("GetHostFPR!");
    }
}

void JitTranslator::EmitSetHostGPR(ir::Inst* inst) {
    auto offset = inst->GetArg<ir::Imm>(2).Get();
    if (!offset) {
        return;
    }
    auto reg_index = inst->GetArg<ir::Imm>(1).Get();
    auto host_reg = XRegister(reg_index);
    auto value_reg = context.X(inst->GetArg<ir::Value>(0));
    if (value_reg != host_reg) {
        __ Bfi(host_reg, value_reg, reg_index * 8, ir::GetValueSizeByte(inst->ReturnType()));
    }
}

void JitTranslator::EmitSetHostFPR(ir::Inst* inst) {

}

void JitTranslator::EmitLoadUniform(ir::Inst* inst) {
    auto uni = inst->GetArg<ir::Uniform>(0);
    s32 offset = offsetof(State, uniform_buffer_begin) + uni.GetOffset();
    auto reg = context.Get(inst);
    VisitVariant<void>(reg, [this, inst, offset] (auto x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Register>) {
            switch (GetValueSizeByte(inst->ReturnType())) {
                case 1:
                    __ Ldrb(x, MemOperand(state, offset));
                    break;
                case 2:
                    __ Ldrh(x, MemOperand(state, offset));
                    break;
                case 4:
                    __ Ldr(x.W(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Ldr(x, MemOperand(state, offset));
                    break;
            }
        } else if constexpr (std::is_same_v<T, VRegister>) {
            switch (GetValueSizeByte(inst->ReturnType())) {
                case 1:
                    __ Ldr(x.B(), MemOperand(state, offset));
                    break;
                case 2:
                    __ Ldr(x.H(), MemOperand(state, offset));
                    break;
                case 4:
                    __ Ldr(x.S(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Ldr(x.D(), MemOperand(state, offset));
                    break;
                case 16:
                    __ Ldr(x.Q(), MemOperand(state, offset));
                    break;
            }
        } else {
            PANIC();
        }
    });
}

void JitTranslator::EmitStoreUniform(ir::Inst* inst) {
    auto uni = inst->GetArg<ir::Uniform>(0);
    s32 offset = offsetof(State, uniform_buffer_begin) + uni.GetOffset();
    auto reg = context.Get(inst->GetArg<ir::Value>(1));
    VisitVariant<void>(reg, [this, inst, offset] (auto x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Register>) {
            switch (GetValueSizeByte(inst->ReturnType())) {
                case 1:
                    __ Strb(x, MemOperand(state, offset));
                    break;
                case 2:
                    __ Strh(x, MemOperand(state, offset));
                    break;
                case 4:
                    __ Str(x.W(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Str(x, MemOperand(state, offset));
                    break;
            }
        } else if constexpr (std::is_same_v<T, VRegister>) {
            switch (GetValueSizeByte(inst->ReturnType())) {
                case 1:
                    __ Str(x.B(), MemOperand(state, offset));
                    break;
                case 2:
                    __ Str(x.H(), MemOperand(state, offset));
                    break;
                case 4:
                    __ Str(x.S(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Str(x.D(), MemOperand(state, offset));
                    break;
                case 16:
                    __ Str(x.Q(), MemOperand(state, offset));
                    break;
            }
        } else {
            PANIC();
        }
    });
}

void JitTranslator::EmitLoadLocal(ir::Inst* inst) { PANIC("TODO"); }

void JitTranslator::EmitStoreLocal(ir::Inst* inst) { PANIC("TODO"); }

void JitTranslator::EmitLoadMemory(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = inst->GetArg<ir::Value>(1);
    auto type = value.Type();
    auto vixl_operand = EmitMemOperand(operand, type, false);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldrb(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldrh(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldr(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldr(context.X(value), vixl_operand);
            break;
        case ir::ValueType::V8:
            __ Ldr(context.V(value).B(), vixl_operand);
            break;
        case ir::ValueType::V16:
            __ Ldr(context.V(value).H(), vixl_operand);
            break;
        case ir::ValueType::V32:
            __ Ldr(context.V(value).S(), vixl_operand);
            break;
        case ir::ValueType::V64:
            __ Ldr(context.V(value).D(), vixl_operand);
            break;
        case ir::ValueType::V128:
            __ Ldr(context.V(value).Q(), vixl_operand);
            break;
        default:
            PANIC("UnImplement!");
            break;
    }
}

void JitTranslator::EmitStoreMemory(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = inst->GetArg<ir::Value>(1);
    auto type = value.Type();
    auto vixl_operand = EmitMemOperand(operand, type, false);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Strb(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Strh(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Str(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Str(context.X(value), vixl_operand);
            break;
        case ir::ValueType::V8:
            __ Str(context.V(value).B(), vixl_operand);
            break;
        case ir::ValueType::V16:
            __ Str(context.V(value).H(), vixl_operand);
            break;
        case ir::ValueType::V32:
            __ Str(context.V(value).S(), vixl_operand);
            break;
        case ir::ValueType::V64:
            __ Str(context.V(value).D(), vixl_operand);
            break;
        case ir::ValueType::V128:
            __ Str(context.V(value).Q(), vixl_operand);
            break;
        default:
            PANIC("UnImplement!");
            break;
    }
}

void JitTranslator::EmitLoadMemoryTSO(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = inst->GetArg<ir::Value>(1);
    auto type = value.Type();
    auto vixl_operand = EmitMemOperand(operand, type, false);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldrb(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldrh(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldr(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldr(context.X(value), vixl_operand);
            break;
        case ir::ValueType::V8:
            __ Ldr(context.V(value).B(), vixl_operand);
            break;
        case ir::ValueType::V16:
            __ Ldr(context.V(value).H(), vixl_operand);
            break;
        case ir::ValueType::V32:
            __ Ldr(context.V(value).S(), vixl_operand);
            break;
        case ir::ValueType::V64:
            __ Ldr(context.V(value).D(), vixl_operand);
            break;
        case ir::ValueType::V128:
            __ Ldr(context.V(value).Q(), vixl_operand);
            break;
        default:
            PANIC("UnImplement!");
            break;
    }
}

void JitTranslator::EmitStoreMemoryTSO(ir::Inst* inst) {

}

void JitTranslator::EmitMemoryCopy(ir::Inst* inst) {

}

void JitTranslator::EmitMemoryCopyTSO(ir::Inst* inst) {

}

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
        auto left_value = ir_op.GetRight().value;
        auto left_reg = context.R(left_value, true);
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
                auto tmp = context.GetTmpGPR(left_value.Type());
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
            auto tmp = context.GetTmpGPR(left_value.Type());
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

MemOperand JitTranslator::EmitMemOperand(ir::Operand& ir_op, ir::ValueType type, bool pair) {
    auto access_size = ir::GetValueSizeByte(type);
    if (ir_op.GetRight().Null()) {
        if (ir_op.GetLeft().IsImm()) {
            auto imm = ir_op.GetLeft().imm.Get();
            auto imm_signed = ir_op.GetLeft().imm.GetSigned();
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
            if (addr_value.Def()->GetUses() == 2) {
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
                            return MemOperand{context.R(addr_value), imm, PostIndex};
                        } else {
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
            return MemOperand{context.R(addr_value)};
        }
    } else {
        auto left_value = ir_op.GetRight().value;
        auto right = ir_op.GetRight();
        if (right.IsImm()) {
            auto imm = right.imm.GetSigned();
            bool can_imm = pair ? __ IsImmLSPair(imm, access_size) : __ IsImmLSUnscaled(imm);
            if (can_imm) {
                if (ir_op.GetOp() == ir::OperandOp::Plus) {
                    return MemOperand{context.R(left_value, true), imm};
                } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                    auto tmp = context.GetTmpX();
                    __ Lsl(tmp, context.R(left_value, true), imm);
                    return MemOperand{tmp};
                } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                    auto tmp = context.GetTmpX();
                    __ Lsr(tmp, context.R(left_value, true), imm);
                    return MemOperand{tmp};
                } else {
                    PANIC();
                }
            } else {
                auto tmp = context.GetTmpX();
                __ Mov(tmp, imm);
                if (ir_op.GetOp() == ir::OperandOp::Plus) {
                    return MemOperand{context.R(left_value, true), tmp};
                } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                    return MemOperand{context.R(left_value, true), tmp, LSL};
                } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                    return MemOperand{context.R(left_value, true), tmp, LSR};
                } else {
                    PANIC();
                }
            }
        } else {
            auto left_reg = context.R(left_value, true);
            auto right_reg = context.R(right.value, true);
            if (ir_op.GetOp() == ir::OperandOp::Plus) {
                return MemOperand{left_reg, right_reg};
            } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                return MemOperand{left_reg, right_reg, LSL};
            } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                return MemOperand{left_reg, right_reg, LSR};
            } else if (ir_op.GetOp() == ir::OperandOp::PlusExt) {
                auto shift_amount = ir_op.GetOp().shift_ext;
                if (ir::GetValueSizeByte(right.value.Type()) == shift_amount) {
                    return MemOperand{left_reg, right_reg, LSL, shift_amount};
                } else {
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

void JitTranslator::EmitAdd(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        __ Adds(result, left_register, right_operand);
        SaveHostFlags(HostFlags::NZCV, pseudo_flags.set);
        if (True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
        if (True(pseudo_flags.set & ir::Flags::AuxiliaryCarry)) {
            SaveAuxiliaryCarry(left_register, result);
        }
    } else {
        __ Add(result, left_register, right_operand);
    }
}

void JitTranslator::EmitAdc(ir::Inst* inst) {}

void JitTranslator::EmitAnd(ir::Inst* inst) {

}

void JitTranslator::EmitAddPhi(ir::Inst* inst) {}

void JitTranslator::EmitAndNot(ir::Inst* inst) {}

void JitTranslator::EmitAsrImm(ir::Inst* inst) {}

void JitTranslator::EmitLslImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto lsl = inst->GetArg<ir::Imm>(1).Get();
    auto result = context.R(ir::Value{inst});
    __ Lsl(result, context.R(value), lsl);
}

void JitTranslator::EmitLsrImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto lsr = inst->GetArg<ir::Imm>(1).Get();
    auto result = context.R(ir::Value{inst});
    __ Lsl(result, context.R(value), lsr);
}

void JitTranslator::EmitPopRSB(ir::Inst* inst) {}

void JitTranslator::EmitRorImm(ir::Inst* inst) {}

void JitTranslator::EmitVec4Or(ir::Inst* inst) {}

void JitTranslator::EmitBitCast(ir::Inst* inst) {
    // Ignore
}

void JitTranslator::EmitLoadImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Imm>(1);
    auto result = context.R(ir::Value{inst});
    __ Mov(result, value.Get());
}

void JitTranslator::EmitNotGoto(ir::Inst* inst) {

}

void JitTranslator::EmitPushRSB(ir::Inst* inst) {

}

void JitTranslator::EmitTestBit(ir::Inst* inst) {

}

void JitTranslator::EmitVec4Add(ir::Inst* inst) {}

void JitTranslator::EmitVec4And(ir::Inst* inst) {}

void JitTranslator::EmitVec4Mul(ir::Inst* inst) {}

void JitTranslator::EmitVec4Sub(ir::Inst* inst) {}

void JitTranslator::EmitAsrValue(ir::Inst* inst) {}

void JitTranslator::EmitBitClear(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto left = inst->GetArg<ir::Imm>(1).Get();
    auto bits = inst->GetArg<ir::Imm>(2).Get();
    auto value_reg = context.R(value);
    auto result = context.R(ir::Value{inst});
    if (value_reg != result) {
        __ Mov(result, left);
    }
    __ Bfc(result, left, bits);
}

void JitTranslator::EmitGetFlags(ir::Inst* inst) {}

void JitTranslator::EmitLslValue(ir::Inst* inst) {}

void JitTranslator::EmitLsrValue(ir::Inst* inst) {}

void JitTranslator::EmitRorValue(ir::Inst* inst) {}

void JitTranslator::EmitTestZero(ir::Inst* inst) {}

void JitTranslator::EmitBindLabel(ir::Inst* inst) {}

void JitTranslator::EmitBitInsert(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto left = inst->GetArg<ir::Imm>(1).Get();
    auto bits = inst->GetArg<ir::Imm>(2).Get();
    auto result = context.R(ir::Value{inst});
    __ Bfi(result, context.R(value), left, bits);
}

void JitTranslator::EmitTestFlags(ir::Inst* inst) {}

void JitTranslator::EmitBitExtract(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto left = inst->GetArg<ir::Imm>(1).Get();
    auto bits = inst->GetArg<ir::Imm>(2).Get();
    auto result = context.R(ir::Value{inst});
    __ Ubfx(result, context.R(value), left, bits);
}

void JitTranslator::EmitCallLambda(ir::Inst* inst) {}

void JitTranslator::EmitGetOperand(ir::Inst* inst) {}

void JitTranslator::EmitSignExtend(ir::Inst* inst) {}

void JitTranslator::EmitCallDynamic(ir::Inst* inst) {}

void JitTranslator::EmitDefineLocal(ir::Inst* inst) {}

void JitTranslator::EmitGetLocation(ir::Inst* inst) {}

void JitTranslator::EmitSetLocation(ir::Inst* inst) {}

void JitTranslator::EmitTestNotZero(ir::Inst* inst) {}

void JitTranslator::EmitCallLocation(ir::Inst* inst) {}

void JitTranslator::EmitTestNotFlags(ir::Inst* inst) {}

void JitTranslator::EmitZeroExtend32(ir::Inst* inst) {}

void JitTranslator::EmitZeroExtend64(ir::Inst* inst) {}

void JitTranslator::EmitCompareAndSwap(ir::Inst* inst) {}

void JitTranslator::EmitUniformBarrier(ir::Inst* inst) {}

void JitTranslator::EmitOr(ir::Inst* inst) {}

void JitTranslator::EmitDiv(ir::Inst* inst) {}

void JitTranslator::EmitMul(ir::Inst* inst) {}

void JitTranslator::EmitNop(ir::Inst* inst) {}

void JitTranslator::EmitNot(ir::Inst* inst) {}

void JitTranslator::EmitSbb(ir::Inst* inst) {}

void JitTranslator::EmitSub(ir::Inst* inst) {}

void JitTranslator::EmitXor(ir::Inst* inst) {}

void JitTranslator::EmitGoto(ir::Inst* inst) {}

void JitTranslator::EmitZero(ir::Inst* inst) {}

#undef masm

}  // namespace swift::runtime::backend::arm64