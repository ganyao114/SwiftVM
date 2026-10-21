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

namespace {

ir::Value ResolveWidthChainBitCast(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<ir::Value>(0);
    }
    return value;
}




}  // namespace

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
    ASSERT(gpr_plan->consumers);
    const auto& action = gpr_plan->consumers->booleans.at(inst);
    if (action.logical_user && local_conditions.contains(action.logical_user)) return;
    if (flag_state.save_in_nzcv && flag_state.nzcv_dirty &&
        !action.later_local_control) {
        EmitZeroTestPreservingPstate(inst, false);
        return;
    }
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





void JitTranslator::EmitBitExtract(ir::Inst* inst) {
    if (auto fused = NarrowBits().masked_extracts.find(inst);
        fused != NarrowBits().masked_extracts.end()) {
        const auto candidate = NarrowBits().masks.find(fused->second);
        ASSERT(candidate != NarrowBits().masks.end() && candidate->second.extract == inst);
        return;
    }
    if (auto value = gpr_plan->memory_values.find(inst);
        value != gpr_plan->memory_values.end()) {
        return;
    }
    if (auto fused = NarrowBits().extracts.find(inst);
        fused != NarrowBits().extracts.end()) {
        const auto candidate = NarrowBits().extensions.find(fused->second);
        ASSERT(candidate != NarrowBits().extensions.end() && candidate->second.extract == inst);
        return;
    }
    if (HasGPRLowView(inst)) {
        return;
    }
    if (IntegerFlagPreparation().inputs.contains(inst)) {
        return;
    }
    if (ScalarPreparation().copies.InputDiscarded(inst)) {
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
        ASSERT_MSG(context.GetAllocation().WidthElisionVerified(inst->Id()),
                   "width-chain BitExtract proof drifted before emission at IR {}",
                   inst->Id());
        return;
    }
    if (context.IsLow32CopyCoalesced(inst->Id())) {
        ASSERT_MSG(context.GetAllocation().Low32ElisionVerified(inst->Id()),
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

void JitTranslator::EmitSignExtend(ir::Inst* inst) {
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
    const u32 source_width = ir::GetValueSizeByte(value.Type());
    const u32 source_bits = source_width * 8;
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
            __ Mov(result, context.X(value));
            break;
        default:
            PANIC();
    }
}

bool JitTranslator::CanFusePinnedZeroExtendPublication(ir::Inst* inst) const {
    return inst && IsGPRZeroExtendElided(inst);
}



bool JitTranslator::CanConsumeForwardedWidthSpill(ir::Inst* inst) {
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
            return !CanUseZeroStoreRegister(ir::Value{inst}) &&
                   !reads_fixed_home() && !IsGPRZeroExtendElided(inst) &&
                   !NarrowBits().extensions.contains(inst);
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

void JitTranslator::EmitTestNotZero(ir::Inst* inst) {
    ASSERT(gpr_plan->consumers);
    const auto& action = gpr_plan->consumers->booleans.at(inst);
    if (action.logical_user && local_conditions.contains(action.logical_user)) return;
    if (flag_state.save_in_nzcv && flag_state.nzcv_dirty &&
        !action.later_local_control) {
        EmitZeroTestPreservingPstate(inst, true);
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.W(ir::Value{inst});
    MergeNZCV();
    __ Cmp(context.R(value), 0);
    __ Cset(result, ne);
}

void JitTranslator::EmitZeroTestPreservingPstate(ir::Inst* inst, bool nonzero) {
    const auto value = context.R(inst->GetArg<ir::Value>(0));
    const auto result = context.X(ir::Value{inst});
    const auto scratch = context.GetSharedTmpX();
    if (value.Is64Bits()) {
        __ Clz(scratch, value);
        __ Lsr(result, scratch, 6);
    } else {
        __ Clz(scratch.W(), value.W());
        __ Lsr(result.W(), scratch.W(), 5);
    }
    if (nonzero) {
        __ Eor(result.W(), result.W(), 1);
    }
}

void JitTranslator::EmitZeroExtend32(ir::Inst* inst) {
    if (CanUseZeroStoreRegister(ir::Value{inst})) {
        return;
    }
    if (IsGPRZeroExtendElided(inst) ||
        NarrowBits().tied_load_extensions.contains(inst)) {
        return;
    }
    if (auto fused = NarrowBits().extensions.find(inst);
        fused != NarrowBits().extensions.end()) {
        if (fused->second.shift) {
            return;
        }
        auto result = context.W(ir::Value{inst});
        auto source = context.W(fused->second.source);
        if (fused->second.source_high_zero && result == source) {
            return;
        }
        if (fused->second.width == sizeof(u8)) {
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
    const u32 source_bits = ir::GetValueSizeByte(value.Type()) * 8;
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

void JitTranslator::EmitZeroExtend32To64(ir::Inst* inst) {
    const bool no_elide = GetSvmConfig().zext_no_elide;
    if (!no_elide && CanUseZeroStoreRegister(ir::Value{inst})) {
        return;
    }
    if (!no_elide && (IsGPRZeroExtendElided(inst) ||
                       NarrowBits().tied_load_extensions.contains(inst))) {
        return;
    }
    auto source = inst->GetArg<ir::Value>(0);
    if (!no_elide && context.IsWidthChainCoalesced(inst->Id())) {
        ASSERT_MSG(context.GetAllocation().WidthElisionVerified(inst->Id()),
                   "width-chain ZeroExtend32To64 proof drifted before emission at IR {}",
                   inst->Id());
        return;
    }
    if (!no_elide && source.Def() &&
        context.IsLow32CopyCoalesced(source.Id())) {
        ASSERT_MSG(context.GetAllocation().Low32ElisionVerified(source.Id()),
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


#undef __

}  // namespace swift::runtime::backend::arm64
