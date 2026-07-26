#include "translator.h"

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::EmitAdd(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);
    auto right_operand = EmitOperand(right);
    auto result = context.R(ir::Value{inst});
    auto left_register = context.R(left, true);

    auto pseudo_flags = GetPseudoFlags(inst);

    if (!pseudo_flags.Null()) {
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv) {
            MergeNZCV();
            __ Adds(result, left_register, right_operand);
            auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        } else {
            // AF/PF only: use non-flag form to avoid clobbering host NZCV.
            __ Add(result, left_register, right_operand);
        }
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
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv) {
            MergeNZCV();
            __ Subs(result, left_register, right_operand);
            auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        } else {
            __ Sub(result, left_register, right_operand);
        }
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
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv) {
            __ Adcs(result, left_register, right_operand);
            auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        } else {
            __ Adc(result, left_register, right_operand);
        }
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
        const bool needs_nzcv = True(pseudo_flags.set & ir::Flags::NZCV);
        if (needs_nzcv) {
            __ Sbcs(result, left_register, right_operand);
            auto guest_nzcv = pseudo_flags.set & ir::Flags::NZCV;
            SaveHostFlags(GuestNZCVToHost(guest_nzcv), guest_nzcv);
        } else {
            __ Sbc(result, left_register, right_operand);
        }
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
        MergeLogicalFlagsNZ(pseudo_flags.set);
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
        MergeLogicalFlagsNZ(pseudo_flags.set);
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

void JitTranslator::EmitRorImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto ror = inst->GetArg<ir::Imm>(1).Get();
    auto result = context.R(ir::Value{inst});
    __ Ror(result, context.R(value), ror);
}

void JitTranslator::EmitByteSwap(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.R(ir::Value{inst});
    const u32 width = inst->GetArg<ir::Imm>(1).Get();
    switch (width) {
        case 16:
            __ Rev16(result.W(), context.W(value));
            break;
        case 32:
            __ Rev(result.W(), context.W(value));
            break;
        case 64:
            __ Rev(result.X(), context.X(value));
            break;
        default:
            PANIC("invalid byte-swap width {}", width);
    }
}

void JitTranslator::EmitVec4Or(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Orr(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitBitCast(ir::Inst* inst) {
    // Ignore
}

void JitTranslator::EmitLoadImm(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Imm>(0);
    auto result = context.R(ir::Value{inst});
    __ Mov(result, value.Get());
}

void JitTranslator::EmitVec4Add(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Add(result.V4S(), left.V4S(), right.V4S());
}

void JitTranslator::EmitVec4And(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ And(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitVec4Mul(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Mul(result.V4S(), left.V4S(), right.V4S());
}

void JitTranslator::EmitVec4Sub(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Sub(result.V4S(), left.V4S(), right.V4S());
}

void JitTranslator::EmitVecXor(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Eor(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitVecAnd(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ And(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitVecOr(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Orr(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitVecAndNot(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Bic(result.V16B(), left.V16B(), right.V16B());
}

void JitTranslator::EmitVecAdd(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Add(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Add(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            __ Add(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            __ Add(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

void JitTranslator::EmitVecSub(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Sub(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Sub(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            __ Sub(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            __ Sub(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

void JitTranslator::EmitVecCmpEq(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Cmeq(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Cmeq(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            __ Cmeq(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            __ Cmeq(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

void JitTranslator::EmitVecCmpGt(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Cmgt(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Cmgt(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            __ Cmgt(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            __ Cmgt(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

void JitTranslator::EmitVecAvg(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Urhadd(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Urhadd(result.V8H(), left.V8H(), right.V8H());
            break;
        default:
            PANIC("invalid vector average lane width");
    }
}

void JitTranslator::EmitVecMin(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const bool is_signed = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            if (is_signed)
                __ Smin(result.V16B(), left.V16B(), right.V16B());
            else
                __ Umin(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            if (is_signed)
                __ Smin(result.V8H(), left.V8H(), right.V8H());
            else
                __ Umin(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            if (is_signed)
                __ Smin(result.V4S(), left.V4S(), right.V4S());
            else
                __ Umin(result.V4S(), left.V4S(), right.V4S());
            break;
        default:
            PANIC("invalid vector min lane width");
    }
}

void JitTranslator::EmitVecMax(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const bool is_signed = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            if (is_signed)
                __ Smax(result.V16B(), left.V16B(), right.V16B());
            else
                __ Umax(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            if (is_signed)
                __ Smax(result.V8H(), left.V8H(), right.V8H());
            else
                __ Umax(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            if (is_signed)
                __ Smax(result.V4S(), left.V4S(), right.V4S());
            else
                __ Umax(result.V4S(), left.V4S(), right.V4S());
            break;
        default:
            PANIC("invalid vector max lane width");
    }
}

void JitTranslator::EmitVecMul(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Mul(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            __ Mul(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            __ Mul(result.V4S(), left.V4S(), right.V4S());
            break;
        default:
            PANIC("invalid vector multiply lane width");
    }
}

void JitTranslator::EmitVecMulHigh16(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto low = context.GetTmpV();
    auto high = context.GetTmpV();
    const bool signed_lanes = inst->GetArg<ir::Imm>(2).Get() != 0;
    if (signed_lanes) {
        __ Smull(low.V4S(), left.V4H(), right.V4H());
        __ Smull2(high.V4S(), left.V8H(), right.V8H());
    } else {
        __ Umull(low.V4S(), left.V4H(), right.V4H());
        __ Umull2(high.V4S(), left.V8H(), right.V8H());
    }
    // Signedness only changes how the 16-bit operands are widened.  Once the
    // complete 32-bit products exist, both PMULHW and PMULHUW select bits 31:16.
    __ Shrn(result.V4H(), low.V4S(), 16);
    __ Shrn2(result.V8H(), high.V4S(), 16);
}

// Widening multiply of the even lanes: destination lane i is the full
// 2*src_bits product of source lane 2i of each operand.
//
// AArch64's UMULL/SMULL widen the CONTIGUOUS low half of the lanes (UMULL2 the
// high half), which is a different lane selection from this opcode's, so the
// even lanes are compacted first.  UZP1 Vt.4S, Vn.4S, Vn.4S yields
// {n0, n2, n0, n2}; UMULL then reads its low two lanes, which are exactly the
// even lanes wanted.  UMULL2 is deliberately NOT used -- it would pair lanes
// 2 and 3, i.e. one even and one odd lane, and silently produce the wrong
// second result lane.
//
// Three host instructions and two vector temporaries.  `result` is only
// written by the last one, so it may share a physical register with either
// source without the compaction clobbering an operand that is still needed.
void JitTranslator::EmitVecMulWiden(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto even_left = context.GetTmpV();
    auto even_right = context.GetTmpV();
    const bool is_signed = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            __ Uzp1(even_left.V16B(), left.V16B(), left.V16B());
            __ Uzp1(even_right.V16B(), right.V16B(), right.V16B());
            if (is_signed) {
                __ Smull(result.V8H(), even_left.V8B(), even_right.V8B());
            } else {
                __ Umull(result.V8H(), even_left.V8B(), even_right.V8B());
            }
            break;
        case 16:
            __ Uzp1(even_left.V8H(), left.V8H(), left.V8H());
            __ Uzp1(even_right.V8H(), right.V8H(), right.V8H());
            if (is_signed) {
                __ Smull(result.V4S(), even_left.V4H(), even_right.V4H());
            } else {
                __ Umull(result.V4S(), even_left.V4H(), even_right.V4H());
            }
            break;
        case 32:
            __ Uzp1(even_left.V4S(), left.V4S(), left.V4S());
            __ Uzp1(even_right.V4S(), right.V4S(), right.V4S());
            if (is_signed) {
                __ Smull(result.V2D(), even_left.V2S(), even_right.V2S());
            } else {
                __ Umull(result.V2D(), even_left.V2S(), even_right.V2S());
            }
            break;
        default:
            PANIC("invalid widening vector multiply source lane width");
    }
}

void JitTranslator::EmitVecAbsDiffSum8(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto low = context.GetTmpV();
    auto high = context.GetTmpV();
    __ Uabdl(low.V8H(), left.V8B(), right.V8B());
    __ Uabdl2(high.V8H(), left.V16B(), right.V16B());
    __ Addv(low.H(), low.V8H());
    __ Addv(high.H(), high.V8H());
    __ Eor(result.V16B(), result.V16B(), result.V16B());
    __ Ins(result.V8H(), 0, low.V8H(), 0);
    __ Ins(result.V8H(), 4, high.V8H(), 0);
}

void JitTranslator::EmitVecMadd16(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto low = context.GetTmpV();
    auto high = context.GetTmpV();
    __ Smull(low.V4S(), left.V4H(), right.V4H());
    __ Smull2(high.V4S(), left.V8H(), right.V8H());
    __ Addp(result.V4S(), low.V4S(), high.V4S());
}

namespace {

VRegister VecLaneFormat(VRegister reg, u32 lane_bits) {
    switch (lane_bits) {
        case 8:
            return reg.V16B();
        case 16:
            return reg.V8H();
        case 32:
            return reg.V4S();
        case 64:
            return reg.V2D();
        default:
            PANIC("invalid vector shift lane width");
    }
    return reg;
}

}  // namespace

void JitTranslator::EmitVecSatAdd(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool signed_lanes = inst->GetArg<ir::Imm>(3).Get() != 0;
    if (signed_lanes)
        __ Sqadd(VecLaneFormat(result, bits), VecLaneFormat(left, bits), VecLaneFormat(right, bits));
    else
        __ Uqadd(VecLaneFormat(result, bits), VecLaneFormat(left, bits), VecLaneFormat(right, bits));
}

void JitTranslator::EmitVecSatSub(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool signed_lanes = inst->GetArg<ir::Imm>(3).Get() != 0;
    if (signed_lanes)
        __ Sqsub(VecLaneFormat(result, bits), VecLaneFormat(left, bits), VecLaneFormat(right, bits));
    else
        __ Uqsub(VecLaneFormat(result, bits), VecLaneFormat(left, bits), VecLaneFormat(right, bits));
}

void JitTranslator::EmitVecPack(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 source_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool unsigned_destination = inst->GetArg<ir::Imm>(3).Get() != 0;
    if (source_bits == 16) {
        if (unsigned_destination) {
            __ Sqxtun(result.V8B(), left.V8H());
            __ Sqxtun2(result.V16B(), right.V8H());
        } else {
            __ Sqxtn(result.V8B(), left.V8H());
            __ Sqxtn2(result.V16B(), right.V8H());
        }
    } else if (unsigned_destination) {
        // PACKUSDW: signed 32-bit sources saturated into the UNSIGNED 16-bit
        // range, which is exactly SQXTUN. This branch had been missing, so a
        // 32-bit unsigned pack silently took the signed path below and turned
        // 0x8000..0xFFFF into 0x7FFF — and diverged from RunVecPack, which
        // honours the flag at every width. No decoder emits it yet (legacy
        // packusdw is unimplemented), so this was latent rather than live.
        __ Sqxtun(result.V4H(), left.V4S());
        __ Sqxtun2(result.V8H(), right.V4S());
    } else {
        __ Sqxtn(result.V4H(), left.V4S());
        __ Sqxtn2(result.V8H(), right.V4S());
    }
}

void JitTranslator::EmitVecShiftLeft(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto count = context.X(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto clamped = context.GetTmpX();
    auto shifts = context.GetTmpV();
    __ Mov(clamped, lane_bits);
    __ Cmp(count, clamped);
    __ Csel(clamped, count, clamped, ls);
    auto shift_format = VecLaneFormat(shifts, lane_bits);
    __ Dup(shift_format, lane_bits == 64 ? Register(clamped) : Register(clamped.W()));
    __ Ushl(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), shift_format);
}

void JitTranslator::EmitVecShiftRight(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto count = context.X(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto clamped = context.GetTmpX();
    auto shifts = context.GetTmpV();
    __ Mov(clamped, lane_bits);
    __ Cmp(count, clamped);
    __ Csel(clamped, count, clamped, ls);
    __ Neg(clamped, clamped);
    auto shift_format = VecLaneFormat(shifts, lane_bits);
    __ Dup(shift_format, lane_bits == 64 ? Register(clamped) : Register(clamped.W()));
    __ Ushl(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), shift_format);
}

void JitTranslator::EmitVecShiftRightArithmetic(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto count = context.X(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto clamped = context.GetTmpX();
    auto shifts = context.GetTmpV();
    __ Mov(clamped, lane_bits - 1);
    __ Cmp(count, clamped);
    __ Csel(clamped, count, clamped, ls);
    __ Neg(clamped, clamped);
    auto shift_format = VecLaneFormat(shifts, lane_bits);
    __ Dup(shift_format, lane_bits == 64 ? Register(clamped) : Register(clamped.W()));
    __ Sshl(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), shift_format);
}

void JitTranslator::EmitVecShuffle32(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto indexes = context.GetTmpV();
    auto tmp = context.GetTmpX();
    const u32 control = inst->GetArg<ir::Imm>(1).Get();
    u64 index_lo = 0;
    u64 index_hi = 0;
    for (u32 byte = 0; byte < 16; ++byte) {
        const u32 output_lane = byte / 4;
        const u32 selected_lane = (control >> (output_lane * 2)) & 3;
        const u8 index = selected_lane * 4 + (byte & 3);
        auto& half = byte < 8 ? index_lo : index_hi;
        half |= u64(index) << ((byte & 7) * 8);
    }
    __ Mov(tmp, index_lo);
    __ Fmov(indexes.D(), tmp);
    __ Mov(tmp, index_hi);
    __ Ins(indexes.V2D(), 1, tmp);
    __ Tbl(result.V16B(), src.V16B(), indexes.V16B());
}

void JitTranslator::EmitVecShuffle16(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto indexes = context.GetTmpV();
    auto tmp = context.GetTmpX();
    const u32 control = inst->GetArg<ir::Imm>(1).Get();
    const u32 base_lane = inst->GetArg<ir::Imm>(2).Get() ? 4 : 0;
    u64 index_lo = 0;
    u64 index_hi = 0;
    for (u32 byte = 0; byte < 16; ++byte) {
        const u32 lane = byte / 2;
        u32 selected_lane = lane;
        if (lane >= base_lane && lane < base_lane + 4) {
            selected_lane = base_lane + ((control >> ((lane - base_lane) * 2)) & 3);
        }
        const u8 index = selected_lane * 2 + (byte & 1);
        auto& half = byte < 8 ? index_lo : index_hi;
        half |= u64(index) << ((byte & 7) * 8);
    }
    __ Mov(tmp, index_lo);
    __ Fmov(indexes.D(), tmp);
    __ Mov(tmp, index_hi);
    __ Ins(indexes.V2D(), 1, tmp);
    __ Tbl(result.V16B(), src.V16B(), indexes.V16B());
}

void JitTranslator::EmitVecZip(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const bool high = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            if (high)
                __ Zip2(result.V16B(), left.V16B(), right.V16B());
            else
                __ Zip1(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            if (high)
                __ Zip2(result.V8H(), left.V8H(), right.V8H());
            else
                __ Zip1(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            if (high)
                __ Zip2(result.V4S(), left.V4S(), right.V4S());
            else
                __ Zip1(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            if (high)
                __ Zip2(result.V2D(), left.V2D(), right.V2D());
            else
                __ Zip1(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

void JitTranslator::EmitVecDupPairs32(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (inst->GetArg<ir::Imm>(1).Get()) {
        __ Trn2(result.V4S(), src.V4S(), src.V4S());
    } else {
        __ Trn1(result.V4S(), src.V4S(), src.V4S());
    }
}

void JitTranslator::EmitVecDup64(ir::Inst* inst) {
    auto src = context.X(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    __ Dup(result.V2D(), src);
}

void JitTranslator::EmitVecExtract64(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.X(ir::Value{inst});
    const u32 lane = inst->GetArg<ir::Imm>(1).Get() & 1;
    __ Umov(result, src.V2D(), lane);
}

void JitTranslator::EmitVecExtract16(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.W(ir::Value{inst});
    const u32 lane = inst->GetArg<ir::Imm>(1).Get() & 7;
    __ Umov(result, src.V8H(), lane);
}

void JitTranslator::EmitVecInsert16(ir::Inst* inst) {
    auto dest = context.V(inst->GetArg<ir::Value>(0));
    auto value = context.W(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane = inst->GetArg<ir::Imm>(2).Get() & 7;
    __ Orr(result.V16B(), dest.V16B(), dest.V16B());
    __ Ins(result.V8H(), lane, value);
}

void JitTranslator::EmitVecMovMask(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.W(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(1).Get();
    if (lane_bits == 8) {
        auto packed = context.GetTmpV();
        auto work = context.GetTmpV();
        __ Ushr(work.V16B(), src.V16B(), 7);
        __ Uzp1(packed.V16B(), work.V16B(), work.V16B());
        __ Uzp2(work.V16B(), work.V16B(), work.V16B());
        __ Sli(packed.V16B(), work.V16B(), 1);
        __ Uzp2(work.V16B(), packed.V16B(), packed.V16B());
        __ Uzp1(packed.V16B(), packed.V16B(), packed.V16B());
        __ Sli(packed.V16B(), work.V16B(), 2);
        __ Uzp2(work.V16B(), packed.V16B(), packed.V16B());
        __ Uzp1(packed.V16B(), packed.V16B(), packed.V16B());
        __ Sli(packed.V16B(), work.V16B(), 4);
        __ Umov(result, packed.V8H(), 0);
        return;
    }
    if (lane_bits == 32) {
        auto work = context.GetTmpV();
        auto shifts = context.GetTmpV();
        __ Ushr(work.V4S(), src.V4S(), 31);
        __ Movi(shifts.V16B(), 0x0000000300000002ull, 0x0000000100000000ull);
        __ Ushl(work.V4S(), work.V4S(), shifts.V4S());
        __ Addv(work.S(), work.V4S());
        __ Umov(result, work.V4S(), 0);
        return;
    }
    if (lane_bits == 64) {
        auto work = context.GetTmpV();
        auto packed = context.GetTmpX();
        __ Uzp2(work.V4S(), src.V4S(), src.V4S());
        __ Umov(packed, work.V2D(), 0);
        __ Bfi(packed, packed, 31, 32);
        __ Lsr(packed, packed, 62);
        __ Mov(result, packed.W());
        return;
    }
    PANIC("invalid vector movmask lane width");
}

void JitTranslator::EmitVecTableLookup8(ir::Inst* inst) {
    auto table = context.V(inst->GetArg<ir::Value>(0));
    auto control = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto indexes = context.GetTmpV();
    __ Movi(indexes.V16B(), 0x8F);
    __ And(indexes.V16B(), control.V16B(), indexes.V16B());
    __ Tbl(result.V16B(), table.V16B(), indexes.V16B());
}

void JitTranslator::EmitVecFAddScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.W(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto scalar = context.GetTmpV();
    auto rhs = context.GetTmpV();
    __ Fmov(rhs.S(), right);
    __ Fadd(scalar.S(), left.S(), rhs.S());
    EmitVecFloatNaNFixup(scalar, left, rhs, 32, 1);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFSubScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.W(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto scalar = context.GetTmpV();
    auto rhs = context.GetTmpV();
    __ Fmov(rhs.S(), right);
    __ Fsub(scalar.S(), left.S(), rhs.S());
    EmitVecFloatNaNFixup(scalar, left, rhs, 32, 1);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFMulScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.W(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto scalar = context.GetTmpV();
    auto rhs = context.GetTmpV();
    __ Fmov(rhs.S(), right);
    __ Fmul(scalar.S(), left.S(), rhs.S());
    EmitVecFloatNaNFixup(scalar, left, rhs, 32, 1);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFDivScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.W(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto scalar = context.GetTmpV();
    auto rhs = context.GetTmpV();
    __ Fmov(rhs.S(), right);
    __ Fdiv(scalar.S(), left.S(), rhs.S());
    EmitVecFloatNaNFixup(scalar, left, rhs, 32, 1);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFAddScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.X(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto rhs = context.GetTmpV();
    __ Fmov(rhs.D(), right);
    __ Fadd(result.D(), left.D(), rhs.D());
    EmitVecFloatNaNFixup(result, left, rhs, 64, 1);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}

void JitTranslator::EmitVecFSubScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.X(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto rhs = context.GetTmpV();
    __ Fmov(rhs.D(), right);
    __ Fsub(result.D(), left.D(), rhs.D());
    EmitVecFloatNaNFixup(result, left, rhs, 64, 1);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}

void JitTranslator::EmitVecFMulScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.X(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto rhs = context.GetTmpV();
    __ Fmov(rhs.D(), right);
    __ Fmul(result.D(), left.D(), rhs.D());
    EmitVecFloatNaNFixup(result, left, rhs, 64, 1);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}

void JitTranslator::EmitVecFDivScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.X(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto rhs = context.GetTmpV();
    __ Fmov(rhs.D(), right);
    __ Fdiv(result.D(), left.D(), rhs.D());
    EmitVecFloatNaNFixup(result, left, rhs, 64, 1);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}

void JitTranslator::EmitVecFloatNaNFixup(const VRegister& result,
                                         const VRegister& left,
                                         const VRegister& right,
                                         u32 lane_bits,
                                         u32 lane_count) {
    ASSERT(lane_bits == 32 || lane_bits == 64);
    const u32 lanes = lane_count == 0 ? 128 / lane_bits : lane_count;

    // Mov may use VIXL's ip0/ip1 scratch registers for wide immediates. Keep
    // those registers out of this emitter's long-lived temporary set.
    context.ReserveTmpX(ip0);
    context.ReserveTmpX(ip1);

    // These eight physical temporaries are reused as soon as a logical value
    // dies. GetTmpX has instruction scope rather than lexical scope, so
    // acquiring one register for every logical name below would keep all of
    // them dirty across every lane and exhaust GPRs in high-pressure blocks.
    auto raw_left = context.GetTmpX();
    auto raw_right = context.GetTmpX();
    auto raw_result = context.GetTmpX();
    auto mask = context.GetTmpX();
    auto scratch = context.GetTmpX();
    auto nan_left = context.GetTmpX();
    auto nan_right = context.GetTmpX();
    auto chosen = context.GetTmpX();

    const u64 exponent_mask = lane_bits == 32 ? 0x7F800000u : 0x7FF0000000000000ull;
    const u64 fraction_mask = lane_bits == 32 ? 0x007FFFFFu : 0x000FFFFFFFFFFFFFull;
    const u64 quiet_mask = lane_bits == 32 ? 0x00400000u : 0x0008000000000000ull;
    for (u32 lane = 0; lane < lanes; ++lane) {
        if (lane_bits == 32) {
            __ Umov(raw_left.W(), left.V4S(), lane);
            __ Umov(raw_right.W(), right.V4S(), lane);
            __ Umov(raw_result.W(), result.V4S(), lane);

            __ Mov(mask.W(), static_cast<u32>(exponent_mask));
            __ And(scratch.W(), raw_left.W(), mask.W());
            __ Cmp(scratch.W(), mask.W());
            __ Cset(scratch.W(), eq);
            __ Mov(mask.W(), static_cast<u32>(fraction_mask));
            __ And(mask.W(), raw_left.W(), mask.W());
            __ Cmp(mask.W(), 0);
            __ Cset(mask.W(), ne);
            __ And(nan_left.W(), scratch.W(), mask.W());

            __ Mov(mask.W(), static_cast<u32>(exponent_mask));
            __ And(scratch.W(), raw_right.W(), mask.W());
            __ Cmp(scratch.W(), mask.W());
            __ Cset(scratch.W(), eq);
            __ Mov(mask.W(), static_cast<u32>(fraction_mask));
            __ And(mask.W(), raw_right.W(), mask.W());
            __ Cmp(mask.W(), 0);
            __ Cset(mask.W(), ne);
            __ And(nan_right.W(), scratch.W(), mask.W());

            __ Mov(mask.W(), static_cast<u32>(quiet_mask));
            __ Orr(scratch.W(), raw_left.W(), mask.W());
            // raw_left is dead after quieting operand 1, so reuse it for the
            // quieted operand 2.
            __ Orr(raw_left.W(), raw_right.W(), mask.W());
            __ Cmp(nan_left.W(), 0);
            __ Csel(chosen.W(), scratch.W(), raw_result.W(), ne);
            // Real x86 gives operand 1 priority when both inputs are NaN.
            // Select operand 2 only when operand 1 was not NaN.
            __ Cmp(nan_left.W(), 0);
            __ Cset(scratch.W(), eq);
            __ And(scratch.W(), scratch.W(), nan_right.W());
            __ Cmp(scratch.W(), 0);
            __ Csel(chosen.W(), raw_left.W(), chosen.W(), ne);

            __ Mov(mask.W(), static_cast<u32>(exponent_mask));
            __ And(raw_left.W(), raw_result.W(), mask.W());
            __ Cmp(raw_left.W(), mask.W());
            __ Cset(raw_left.W(), eq);
            __ Mov(mask.W(), static_cast<u32>(fraction_mask));
            __ And(mask.W(), raw_result.W(), mask.W());
            __ Cmp(mask.W(), 0);
            __ Cset(mask.W(), ne);
            __ And(raw_left.W(), raw_left.W(), mask.W());
            __ Orr(raw_right.W(), nan_left.W(), nan_right.W());
            __ Cmp(raw_right.W(), 0);
            __ Cset(raw_right.W(), eq);
            __ And(raw_left.W(), raw_left.W(), raw_right.W());
            __ Mov(raw_result.W(), 0xFFC00000u);
            __ Cmp(raw_left.W(), 0);
            __ Csel(chosen.W(), raw_result.W(), chosen.W(), ne);
            __ Ins(result.V4S(), lane, chosen.W());
        } else {
            __ Umov(raw_left, left.V2D(), lane);
            __ Umov(raw_right, right.V2D(), lane);
            __ Umov(raw_result, result.V2D(), lane);

            __ Mov(mask, exponent_mask);
            __ And(scratch, raw_left, mask);
            __ Cmp(scratch, mask);
            __ Cset(scratch, eq);
            __ Mov(mask, fraction_mask);
            __ And(mask, raw_left, mask);
            __ Cmp(mask, 0);
            __ Cset(mask, ne);
            __ And(nan_left, scratch, mask);

            __ Mov(mask, exponent_mask);
            __ And(scratch, raw_right, mask);
            __ Cmp(scratch, mask);
            __ Cset(scratch, eq);
            __ Mov(mask, fraction_mask);
            __ And(mask, raw_right, mask);
            __ Cmp(mask, 0);
            __ Cset(mask, ne);
            __ And(nan_right, scratch, mask);

            __ Mov(mask, quiet_mask);
            __ Orr(scratch, raw_left, mask);
            __ Orr(raw_left, raw_right, mask);
            __ Cmp(nan_left, 0);
            __ Csel(chosen, scratch, raw_result, ne);
            // Real x86 gives operand 1 priority when both inputs are NaN.
            __ Cmp(nan_left, 0);
            __ Cset(scratch, eq);
            __ And(scratch, scratch, nan_right);
            __ Cmp(scratch, 0);
            __ Csel(chosen, raw_left, chosen, ne);

            __ Mov(mask, exponent_mask);
            __ And(raw_left, raw_result, mask);
            __ Cmp(raw_left, mask);
            __ Cset(raw_left, eq);
            __ Mov(mask, fraction_mask);
            __ And(mask, raw_result, mask);
            __ Cmp(mask, 0);
            __ Cset(mask, ne);
            __ And(raw_left, raw_left, mask);
            __ Orr(raw_right, nan_left, nan_right);
            __ Cmp(raw_right, 0);
            __ Cset(raw_right, eq);
            __ And(raw_left, raw_left, raw_right);
            __ Mov(raw_result, 0xFFF8000000000000ull);
            __ Cmp(raw_left, 0);
            __ Csel(chosen, raw_result, chosen, ne);
            __ Ins(result.V2D(), lane, chosen);
        }
    }
}

void JitTranslator::EmitVecFAdd(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (lane_bits == 32)
        __ Fadd(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fadd(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, left, right, lane_bits);
}

void JitTranslator::EmitVecFSub(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (lane_bits == 32)
        __ Fsub(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fsub(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, left, right, lane_bits);
}

void JitTranslator::EmitVecFMul(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (lane_bits == 32)
        __ Fmul(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fmul(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, left, right, lane_bits);
}

void JitTranslator::EmitVecFDiv(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (lane_bits == 32)
        __ Fdiv(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fdiv(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, left, right, lane_bits);
}

void JitTranslator::EmitVecFMinMax(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool maximum = inst->GetArg<ir::Imm>(3).Get() != 0;
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    auto mask = context.GetTmpV();
    auto selected = context.GetTmpV();
    if (bits == 32) {
        if (maximum)
            __ Fcmgt(mask.V4S(), left.V4S(), right.V4S());
        else
            __ Fcmgt(mask.V4S(), right.V4S(), left.V4S());
    } else {
        if (maximum)
            __ Fcmgt(mask.V2D(), left.V2D(), right.V2D());
        else
            __ Fcmgt(mask.V2D(), right.V2D(), left.V2D());
    }
    __ Bsl(mask.V16B(), left.V16B(), right.V16B());
    __ Orr(selected.V16B(), mask.V16B(), mask.V16B());
    if (!scalar) {
        __ Orr(result.V16B(), selected.V16B(), selected.V16B());
    } else {
        __ Orr(result.V16B(), left.V16B(), left.V16B());
        if (bits == 32)
            __ Ins(result.V4S(), 0, selected.V4S(), 0);
        else
            __ Ins(result.V2D(), 0, selected.V2D(), 0);
    }
}

void JitTranslator::EmitVecFUnary(ir::Inst* inst) {
    auto source = context.V(inst->GetArg<ir::Value>(0));
    auto merge = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 kind = inst->GetArg<ir::Imm>(3).Get();
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    auto value = context.GetTmpV();
    // x86 SQRT of a negative operand raises #I and delivers the QNaN
    // *indefinite*, whose sign bit is SET (0xFFC00000 / 0xFFF8000000000000).
    // ARM's FSQRT delivers the default NaN, whose sign is clear, so the two
    // disagree on exactly those lanes — verified against real x86 via Rosetta:
    // sqrtps(-4) = ffc00000, sqrtpd(-4) = fff8000000000000.
    //
    // FCMLT against zero is false for both NaN and -0.0, which is precisely the
    // wanted predicate: a NaN operand must keep propagating through FSQRT, and
    // sqrt(-0.0) is legitimately -0.0 on both architectures.
    //
    // This is not AVX-specific: legacy SSE sqrtps/sqrtpd/sqrtss/sqrtsd reach
    // the same IR opcode and were wrong in the same way.
    if (bits == 32) {
        if (kind == 0) {
            __ Fsqrt(value.V4S(), source.V4S());
            auto negative = context.GetTmpV();
            auto indef = context.GetTmpV();
            auto imm = context.GetTmpX();
            __ Fcmlt(negative.V4S(), source.V4S(), 0.0);
            __ Mov(imm.W(), 0xFFC00000u);
            __ Dup(indef.V4S(), imm.W());
            __ Bit(value.V16B(), indef.V16B(), negative.V16B());
        } else if (kind == 1) {
            __ Frecpe(value.V4S(), source.V4S());
            auto step = context.GetTmpV();
            // FRECPE alone is less accurate than x86 RCPPS/RCPSS require.
            // Two Newton-Raphson refinements comfortably satisfy the x86
            // relative-error bound while retaining the estimate semantics.
            for (u32 i = 0; i < 2; ++i) {
                __ Frecps(step.V4S(), source.V4S(), value.V4S());
                __ Fmul(value.V4S(), value.V4S(), step.V4S());
            }
        } else {
            __ Frsqrte(value.V4S(), source.V4S());
            auto square = context.GetTmpV();
            auto step = context.GetTmpV();
            for (u32 i = 0; i < 2; ++i) {
                __ Fmul(square.V4S(), value.V4S(), value.V4S());
                __ Frsqrts(step.V4S(), source.V4S(), square.V4S());
                __ Fmul(value.V4S(), value.V4S(), step.V4S());
            }
        }
    } else {
        __ Fsqrt(value.V2D(), source.V2D());
        auto negative = context.GetTmpV();
        auto indef = context.GetTmpV();
        auto imm = context.GetTmpX();
        __ Fcmlt(negative.V2D(), source.V2D(), 0.0);
        __ Mov(imm, UINT64_C(0xFFF8000000000000));
        __ Dup(indef.V2D(), imm);
        __ Bit(value.V16B(), indef.V16B(), negative.V16B());
    }
    if (!scalar) {
        __ Orr(result.V16B(), value.V16B(), value.V16B());
    } else {
        __ Orr(result.V16B(), merge.V16B(), merge.V16B());
        if (bits == 32)
            __ Ins(result.V4S(), 0, value.V4S(), 0);
        else
            __ Ins(result.V2D(), 0, value.V2D(), 0);
    }
}

void JitTranslator::EmitVecFCmp(ir::Inst* inst) {
    auto left_raw = context.X(inst->GetArg<ir::Value>(0));
    auto right_raw = context.X(inst->GetArg<ir::Value>(1));
    auto result = context.X(ir::Value{inst});
    auto bit = context.GetTmpX();
    auto left = context.GetTmpV();
    auto right = context.GetTmpV();
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    if (bits == 32) {
        __ Fmov(left.S(), left_raw.W());
        __ Fmov(right.S(), right_raw.W());
        __ Fcmp(left.S(), right.S());
    } else {
        __ Fmov(left.D(), left_raw);
        __ Fmov(right.D(), right_raw);
        __ Fcmp(left.D(), right.D());
    }

    // ARM FPCompare NZCV: less=N, equal=Z, greater=C, unordered=C|V.
    // x86 UCOMIS flags are CF=less|unordered, PF=unordered,
    // ZF=equal|unordered.
    __ Cset(bit, lt);
    __ Mov(result, bit);
    __ Cset(bit, vs);
    __ Orr(result, result, bit);
    __ Lsl(bit, bit, 1);
    __ Orr(result, result, bit);
    __ Cset(bit, eq);
    auto unordered = context.GetTmpX();
    __ Cset(unordered, vs);
    __ Orr(bit, bit, unordered);
    __ Lsl(bit, bit, 2);
    __ Orr(result, result, bit);
}

void JitTranslator::EmitVecFCmpMask(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 predicate = inst->GetArg<ir::Imm>(3).Get() & 7;
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    auto compare = context.GetTmpV();
    auto ordered_left = context.GetTmpV();
    auto ordered_right = context.GetTmpV();
    auto format = [bits](const VRegister& value) {
        return bits == 32 ? value.V4S() : value.V2D();
    };
    switch (predicate) {
        case 0: __ Fcmeq(format(compare), format(left), format(right)); break;
        case 1: __ Fcmgt(format(compare), format(right), format(left)); break;
        case 2: __ Fcmge(format(compare), format(right), format(left)); break;
        case 3:
        case 7:
            __ Fcmeq(format(ordered_left), format(left), format(left));
            __ Fcmeq(format(ordered_right), format(right), format(right));
            __ And(compare.V16B(), ordered_left.V16B(), ordered_right.V16B());
            if (predicate == 3) __ Mvn(compare.V16B(), compare.V16B());
            break;
        case 4:
            __ Fcmeq(format(compare), format(left), format(right));
            __ Mvn(compare.V16B(), compare.V16B());
            break;
        case 5:
            __ Fcmgt(format(compare), format(right), format(left));
            __ Mvn(compare.V16B(), compare.V16B());
            break;
        case 6:
            __ Fcmge(format(compare), format(right), format(left));
            __ Mvn(compare.V16B(), compare.V16B());
            break;
    }
    if (!scalar) {
        __ Orr(result.V16B(), compare.V16B(), compare.V16B());
    } else {
        __ Orr(result.V16B(), left.V16B(), left.V16B());
        if (bits == 32)
            __ Ins(result.V4S(), 0, compare.V4S(), 0);
        else
            __ Ins(result.V2D(), 0, compare.V2D(), 0);
    }
}

void JitTranslator::EmitVecFCvtIntToFloat(ir::Inst* inst) {
    auto source = context.X(inst->GetArg<ir::Value>(0));
    auto result = context.X(ir::Value{inst});
    auto fp = context.GetTmpV();
    const u32 src_bits = inst->GetArg<ir::Imm>(1).Get();
    const u32 dst_bits = inst->GetArg<ir::Imm>(2).Get();
    if (dst_bits == 32) {
        if (src_bits == 32)
            __ Scvtf(fp.S(), source.W());
        else
            __ Scvtf(fp.S(), source);
        __ Fmov(result.W(), fp.S());
    } else {
        if (src_bits == 32)
            __ Scvtf(fp.D(), source.W());
        else
            __ Scvtf(fp.D(), source);
        __ Fmov(result, fp.D());
    }
}

void JitTranslator::EmitVecFCvtFloatToInt(ir::Inst* inst) {
    auto source = context.X(inst->GetArg<ir::Value>(0));
    auto result = context.X(ir::Value{inst});
    auto fp = context.GetTmpV();
    auto bound = context.GetTmpV();
    auto bound_bits = context.GetTmpX();
    auto converted = context.GetTmpX();
    auto invalid = context.GetTmpX();
    auto test = context.GetTmpX();
    auto indefinite = context.GetTmpX();
    const u32 src_bits = inst->GetArg<ir::Imm>(1).Get();
    const u32 dst_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool round_nearest = inst->GetArg<ir::Imm>(3).Get() != 0;

    if (src_bits == 32) {
        __ Fmov(fp.S(), source.W());
    } else {
        __ Fmov(fp.D(), source);
    }
    if (dst_bits == 32) {
        if (src_bits == 32) {
            if (round_nearest) __ Fcvtns(converted.W(), fp.S());
            else __ Fcvtzs(converted.W(), fp.S());
        } else {
            if (round_nearest) __ Fcvtns(converted.W(), fp.D());
            else __ Fcvtzs(converted.W(), fp.D());
        }
    } else {
        if (src_bits == 32) {
            if (round_nearest) __ Fcvtns(converted, fp.S());
            else __ Fcvtzs(converted, fp.S());
        } else {
            if (round_nearest) __ Fcvtns(converted, fp.D());
            else __ Fcvtzs(converted, fp.D());
        }
    }

    // Detect NaN first (FCMP x,x => V set only for unordered), then the two
    // half-open integer bounds.  The lower endpoint is valid; the upper
    // endpoint is invalid because truncation cannot represent 2^N.
    if (src_bits == 32)
        __ Fcmp(fp.S(), fp.S());
    else
        __ Fcmp(fp.D(), fp.D());
    __ Cset(invalid, vs);

    const u64 upper_bits = src_bits == 32 ? (dst_bits == 32 ? 0x4F000000u : 0x5F000000u)
                           : (dst_bits == 32 ? 0x41E0000000000000ull
                                             : 0x43E0000000000000ull);
    const u64 lower_bits = src_bits == 32 ? (dst_bits == 32 ? 0xCF000000u : 0xDF000000u)
                           : (dst_bits == 32 ? 0xC1E0000000000000ull
                                             : 0xC3E0000000000000ull);
    __ Mov(bound_bits, upper_bits);
    if (src_bits == 32)
        __ Fmov(bound.S(), bound_bits.W());
    else
        __ Fmov(bound.D(), bound_bits);
    if (src_bits == 32)
        __ Fcmp(fp.S(), bound.S());
    else
        __ Fcmp(fp.D(), bound.D());
    __ Cset(test, hs);
    __ Orr(invalid, invalid, test);

    __ Mov(bound_bits, lower_bits);
    if (src_bits == 32)
        __ Fmov(bound.S(), bound_bits.W());
    else
        __ Fmov(bound.D(), bound_bits);
    if (src_bits == 32)
        __ Fcmp(fp.S(), bound.S());
    else
        __ Fcmp(fp.D(), bound.D());
    __ Cset(test, lt);
    __ Orr(invalid, invalid, test);

    __ Mov(indefinite, dst_bits == 32 ? 0x80000000u : 0x8000000000000000ull);
    __ Cmp(invalid, 0);
    if (dst_bits == 32)
        __ Csel(result.W(), indefinite.W(), converted.W(), ne);
    else
        __ Csel(result, indefinite, converted, ne);
}

void JitTranslator::EmitVecFCvtScalar(ir::Inst* inst) {
    auto source = context.X(inst->GetArg<ir::Value>(0));
    auto result = context.X(ir::Value{inst});
    auto fp = context.GetTmpV();
    const u32 src_bits = inst->GetArg<ir::Imm>(1).Get();
    if (src_bits == 32) {
        __ Fmov(fp.S(), source.W());
        __ Fcvt(fp.D(), fp.S());
        __ Fmov(result, fp.D());
    } else {
        __ Fmov(fp.D(), source);
        __ Fcvt(fp.S(), fp.D());
        __ Fmov(result.W(), fp.S());
    }
}

void JitTranslator::EmitVecFCvtPacked(ir::Inst* inst) {
    auto source = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto wide = context.GetTmpV();
    const u32 kind = inst->GetArg<ir::Imm>(1).Get();
    switch (kind) {
        case 0: __ Scvtf(result.V4S(), source.V4S()); break;
        case 1:
            __ Sshll(wide.V2D(), source.V2S(), 0);
            __ Scvtf(result.V2D(), wide.V2D());
            break;
        case 2:
        case 3: {
            if (kind == 2)
                __ Fcvtns(result.V4S(), source.V4S());
            else
                __ Fcvtzs(result.V4S(), source.V4S());
            auto invalid = context.GetTmpV();
            auto compare = context.GetTmpV();
            auto bound = context.GetTmpV();
            auto bits = context.GetTmpX();
            auto indefinite = context.GetTmpV();
            __ Fcmeq(invalid.V4S(), source.V4S(), source.V4S());
            __ Mvn(invalid.V16B(), invalid.V16B());
            __ Mov(bits.W(), 0x4F000000u);  // +2^31
            __ Dup(bound.V4S(), bits.W());
            __ Fcmge(compare.V4S(), source.V4S(), bound.V4S());
            __ Orr(invalid.V16B(), invalid.V16B(), compare.V16B());
            __ Mov(bits.W(), 0xCF000000u);  // -2^31 (valid endpoint)
            __ Dup(bound.V4S(), bits.W());
            __ Fcmgt(compare.V4S(), bound.V4S(), source.V4S());
            __ Orr(invalid.V16B(), invalid.V16B(), compare.V16B());
            __ Mov(bits.W(), 0x80000000u);
            __ Dup(indefinite.V4S(), bits.W());
            __ Bsl(invalid.V16B(), indefinite.V16B(), result.V16B());
            __ Orr(result.V16B(), invalid.V16B(), invalid.V16B());
            break;
        }
        case 4:
        case 5:
            if (kind == 4)
                __ Fcvtns(wide.V2D(), source.V2D());
            else
                __ Fcvtzs(wide.V2D(), source.V2D());
            {
                auto invalid = context.GetTmpV();
                auto compare = context.GetTmpV();
                auto bound = context.GetTmpV();
                auto bits = context.GetTmpX();
                auto indefinite = context.GetTmpV();
                __ Fcmeq(invalid.V2D(), source.V2D(), source.V2D());
                __ Mvn(invalid.V16B(), invalid.V16B());
                __ Mov(bits, 0x41E0000000000000ull);  // +2^31
                __ Dup(bound.V2D(), bits);
                __ Fcmge(compare.V2D(), source.V2D(), bound.V2D());
                __ Orr(invalid.V16B(), invalid.V16B(), compare.V16B());
                __ Mov(bits, 0xC1E0000000000000ull);  // -2^31
                __ Dup(bound.V2D(), bits);
                __ Fcmgt(compare.V2D(), bound.V2D(), source.V2D());
                __ Orr(invalid.V16B(), invalid.V16B(), compare.V16B());
                __ Mov(bits, 0x80000000u);
                __ Dup(indefinite.V2D(), bits);
                __ Bsl(invalid.V16B(), indefinite.V16B(), wide.V16B());
                __ Orr(wide.V16B(), invalid.V16B(), invalid.V16B());
            }
            __ Xtn(result.V2S(), wide.V2D());
            break;
        case 6: __ Fcvtl(result.V2D(), source.V2S()); break;
        case 7: __ Fcvtn(result.V2S(), source.V2D()); break;
    }
}

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
    auto value = inst->GetArg<ir::Value>(0);
    auto left = inst->GetArg<ir::Imm>(1).Get();
    auto bits = inst->GetArg<ir::Imm>(2).Get();
    auto result = context.R(ir::Value{inst});
    __ Ubfx(result, context.R(value), left, bits);
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

void JitTranslator::EmitTestNotZero(ir::Inst* inst) {
    auto value = inst->GetArg<ir::Value>(0);
    auto result = context.W(ir::Value{inst});
    MergeNZCV();
    __ Cmp(context.R(value), 0);
    __ Cset(result, ne);
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

// De-interleave {left, right}, keeping the even (UZP1) or odd (UZP2) lanes.
// At 64-bit lanes UZP1/UZP2 coincide with ZIP1/ZIP2; the opcode still accepts
// that width so a caller does not have to special-case it.
void JitTranslator::EmitVecUnzip(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const bool odd = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            if (odd)
                __ Uzp2(result.V16B(), left.V16B(), right.V16B());
            else
                __ Uzp1(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            if (odd)
                __ Uzp2(result.V8H(), left.V8H(), right.V8H());
            else
                __ Uzp1(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            if (odd)
                __ Uzp2(result.V4S(), left.V4S(), right.V4S());
            else
                __ Uzp1(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            if (odd)
                __ Uzp2(result.V2D(), left.V2D(), right.V2D());
            else
                __ Uzp1(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

}  // namespace swift::runtime::backend::arm64
