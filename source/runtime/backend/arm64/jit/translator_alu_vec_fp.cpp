#include "translator.h"

#include <algorithm>
#include <cstring>
#include <functional>

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

bool JitTranslator::UseAFPNaN(ir::Inst* inst) const {
    if (!sse_afp_nan || !inst) {
        return false;
    }

    // P1 is deliberately an opcode-and-shape allowlist. Do not infer finite
    // values or admit a neighbouring FP opcode merely because AH happens to
    // improve its behaviour. Scalar binary opcodes encode their element type
    // in the opcode; packed binary and unary shapes carry it as an immediate.
    switch (inst->GetOp()) {
        case ir::OpCode::VecFAddScalar32:
        case ir::OpCode::VecFSubScalar32:
        case ir::OpCode::VecFMulScalar32:
        case ir::OpCode::VecFDivScalar32:
        case ir::OpCode::VecFAddScalar64:
        case ir::OpCode::VecFSubScalar64:
        case ir::OpCode::VecFMulScalar64:
        case ir::OpCode::VecFDivScalar64:
            return true;
        case ir::OpCode::VecFAdd:
        case ir::OpCode::VecFSub:
        case ir::OpCode::VecFMul:
        case ir::OpCode::VecFDiv: {
            const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
            return lane_bits == 32 || lane_bits == 64;
        }
        case ir::OpCode::VecFUnary: {
            const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
            const u32 kind = inst->GetArg<ir::Imm>(3).Get();
            const u32 scalar = inst->GetArg<ir::Imm>(4).Get();
            return kind == 0 && (lane_bits == 32 || lane_bits == 64) &&
                   scalar <= 1;
        }
        default:
            // FMA, MIN/MAX, COMIS, RCP and RSQRT stay on their existing
            // correction/lowering paths until separately proven.
            return false;
    }
}

VRegister JitTranslator::PreserveNaNColdSource(ir::Inst* inst,
                                                const VRegister& source,
                                                const VRegister& result,
                                                const VRegister& reserved) {
    if (UseAFPNaN(inst)) {
        return source;
    }
    if (sse_nan_coldpath && source.GetCode() == result.GetCode()) {
        __ Orr(reserved.V16B(), source.V16B(), source.V16B());
        return reserved;
    }
    return source;
}

void JitTranslator::QueueVecNaNColdPath(VecNaNColdKind kind,
                                        const VRegister& result,
                                        const VRegister& left,
                                        const VRegister& right) {
    ASSERT_MSG(!(context.GetFeatures().fpr_ipv_reclaim && sse_afp_nan),
               "AFP FPR reclamation reached the v11-v14 NaN cold ABI");
    auto site = std::make_unique<VecNaNColdSite>(
            VecNaNColdSite{kind, left, right, result});

    // For add/sub/mul/div, this one predicate is the exact OR of all reasons
    // the repair is needed:
    //
    //   isnan(result) ==
    //       isnan(left) || isnan(right) || operation_was_invalid
    //
    // FSQRT has the analogous identity with a NaN input or a negative finite
    // input. The host operation therefore performs both input and generated-
    // NaN detection for us, with a single conditional branch at the site.
    const bool scalar =
            kind == VecNaNColdKind::BinaryScalar32 ||
            kind == VecNaNColdKind::BinaryScalar64 ||
            kind == VecNaNColdKind::SqrtScalar32 ||
            kind == VecNaNColdKind::SqrtScalar64;
    const bool bits32 =
            kind == VecNaNColdKind::BinaryScalar32 ||
            kind == VecNaNColdKind::BinaryPacked32 ||
            kind == VecNaNColdKind::SqrtScalar32 ||
            kind == VecNaNColdKind::SqrtPacked32;

    if (scalar) {
        // FCMP owns NZCV. Commit a preceding lazy x86 flag producer before
        // using it as the cold predicate; the FP opcode itself defines no x86
        // integer flags.
        MergeNZCV();
        context.BeginHotNaNGuard(2);
        if (bits32) {
            __ Fcmp(result.S(), result.S());
        } else {
            __ Fcmp(result.D(), result.D());
        }
        __ B(site->slow.get(), vs);
        context.EndHotNaNGuard();
    } else if (bits32) {
        // FCMEQ produces all ones for every ordered lane. UMINV reduces that
        // to zero iff at least one lane is NaN, while leaving NZCV untouched.
        context.BeginHotNaNGuard(4);
        __ Fcmeq(ipv3.V4S(), result.V4S(), result.V4S());
        __ Uminv(ipv3.S(), ipv3.V4S());
        const auto ordered = context.GetSharedTmpX();
        __ Fmov(ordered.W(), ipv3.S());
        __ Cbz(ordered.W(), site->slow.get());
        context.EndHotNaNGuard();
    } else {
        context.BeginHotNaNGuard(5);
        __ Fcmeq(ipv3.V2D(), result.V2D(), result.V2D());
        const auto lane0 =
                backend::ScratchXPoolEnabled(context.GetFeatures()) ? context.GetTmpX() : ip0;
        const auto lane1 =
                backend::ScratchXPoolEnabled(context.GetFeatures()) ? context.GetTmpX() : ip1;
        __ Umov(lane0, ipv3.V2D(), 0);
        __ Umov(lane1, ipv3.V2D(), 1);
        __ And(lane0, lane0, lane1);
        __ Cbz(lane0, site->slow.get());
        context.EndHotNaNGuard();
    }

    __ Bind(site->continuation.get());
    vec_nan_cold_sites.push_back(std::move(site));
}

void JitTranslator::EmitVecNaNColdHandler(VecNaNColdKind kind) {
    const bool binary =
            kind == VecNaNColdKind::BinaryScalar32 ||
            kind == VecNaNColdKind::BinaryScalar64 ||
            kind == VecNaNColdKind::BinaryPacked32 ||
            kind == VecNaNColdKind::BinaryPacked64;
    const bool scalar =
            kind == VecNaNColdKind::BinaryScalar32 ||
            kind == VecNaNColdKind::BinaryScalar64 ||
            kind == VecNaNColdKind::SqrtScalar32 ||
            kind == VecNaNColdKind::SqrtScalar64;
    const bool bits32 =
            kind == VecNaNColdKind::BinaryScalar32 ||
            kind == VecNaNColdKind::BinaryPacked32 ||
            kind == VecNaNColdKind::SqrtScalar32 ||
            kind == VecNaNColdKind::SqrtPacked32;
    const u32 lane_bits = bits32 ? 32 : 64;
    auto bytes = [scalar](const VRegister& v) {
        return scalar ? v.V8B() : v.V16B();
    };
    auto ordered = [&](const VRegister& d, const VRegister& s) {
        if (scalar) {
            if (bits32) {
                __ Fcmeq(d.S(), s.S(), s.S());
            } else {
                __ Fcmeq(d.D(), s.D(), s.D());
            }
        } else if (bits32) {
            __ Fcmeq(d.V4S(), s.V4S(), s.V4S());
        } else {
            __ Fcmeq(d.V2D(), s.V2D(), s.V2D());
        }
    };
    auto splat = [&](const VRegister& d, bool indefinite) {
        if (indefinite) {
            __ Mvni(d.V4S(), lane_bits == 32 ? 0x3F : 0x07, MSL, 16);
        } else {
            __ Movi(d.V4S(), lane_bits == 32 ? 0x00400000u : 0x00080000u);
        }
        if (lane_bits == 64) {
            __ Shl(d.V2D(), d.V2D(), 32);
        }
    };
    auto insert_scalar = [&](const VRegister& dst, const VRegister& src) {
        if (bits32) {
            __ Ins(dst.V4S(), 0, src.V4S(), 0);
        } else {
            __ Ins(dst.V2D(), 0, src.V2D(), 0);
        }
    };

    if (binary) {
        // Cold ABI:
        //   v11 = original operand 1
        //   v12 = original operand 2, then selected/quieted replacement
        //   v13 = hardware result, then repaired result
        //   v14 = ordered mask
        ordered(ipv3, ipv0);
        __ Bif(bytes(ipv1), bytes(ipv0), bytes(ipv3));
        ordered(ipv3, ipv1);
        if (bits32) {
            __ Orr(ipv1.V4S(), 0x40, 16);
        } else {
            splat(ipv0, false);
            __ Orr(bytes(ipv1), bytes(ipv1), bytes(ipv0));
        }
        splat(ipv0, true);
        __ Bit(bytes(ipv1), bytes(ipv0), bytes(ipv3));
        if (scalar) {
            insert_scalar(ipv2, ipv1);
        } else {
            ordered(ipv3, ipv2);
            __ Bif(bytes(ipv2), bytes(ipv1), bytes(ipv3));
        }
    } else {
        // Unary SQRT selects a quieted source NaN, or x86's negative
        // indefinite when an ordered (therefore negative finite) input made
        // FSQRT return NaN. Valid lanes retain the hardware result.
        ordered(ipv3, ipv0);
        if (bits32) {
            __ Orr(ipv0.V4S(), 0x40, 16);
        } else {
            splat(ipv1, false);
            __ Orr(bytes(ipv0), bytes(ipv0), bytes(ipv1));
        }
        splat(ipv1, true);
        __ Bit(bytes(ipv0), bytes(ipv1), bytes(ipv3));
        if (scalar) {
            insert_scalar(ipv2, ipv0);
        } else {
            ordered(ipv3, ipv2);
            __ Bif(bytes(ipv2), bytes(ipv0), bytes(ipv3));
        }
    }
    __ Br(atomic_pair_scratch);
}

void JitTranslator::EmitVecNaNColdPaths() {
    if (vec_nan_cold_sites.empty()) {
        return;
    }

    constexpr size_t kKindCount = 8;
    std::array<Label, kKindCount> handlers;
    std::array<bool, kKindCount> used{};
    auto index = [](VecNaNColdKind kind) {
        return static_cast<size_t>(kind);
    };
    auto is_binary = [](VecNaNColdKind kind) {
        return kind == VecNaNColdKind::BinaryScalar32 ||
               kind == VecNaNColdKind::BinaryScalar64 ||
               kind == VecNaNColdKind::BinaryPacked32 ||
               kind == VecNaNColdKind::BinaryPacked64;
    };

    // Normal execution has already terminated before this point. Each cold
    // veneer copies the site-specific physical registers into the fixed ABI,
    // calls one shared repair body, writes the repaired value back, and
    // resumes immediately after that site's test.
    for (auto& site : vec_nan_cold_sites) {
        const auto i = index(site->kind);
        used[i] = true;
        __ Bind(site->slow.get());
        if (site->left.GetCode() != ipv0.GetCode()) {
            __ Orr(ipv0.V16B(), site->left.V16B(), site->left.V16B());
        }
        if (is_binary(site->kind) && site->right.GetCode() != ipv1.GetCode()) {
            __ Orr(ipv1.V16B(), site->right.V16B(), site->right.V16B());
        }
        __ Orr(ipv2.V16B(), site->result.V16B(), site->result.V16B());
        __ Adr(atomic_pair_scratch, site->repaired.get());
        __ B(&handlers[i]);
        __ Bind(site->repaired.get());
        __ Orr(site->result.V16B(), ipv2.V16B(), ipv2.V16B());
        __ B(site->continuation.get());
    }

    for (size_t i = 0; i < kKindCount; ++i) {
        if (!used[i]) {
            continue;
        }
        __ Bind(&handlers[i]);
        EmitVecNaNColdHandler(static_cast<VecNaNColdKind>(i));
    }
    vec_nan_cold_sites.clear();
}

void JitTranslator::EmitVecFAddScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    auto scalar = context.GetTmpV();
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 32);
    auto repair_left = PreserveNaNColdSource(inst, left, scalar, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, scalar, ipv1);
    __ Fadd(scalar.S(), left.S(), right.S());
    EmitVecFloatNaNFixup(scalar, repair_left, repair_right, 32, 1, inst);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFSubScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    auto scalar = context.GetTmpV();
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 32);
    auto repair_left = PreserveNaNColdSource(inst, left, scalar, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, scalar, ipv1);
    __ Fsub(scalar.S(), left.S(), right.S());
    EmitVecFloatNaNFixup(scalar, repair_left, repair_right, 32, 1, inst);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFMulScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    auto scalar = context.GetTmpV();
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 32);
    auto repair_left = PreserveNaNColdSource(inst, left, scalar, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, scalar, ipv1);
    __ Fmul(scalar.S(), left.S(), right.S());
    EmitVecFloatNaNFixup(scalar, repair_left, repair_right, 32, 1, inst);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFDivScalar32(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 32);
        return;
    }
    auto scalar = context.GetTmpV();
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 32);
    auto repair_left = PreserveNaNColdSource(inst, left, scalar, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, scalar, ipv1);
    __ Fdiv(scalar.S(), left.S(), right.S());
    EmitVecFloatNaNFixup(scalar, repair_left, repair_right, 32, 1, inst);
    __ Orr(result.V16B(), left.V16B(), left.V16B());
    __ Ins(result.V4S(), 0, scalar.V4S(), 0);
}

void JitTranslator::EmitVecFAddScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 64);
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    __ Fadd(result.D(), left.D(), right.D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, 64, 1, inst);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}

void JitTranslator::EmitVecFSubScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 64);
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    __ Fsub(result.D(), left.D(), right.D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, 64, 1, inst);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}

void JitTranslator::EmitVecFMulScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 64);
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    __ Fmul(result.D(), left.D(), right.D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, 64, 1, inst);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}

void JitTranslator::EmitVecFDivScalar64(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (sse_scalar_insert) {
        EmitVecFScalarBinaryTied(inst, 64);
        return;
    }
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1), 64);
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    __ Fdiv(result.D(), left.D(), right.D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, 64, 1, inst);
    __ Ins(result.V2D(), 1, left.V2D(), 1);
}



void JitTranslator::EmitVecFScalarBinaryTied(ir::Inst* inst, u32 lane_bits) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto right_value = inst->GetArg<ir::Value>(1);
    const bool right_is_vector = ir::IsFloatValueType(right_value.Type());
    auto right = right_is_vector ? context.V(right_value) : context.GetTmpV();
    if (result.GetCode() == left.GetCode()) {
        auto left_value = inst->GetArg<ir::Value>(0);
        while (left_value.Defined() && left_value.Def()->IsBitCastOperation()) {
            left_value = left_value.Def()->GetArg<ir::Value>(0);
        }
        if (sse_scalar_tie && left_value.Defined() && left_value.Def() &&
            left_value.Def()->GetOp() == ir::OpCode::GetHostFPR &&
            left_value.Def()->GetArg<ir::Imm>(0).Get() >= 16 &&
            context.IsHostReadCoalesced(left_value.Id())) {
            ASSERT_MSG(ReproveScalarFPRTie(inst),
                       "scalar FPR fixed-home tie proof diverged at IR {}", inst->Id());
        }
    } else {
        // A still-live dst-in cannot be tied by RA. Seed the new destination
        // once, then let NEP update lane 0 in place; this is still one copy
        // instead of the legacy post-op full-copy plus lane insert.
        __ Orr(result.V16B(), left.V16B(), left.V16B());
    }
    if (!right_is_vector) {
        if (lane_bits == 32) {
            __ Fmov(right.S(), context.W(right_value));
        } else {
            __ Fmov(right.D(), context.X(right_value));
        }
    }
    auto emit_operation = [&] {
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
    };
    if (UseAFPNaN(inst)) {
        emit_operation();
        return;
    }
    if (sse_nan_coldpath) {
        auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
        auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
        emit_operation();
        EmitVecFloatNaNFixup(result,
                             repair_left,
                             repair_right,
                             lane_bits,
                             1,
                             inst);
        return;
    }

    // Build the exact x86 NaN result before a tied operation overwrites
    // `left`. All bitwise work targets temporaries; only the rare NaN result
    // takes the lane insert below, so ordinary arithmetic has no merge path.
    auto ordered = context.GetTmpV();
    auto propagated = context.GetTmpV();
    auto tmp = context.GetTmpV();
    auto ordered_scalar = [&](const VRegister& d, const VRegister& s) {
        if (lane_bits == 32) {
            __ Fcmeq(d.S(), s.S(), s.S());
        } else {
            __ Fcmeq(d.D(), s.D(), s.D());
        }
    };
    auto bytes = [](const VRegister& v) { return v.V8B(); };
    auto splat = [&](const VRegister& d, bool indefinite) {
        if (indefinite) {
            __ Mvni(d.V4S(), lane_bits == 32 ? 0x3F : 0x07, MSL, 16);
        } else {
            __ Movi(d.V4S(), lane_bits == 32 ? 0x00400000u : 0x00080000u);
        }
        if (lane_bits == 64) {
            __ Shl(d.V2D(), d.V2D(), 32);
        }
    };

    ordered_scalar(ordered, left);
    __ Orr(bytes(propagated), bytes(right), bytes(right));
    __ Bif(bytes(propagated), bytes(left), bytes(ordered));
    ordered_scalar(ordered, propagated);
    if (lane_bits == 32) {
        __ Orr(propagated.V4S(), 0x40, 16);
    } else {
        splat(tmp, false);
        __ Orr(bytes(propagated), bytes(propagated), bytes(tmp));
    }
    splat(tmp, true);
    __ Bit(bytes(propagated), bytes(tmp), bytes(ordered));

    emit_operation();
    Label done;
    if (lane_bits == 32) {
        __ Fcmp(result.S(), result.S());
        __ B(&done, vc);
        __ Ins(result.V4S(), 0, propagated.V4S(), 0);
    } else {
        __ Fcmp(result.D(), result.D());
        __ B(&done, vc);
        __ Ins(result.V2D(), 0, propagated.V2D(), 0);
    }
    __ Bind(&done);
}

void JitTranslator::EmitVecFloatNaNFixup(const VRegister& result,
                                         const VRegister& left,
                                         const VRegister& right,
                                         u32 lane_bits,
                                         u32 lane_count,
                                         ir::Inst* inst) {
    if (UseAFPNaN(inst)) {
        return;
    }
    ASSERT(lane_bits == 32 || lane_bits == 64);
    if (sse_nan_coldpath) {
        QueueVecNaNColdPath(
                lane_count == 1
                        ? (lane_bits == 32
                                   ? VecNaNColdKind::BinaryScalar32
                                   : VecNaNColdKind::BinaryScalar64)
                        : (lane_bits == 32
                                   ? VecNaNColdKind::BinaryPacked32
                                   : VecNaNColdKind::BinaryPacked64),
                result,
                left,
                right);
        return;
    }
    // The scalar forms (lane_count == 1) define lane 0 only; their callers
    // merge the surviving lanes from `left` after this returns. Keeping the
    // bitwise ops 64-bit wide there is not an optimisation but a correctness
    // requirement for the 64-bit scalar shapes: writing the upper half of
    // `result` would destroy the very lane the caller is about to copy back
    // whenever RegAlloc gives `result` and `left` the same physical register.
    const bool scalar = lane_count == 1;
    auto bytes = [scalar](const VRegister& v) { return scalar ? v.V8B() : v.V16B(); };
    // isnan(x) == !(x == x). FCMEQ yields all-ones lanes where the compare
    // holds, so every mask below is the *complement* of "is NaN"; BIF/BIT read
    // that polarity directly and no NOT is needed anywhere.
    auto ordered = [&](const VRegister& d, const VRegister& s) {
        if (scalar) {
            if (lane_bits == 32) {
                __ Fcmeq(d.S(), s.S(), s.S());
            } else {
                __ Fcmeq(d.D(), s.D(), s.D());
            }
        } else {
            if (lane_bits == 32) {
                __ Fcmeq(d.V4S(), s.V4S(), s.V4S());
            } else {
                __ Fcmeq(d.V2D(), s.V2D(), s.V2D());
            }
        }
    };
    // Both constants are built with NEON modified immediates (one or two
    // instructions, no GPR): MOVI/MVNI cannot encode the 64-bit patterns
    // directly, but each is its 32-bit half shifted into place. Going through
    // MacroAssembler::Movi(vd, u64) instead would spill the value through
    // VIXL's scratch pool -- contract-leased, so correct, but a GPR round trip
    // and three instructions where these are two.
    auto splat = [&](const VRegister& d, bool indefinite) {
        if (indefinite) {
            // MSL: ~((imm8 << 16) | 0xFFFF).
            __ Mvni(d.V4S(), lane_bits == 32 ? 0x3F : 0x07, MSL, 16);
        } else {
            __ Movi(d.V4S(), lane_bits == 32 ? 0x00400000u : 0x00080000u);
        }
        if (lane_bits == 64) {
            __ Shl(d.V2D(), d.V2D(), 32);
        }
    };

    auto ordered_mask = context.GetTmpV();
    auto propagated = context.GetTmpV();
    auto tmp = context.GetTmpV();

    // x86 propagates operand 1's NaN in preference to operand 2's; ARM ranks
    // signalling NaNs above quiet ones instead, so the choice has to be redone
    // from the inputs rather than read off the hardware result.
    ordered(ordered_mask, left);
    __ Orr(bytes(propagated), bytes(right), bytes(right));
    __ Bif(bytes(propagated), bytes(left), bytes(ordered_mask));
    // `propagated` is a NaN exactly when one of the two inputs was (it *is*
    // one of them), so re-testing it costs one register less than keeping the
    // left mask alive and ANDing in a second one.
    ordered(ordered_mask, propagated);

    // Force the quiet bit on: a signalling input is forwarded as the matching
    // quiet NaN with its payload intact. The 32-bit bit pattern is an ORR
    // vector immediate; the 64-bit one is not encodable as one (the same
    // immediate would land in both halves of every lane and corrupt the
    // payload), so it goes through a register.
    if (lane_bits == 32) {
        __ Orr(propagated.V4S(), 0x40, 16);
    } else {
        splat(tmp, false);
        __ Orr(bytes(propagated), bytes(propagated), bytes(tmp));
    }

    // Lanes where the operation itself was invalid (inf-inf, 0*inf, 0/0, and
    // nothing else once neither input is a NaN) get x86's "real indefinite"
    // QNaN, which differs from ARM's default NaN in the sign bit.
    splat(tmp, true);
    __ Bit(bytes(propagated), bytes(tmp), bytes(ordered_mask));

    // Add/sub/mul/div return a NaN for *every* NaN input, so "the result is a
    // NaN" is exactly the set of lanes that needs one of the two substitutions
    // above; lanes that produced a number are already correct.
    ordered(ordered_mask, result);
    __ Bif(bytes(result), bytes(propagated), bytes(ordered_mask));
}

void JitTranslator::EmitVecFAdd(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    if (lane_bits == 32)
        __ Fadd(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fadd(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, lane_bits, 0, inst);
}

void JitTranslator::EmitVecFSub(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    if (lane_bits == 32)
        __ Fsub(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fsub(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, lane_bits, 0, inst);
}

void JitTranslator::EmitVecFMul(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    if (lane_bits == 32)
        __ Fmul(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fmul(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, lane_bits, 0, inst);
}

void JitTranslator::EmitVecFDiv(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    auto repair_left = PreserveNaNColdSource(inst, left, result, ipv0);
    auto repair_right = PreserveNaNColdSource(inst, right, result, ipv1);
    if (lane_bits == 32)
        __ Fdiv(result.V4S(), left.V4S(), right.V4S());
    else
        __ Fdiv(result.V2D(), left.V2D(), right.V2D());
    EmitVecFloatNaNFixup(result, repair_left, repair_right, lane_bits, 0, inst);
}

void JitTranslator::EmitVecFMinMax(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool maximum = inst->GetArg<ir::Imm>(3).Get() != 0;
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    if (sse_afp_minmax && scalar && result.GetCode() == left.GetCode()) {
        // AH selects operand 2 for unordered and equal inputs, matching x86
        // MIN/MAX including NaN payload and signed-zero selection.  NEP keeps
        // the tied destination's upper lanes intact.
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
    if (sse_scalar_insert && scalar) {
        // x86 selects operand 2 for unordered and equal. Keep dst-in as the
        // default, and insert operand 2 only when it wins.
        if (result.GetCode() != left.GetCode()) {
            __ Orr(result.V16B(), left.V16B(), left.V16B());
        }
        Label keep_left;
        if (bits == 32) {
            __ Fcmp(left.S(), right.S());
            __ B(&keep_left, maximum ? gt : mi);
            __ Ins(result.V4S(), 0, right.V4S(), 0);
        } else {
            __ Fcmp(left.D(), right.D());
            __ B(&keep_left, maximum ? gt : mi);
            __ Ins(result.V2D(), 0, right.V2D(), 0);
        }
        __ Bind(&keep_left);
        return;
    }
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

// Fused multiply-add: dst = +-(a*b) +- c with a SINGLE rounding (ir.inc).
//
// AArch64 has exactly the two accumulate forms, both fused:
//     FMLA Vd, Vn, Vm   ->  Vd = Vd + Vn*Vm
//     FMLS Vd, Vn, Vm   ->  Vd = Vd - Vn*Vm
// so the sign of the PRODUCT picks the mnemonic and the sign of the ADDEND is
// applied to the accumulator up front with FNEG.  Negating the addend before
// the fused step is exact for every finite value and for infinities, so it
// cannot introduce a rounding difference; it does flip the sign bit of a NaN
// addend, which is why the NaN fixup below reads the ORIGINAL c and not the
// negated accumulator.
//
// WHY THE NaN FIXUP IS VECTOR CODE AND NOT THE USUAL LANE LOOP
// ------------------------------------------------------------
// EmitVecFloatNaNFixup extracts every lane into GPRs; at three operands that
// would need half again as many live GPRs as the two-operand version, which
// already had to be cut from 16 temporaries to 8 to stop exhausting the pool.
// FCMEQ(x, x) is false exactly for NaN, so the whole predicate is available in
// the vector unit for one instruction per operand and this emitter needs no
// GPR at all beyond the two immediate materializations.
//
// The x86 rule being reproduced (same as VecFloatBinary / EmitVecFloatNaNFixup):
// a NaN source is returned quieted, earlier sources win over later ones, and an
// invalid operation with no NaN source (Inf*0, or Inf added to the opposite
// Inf) returns the QNaN indefinite -- whose sign bit is SET, unlike ARM's
// default NaN.  Source order here is the arithmetic order a, b, c.
void JitTranslator::EmitVecFMulAdd(ir::Inst* inst) {
    auto a = context.V(inst->GetArg<ir::Value>(0));
    auto b = context.V(inst->GetArg<ir::Value>(1));
    auto c = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(3).Get();
    const u32 flags = inst->GetArg<ir::Imm>(4).Get();
    const bool negate_product = (flags & 1u) != 0;
    const bool negate_addend = (flags & 2u) != 0;
    ASSERT(lane_bits == 32 || lane_bits == 64);

    auto acc = context.GetTmpV();
    auto ok = context.GetTmpV();      // lanes where every source is non-NaN
    auto mask = context.GetTmpV();    // scratch predicate
    auto value = context.GetTmpV();   // scratch replacement value
    auto imm = context.GetTmpX();

    const auto fmt_of = [&](const VRegister& v) { return lane_bits == 32 ? v.V4S() : v.V2D(); };

    // acc = (negate_addend ? -c : c), then the fused step.
    if (negate_addend) {
        __ Fneg(fmt_of(acc), fmt_of(c));
    } else {
        __ Orr(acc.V16B(), c.V16B(), c.V16B());
    }
    if (negate_product) {
        __ Fmls(fmt_of(acc), fmt_of(a), fmt_of(b));
    } else {
        __ Fmla(fmt_of(acc), fmt_of(a), fmt_of(b));
    }

    // ok = ~isnan(a) & ~isnan(b) & ~isnan(c)
    __ Fcmeq(fmt_of(ok), fmt_of(a), fmt_of(a));
    __ Fcmeq(fmt_of(mask), fmt_of(b), fmt_of(b));
    __ And(ok.V16B(), ok.V16B(), mask.V16B());
    __ Fcmeq(fmt_of(mask), fmt_of(c), fmt_of(c));
    __ And(ok.V16B(), ok.V16B(), mask.V16B());

    // A NaN produced by the operation itself (no NaN source) is x86's QNaN
    // indefinite.  mask = ok & isnan(acc).
    __ Fcmeq(fmt_of(mask), fmt_of(acc), fmt_of(acc));
    __ Bic(mask.V16B(), ok.V16B(), mask.V16B());
    if (lane_bits == 32) {
        __ Mov(imm.W(), 0xFFC00000u);
        __ Dup(value.V4S(), imm.W());
    } else {
        __ Mov(imm, UINT64_C(0xFFF8000000000000));
        __ Dup(value.V2D(), imm);
    }
    // BIT: result lanes selected by `mask` take `value`.
    __ Bit(acc.V16B(), value.V16B(), mask.V16B());

    // Quieted source propagation, applied lowest priority first so that a wins.
    if (lane_bits == 32) {
        __ Mov(imm.W(), 0x00400000u);
        __ Dup(value.V4S(), imm.W());
    } else {
        __ Mov(imm, UINT64_C(0x0008000000000000));
        __ Dup(value.V2D(), imm);
    }
    auto quiet = context.GetTmpV();
    __ Orr(quiet.V16B(), value.V16B(), value.V16B());
    for (const VRegister* source : {&c, &b, &a}) {
        __ Fcmeq(fmt_of(mask), fmt_of(*source), fmt_of(*source));
        __ Orr(value.V16B(), source->V16B(), quiet.V16B());
        // BIF: lanes where the predicate is FALSE (i.e. this source is NaN)
        // take `value`.
        __ Bif(acc.V16B(), value.V16B(), mask.V16B());
    }
    __ Orr(result.V16B(), acc.V16B(), acc.V16B());
}

void JitTranslator::EmitVecFUnary(ir::Inst* inst) {
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
            if (UseAFPNaN(inst)) {
                if (bits == 32) {
                    __ Fsqrt(result.S(), source.S());
                } else {
                    __ Fsqrt(result.D(), source.D());
                }
                return;
            }
            if (sse_nan_coldpath) {
                auto repair_source = source;
                if (source.GetCode() == result.GetCode()) {
                    repair_source = ipv0;
                    __ Orr(repair_source.V16B(), source.V16B(), source.V16B());
                }
                if (bits == 32) {
                    __ Fsqrt(result.S(), source.S());
                    QueueVecNaNColdPath(VecNaNColdKind::SqrtScalar32,
                                        result,
                                        repair_source);
                } else {
                    __ Fsqrt(result.D(), source.D());
                    QueueVecNaNColdPath(VecNaNColdKind::SqrtScalar64,
                                        result,
                                        repair_source);
                }
                return;
            }
            // Compare before FSQRT because source may be the tied result.
            // Negative finite inputs select x86's signed indefinite; NaN and
            // -0.0 retain the hardware result.
            if (bits == 32) {
                __ Fcmp(source.S(), 0.0);
                __ Fsqrt(result.S(), source.S());
                auto indef = context.GetTmpV();
                auto imm = context.GetTmpX();
                __ Mov(imm.W(), 0xFFC00000u);
                __ Fmov(indef.S(), imm.W());
                Label done;
                __ B(&done, pl);
                __ Ins(result.V4S(), 0, indef.V4S(), 0);
                __ Bind(&done);
            } else {
                __ Fcmp(source.D(), 0.0);
                __ Fsqrt(result.D(), source.D());
                auto indef = context.GetTmpV();
                auto imm = context.GetTmpX();
                __ Mov(imm, UINT64_C(0xFFF8000000000000));
                __ Fmov(indef.D(), imm);
                Label done;
                __ B(&done, pl);
                __ Ins(result.V2D(), 0, indef.V2D(), 0);
                __ Bind(&done);
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
            if (UseAFPNaN(inst)) {
                __ Fsqrt(value.V4S(), source.V4S());
            } else if (sse_nan_coldpath) {
                auto repair_source = source;
                if (source.GetCode() == value.GetCode()) {
                    repair_source = ipv0;
                    __ Orr(repair_source.V16B(), source.V16B(), source.V16B());
                }
                __ Fsqrt(value.V4S(), source.V4S());
                QueueVecNaNColdPath(
                        scalar ? VecNaNColdKind::SqrtScalar32
                               : VecNaNColdKind::SqrtPacked32,
                        value,
                        repair_source);
            } else {
                __ Fsqrt(value.V4S(), source.V4S());
                auto negative = context.GetTmpV();
                auto indef = context.GetTmpV();
                auto imm = context.GetTmpX();
                __ Fcmlt(negative.V4S(), source.V4S(), 0.0);
                __ Mov(imm.W(), 0xFFC00000u);
                __ Dup(indef.V4S(), imm.W());
                __ Bit(value.V16B(), indef.V16B(), negative.V16B());
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
    } else {
        if (UseAFPNaN(inst)) {
            __ Fsqrt(value.V2D(), source.V2D());
        } else if (sse_nan_coldpath) {
            auto repair_source = source;
            if (source.GetCode() == value.GetCode()) {
                repair_source = ipv0;
                __ Orr(repair_source.V16B(), source.V16B(), source.V16B());
            }
            __ Fsqrt(value.V2D(), source.V2D());
            QueueVecNaNColdPath(
                    scalar ? VecNaNColdKind::SqrtScalar64
                           : VecNaNColdKind::SqrtPacked64,
                    value,
                    repair_source);
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
    auto left = GetVecScalarOperand(inst->GetArg<ir::Value>(0),
                                    inst->GetArg<ir::Imm>(2).Get());
    auto right = GetVecScalarOperand(inst->GetArg<ir::Value>(1),
                                     inst->GetArg<ir::Imm>(2).Get());
    auto result = context.X(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool compact = inst->GetArg<ir::Imm>(3).Get() != 0;
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
        // Preserve the one relation AXFLAG discards.  VC is ordered, which is
        // also the raw parity byte representation: 1 has odd parity (PF=0),
        // while unordered produces 0 (PF=1).
        __ Cset(result, vc);
        return;
    }

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
void JitTranslator::EmitVecFCmpMask(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 relations = inst->GetArg<ir::Imm>(3).Get() & 15;
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    const bool invert = relations >= 8;
    const u32 predicate = invert ? 15 - relations : relations;
    auto compare = context.GetTmpV();
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

// VecFRoundInt -- round each lane to an integral floating-point value.
//
// The four IR modes map one-for-one onto the AArch64 FRINT family, so this is
// a single instruction per half.  Deliberately NOT FRINTX/FRINTI: those follow
// FPCR, and the IR's mode is an immediate precisely so that the result cannot
// depend on host state the front end never set.
//
// NO NaN FIXUP IS NEEDED HERE, unlike VecFAdd/VecFMul.  EmitVecFloatNaNFixup
// exists because ARM MANUFACTURES a default NaN (7FC0_0000, sign clear) where
// x86 manufactures its indefinite (FFC0_0000, sign set) -- inf-inf, 0*inf and
// friends.  FRINT* cannot manufacture anything: with FPCR.DN clear (this
// runtime never sets it) it returns the operand with the quiet bit forced on
// and every other bit, including the sign, untouched.  That is exactly what
// x86 ROUNDPS/ROUNDPD/ROUNDSS/ROUNDSD do with a NaN operand.  Verified against
// Rosetta on QNaN, SNaN and both signed zeroes -- see avx_misc_test.cpp.
void JitTranslator::EmitVecFRoundInt(ir::Inst* inst) {
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
