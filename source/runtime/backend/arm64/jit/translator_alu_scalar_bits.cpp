#include "translator.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/context.h"
#include "runtime/ir/low32_copy.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::EmitLslImm(const ir::Inst* inst) { EmitIntegerShift(inst); }
void JitTranslator::EmitLslValue(const ir::Inst* inst) { EmitIntegerShift(inst); }
void JitTranslator::EmitLsrImm(const ir::Inst* inst) { EmitIntegerShift(inst); }
void JitTranslator::EmitLsrValue(const ir::Inst* inst) { EmitIntegerShift(inst); }
void JitTranslator::EmitAsrImm(const ir::Inst* inst) { EmitIntegerShift(inst); }
void JitTranslator::EmitAsrValue(const ir::Inst* inst) { EmitIntegerShift(inst); }
void JitTranslator::EmitRorImm(const ir::Inst* inst) { EmitIntegerShift(inst); }
void JitTranslator::EmitRorValue(const ir::Inst* inst) { EmitIntegerShift(inst); }

void JitTranslator::EmitIntegerShift(const ir::Inst* inst) {
    const auto op = inst->GetOp();
    if ((op == ir::OpCode::LsrImm || op == ir::OpCode::AsrImm) &&
        context.GetActions().HasScalar(inst->Id(), ScalarFlag::BitInputCovered)) return;
    if (op == ir::OpCode::LsrImm) {
        if (const auto* fused = context.GetActions().Narrow(inst->Id())) {
            ASSERT(fused->kind == NarrowAction::ExtractShift);
            const u32 amount = inst->GetArg<ir::Imm>(1).Get();
            __ Ubfx(context.W(ir::Value{inst}), context.W(fused->Source(*inst)),
                    amount, fused->Width() * 8 - amount);
            return;
        }
    }
    const auto kind = ir::ClassifyIntegerShift(op);
    const auto value = inst->GetArg<ir::Value>(0);
    const u32 bits = ir::GetScalarIntegerBits(inst->ReturnType());
    const u32 input_bits = ir::IntegerShiftInputBits(ir::GetScalarIntegerBits(value.Type()), bits);
    const auto result = context.R(ir::Value{inst});
    auto source = bits == 64 ? Register{context.X(value)} : Register{context.W(value)};
    const bool stage = kind == ir::IntegerShiftKind::Left ? input_bits < bits
                                                       : input_bits < (result.Is64Bits() ? 64u : 32u);
    const bool immediate = inst->ArgAt(1).IsImm();
    if (immediate && stage && (kind == ir::IntegerShiftKind::Right ||
                               kind == ir::IntegerShiftKind::ArithmeticRight)) {
        const auto amount = ir::NormalizeIntegerShiftCount(inst->GetArg<ir::Imm>(1).Get(), bits);
        if (kind == ir::IntegerShiftKind::Right) {
            if (amount >= input_bits) __ Mov(result, 0);
            else __ Ubfx(result, source, amount, input_bits - amount);
        } else {
            const auto position = std::min(amount, input_bits - 1);
            __ Sbfx(result, source, position, input_bits - position);
        }
        return;
    }
    Register count;
    if (!immediate) {
        const auto amount = inst->GetArg<ir::Value>(1);
        count = bits == 64 ? Register{context.X(amount)} : Register{context.W(amount)};
        const bool mask_count = bits < 32 && kind != ir::IntegerShiftKind::RotateRight;
        if (mask_count || (stage && result.GetCode() == count.GetCode())) {
            const auto temporary = context.GetTmpX();
            const auto saved = bits == 64 ? Register{temporary} : Register{temporary.W()};
            if (mask_count) __ And(saved, count, bits - 1);
            else __ Mov(saved, count);
            count = saved;
        }
    }
    if (stage) {
        if (kind == ir::IntegerShiftKind::ArithmeticRight)
            __ Sbfx(result, source, 0, input_bits);
        else __ Ubfx(result, source, 0, input_bits);
        source = result;
    }
    if (kind == ir::IntegerShiftKind::RotateRight && bits < 32) {
        // Repeating the narrow pattern makes the W rotate periodic in the IR
        // width, including unmasked variable counts, without a second temp.
        if (bits == 8) __ Orr(result, source, Operand(source, LSL, 8));
        __ Orr(result, result, Operand(result, LSL, 16));
        source = result;
    }
    if (immediate) {
        const auto amount = ir::NormalizeIntegerShiftCount(inst->GetArg<ir::Imm>(1).Get(), bits);
        switch (kind) {
            case ir::IntegerShiftKind::Left: __ Lsl(result, source, amount); break;
            case ir::IntegerShiftKind::Right: __ Lsr(result, source, amount); break;
            case ir::IntegerShiftKind::ArithmeticRight: __ Asr(result, source, amount); break;
            case ir::IntegerShiftKind::RotateRight: __ Ror(result, source, amount); break;
            case ir::IntegerShiftKind::None: break;
        }
    } else {
        switch (kind) {
            case ir::IntegerShiftKind::Left: __ Lsl(result, source, count); break;
            case ir::IntegerShiftKind::Right: __ Lsr(result, source, count); break;
            case ir::IntegerShiftKind::ArithmeticRight: __ Asr(result, source, count); break;
            case ir::IntegerShiftKind::RotateRight: __ Ror(result, source, count); break;
            case ir::IntegerShiftKind::None: break;
        }
    }
}

void JitTranslator::EmitBitClear(const ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto lsb = inst->GetArg<ir::Imm>(1).Get();
    auto bits = inst->GetArg<ir::Imm>(2).Get();
    if (!bits) {
        const auto result = context.R(ir::Value{inst});
        __ Mov(result, result.Is64Bits() ? Register{context.X(value)} : Register{context.W(value)});
        return;
    }
    auto value_reg = context.R(value);
    auto result = context.R(ir::Value{inst});
    if (value_reg != result) {
        __ Mov(result, value_reg);
    }
    __ Bfc(result, lsb, bits);
}

void JitTranslator::CompareIntegerZero(ir::Value value) {
    const auto source = context.R(value);
    const u32 bits = ir::GetScalarIntegerBits(value.Type());
    if (bits < 32 && !value.Def()->HasBooleanResult())
        __ Tst(source.W(), (u64{1} << bits) - 1);
    else __ Cmp(source, 0);
}

Register JitTranslator::ConditionRegister(ir::Value value) {
    const auto source = context.R(value);
    const u32 bits = ir::GetScalarIntegerBits(value.Type());
    if (bits >= 32 || value.Def()->HasBooleanResult()) return source;
    const auto temporary = context.GetSharedTmpX().W();
    __ Ubfx(temporary, source.W(), 0, bits);
    return temporary;
}

void JitTranslator::EmitTestZero(const ir::Inst* inst) { EmitIntegerPredicate(inst); }
void JitTranslator::EmitTestNotZero(const ir::Inst* inst) { EmitIntegerPredicate(inst); }
void JitTranslator::EmitTestBit(const ir::Inst* inst) { EmitIntegerPredicate(inst); }

void JitTranslator::EmitNot(const ir::Inst* inst) {
    if (inst->GetIntegerPredicateKind() != ir::IntegerPredicateKind::None)
        EmitIntegerPredicate(inst);
    else {
        auto operand = inst->GetArg<ir::Operand>(1);
        __ Mvn(context.R(ir::Value{inst}), EmitOperand(operand));
    }
}

void JitTranslator::EmitIntegerPredicate(const ir::Inst* inst) {
    const auto kind = inst->GetIntegerPredicateKind();
    const auto input = inst->GetArg<ir::Value>(0);
    if (kind == ir::IntegerPredicateKind::Bit) {
        const u64 bit = inst->GetArg<ir::Imm>(1).Get();
        const u32 bits = ir::GetScalarIntegerBits(input.Type());
        if (bit >= bits) __ Mov(context.W(ir::Value{inst}), 0);
        else if (bits == 64) __ Ubfx(context.X(ir::Value{inst}), context.X(input), bit, 1);
        else __ Ubfx(context.W(ir::Value{inst}), context.W(input), bit, 1);
        return;
    }
    const auto* action = context.GetActions().Consumer(inst->Id());
    if (inst->GetOp() != ir::OpCode::Not)
        ASSERT(action && action->Has(ConsumerFlag::Boolean));
    if (action && action->Has(ConsumerFlag::Boolean) && action->related != UINT16_MAX &&
        local_conditions.contains(action->related)) return;
    const bool nonzero = kind == ir::IntegerPredicateKind::Nonzero;
    if (flag_state.save_in_nzcv && flag_state.nzcv_dirty &&
        !(action && action->Has(ConsumerFlag::LaterControl))) {
        EmitZeroTestPreservingPstate(inst, nonzero);
        return;
    }
    MergeNZCV();
    CompareIntegerZero(input);
    __ Cset(context.W(ir::Value{inst}), nonzero ? ne : eq);
}

void JitTranslator::EmitBitInsert(const ir::Inst* inst) {
    auto dest = inst->GetArg<ir::Value>(0);
    auto src = inst->GetArg<ir::Value>(1);
    auto lsb = inst->GetArg<ir::Imm>(2).Get();
    auto bits = inst->GetArg<ir::Imm>(3).Get();
    if (!bits) {
        const auto result = context.R(ir::Value{inst});
        __ Mov(result, result.Is64Bits() ? Register{context.X(dest)} : Register{context.W(dest)});
        return;
    }
    auto result = context.R(ir::Value{inst});
    auto dest_reg = context.R(dest);
    if (result != dest_reg) {
        __ Mov(result, dest_reg);
    }
    __ Bfi(result, context.R(src), lsb, bits);
}

void JitTranslator::EmitBitExtract(const ir::Inst* inst) {
    if (!inst->GetArg<ir::Imm>(2).Get()) {
        __ Mov(context.R(ir::Value{inst}), 0);
        return;
    }
    if (context.GetActions().HasScalar(inst->Id(), ScalarFlag::BitInputCovered)) return;
    if (const auto* value = context.GetActions().MemoryTransfer(inst->Id());
        value && value->kind == MemoryTransferAction::Value) {
        return;
    }
    if (HasGPRLowView(inst)) {
        return;
    }
    if (context.GetActions().FlagsInput(inst->Id())) {
        return;
    }
    if (context.GetActions().HasScalar(inst->Id(), ScalarFlag::CopyInputDiscarded)) {
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    auto left = inst->GetArg<ir::Imm>(1).Get();
    auto bits = inst->GetArg<ir::Imm>(2).Get();
    auto result = [&]() -> Register {
        if (auto pinned = context.PlannedGPR<XRegister>(ir::Value{inst}, cur_instr)) {
            return *pinned;
        }
        return context.R(ir::Value{inst});
    }();
    if (context.IsWidthChainCoalesced(inst->Id())) {
        ASSERT_MSG(context.GetActions().WidthElisionVerified(inst->Id()),
                   "width-chain BitExtract proof drifted before emission at IR {}",
                   inst->Id());
        return;
    }
    if (context.IsLow32CopyCoalesced(inst->Id())) {
        ASSERT_MSG(context.GetActions().Low32ElisionVerified(inst->Id()),
                   "low32 copy proof drifted before emission at IR {}",
                   inst->Id());
        return;
    }
    if (shift_imm_fast && left == 0 &&
        bits == ir::GetValueSizeByte(inst->ReturnType()) * 8 &&
        context.SharesGPR(value, ir::Value{inst})) {
        return;
    }
    __ Ubfx(result, context.R(value), left, bits);
}

void JitTranslator::EmitSignExtend(const ir::Inst* inst) {
    if (IsGPRSignExtendElided(inst)) {
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    const auto pinned = context.PlannedGPR<XRegister>(ir::Value{inst}, cur_instr);
    auto result = pinned ? Register{*pinned} : context.R(ir::Value{inst});
    const auto residence = FixedGPRHomeForUse(value, inst);
    const auto fused = GPRLowViewHome(value.Def());
    auto src = residence
            ? WRegister(residence->home)
            : fused
            ? WRegister(*fused)
            : context.W(value);
    const u32 source_bits = ir::IntegerConversionInputBits(inst->GetOp(), ir::GetScalarIntegerBits(value.Type()));
    const u32 source_width = source_bits / 8;
    const u32 result_bits = result.Is64Bits() ? 64 : 32;
    if (residence &&
        residence->extension.KnownSignExtended(source_bits, result_bits)) {
        const auto resident = result.Is64Bits()
                ? Register{XRegister(residence->home)}
                : Register{WRegister(residence->home)};
        if (result != resident) {
            __ Mov(result, resident);
        }
        return;
    }
    switch (source_width) {
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
            if (result.Is64Bits()) __ Mov(result, context.X(value));
            else __ Mov(result, context.W(value));
            break;
        default:
            PANIC();
    }
}

bool JitTranslator::CanFusePinnedZeroExtendPublication(const ir::Inst* inst) const {
    return inst && IsGPRZeroExtendElided(inst);
}

bool JitTranslator::CanConsumeForwardedWidthSpill(const ir::Inst* inst) {
    if (!inst) {
        return false;
    }
    const auto reads_fixed_home = [&] {
        const auto source = inst->GetArg<ir::Value>(0);
        return FixedGPRHomeForUse(source, inst).has_value() ||
               (source.Def() && HasGPRLowView(source.Def()));
    };
    switch (inst->GetOp()) {
        case ir::OpCode::ZeroExtend32:
            return !CanUseZeroStoreRegister(ir::Value{inst}) && !reads_fixed_home() &&
                   !IsGPRZeroExtendElided(inst) &&
                   context.GetActions().Narrow(inst->Id()) == nullptr;
        case ir::OpCode::ZeroExtend32To64: {
            const auto source = inst->GetArg<ir::Value>(0);
            return !CanUseZeroStoreRegister(ir::Value{inst}) &&
                   !reads_fixed_home() && !IsGPRZeroExtendElided(inst) &&
                   !context.IsWidthChainCoalesced(inst->Id()) &&
                   !(source.Def() && context.IsLow32CopyCoalesced(source.Id())) &&
                   !CanFusePinnedZeroExtendPublication(inst);
        }
        case ir::OpCode::ZeroExtend64:
            return true;
        case ir::OpCode::SignExtend:
            return !reads_fixed_home() && !IsGPRSignExtendElided(inst);
        default:
            return false;
    }
}

void JitTranslator::EmitZeroTestPreservingPstate(const ir::Inst* inst, bool nonzero) {
    const auto input = inst->GetArg<ir::Value>(0);
    auto value = context.R(input);
    const u32 bits = ir::GetScalarIntegerBits(input.Type());
    const auto result = context.X(ir::Value{inst});
    if (bits < 32) {
        __ Ubfx(result.W(), value.W(), 0, bits);
        value = result.W();
    }
    if (value.Is64Bits()) {
        __ Clz(result, value);
        __ Lsr(result, result, 6);
    } else {
        __ Clz(result.W(), value.W());
        __ Lsr(result.W(), result.W(), 5);
    }
    if (nonzero) {
        __ Eor(result.W(), result.W(), 1);
    }
}

void JitTranslator::EmitZeroExtend32(const ir::Inst* inst) {
    if (CanUseZeroStoreRegister(ir::Value{inst})) {
        return;
    }
    if (IsGPRZeroExtendElided(inst) ||
        context.GetActions().HasScalar(inst->Id(), ScalarFlag::TiedZeroExtend)) {
        return;
    }
    if (const auto* fused = context.GetActions().Narrow(inst->Id())) {
        ASSERT(fused->kind == NarrowAction::Extend || fused->kind == NarrowAction::DeferredExtend);
        if (fused->kind == NarrowAction::DeferredExtend) {
            return;
        }
        auto result = context.W(ir::Value{inst});
        auto source = context.W(fused->Source(*inst));
        if (fused->SourceHighZero() && result == source) {
            return;
        }
        if (fused->Width() == sizeof(u8)) {
            __ Uxtb(result, source);
        } else {
            __ Uxth(result, source);
        }
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.W(ir::Value{inst});
    const auto residence = FixedGPRHomeForUse(value, inst);
    const auto fused = GPRLowViewHome(value.Def());
    auto src = residence
            ? WRegister(residence->home)
            : fused
            ? WRegister(*fused)
            : context.W(value);
    const u32 source_bits = ir::IntegerConversionInputBits(inst->GetOp(), ir::GetScalarIntegerBits(value.Type()));
    if (residence && residence->extension.KnownZeroAbove(source_bits)) {
        if (result != src) {
            __ Mov(result, src);
        }
        return;
    }
    if (shift_imm_fast && value.Def() &&
        value.Def()->GetOp() == ir::OpCode::LoadUniform &&
        ir::GetValueSizeByte(value.Type()) <= 2 &&
        context.SharesGPR(value, ir::Value{inst})) {
        // LDRB/LDRH already wrote a W register and therefore already provided
        // the zero extension needed by an immediate GPR shift.
        return;
    }
    if (value.Def() && value.Def()->GetOp() == ir::OpCode::BitExtract &&
        context.SharesGPR(value, ir::Value{inst})) {
        auto extracted = value.Def();
        auto source = extracted->GetArg<ir::Value>(0);
        if (extracted->GetArg<ir::Imm>(1).Get() == 0 &&
            extracted->GetArg<ir::Imm>(2).Get() ==
                    ir::GetValueSizeByte(value.Type()) * 8 &&
            source.Def() && source.Def()->GetOp() == ir::OpCode::GetHostGPR &&
            source.Def()->GetArg<ir::Imm>(0).Get() <= 9) {
            // UBFX already zero-filled the tied W destination.
            return;
        }
    }
    switch (source_bits / 8) {
        case 1:
            __ Uxtb(result, src);
            break;
        case 2:
            __ Uxth(result, src);
            break;
        default:
            if (result != src || ir::IsScalarIntegerType(inst->ReturnType(), 64)) {
                __ Mov(result, src);
            }
            break;
    }
}

void JitTranslator::EmitZeroExtend32To64(const ir::Inst* inst) {
    const bool no_elide = GetSvmConfig().zext_no_elide;
    if (!no_elide && CanUseZeroStoreRegister(ir::Value{inst})) {
        return;
    }
    if (!no_elide && (IsGPRZeroExtendElided(inst) ||
                      context.GetActions().HasScalar(inst->Id(), ScalarFlag::TiedZeroExtend))) {
        return;
    }
    auto source = inst->GetArg<ir::Value>(0);
    if (!no_elide && context.IsWidthChainCoalesced(inst->Id())) {
        ASSERT_MSG(context.GetActions().WidthElisionVerified(inst->Id()),
                   "width-chain ZeroExtend32To64 proof drifted before emission at IR {}",
                   inst->Id());
        return;
    }
    if (!no_elide && source.Def() &&
        context.IsLow32CopyCoalesced(source.Id())) {
        ASSERT_MSG(context.GetActions().Low32ElisionVerified(source.Id()),
                   "low32 copy proof drifted at ZeroExtend32To64 IR {}",
                   inst->Id());
        if (context.SharesGPR(source, ir::Value{inst})) {
            // The coalesced BitExtract emitted no UBFX, so its destination
            // can still contain the parent's high bits even when tied here.
            const auto result = context.W(ir::Value{inst});
            __ Mov(result, result);
        } else {
            EmitZeroExtend32(inst);
        }
        return;
    }
    if (!no_elide && ir::GetValueSizeByte(source.Type()) == sizeof(u32) &&
        context.SharesGPR(source, ir::Value{inst})) {
        // Tied to the same register: the W write that clears [63:32] is still
        // owed. The source register may be a pinned-home U32 view whose upper
        // half holds the live guest value, so mov wN,wN is not optional.
        auto result = context.W(ir::Value{inst});
        __ Mov(result, result);
        return;
    }
    if (!no_elide && CanFusePinnedZeroExtendPublication(inst)) {
        return;
    }
    // The destination remains U64-typed in IR so the following StoreUniform
    // updates the full guest GPR. On arm64, writing W is exactly the required
    // 32->64 zero extension and clears the paired X register's high half.
    EmitZeroExtend32(inst);
}

void JitTranslator::EmitZeroExtend64(const ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.X(ir::Value{inst});
    switch (ir::IntegerConversionInputBits(inst->GetOp(), ir::GetScalarIntegerBits(value.Type()))) {
        case 8:
            __ Uxtb(result.W(), context.W(value));
            break;
        case 16:
            __ Uxth(result.W(), context.W(value));
            break;
        case 32:
            __ Mov(result.W(), context.W(value));
            break;
        default:
            if (result != context.X(value)) {
                __ Mov(result, context.X(value));
            }
            break;
    }
}

#undef __

}  // namespace swift::runtime::backend::arm64
