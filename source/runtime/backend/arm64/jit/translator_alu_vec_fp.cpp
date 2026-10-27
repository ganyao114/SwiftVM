#include "translator.h"

#include <algorithm>
#include <cstring>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

#include "translator_alu_internal.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

VRegister JitTranslator::GetVecScalarOperand(ir::Value value, u32 lane_bits) {
    ASSERT(lane_bits == 32 || lane_bits == 64);
    if (ir::IsFloatValueType(value.Type())) {
        return context.V(value);
    }
    auto result = context.GetTmpV();
    if (lane_bits == 32) {
        __ Fmov(result.S(), context.W(value));
    } else {
        __ Fmov(result.D(), context.X(value));
    }
    return result;
}

// Ordinary arithmetic follows the host NaN propagation policy, as FEX does.
// MIN/MAX, comparisons and conversions retain their opcode-specific x86 rules.
// FEAT_AFP selects its supported behaviour through guest FPCR; non-AFP hosts
// do not add per-result predicates or software payload repair.

void JitTranslator::EmitVecFAddScalar32(const ir::Inst* inst) {
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    EmitVecFScalarBinaryLegacy(inst, 32);
}

void JitTranslator::EmitVecFSubScalar32(const ir::Inst* inst) {
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    EmitVecFScalarBinaryLegacy(inst, 32);
}

void JitTranslator::EmitVecFMulScalar32(const ir::Inst* inst) {
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    EmitVecFScalarBinaryLegacy(inst, 32);
}

void JitTranslator::EmitVecFDivScalar32(const ir::Inst* inst) {
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    EmitVecFScalarBinaryLegacy(inst, 32);
}

void JitTranslator::EmitVecFAddScalar64(const ir::Inst* inst) {
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    EmitVecFScalarBinaryLegacy(inst, 64);
}

void JitTranslator::EmitVecFSubScalar64(const ir::Inst* inst) {
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    EmitVecFScalarBinaryLegacy(inst, 64);
}

void JitTranslator::EmitVecFMulScalar64(const ir::Inst* inst) {
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    EmitVecFScalarBinaryLegacy(inst, 64);
}

void JitTranslator::EmitVecFDivScalar64(const ir::Inst* inst) {
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    EmitVecFScalarBinaryLegacy(inst, 64);
}

void JitTranslator::EmitVecFScalarBinaryLegacy(const ir::Inst* inst, u32 lane_bits) {
    ASSERT(lane_bits == 32 || lane_bits == 64);
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    const bool tied = result.GetCode() == left.GetCode();
    auto value = lane_bits == 32
            ? context.GetTmpV()
            : (tied ? context.GetTmpV() : result);
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), lane_bits);
    switch (inst->GetOp()) {
        case ir::OpCode::VecFAddScalar32:
            ASSERT(lane_bits == 32);
            __ Fadd(value.S(), left.S(), right.S());
            break;
        case ir::OpCode::VecFSubScalar32:
            ASSERT(lane_bits == 32);
            __ Fsub(value.S(), left.S(), right.S());
            break;
        case ir::OpCode::VecFMulScalar32:
            ASSERT(lane_bits == 32);
            __ Fmul(value.S(), left.S(), right.S());
            break;
        case ir::OpCode::VecFDivScalar32:
            ASSERT(lane_bits == 32);
            __ Fdiv(value.S(), left.S(), right.S());
            break;
        case ir::OpCode::VecFAddScalar64:
            ASSERT(lane_bits == 64);
            __ Fadd(value.D(), left.D(), right.D());
            break;
        case ir::OpCode::VecFSubScalar64:
            ASSERT(lane_bits == 64);
            __ Fsub(value.D(), left.D(), right.D());
            break;
        case ir::OpCode::VecFMulScalar64:
            ASSERT(lane_bits == 64);
            __ Fmul(value.D(), left.D(), right.D());
            break;
        case ir::OpCode::VecFDivScalar64:
            ASSERT(lane_bits == 64);
            __ Fdiv(value.D(), left.D(), right.D());
            break;
        default:
            PANIC();
    }
    if (lane_bits == 32) {
        if (!tied) {
            __ Orr(result.V16B(), left.V16B(), left.V16B());
        }
        __ Ins(result.V4S(), 0, value.V4S(), 0);
    } else if (tied) {
        __ Ins(result.V2D(), 0, value.V2D(), 0);
    } else {
        __ Ins(result.V2D(), 1, left.V2D(), 1);
    }
}

void JitTranslator::EmitVecFScalarBinaryTied(const ir::Inst* inst, u32 lane_bits) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto right_value = inst->GetArg<ir::Value>(1);
    const bool right_is_vector = ir::IsFloatValueType(right_value.Type());
    auto right = right_is_vector ? context.V(right_value) : context.GetTmpV();
    // NEP takes the upper lanes from the left operand. Copying left into the
    // destination first would overwrite a right operand sharing that register.
    if (result.GetCode() == left.GetCode()) {
        if (sse_scalar_tie && result.GetCode() >= 16) {
            ASSERT_MSG(context.GetActions().FPRVerified(inst->Id(), FPRAction::ScalarTie),
                       "scalar FPR fixed-home tie proof diverged at IR {}",
                       inst->Id());
        }
    }
    if (!right_is_vector) {
        if (lane_bits == 32) {
            __ Fmov(right.S(), context.W(right_value));
        } else {
            __ Fmov(right.D(), context.X(right_value));
        }
    }
    switch (inst->GetOp()) {
        case ir::OpCode::VecFAddScalar32:
            __ Fadd(result.S(), left.S(), right.S());
            break;
        case ir::OpCode::VecFSubScalar32:
            __ Fsub(result.S(), left.S(), right.S());
            break;
        case ir::OpCode::VecFMulScalar32:
            __ Fmul(result.S(), left.S(), right.S());
            break;
        case ir::OpCode::VecFDivScalar32:
            __ Fdiv(result.S(), left.S(), right.S());
            break;
        case ir::OpCode::VecFAddScalar64:
            __ Fadd(result.D(), left.D(), right.D());
            break;
        case ir::OpCode::VecFSubScalar64:
            __ Fsub(result.D(), left.D(), right.D());
            break;
        case ir::OpCode::VecFMulScalar64:
            __ Fmul(result.D(), left.D(), right.D());
            break;
        case ir::OpCode::VecFDivScalar64:
            __ Fdiv(result.D(), left.D(), right.D());
            break;
        default:
            PANIC();
    }
}

void JitTranslator::EmitVecFAdd(const ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (lane_bits == 32)
        __ Fadd(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fadd(result.V2D(), left.V2D(), right.V2D());
}

void JitTranslator::EmitVecFSub(const ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (lane_bits == 32)
        __ Fsub(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fsub(result.V2D(), left.V2D(), right.V2D());
}

void JitTranslator::EmitVecFMul(const ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (lane_bits == 32)
        __ Fmul(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fmul(result.V2D(), left.V2D(), right.V2D());
}

void JitTranslator::EmitVecFDiv(const ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    if (lane_bits == 32)
        __ Fdiv(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fdiv(result.V2D(), left.V2D(), right.V2D());
}

void JitTranslator::EmitVecFMinMax(const ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool maximum = inst->GetArg<ir::Imm>(3).Get() != 0;
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    if (sse_afp_minmax && scalar) {
        // AH selects operand 2 for unordered and equal inputs, matching x86
        // MIN/MAX including NaN payload and signed-zero selection. NEP takes
        // the upper lanes from the left input, including an untied result.
        if (bits == 32) {
            if (maximum)
                __ Fmax(result.S(), left.S(), right.S());
            else
                __ Fmin(result.S(), left.S(), right.S());
        } else {
            if (maximum)
                __ Fmax(result.D(), left.D(), right.D());
            else
                __ Fmin(result.D(), left.D(), right.D());
        }
        return;
    }
    // Vector comparison leaves pending x86 flags in host NZCV untouched.
    // Select before writing result, which may reuse either input register.
    auto mask = context.GetTmpV();
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
    if (!scalar) {
        __ Orr(result.V16B(), mask.V16B(), mask.V16B());
    } else {
        if (result.GetCode() != left.GetCode()) {
            __ Orr(result.V16B(), left.V16B(), left.V16B());
        }
        if (bits == 32)
            __ Ins(result.V4S(), 0, mask.V4S(), 0);
        else
            __ Ins(result.V2D(), 0, mask.V2D(), 0);
    }
}

// Preserve fused rounding and use the host NaN policy for all FMA variants.
void JitTranslator::EmitVecFMulAdd(const ir::Inst* inst) {
    auto a = context.V(inst->GetArg<ir::Value>(0));
    auto b = context.V(inst->GetArg<ir::Value>(1));
    auto c = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(3).Get();
    const u32 flags = inst->GetArg<ir::Imm>(4).Get();
    const bool negate_product = (flags & 1u) != 0;
    const bool negate_addend = (flags & 2u) != 0;
    ASSERT(lane_bits == 32 || lane_bits == 64);

    // Accumulator setup must not overwrite a multiplicand before FMLA/FMLS
    // reads it. A result already tied to an unnegated addend needs no copy.
    const bool aliases_product = result.GetCode() == a.GetCode() ||
                                 result.GetCode() == b.GetCode();
    auto acc = aliases_product &&
                       (negate_addend || result.GetCode() != c.GetCode())
            ? context.GetTmpV()
            : result;
    const auto fmt_of = [lane_bits](const VRegister& v) {
        return lane_bits == 32 ? v.V4S() : v.V2D();
    };
    if (negate_addend) {
        __ Fneg(fmt_of(acc), fmt_of(c));
    } else if (acc.GetCode() != c.GetCode()) {
        __ Orr(acc.V16B(), c.V16B(), c.V16B());
    }
    if (negate_product) {
        __ Fmls(fmt_of(acc), fmt_of(a), fmt_of(b));
    } else {
        __ Fmla(fmt_of(acc), fmt_of(a), fmt_of(b));
    }
    if (acc.GetCode() != result.GetCode()) {
        __ Orr(result.V16B(), acc.V16B(), acc.V16B());
    }
}

void JitTranslator::EmitVecFUnary(const ir::Inst* inst) {
    auto source = context.V(inst->GetArg<ir::Value>(0));
    auto merge = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 kind = inst->GetArg<ir::Imm>(3).Get();
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    if (sse_scalar_insert && scalar &&
        (result.GetCode() == merge.GetCode() || source.GetCode() != result.GetCode()) &&
        (kind == 0 || source.GetCode() != result.GetCode())) {
        if (result.GetCode() != merge.GetCode()) {
            __ Orr(result.V16B(), merge.V16B(), merge.V16B());
        }
        if (kind == 0) {
            if (bits == 32) {
                __ Fsqrt(result.S(), source.S());
            } else {
                __ Fsqrt(result.D(), source.D());
            }
        } else if (kind == 1) {
            __ Frecpe(result.S(), source.S());
            auto step = context.GetTmpV();
            for (u32 i = 0; i < 2; ++i) {
                __ Frecps(step.S(), source.S(), result.S());
                __ Fmul(result.S(), result.S(), step.S());
            }
        } else {
            __ Frsqrte(result.S(), source.S());
            auto square = context.GetTmpV();
            auto step = context.GetTmpV();
            for (u32 i = 0; i < 2; ++i) {
                __ Fmul(square.S(), result.S(), result.S());
                __ Frsqrts(step.S(), source.S(), square.S());
                __ Fmul(result.S(), result.S(), step.S());
            }
        }
        return;
    }
    auto value = !scalar && kind == 0 ? result : context.GetTmpV();
    if (bits == 32) {
        if (kind == 0) {
            if (scalar) {
                __ Fsqrt(value.S(), source.S());
            } else {
                __ Fsqrt(value.V4S(), source.V4S());
            }
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
    } else if (scalar) {
        __ Fsqrt(value.D(), source.D());
    } else {
        __ Fsqrt(value.V2D(), source.V2D());
    }
    if (!scalar) {
        if (result.GetCode() != value.GetCode()) {
            __ Orr(result.V16B(), value.V16B(), value.V16B());
        }
    } else {
        if (result.GetCode() != merge.GetCode()) {
            __ Orr(result.V16B(), merge.V16B(), merge.V16B());
        }
        if (bits == 32)
            __ Ins(result.V4S(), 0, value.V4S(), 0);
        else
            __ Ins(result.V2D(), 0, value.V2D(), 0);
    }
}

void JitTranslator::EmitVecFCmp(const ir::Inst* inst) {
    auto left = GetVecScalarOperand(inst->GetArg<ir::Value>(0), inst->GetArg<ir::Imm>(2).Get());
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1),
                                     inst->GetArg<ir::Imm>(2).Get());
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool compact = IsCompactFCmp(ir::Value{inst});
    // FCMP overwrites host NZCV.  Guest decoding normally puts an AdvancePC
    // boundary immediately before us; keep the IR opcode safe on its own too.
    MergeNZCV();
    FlushFlags();
    if (bits == 32) {
        __ Fcmp(left.S(), right.S());
    } else {
        __ Fcmp(left.D(), right.D());
    }

    if (compact) {
        if (RawFCmpCondition(inst)) {
            return;
        }
        // Preserve the one relation AXFLAG discards.  VC is ordered, which is
        // also the raw parity byte representation: 1 has odd parity (PF=0),
        // while unordered produces 0 (PF=1).
        if (CanUseCompactFCmpCarrier(inst)) {
            const u32 begin = context.CurrentBufferSize();
            __ Cset(flags.W(), vc);
            RecordPFAFDensity(PFAFDensityKind::SharedPack, begin);
        } else {
            __ Cset(context.X(ir::Value{inst}), vc);
        }
        return;
    }

    if (RawFCmpCondition(inst)) {
        return;
    }

    auto result = context.X(ir::Value{inst});

    // ARM FPCompare NZCV: less=N, equal=Z, greater=C, unordered=C|V.
    // x86 UCOMIS flags are CF=less|unordered, PF=unordered,
    // ZF=equal|unordered.
    auto bit = context.GetTmpX();
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

// The predicate is a relation set -- bit 0 = less, 1 = equal, 2 = greater,
// 3 = unordered; see the comment on VecFCmpMask in ir/ir.inc.
//
// All 16 sets are reachable, but only eight sequences are needed: a set and
// its complement give complementary masks, so `m` in 8..15 is emitted as the
// sequence for `15 - m` (which is always in 0..7) followed by one MVN.  That
// pairing is what makes the table below total by construction rather than by
// sixteen hand-checked cases -- 15-8=7 (ordered -> unordered), 15-9=6
// (ge -> !ge), 15-11=4 (gt -> !gt), 15-15=0 (never -> always), and so on.
//
// AArch64 FCMEQ/FCMGT/FCMGE are ORDERED: they give false for a NaN operand,
// which is what x86's ordered predicates want.  `unordered` has no direct
// instruction and is built from FCMEQ(x, x), false exactly for a NaN.  These
// primitives may raise FPSR.IOC where x86 would not; SwiftVM propagates no FP
// exception state either way, which is the same reason the front end drops
// AVX's signalling-vs-quiet dimension (frontend/x86/fp_cmp_predicate.h).
void JitTranslator::EmitVecFCmpMask(const ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 relations = inst->GetArg<ir::Imm>(3).Get() & 15;
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    const bool invert = relations >= 8;
    const u32 predicate = invert ? 15 - relations : relations;
    auto compare = scalar ? context.GetTmpV() : result;
    auto ordered_left = context.GetTmpV();
    auto ordered_right = context.GetTmpV();
    auto format = [bits](const VRegister& value) {
        return bits == 32 ? value.V4S() : value.V2D();
    };
    switch (predicate) {
        case 0:  // {} -- never; with invert, {<,==,>,unord} -- always.
            __ Movi(compare.V16B(), 0);
            break;
        case 1:  // {<}
            __ Fcmgt(format(compare), format(right), format(left));
            break;
        case 2:  // {==}
            __ Fcmeq(format(compare), format(left), format(right));
            break;
        case 3:  // {<, ==}
            __ Fcmge(format(compare), format(right), format(left));
            break;
        case 4:  // {>}
            __ Fcmgt(format(compare), format(left), format(right));
            break;
        case 5:  // {<, >} -- ordered-and-not-equal.  Two ordered compares are
                 // cheaper than "ordered AND not equal", which needs three.
            __ Fcmgt(format(ordered_left), format(right), format(left));
            __ Fcmgt(format(ordered_right), format(left), format(right));
            __ Orr(compare.V16B(), ordered_left.V16B(), ordered_right.V16B());
            break;
        case 6:  // {==, >}
            __ Fcmge(format(compare), format(left), format(right));
            break;
        case 7:  // {<, ==, >} -- ordered; with invert, unordered.
            __ Fcmeq(format(ordered_left), format(left), format(left));
            __ Fcmeq(format(ordered_right), format(right), format(right));
            __ And(compare.V16B(), ordered_left.V16B(), ordered_right.V16B());
            break;
    }
    if (invert) {
        __ Mvn(compare.V16B(), compare.V16B());
    }
    if (scalar) {
        __ Orr(result.V16B(), left.V16B(), left.V16B());
        if (bits == 32)
            __ Ins(result.V4S(), 0, compare.V4S(), 0);
        else
            __ Ins(result.V2D(), 0, compare.V2D(), 0);
    }
}

void JitTranslator::EmitVecFCvtIntToFloat(const ir::Inst* inst) {
    auto source = context.X(inst->GetArg<ir::Value>(0));
    const u32 src_bits = inst->GetArg<ir::Imm>(1).Get();
    const u32 dst_bits = inst->GetArg<ir::Imm>(2).Get();
    if (const auto* recipe = context.GetActions().ScalarFPRConversion(inst->Id())) {
        auto fp = VRegister::GetQRegFromCode(recipe->target);
        if (dst_bits == 32) {
            if (src_bits == 32)
                __ Scvtf(fp.S(), source.W());
            else
                __ Scvtf(fp.S(), source);
            if (recipe->ExportsResult()) {
                __ Fmov(context.W(ir::Value{inst}), fp.S());
            }
        } else {
            if (src_bits == 32)
                __ Scvtf(fp.D(), source.W());
            else
                __ Scvtf(fp.D(), source);
            if (recipe->ExportsResult()) {
                __ Fmov(context.X(ir::Value{inst}), fp.D());
            }
        }
        return;
    }

    auto result = context.X(ir::Value{inst});
    auto fp = context.GetTmpV();
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

void JitTranslator::EmitFRINTTSFloatToInt(const Register& result,
                                          const VRegister& source,
                                          u32 src_bits,
                                          u32 dst_bits,
                                          bool host_rounding) {
    auto rounded = context.GetTmpV();
    if (dst_bits == 32) {
        if (src_bits == 32) {
            if (host_rounding) __ Frint32x(rounded.S(), source.S());
            else __ Frint32z(rounded.S(), source.S());
            __ Fcvtzs(result.W(), rounded.S());
        } else {
            if (host_rounding) __ Frint32x(rounded.D(), source.D());
            else __ Frint32z(rounded.D(), source.D());
            __ Fcvtzs(result.W(), rounded.D());
        }
    } else {
        if (src_bits == 32) {
            if (host_rounding) __ Frint64x(rounded.S(), source.S());
            else __ Frint64z(rounded.S(), source.S());
            __ Fcvtzs(result, rounded.S());
        } else {
            if (host_rounding) __ Frint64x(rounded.D(), source.D());
            else __ Frint64z(rounded.D(), source.D());
            __ Fcvtzs(result, rounded.D());
        }
    }
}

void JitTranslator::EmitVecFCvtFloatToInt(const ir::Inst* inst) {
    auto source_value = inst->GetArg<ir::Value>(0);
    auto result = context.X(ir::Value{inst});
    const u32 src_bits = inst->GetArg<ir::Imm>(1).Get();
    const u32 dst_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool host_rounding = inst->GetArg<ir::Imm>(3).Get() != 0;
    auto fp = GetVecScalarOperand(source_value, src_bits);

    if (True(context.GetConfig().arm64_features & Arm64Features::FRINTTS)) {
        EmitFRINTTSFloatToInt(result, fp, src_bits, dst_bits, host_rounding);
        return;
    }

    auto bound = context.GetTmpV();
    auto bound_bits = context.GetTmpX();
    auto converted = context.GetTmpX();
    auto invalid = context.GetTmpX();
    auto test = context.GetTmpX();
    auto indefinite = context.GetTmpX();
    if (dst_bits == 32) {
        if (src_bits == 32) {
            if (host_rounding) __ Fcvtns(converted.W(), fp.S());
            else __ Fcvtzs(converted.W(), fp.S());
        } else {
            if (host_rounding) __ Fcvtns(converted.W(), fp.D());
            else __ Fcvtzs(converted.W(), fp.D());
        }
    } else {
        if (src_bits == 32) {
            if (host_rounding) __ Fcvtns(converted, fp.S());
            else __ Fcvtzs(converted, fp.S());
        } else {
            if (host_rounding) __ Fcvtns(converted, fp.D());
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

void JitTranslator::EmitVecFCvtScalar(const ir::Inst* inst) {
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

void JitTranslator::EmitVecFCvtPacked(const ir::Inst* inst) {
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

// VecFRoundInt -- round each lane to an integral floating-point value.
//
// The four IR modes map one-for-one onto the AArch64 FRINT family, so this is
// a single instruction per half.  Deliberately NOT FRINTX/FRINTI: those follow
// FPCR, and the IR's mode is an immediate precisely so that the result cannot
// depend on host state the front end never set.
//
// With FPCR.DN clear, FRINT quiets a NaN while preserving its payload and
// sign. Its existing bitwise Rosetta tests remain distinct from the ordinary
// arithmetic native-NaN policy above.
void JitTranslator::EmitVecFRoundInt(const ir::Inst* inst) {
    auto source = context.V(inst->GetArg<ir::Value>(0));
    auto merge = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 mode = inst->GetArg<ir::Imm>(3).Get();
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    ASSERT(bits == 32 || bits == 64);
    auto value = context.GetTmpV();
    auto out = VecLaneFormat(value, bits);
    auto in = VecLaneFormat(source, bits);
    switch (mode) {
        case 0:
            __ Frintn(out, in);  // nearest, ties to even
            break;
        case 1:
            __ Frintm(out, in);  // toward -infinity
            break;
        case 2:
            __ Frintp(out, in);  // toward +infinity
            break;
        case 3:
            __ Frintz(out, in);  // toward zero
            break;
        default:
            PANIC("invalid rounding mode");
    }
    // Same merge shape as EmitVecFUnary: lane 0 from the rounded value, the
    // rest of the destination from `merge`.
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

#undef __

}  // namespace swift::runtime::backend::arm64
