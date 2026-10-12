#include "translator.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"
#include "runtime/common/saturating_multiply.h"
#include "runtime/common/saturating_shift.h"
#include "runtime/common/unsigned_estimate.h"
#include "runtime/common/saturating_add_sub.h"

#include "translator_alu_internal.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::EmitVecUnsignedEstimate(ir::Inst* inst) {
    const auto operation = ValidateUnsignedEstimate(inst);
    const auto input = context.V(inst->GetArg<ir::Value>(0));
    const auto result = context.V(ir::Value{inst});
    if (operation) __ Ursqrte(result.V4S(), input.V4S());
    else __ Urecpe(result.V4S(), input.V4S());
}

void JitTranslator::EmitVec4Or(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Orr(result.V16B(), left.V16B(), right.V16B());
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

void JitTranslator::EmitVecDot(ir::Inst* inst) {
    if (inst->ReturnType() != ir::ValueType::V128 || inst->GetArg<ir::Imm>(3).Get() > 1)
        throw std::runtime_error("VecDot requires V128 and a boolean signed-byte mode");
    for (u32 input = 0; input < 3; ++input)
        if (inst->GetArg<ir::Value>(input).Type() != ir::ValueType::V128 ||
            inst->GetArg<ir::Value>(input).Def()->ReturnType() != ir::ValueType::V128)
            throw std::runtime_error("VecDot requires three V128 inputs");
    const auto left = context.V(inst->GetArg<ir::Value>(0));
    const auto right = context.V(inst->GetArg<ir::Value>(1));
    const auto accumulator = context.V(inst->GetArg<ir::Value>(2));
    const auto result = context.V(ir::Value{inst});
    auto low = context.GetTmpV();
    auto high = context.GetTmpV();
    const bool signed_bytes = inst->GetArg<ir::Imm>(3).Get() != 0;
    // Consume both complete byte sources into independent scratch registers
    // before writing a destination that may share any source allocation.
    // Baseline AdvSIMD is sufficient; no DotProduct extension is required.
    if (signed_bytes) {
        __ Smull(low.V8H(), left.V8B(), right.V8B());
        __ Smull2(high.V8H(), left.V16B(), right.V16B());
        __ Saddlp(low.V4S(), low.V8H());
        __ Saddlp(high.V4S(), high.V8H());
    } else {
        __ Umull(low.V8H(), left.V8B(), right.V8B());
        __ Umull2(high.V8H(), left.V16B(), right.V16B());
        __ Uaddlp(low.V4S(), low.V8H());
        __ Uaddlp(high.V4S(), high.V8H());
    }
    // [p0+p1,p2+p3,...] -> [p0+p1+p2+p3,...] in original byte order.
    // These sums fit S32/U32 respectively; the final ADD wraps modulo 2^32.
    __ Addp(low.V4S(), low.V4S(), high.V4S());
    __ Add(result.V4S(), accumulator.V4S(), low.V4S());
}

void JitTranslator::EmitVecSatMulHigh(ir::Inst* inst) {
    const auto shape = ValidateSaturatingMultiply(inst);
    // The final lane clamps use CMP/CSEL. Publish pending guest flags before
    // those overwrite PSTATE, while retaining the pure IR flag contract.
    MergeNZCV();
    // Snapshot into private temporaries before destination allocation can
    // reuse any source; inactive lanes never reach the arithmetic below.
    auto left = context.GetTmpV(), right = context.GetTmpV(), accumulator = context.GetTmpV();
    __ Mov(left.V16B(), context.V(inst->GetArg<ir::Value>(0)).V16B());
    __ Mov(right.V16B(), context.V(inst->GetArg<ir::Value>(1)).V16B());
    __ Mov(accumulator.V16B(), context.V(inst->GetArg<ir::Value>(2)).V16B());
    const auto result = context.V(ir::Value{inst});
    __ Movi(result.V16B(), 0);
    const auto a = context.GetTmpX(), b = context.GetTmpX(), value = context.GetTmpX();
    const auto qc = context.GetTmpX(), limit = context.GetTmpX();
    __ Mov(qc, 0);
    const s64 lower = -(s64{1} << (shape.bits - 1)), upper = -lower - 1;
    for (u32 lane = 0; lane < shape.lanes; ++lane) {
        if (shape.bits == 16) {
            __ Umov(a.W(), left.V8H(), lane); __ Sxth(a, a.W());
            __ Umov(b.W(), right.V8H(), lane); __ Sxth(b, b.W());
        } else {
            __ Umov(a.W(), left.V4S(), lane); __ Sxtw(a, a.W());
            __ Umov(b.W(), right.V4S(), lane); __ Sxtw(b, b.W());
        }
        __ Mul(value, a, b);
        if (shape.mode == 3) __ Neg(value, value);
        if (shape.mode != 0) {
            // Use an existing lease for the 32-bit rounding constant; an
            // immediate Add macro may otherwise request hidden VIXL scratch.
            __ Mov(limit, s64{1} << (shape.bits - 2)); __ Add(value, value, limit);
        }
        __ Asr(value, value, shape.bits - 1);
        if (shape.mode >= 2) {
            if (shape.bits == 16) { __ Umov(a.W(), accumulator.V8H(), lane); __ Sxth(a, a.W()); }
            else { __ Umov(a.W(), accumulator.V4S(), lane); __ Sxtw(a, a.W()); }
            __ Add(value, value, a);
        }
        __ Mov(limit, lower); __ Cmp(value, limit); __ Cset(a.W(), lt);
        __ Csel(value, limit, value, lt); __ Orr(qc, qc, a);
        __ Mov(limit, upper); __ Cmp(value, limit); __ Cset(a.W(), gt);
        __ Csel(value, limit, value, gt); __ Orr(qc, qc, a);
        if (shape.bits == 16) __ Ins(result.V8H(), lane, value.W());
        else __ Ins(result.V4S(), lane, value.W());
    }
    for (auto* pseudo : inst->GetPseudoOperations(ir::OpCode::VecSatMulQC)) {
        ValidateSaturatingMultiplyQC(pseudo);
        const auto pseudo_value = ir::Value{pseudo};
        auto& allocation = context.GetAllocation();
        if (allocation.ValueType(pseudo_value) == RegAlloc::MEM) {
            // These are early definitions, not current-instruction results.
            // Store the existing QC lease directly instead of consuming one
            // unbudgeted reload scratch register per spilled pseudo output.
            const auto slot = allocation.ValueMem(pseudo_value).offset;
            __ Str(qc, MemOperand(state, state_offset_spill_area + slot * sizeof(u64)));
            continue;
        }
        const auto output = context.RForWrite(pseudo_value);
        context.ReserveTmpX(XRegister{output.GetCode()});
        __ Mov(WRegister{output.GetCode()}, qc.W());
    }
}

void JitTranslator::EmitVecSatMulQC(ir::Inst* inst) {
    ValidateSaturatingMultiplyQC(inst);
}

void JitTranslator::EmitVecSatShiftImm(ir::Inst* inst) {
    const auto shape = ValidateSaturatingShift(inst);
    const auto snapshot = context.GetTmpV();
    __ Mov(snapshot.V16B(), context.V(inst->GetArg<ir::Value>(0)).V16B());
    const auto result = context.V(ir::Value{inst});
    __ Movi(result.V16B(), 0);
    const auto input = context.GetTmpX(), value = context.GetTmpX(), other = context.GetTmpX();
    const auto qc = context.GetTmpX(), limit = context.GetTmpX(), saved_flags = context.GetTmpX();
    // The lane comparisons are private to this pure operation. Retain lazy
    // guest flag bindings by restoring the original PSTATE before returning.
    __ Mrs(saved_flags, NZCV); __ Mov(qc, 0);
    const u32 source_bits = shape.SourceBits();
    for (u32 lane = 0; lane < shape.lanes; ++lane) {
        if (source_bits == 8) __ Umov(input.W(), snapshot.V16B(), lane);
        else if (source_bits == 16) __ Umov(input.W(), snapshot.V8H(), lane);
        else if (source_bits == 32) __ Umov(input.W(), snapshot.V4S(), lane);
        else __ Umov(input, snapshot.V2D(), lane);
        if (shape.Left()) {
            __ Lsl(value, input, shape.shift);
            if (shape.bits < 64) __ Ubfx(value, value, 0, shape.bits);
            if (shape.SignedResult()) {
                __ Sbfm(input, input, 0, shape.bits - 1);
                __ Sbfm(other, value, 0, shape.bits - 1);
                if (shape.shift) __ Asr(other, other, shape.shift);
            } else {
                __ Lsr(other, value, shape.shift);
            }
            __ Eor(other, input, other);
            if (shape.mode == SaturatingShiftMode::SQSHLU) {
                __ Ubfx(limit, input, shape.bits - 1, 1); __ Orr(other, other, limit);
            }
        } else {
            if (shape.SignedSource()) { __ Sbfm(value, input, 0, source_bits - 1); __ Asr(value, value, shape.shift); }
            else __ Lsr(value, input, shape.shift);
            if (shape.Rounded()) { __ Ubfx(other, input, shape.shift - 1, 1); __ Add(value, value, other); }
            __ Ubfx(other, value, 0, shape.bits);
            if (shape.SignedResult()) __ Sbfm(other, other, 0, shape.bits - 1);
            __ Eor(other, value, other);
        }
        __ Cmp(other, 0); __ Cset(limit, ne); __ Orr(qc, qc, limit);
        const u64 maximum = shape.SignedResult() ? (u64{1} << (shape.bits - 1)) - 1 :
                            shape.bits == 64 ? ~u64{0} : (u64{1} << shape.bits) - 1;
        if (shape.SignedSource()) {
            Label positive, selected;
            __ Tbz(shape.Left() ? input : value, shape.Left() ? shape.bits - 1 : 63, &positive);
            __ Mov(limit, shape.SignedResult() ? u64{1} << (shape.bits - 1) : 0); __ B(&selected);
            __ Bind(&positive); __ Mov(limit, maximum); __ Bind(&selected);
        } else __ Mov(limit, maximum);
        __ Cmp(other, 0); __ Csel(value, limit, value, ne);
        if (shape.bits == 8) __ Ins(result.V16B(), lane, value.W());
        else if (shape.bits == 16) __ Ins(result.V8H(), lane, value.W());
        else if (shape.bits == 32) __ Ins(result.V4S(), lane, value.W());
        else __ Ins(result.V2D(), lane, value);
    }
    __ Msr(NZCV, saved_flags);
    for (auto* pseudo : inst->GetPseudoOperations(ir::OpCode::VecSatShiftQC)) {
        ValidateSaturatingShiftQC(pseudo);
        const auto pseudo_value = ir::Value{pseudo};
        auto& allocation = context.GetAllocation();
        if (allocation.ValueType(pseudo_value) == RegAlloc::MEM) {
            __ Str(qc, MemOperand(state, state_offset_spill_area + allocation.ValueMem(pseudo_value).offset * sizeof(u64)));
        } else {
            const auto output = context.RForWrite(pseudo_value);
            context.ReserveTmpX(XRegister{output.GetCode()});
            __ Mov(WRegister{output.GetCode()}, qc.W());
        }
    }
}

void JitTranslator::EmitVecSatShiftQC(ir::Inst* inst) { ValidateSaturatingShiftQC(inst); }

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
void JitTranslator::EmitVecSatAdd(ir::Inst* inst) {
    const auto shape = ValidateSaturatingAddSub(inst);
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = shape.bits;
    const bool signed_lanes = shape.sign;
    if (signed_lanes)
        __ Sqadd(VecLaneFormat(result, bits), VecLaneFormat(left, bits), VecLaneFormat(right, bits));
    else
        __ Uqadd(VecLaneFormat(result, bits), VecLaneFormat(left, bits), VecLaneFormat(right, bits));
}

void JitTranslator::EmitVecSatSub(ir::Inst* inst) {
    const auto shape = ValidateSaturatingAddSub(inst);
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = shape.bits;
    const bool signed_lanes = shape.sign;
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
    // The scalar count clamp below uses CMP/CSEL, which overwrites NZCV even
    // though the eventual NEON shift is flag-neutral.  Commit a pending guest
    // arithmetic producer before touching PSTATE so FLAGS_REGS can safely
    // observe it after this operation.
    MergeNZCV();
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
    MergeNZCV();
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
    MergeNZCV();
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

void JitTranslator::EmitVecShiftLeftImm(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    const u32 count = inst->GetArg<ir::Imm>(1).Get();
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (count == 0) {
        if (result.GetCode() != value.GetCode()) {
            __ Orr(result.V16B(), value.V16B(), value.V16B());
        }
    } else if (count >= lane_bits) {
        __ Eor(result.V16B(), result.V16B(), result.V16B());
    } else {
        __ Shl(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), count);
    }
}

void JitTranslator::EmitVecShiftRightImm(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    const u32 count = inst->GetArg<ir::Imm>(1).Get();
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (count == 0) {
        if (result.GetCode() != value.GetCode()) {
            __ Orr(result.V16B(), value.V16B(), value.V16B());
        }
    } else if (count >= lane_bits) {
        __ Eor(result.V16B(), result.V16B(), result.V16B());
    } else {
        __ Ushr(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), count);
    }
}

void JitTranslator::EmitVecShiftRightArithmeticImm(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    const u32 raw_count = inst->GetArg<ir::Imm>(1).Get();
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 count = std::min(raw_count, lane_bits - 1);
    if (count == 0) {
        if (result.GetCode() != value.GetCode()) {
            __ Orr(result.V16B(), value.V16B(), value.V16B());
        }
    } else {
        __ Sshr(VecLaneFormat(result, lane_bits), VecLaneFormat(value, lane_bits), count);
    }
}

void JitTranslator::EmitVecByteShift(ir::Inst* inst) {
    auto value = context.V(inst->GetArg<ir::Value>(0));
    auto zero = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 count = inst->GetArg<ir::Imm>(2).Get();
    const bool left = inst->GetArg<ir::Imm>(3).Get() != 0;
    ASSERT(count > 0 && count < 16);
    if (left) {
        __ Ext(result.V16B(), zero.V16B(), value.V16B(), 16 - count);
    } else {
        __ Ext(result.V16B(), value.V16B(), zero.V16B(), count);
    }
}


#undef __

}  // namespace swift::runtime::backend::arm64
