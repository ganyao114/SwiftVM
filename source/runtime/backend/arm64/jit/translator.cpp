#pragma once

#include "translator.h"
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

JitTranslator::JitTranslator(JitContext& ctx) : context(ctx), masm(ctx.GetMasm()) {
    auto& config = ctx.GetConfig();
    use_memory_base = config.memory_base != nullptr || config.page_table != nullptr;
}

MemOperand JitTranslator::BiasMem(const Register& base, bool atomic) {
    if (!atomic) {
        return MemOperand{base, pt};
    }
    // No register-offset form available: fold the bias into the reserved
    // scratch (mem_scratch is never allocated to a guest value, unlike a
    // GetTmpX register at a VOID instruction — see defines.h).
    __ Add(mem_scratch, base, pt);
    return MemOperand{mem_scratch};
}

MemOperand JitTranslator::BiasMem(const Register& base, s64 imm, bool atomic) {
    if (imm == 0) {
        return BiasMem(base, atomic);
    }
    // [guest base + imm + pt]: fold the immediate into the reserved scratch.
    if (imm > 0) {
        __ Add(mem_scratch, base, imm);
    } else {
        __ Sub(mem_scratch, base, -imm);
    }
    if (atomic) {
        __ Add(mem_scratch, mem_scratch, pt);
        return MemOperand{mem_scratch};
    }
    return MemOperand{mem_scratch, pt};
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
            // TODO: ReturnStackBuffer support; fall back to a plain dispatcher return.
            MergeNZCV();
            __ Ret();
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

void JitTranslator::MergeNZCV() {
    if (save_in_nzcv && nzcv_dirty) {
        // Replace semantics: clear the stale NZCV bits in the flags register
        // first. A plain Orr would accumulate sticky bits (e.g. Z=1 left by an
        // earlier instruction) and poison every later flag consumer.
        u64 clear_nzcv = ~static_cast<u64>(HostFlags::NZCV);
        __ Mrs(ip, NZCV);
        __ And(flags, flags, ForceCast<s64>(clear_nzcv));
        __ And(ip, ip, static_cast<u32>(HostFlags::NZCV));
        __ Orr(flags, flags, ip);
        nzcv_dirty = false;
    }
}

void JitTranslator::LoadNZCVFromFlags() {
    __ And(ip, flags, static_cast<u64>(HostFlags::NZCV));
    __ Msr(NZCV, ip);
}

void JitTranslator::MergeLogicalFlagsNZ() {
    u64 clear_nzcv = ~static_cast<u64>(HostFlags::NZCV);
    __ Mrs(ip, NZCV);
    __ And(flags, flags, ForceCast<s64>(clear_nzcv));
    __ And(ip, ip, static_cast<u32>(HostFlags::NZ));
    __ Orr(flags, flags, ip);
    nzcv_dirty = false;
}

void JitTranslator::SaveLogicalResultFlags(Register& result,
                                           ir::ValueType type,
                                           const PseudoFlags& pseudo) {
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Sxtb(ip, result.W());
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Sxth(ip, result.W());
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Sxtw(ip, result.W());
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Mov(ip, result);
            break;
        default:
            PANIC();
    }
    __ Bics(ip, ip, 0);
    MergeLogicalFlagsNZ();
    if (True(pseudo.set & ir::Flags::Parity)) {
        SaveParity(result);
    }
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
    if (True(guest & ir::Flags::Parity)) {
        // Clear Parity: an odd-parity byte makes TestParityFlag read PF = 0.
        __ Mov(ip, 1);
        __ Bfi(flags, ip, HostFlagsBit::ParityByte, 8);
    }
    if (True(guest & ir::Flags::AuxiliaryCarry)) {
        // AF is a single bit (carry into bit 4).
        __ Bfc(flags, HostFlagsBit::AuxiliaryCarry, 1);
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

void JitTranslator::SaveAuxiliaryCarry(Register &left, const Operand &right, Register &result) {
    // AF = carry into bit 4 = bit4(left) ^ bit4(right) ^ bit4(result). This holds
    // for add/adc/sub/sbb alike (result already reflects any carry-in). Only the
    // three bit-4s matter, so fold the whole values together and extract bit 4.
    auto tmp = context.GetTmpX();
    __ Eor(tmp, left.X(), Operand{result.X()});
    if (right.IsImmediate()) {
        if ((right.GetImmediate() >> 4) & 1) {
            __ Eor(tmp, tmp, 1u << 4);
        }
    } else {
        // Plain or shifted register: materialize its effective bits (.X view keeps
        // bit 4 correct for the W-width shift forms too); only bit 4 survives.
        auto reg = right.GetRegister().X();
        auto shift = right.IsShiftedRegister() ? right.GetShift() : LSL;
        auto amount = right.IsShiftedRegister() ? right.GetShiftAmount() : 0;
        __ Eor(tmp, tmp, Operand{reg, shift, amount});
    }
    __ Ubfx(tmp, tmp, 4, 1);
    __ Bfi(flags, tmp, HostFlagsBit::AuxiliaryCarry, 1);
}

void JitTranslator::EmitAdvancePC(ir::Inst* inst) {
    MergeNZCV();
    FlushFlags();
}

void JitTranslator::GetParityFlag(const Register& result) {
    __ Ubfx(result.W(), flags, HostFlagsBit::ParityByte, 8);
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 4});
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 2});
    __ Eor(result.W(), result.W(), Operand{result.W(), LSR, 1});
}

void JitTranslator::TestParityFlag(const Register& result) {
    GetParityFlag(result);
    __ And(result.W(), result.W(), 1);
    // x86 PF is set on even parity
    __ Eor(result.W(), result.W(), 1);
}

void JitTranslator::TestAuxiliaryCarry(const Register& result) {
    // AF is stored as a single bit (the carry into bit 4) at AuxiliaryCarry.
    __ Ubfx(result, flags, HostFlagsBit::AuxiliaryCarry, 1);
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
    // Multiple SaveFlags may appear in one flush window (e.g. the x86 frontend
    // emits separate PF/AF and NZCV saves for narrow ALU ops); merge them.
    flags_set |= inst->GetArg<ir::Flags>(1);
}

void JitTranslator::EmitClearFlags(ir::Inst* inst) {
    // See EmitSaveFlags: merge instead of asserting on a pending window.
    flags_clear |= inst->GetArg<ir::Flags>(0);
}

void JitTranslator::EmitSetCarry(ir::Inst* inst) {
    // Set guest CF directly in the flags register from a computed 0/1 value.
    // Merge pending NZCV first so no later merge clobbers the bit we write
    // (MergeNZCV leaves nzcv_dirty=false; Bfi does not set it).
    MergeNZCV();
    auto bit = context.R(inst->GetArg<ir::Value>(0));
    __ Bfi(flags, bit.X(), HostFlagsBit::C, 1);
}

void JitTranslator::EmitSetOverflow(ir::Inst* inst) {
    MergeNZCV();
    auto bit = context.R(inst->GetArg<ir::Value>(0));
    __ Bfi(flags, bit.X(), HostFlagsBit::V, 1);
}

void JitTranslator::FlushFlags() {
    if (flags_clear != ir::Flags::None) {
        ClearFlags(flags_clear);
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
    auto value_type = inst->ReturnType() == ir::ValueType::VOID ? uni.GetType() : inst->ReturnType();
    VisitVariant<void>(reg, [this, value_type, offset] (auto x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Register>) {
            switch (GetValueSizeByte(value_type)) {
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
            switch (GetValueSizeByte(value_type)) {
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
    auto value_type = inst->GetArg<ir::Value>(1).Type();
    VisitVariant<void>(reg, [this, value_type, offset] (auto x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Register>) {
            switch (GetValueSizeByte(value_type)) {
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
            switch (GetValueSizeByte(value_type)) {
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
    auto value = ir::Value{inst};
    auto type = inst->ReturnType();
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
    auto value = ir::Value{inst};
    auto type = inst->ReturnType();
    // Ldar* has no register-offset form: the pt bias must be folded.
    auto vixl_operand = EmitMemOperand(operand, type, false, true);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldarb(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldarh(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldar(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldar(context.X(value), vixl_operand);
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
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = inst->GetArg<ir::Value>(1);
    auto type = value.Type();
    // Stlr* has no register-offset form: the pt bias must be folded.
    auto vixl_operand = EmitMemOperand(operand, type, false, true);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Stlrb(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Stlrh(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Stlr(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Stlr(context.X(value), vixl_operand);
            break;
        // Vector stores have no release form; fall back to a plain store.
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
                                         bool atomic) {
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
            if (!use_memory_base && addr_value.Def()->GetUses() == 2) {
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

void JitTranslator::EmitAdd(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        MergeNZCV();
        __ Adds(result, left_register, right_operand);
        auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
        SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        if (True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
        if (True(pseudo_flags.set & ir::Flags::AuxiliaryCarry)) {
            SaveAuxiliaryCarry(left_register, right_operand, result);
        }
    } else {
        __ Add(result, left_register, right_operand);
    }
}

void JitTranslator::EmitSub(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        MergeNZCV();
        __ Subs(result, left_register, right_operand);
        auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
        SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        if (True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
        if (True(pseudo_flags.set & ir::Flags::AuxiliaryCarry)) {
            SaveAuxiliaryCarry(left_register, right_operand, result);
        }
    } else {
        __ Sub(result, left_register, right_operand);
    }
}

void JitTranslator::EmitAdc(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    // Bring the guest carry flag into host C.
    if (!(save_in_nzcv && nzcv_dirty)) {
        LoadNZCVFromFlags();
    }

    if (!pseudo_flags.Null()) {
        __ Adcs(result, left_register, right_operand);
        auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
        SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        if (True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
        if (True(pseudo_flags.set & ir::Flags::AuxiliaryCarry)) {
            SaveAuxiliaryCarry(left_register, right_operand, result);
        }
    } else {
        __ Adc(result, left_register, right_operand);
    }
}

void JitTranslator::EmitSbb(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    // The carry is stored with host (ARM) semantics, so SBC matches the guest borrow.
    if (!(save_in_nzcv && nzcv_dirty)) {
        LoadNZCVFromFlags();
    }

    if (!pseudo_flags.Null()) {
        __ Sbcs(result, left_register, right_operand);
        auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
        SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        if (True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
        if (True(pseudo_flags.set & ir::Flags::AuxiliaryCarry)) {
            SaveAuxiliaryCarry(left_register, right_operand, result);
        }
    } else {
        __ Sbc(result, left_register, right_operand);
    }
}

void JitTranslator::EmitAnd(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        MergeNZCV();
        // x86 logical ops: N/Z from the result, C/V cleared.
        __ Ands(result, left_register, right_operand);
        MergeLogicalFlagsNZ();
        if (True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
    } else {
        __ And(result, left_register, right_operand);
    }
}

void JitTranslator::EmitAddPhi(ir::Inst* inst) {}

void JitTranslator::EmitAndNot(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        MergeNZCV();
        __ Bics(result, left_register, right_operand);
        MergeLogicalFlagsNZ();
        if (True(pseudo_flags.set & ir::Flags::Parity)) {
            SaveParity(result);
        }
    } else {
        __ Bic(result, left_register, right_operand);
    }
}

void JitTranslator::EmitOr(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        MergeNZCV();
    }
    __ Orr(result, left_register, right_operand);
    if (!pseudo_flags.Null()) {
        SaveLogicalResultFlags(result, left.Type(), pseudo_flags);
    }
}

void JitTranslator::EmitXor(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        MergeNZCV();
    }
    __ Eor(result, left_register, right_operand);
    if (!pseudo_flags.Null()) {
        SaveLogicalResultFlags(result, left.Type(), pseudo_flags);
    }
}

void JitTranslator::EmitNot(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.R(ir::Value{inst});
    if (inst->ArgAt(1).IsVoid()) {
        // Unary form: logical not (used for zero checks), result is 0/1.
        MergeNZCV();
        __ Cmp(context.R(value), 0);
        __ Cset(result.W(), eq);
    } else {
        auto right = inst->GetArg<ir::Operand>(1);
        __ Mvn(result, EmitOperand(right));
    }
}

void JitTranslator::EmitAsrImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto asr = inst->GetArg<ir::Imm>(1).Get();
    auto result = context.R(ir::Value{inst});
    __ Asr(result, context.R(value), asr);
}

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
    __ Lsr(result, context.R(value), lsr);
}

void JitTranslator::EmitPopRSB(ir::Inst* inst) {
    // TODO: ReturnStackBuffer support; safe to ignore when the optimization is off.
}

void JitTranslator::EmitRorImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto ror = inst->GetArg<ir::Imm>(1).Get();
    auto result = context.R(ir::Value{inst});
    __ Ror(result, context.R(value), ror);
}

void JitTranslator::EmitVec4Or(ir::Inst* inst) {}

void JitTranslator::EmitBitCast(ir::Inst* inst) {
    // Ignore
}

void JitTranslator::EmitLoadImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Imm>(0);
    auto result = context.R(ir::Value{inst});
    __ Mov(result, value.Get());
}

void JitTranslator::EmitNotGoto(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Value>(0);
    __ Cbz(context.W(cond), GetLocalLabel(inst));
}

void JitTranslator::EmitGoto(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Value>(0);
    __ Cbnz(context.W(cond), GetLocalLabel(inst));
}

void JitTranslator::EmitBindLabel(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    __ Bind(GetLocalLabel(value.Def()));
}

void JitTranslator::EmitPushRSB(ir::Inst* inst) {
    // TODO: ReturnStackBuffer support; safe to ignore when the optimization is off.
}

void JitTranslator::EmitTestBit(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto bit = inst->GetArg<ir::Imm>(1).Get();
    __ Ubfx(context.W(ir::Value{inst}), context.W(value), bit, 1);
}

void JitTranslator::EmitVec4Add(ir::Inst* inst) {}

void JitTranslator::EmitVec4And(ir::Inst* inst) {}

void JitTranslator::EmitVec4Mul(ir::Inst* inst) {}

void JitTranslator::EmitVec4Sub(ir::Inst* inst) {}

void JitTranslator::EmitAsrValue(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto amount = inst->GetArg<ir::Value>(1);
    auto result = context.R(ir::Value{inst});
    if (result.Is64Bits()) {
        __ Asr(result, context.X(value), context.X(amount));
    } else {
        __ Asr(result, context.W(value), context.W(amount));
    }
}

void JitTranslator::EmitBitClear(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto lsb = inst->GetArg<ir::Imm>(1).Get();
    auto bits = inst->GetArg<ir::Imm>(2).Get();
    auto value_reg = context.R(value);
    auto result = context.R(ir::Value{inst});
    if (value_reg != result) {
        __ Mov(result, value_reg);
    }
    __ Bfc(result, lsb, bits);
}

void JitTranslator::EmitGetFlags(ir::Inst* inst) {
    MergeNZCV();
    __ Mov(context.R(ir::Value{inst}), flags);
}

void JitTranslator::EmitLslValue(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto amount = inst->GetArg<ir::Value>(1);
    auto result = context.R(ir::Value{inst});
    if (result.Is64Bits()) {
        __ Lsl(result, context.X(value), context.X(amount));
    } else {
        __ Lsl(result, context.W(value), context.W(amount));
    }
}

void JitTranslator::EmitLsrValue(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto amount = inst->GetArg<ir::Value>(1);
    auto result = context.R(ir::Value{inst});
    if (result.Is64Bits()) {
        __ Lsr(result, context.X(value), context.X(amount));
    } else {
        __ Lsr(result, context.W(value), context.W(amount));
    }
}

void JitTranslator::EmitRorValue(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto amount = inst->GetArg<ir::Value>(1);
    auto result = context.R(ir::Value{inst});
    if (result.Is64Bits()) {
        __ Ror(result, context.X(value), context.X(amount));
    } else {
        __ Ror(result, context.W(value), context.W(amount));
    }
}

void JitTranslator::EmitTestZero(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.W(ir::Value{inst});
    MergeNZCV();
    __ Cmp(context.R(value), 0);
    __ Cset(result, eq);
}

void JitTranslator::EmitBitInsert(ir::Inst* inst) {
    auto dest = inst->GetArg<ir::Value>(0);
    auto src = inst->GetArg<ir::Value>(1);
    auto lsb = inst->GetArg<ir::Imm>(2).Get();
    auto bits = inst->GetArg<ir::Imm>(3).Get();
    auto result = context.R(ir::Value{inst});
    auto dest_reg = context.R(dest);
    if (result != dest_reg) {
        __ Mov(result, dest_reg);
    }
    __ Bfi(result, context.R(src), lsb, bits);
}

void JitTranslator::EmitTestFlags(ir::Inst* inst) {
    auto test = inst->GetArg<ir::Flags>(0);
    auto result = context.W(ir::Value{inst});
    auto nzcv_mask = static_cast<u32>(GuestNZCVToHost(test));
    bool first{true};
    if (nzcv_mask) {
        if (save_in_nzcv && nzcv_dirty) {
            __ Mrs(ip, NZCV);
            __ Tst(ip, nzcv_mask);
        } else {
            __ Tst(flags, nzcv_mask);
        }
        __ Cset(result, ne);
        first = false;
    }
    if (True(test & ir::Flags::Parity)) {
        TestParityFlag(ip);
        if (first) {
            __ Mov(result, ip.W());
        } else {
            __ And(result, result, ip.W());
        }
        first = false;
    }
    if (True(test & ir::Flags::AuxiliaryCarry)) {
        TestAuxiliaryCarry(ip);
        if (first) {
            __ Mov(result, ip.W());
        } else {
            __ And(result, result, ip.W());
        }
        first = false;
    }
    if (first) {
        __ Mov(result, 0);
    }
}

void JitTranslator::EmitBitExtract(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto left = inst->GetArg<ir::Imm>(1).Get();
    auto bits = inst->GetArg<ir::Imm>(2).Get();
    auto result = context.R(ir::Value{inst});
    __ Ubfx(result, context.R(value), left, bits);
}

void JitTranslator::EmitHostCall(const ir::Lambda& lambda,
                                 const std::vector<ir::DataClass>& args,
                                 bool has_result,
                                 const Register& result) {
    ASSERT(args.size() <= 8);
    MergeNZCV();
    FlushFlags();

    // Save all potentially allocated caller-saved GPRs (x0-x10, x12-x17) plus
    // x29/x30: the Blr below clobbers the link register holding this block's
    // return address back to the dispatcher.
    // ip (x11) is reserved scratch; x18 is reserved on Apple; x19+ are callee-saved.
    constexpr u32 kSaveBytes = 16 * 10;
    auto saved_offset = [](u32 code) -> u32 {
        if (code <= 10) {
            return code * 8;
        }
        switch (code) {
            case 12:
                return 88;
            case 13:
                return 96;
            case 14:
                return 104;
            case 15:
                return 112;
            case 16:
                return 120;
            case 17:
                return 128;
            default:
                PANIC();
        }
    };
    constexpr u32 kResultSlot = 136;

    __ Sub(sp, sp, kSaveBytes);
    __ Stp(x0, x1, MemOperand(sp, 0));
    __ Stp(x2, x3, MemOperand(sp, 16));
    __ Stp(x4, x5, MemOperand(sp, 32));
    __ Stp(x6, x7, MemOperand(sp, 48));
    __ Stp(x8, x9, MemOperand(sp, 64));
    __ Stp(x10, x12, MemOperand(sp, 80));
    __ Stp(x13, x14, MemOperand(sp, 96));
    __ Stp(x15, x16, MemOperand(sp, 112));
    __ Str(x17, MemOperand(sp, 128));
    __ Stp(x29, x30, MemOperand(sp, 144));

    // Load arguments into x0-x7.
    u32 index{0};
    for (auto& data : args) {
        auto dst = XRegister(index++);
        if (data.IsImm()) {
            __ Mov(dst, data.imm.Get());
        } else {
            auto src = context.X(data.value);
            if (src.GetCode() <= 17) {
                __ Ldr(dst, MemOperand(sp, saved_offset(src.GetCode())));
            } else {
                __ Mov(dst, src);
            }
        }
    }

    // Function address.
    if (lambda.IsValue()) {
        auto fn = context.X(lambda.GetValue());
        if (fn.GetCode() <= 17) {
            __ Ldr(ip, MemOperand(sp, saved_offset(fn.GetCode())));
        } else {
            __ Mov(ip, fn);
        }
    } else {
        __ Mov(ip, lambda.GetImm().Get());
    }
    __ Blr(ip);

    __ Str(x0, MemOperand(sp, kResultSlot));

    __ Ldp(x0, x1, MemOperand(sp, 0));
    __ Ldp(x2, x3, MemOperand(sp, 16));
    __ Ldp(x4, x5, MemOperand(sp, 32));
    __ Ldp(x6, x7, MemOperand(sp, 48));
    __ Ldp(x8, x9, MemOperand(sp, 64));
    __ Ldp(x10, x12, MemOperand(sp, 80));
    __ Ldp(x13, x14, MemOperand(sp, 96));
    __ Ldp(x15, x16, MemOperand(sp, 112));
    __ Ldr(x17, MemOperand(sp, 128));
    __ Ldp(x29, x30, MemOperand(sp, 144));
    if (has_result) {
        __ Ldr(result, MemOperand(sp, kResultSlot));
    }
    __ Add(sp, sp, kSaveBytes);
}

void JitTranslator::EmitCallLambda(ir::Inst* inst) {
    auto lambda = inst->GetArg<ir::Lambda>(0);
    std::vector<ir::DataClass> args{};
    for (int i = 1; i < 4; i++) {
        if (inst->ArgAt(i).IsValue()) {
            args.emplace_back(inst->GetArg<ir::Value>(i));
        } else if (inst->ArgAt(i).IsImm()) {
            args.emplace_back(inst->GetArg<ir::Imm>(i));
        }
    }
    auto self = ir::Value{inst};
    auto has_result = context.HasAllocation(self);
    Register result{};
    if (has_result) {
        result = context.R(self);
    }
    EmitHostCall(lambda, args, has_result, result);
}

void JitTranslator::EmitGetOperand(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto result = context.R(ir::Value{inst});
    __ Mov(result, EmitOperand(operand));
}

void JitTranslator::EmitSignExtend(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.R(ir::Value{inst});
    auto src = context.W(value);
    switch (ir::GetValueSizeByte(value.Type())) {
        case 1:
            __ Sxtb(result, src);
            break;
        case 2:
            __ Sxth(result, src);
            break;
        case 4:
            if (result.Is64Bits()) {
                __ Sxtw(result, src);
            } else {
                __ Mov(result, src);
            }
            break;
        case 8:
            __ Mov(result, context.X(value));
            break;
        default:
            PANIC();
    }
}

void JitTranslator::EmitCallDynamic(ir::Inst* inst) {
    auto lambda = inst->GetArg<ir::Lambda>(0);
    auto params = inst->GetArg<ir::Params>(1);
    std::vector<ir::DataClass> args{};
    for (auto& param : params) {
        args.emplace_back(param.data);
    }
    auto self = ir::Value{inst};
    auto has_result = context.HasAllocation(self);
    Register result{};
    if (has_result) {
        result = context.R(self);
    }
    EmitHostCall(lambda, args, has_result, result);
}

void JitTranslator::EmitDefineLocal(ir::Inst* inst) {}

void JitTranslator::EmitGetLocation(ir::Inst* inst) {
    __ Ldr(context.X(ir::Value{inst}), MemOperand(state, state_offset_current_loc));
}

void JitTranslator::EmitSetLocation(ir::Inst* inst) {
    auto location = inst->GetArg<ir::Lambda>(0);
    if (location.IsValue()) {
        __ Str(context.X(location.GetValue()), MemOperand(state, state_offset_current_loc));
    } else {
        __ Mov(ip, location.GetImm().Get());
        __ Str(ip, MemOperand(state, state_offset_current_loc));
    }
}

void JitTranslator::EmitTestNotZero(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.W(ir::Value{inst});
    MergeNZCV();
    __ Cmp(context.R(value), 0);
    __ Cset(result, ne);
}

void JitTranslator::EmitCallLocation(ir::Inst* inst) {
    // TODO: semantics assumed to be a host C-ABI call with params, same as CallDynamic.
    auto lambda = inst->GetArg<ir::Lambda>(0);
    auto params = inst->GetArg<ir::Params>(1);
    std::vector<ir::DataClass> args{};
    for (auto& param : params) {
        args.emplace_back(param.data);
    }
    auto self = ir::Value{inst};
    auto has_result = context.HasAllocation(self);
    Register result{};
    if (has_result) {
        result = context.R(self);
    }
    EmitHostCall(lambda, args, has_result, result);
}

void JitTranslator::EmitTestNotFlags(ir::Inst* inst) {
    auto test = inst->GetArg<ir::Flags>(0);
    auto nzcv_mask = static_cast<u32>(GuestNZCVToHost(test));
    if (nzcv_mask && !True(test & (ir::Flags::Parity | ir::Flags::AuxiliaryCarry))) {
        auto result = context.W(ir::Value{inst});
        if (save_in_nzcv && nzcv_dirty) {
            __ Mrs(ip, NZCV);
            __ Tst(ip, nzcv_mask);
        } else {
            __ Tst(flags, nzcv_mask);
        }
        __ Cset(result, eq);
    } else {
        EmitTestFlags(inst);
        auto result = context.W(ir::Value{inst});
        __ Eor(result, result, 1);
        __ And(result, result, 1);
    }
}

void JitTranslator::EmitZeroExtend32(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.W(ir::Value{inst});
    auto src = context.W(value);
    switch (ir::GetValueSizeByte(value.Type())) {
        case 1:
            __ Uxtb(result, src);
            break;
        case 2:
            __ Uxth(result, src);
            break;
        default:
            if (result != src) {
                __ Mov(result, src);
            }
            break;
    }
}

void JitTranslator::EmitZeroExtend64(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.X(ir::Value{inst});
    switch (ir::GetValueSizeByte(value.Type())) {
        case 1:
            __ Uxtb(result.W(), context.W(value));
            break;
        case 2:
            __ Uxth(result.W(), context.W(value));
            break;
        case 4:
            __ Mov(result.W(), context.W(value));
            break;
        default:
            if (result != context.X(value)) {
                __ Mov(result, context.X(value));
            }
            break;
    }
}

void JitTranslator::EmitCompareAndSwap(ir::Inst* inst) {
    // Args: (address, expected, desired); returns the old value.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    auto expected = inst->GetArg<ir::Value>(1);
    auto desired = inst->GetArg<ir::Value>(2);
    auto type = expected.Type();
    auto result = context.R(ir::Value{inst});

    MergeNZCV();

    // Exclusive instructions take a base register only (no offset forms), so
    // under guest address virtualization the pt bias must be folded in
    // explicitly (reserved scratch: CAS is VOID-adjacent and GetTmpX cannot
    // be trusted here — see defines.h mem_scratch).
    if (use_memory_base) {
        __ Add(mem_scratch, address, pt);
        address = mem_scratch;
    }

    Label retry;
    Label done;
    __ Bind(&retry);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldaxrb(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldaxrh(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldaxr(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldaxr(result, MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
    __ Cmp(result, context.R(expected, true));
    __ B(&done, ne);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Stlxrb(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Stlxrh(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Stlxr(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Stlxr(ipw, context.X(desired), MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
    __ Cbnz(ipw, &retry);
    __ Bind(&done);
}

void JitTranslator::EmitUniformBarrier(ir::Inst* inst) {
    // Compiler barrier only: uniform caching is not invalidated across this point.
}

void JitTranslator::EmitDiv(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto type = left.Type();
    auto result = context.R(ir::Value{inst});
    const bool is_signed = ir::IsSignValueType(type);
    const auto size = ir::GetValueSizeByte(type);

    Register dividend = context.R(left, true);
    Register divisor = MaterializeOperand(EmitOperand(right), type);

    // NOTE: division by zero follows ARM64 host semantics (result = 0, no trap).
    // x86 guest #DE behaviour is not modelled here.
    if (size <= 2) {
        auto clean_left = context.GetTmpX();
        auto clean_right = context.GetTmpX();
        if (is_signed) {
            if (size == 1) {
                __ Sxtb(clean_left.W(), dividend.W());
                __ Sxtb(clean_right.W(), divisor.W());
            } else {
                __ Sxth(clean_left.W(), dividend.W());
                __ Sxth(clean_right.W(), divisor.W());
            }
        } else {
            if (size == 1) {
                __ Uxtb(clean_left.W(), dividend.W());
                __ Uxtb(clean_right.W(), divisor.W());
            } else {
                __ Uxth(clean_left.W(), dividend.W());
                __ Uxth(clean_right.W(), divisor.W());
            }
        }
        dividend = clean_left.W();
        divisor = clean_right.W();
    }

    if (is_signed) {
        __ Sdiv(result, dividend, divisor);
    } else {
        __ Udiv(result, dividend, divisor);
    }

    auto pseudo_flags = GetPseudoFlags(inst);
    if (!pseudo_flags.Null() && True(pseudo_flags.set & ir::Flags::CV)) {
        MergeNZCV();
        SaveCV(result, type);
    }
}

void JitTranslator::EmitMul(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto type = left.Type();
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);
    auto multiplier = MaterializeOperand(EmitOperand(right), type);

    auto pseudo_flags = GetPseudoFlags(inst);

    const bool is_64 = ir::GetValueSizeByte(type) == 8;
    const bool is_signed = ir::IsSignValueType(type);
    const bool want_cv = True(pseudo_flags.set & ir::Flags::CV);

    if (!pseudo_flags.Null()) {
        MergeNZCV();
    }

    if (want_cv && !is_64) {
        // Widen the multiply so the upper half can be checked for x86 CF/OF.
        auto wide = context.GetTmpX();
        if (is_signed) {
            __ Smull(wide, left_register.W(), multiplier.W());
        } else {
            __ Umull(wide, left_register.W(), multiplier.W());
        }
        __ Mov(result, wide.W());
        if (is_signed) {
            // Overflow when the upper half is not the sign extension of the result.
            Label no_overflow;
            __ Sxtw(ip, wide.W());
            __ Cmp(ip, wide);
            __ B(&no_overflow, eq);
            __ Orr(flags, flags, 3u << HostFlagsBit::V);
            __ Bind(&no_overflow);
        } else {
            SaveCV(wide, type);
        }
    } else {
        __ Mul(result, left_register, multiplier);
    }

    if (!pseudo_flags.Null() && True(pseudo_flags.set & ir::Flags::Parity)) {
        SaveParity(result);
    }
}

void JitTranslator::EmitNop(ir::Inst* inst) { __ Nop(); }

void JitTranslator::EmitSelect(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Value>(0);
    auto true_value = inst->GetArg<ir::Value>(1);
    auto false_value = inst->GetArg<ir::Value>(2);
    auto result = context.R(ir::Value{inst});
    MergeNZCV();
    __ Cmp(context.W(cond), 0);
    __ Csel(result, context.R(true_value), context.R(false_value), ne);
}

void JitTranslator::EmitCondSelect(ir::Inst* inst) {
    auto cond = inst->GetArg<ir::Cond>(0);
    auto true_value = inst->GetArg<ir::Value>(1);
    auto false_value = inst->GetArg<ir::Value>(2);
    auto result = context.R(ir::Value{inst});
    if (!(save_in_nzcv && nzcv_dirty)) {
        LoadNZCVFromFlags();
    }
    __ Csel(result, context.R(true_value), context.R(false_value), MapCond(cond));
}

void JitTranslator::EmitZero(ir::Inst* inst) {
    auto self = ir::Value{inst};
    if (ir::IsFloatValueType(inst->ReturnType())) {
        __ Fmov(context.V(self).D(), 0.0);
    } else {
        __ Mov(context.R(self), 0);
    }
}

void JitTranslator::EmitGetResult(ir::Inst* inst) {
    auto src = inst->GetArg<ir::Value>(0);
    auto self = ir::Value{inst};
    if (!context.HasAllocation(self)) {
        return;
    }
    auto result = context.R(self);
    auto src_reg = context.R(src);
    if (result != src_reg) {
        __ Mov(result, src_reg);
    }
}

#undef masm

}  // namespace swift::runtime::backend::arm64