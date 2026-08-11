#include "translator.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

#include "translator_alu_internal.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

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
