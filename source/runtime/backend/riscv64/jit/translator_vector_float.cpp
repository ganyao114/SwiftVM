#include "translator.h"

#include <stdexcept>

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

bool JitTranslator::EmitVectorFloat(ir::Inst* inst) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::VecFAddScalar32: case O::VecFSubScalar32: case O::VecFMulScalar32: case O::VecFDivScalar32:
        case O::VecFAddScalar64: case O::VecFSubScalar64: case O::VecFMulScalar64: case O::VecFDivScalar64:
        case O::VecFAdd: case O::VecFSub: case O::VecFMul: case O::VecFDiv:
        case O::VecFMinMax: case O::VecFUnary: case O::VecFCmp: case O::VecFCmpMask:
        case O::VecFMulAdd: case O::VecFRoundInt:
        case O::VecFCvtIntToFloat: case O::VecFCvtFloatToInt: case O::VecFCvtScalar: case O::VecFCvtPacked: break;
        default: return false;
    }
    auto& as = context.GetMasm();
    const bool cvt = op == O::VecFCvtIntToFloat || op == O::VecFCvtFloatToInt || op == O::VecFCvtScalar || op == O::VecFCvtPacked;
    const bool scalar32 = op >= O::VecFAddScalar32 && op <= O::VecFDivScalar32;
    const bool scalar64 = op >= O::VecFAddScalar64 && op <= O::VecFDivScalar64;
    const bool unary = op == O::VecFUnary, round = op == O::VecFRoundInt, fma = op == O::VecFMulAdd;
    const bool minmax = op == O::VecFMinMax, cmp = op == O::VecFCmp, cmp_mask = op == O::VecFCmpMask;
    const u32 bits = cvt ? op == O::VecFCvtPacked ? 32 : inst->GetArg<ir::Imm>(1).Get() : scalar32 ? 32 : scalar64 ? 64 :
            inst->GetArg<ir::Imm>(fma ? 3 : 2).Get();
    if (bits != 32 && bits != 64) throw std::runtime_error("invalid RV64 floating lane width");
    const auto precision = bits == 32 ? Precision::S : Precision::D;
    const auto move_to_fp = [&](FPR target, GPR source, u32 width) {
        if (width == 32) as.FMV_W_X(target, source); else as.FMV_D_X(target, source);
    };
    const auto move_from_fp = [&](GPR target, FPR source, u32 width) {
        if (width == 32) { as.FMV_X_W(target, source); context.Mask(target, 32); }
        else as.FMV_X_D(target, source);
    };
    const auto scalar_float_conversion = [&](GPR target, GPR raw, u32 input_bits) {
        move_to_fp(ft1, raw, input_bits);
        if (input_bits == 32) as.FCVT_D_S(ft0, ft1, RMode::RNE); else as.FCVT_S_D(ft0, ft1, RMode::RNE);
        const u32 output_bits = 96 - input_bits;
        move_from_fp(target, ft0, output_bits);
        as.FCLASS(t3, ft1, input_bits == 32 ? Precision::S : Precision::D);
        as.ANDI(t3, t3, 768); Label finite; as.BEQ(t3, x0, &finite);
        // FCVT canonicalizes NaNs. Reconstruct the converted sign/payload,
        // quieting signalling NaNs exactly like the guest SSE conversion.
        if (input_bits == 32) {
            as.SLLI(t2, raw, 41); as.SRLI(t2, t2, 12);
            as.SRLI(t3, raw, 31); as.SLLI(t3, t3, 63);
            as.LI(t4, 0x7ff8000000000000ULL);
        } else {
            as.SRLI(t2, raw, 29); context.Mask(t2, 23);
            as.SRLI(t3, raw, 32); as.LI(t4, 0x80000000); as.AND(t3, t3, t4);
            as.LI(t4, 0x7fc00000);
        }
        as.OR(target, t2, t3); as.OR(target, target, t4); as.Bind(&finite);
    };
    const auto int_to_fp = [&](GPR target, GPR raw, u32 input_bits, u32 output_bits) {
        if (output_bits == 32) {
            if (input_bits == 32) as.FCVT_S_W(ft0, raw, RMode::RNE); else as.FCVT_S_L(ft0, raw, RMode::RNE);
        } else {
            if (input_bits == 32) as.FCVT_D_W(ft0, raw, RMode::RNE); else as.FCVT_D_L(ft0, raw, RMode::RNE);
        }
        move_from_fp(target, ft0, output_bits);
    };
    const auto fp_to_int = [&](GPR target, GPR raw, u32 input_bits, u32 output_bits, bool nearest) {
        move_to_fp(ft1, raw, input_bits);
        const auto rounding = nearest ? RMode::RNE : RMode::RTZ;
        if (output_bits == 32) {
            if (input_bits == 32) as.FCVT_W_S(target, ft1, rounding); else as.FCVT_W_D(target, ft1, rounding);
        } else {
            if (input_bits == 32) as.FCVT_L_S(target, ft1, rounding); else as.FCVT_L_D(target, ft1, rounding);
        }
        // Negative overflow already produces INT_MIN. Positive overflow and
        // NaN saturate to INT_MAX on RISC-V, but SSE requires INT_MIN.
        const u64 upper = input_bits == 32 ? output_bits == 32 ? 0x4f000000 : 0x5f000000 :
                output_bits == 64 ? 0x43e0000000000000ULL : nearest ? 0x41dfffffffe00000ULL : 0x41e0000000000000ULL;
        as.LI(t5, upper); move_to_fp(ft2, t5, input_bits);
        as.FLE(t3, ft2, ft1, input_bits == 32 ? Precision::S : Precision::D);
        as.FCLASS(t4, ft1, input_bits == 32 ? Precision::S : Precision::D);
        as.ANDI(t4, t4, 768); as.OR(t3, t3, t4);
        Label valid; as.BEQ(t3, x0, &valid); as.LI(target, u64{1} << (output_bits - 1)); as.Bind(&valid);
        context.Mask(target, output_bits);
    };
    if (cvt && op != O::VecFCvtPacked) {
        const bool vector_result = ir::IsFloatValueType(inst->ReturnType());
        const auto result = vector_result ? x0 : context.ResultRegister(inst);
        const auto pair = vector_result ? context.ResultPair(inst) : std::array<GPR, 2>{x0, x0};
        const auto low = vector_result ? pair[0] : result;
        context.Read(a0, inst->GetArg<ir::Value>(0));
        if (op == O::VecFCvtScalar) scalar_float_conversion(low, a0, bits);
        else if (op == O::VecFCvtIntToFloat) int_to_fp(low, a0, bits, inst->GetArg<ir::Imm>(2).Get());
        else fp_to_int(low, a0, bits, inst->GetArg<ir::Imm>(2).Get(), inst->GetArg<ir::Imm>(3).Get() != 0);
        if (vector_result) { as.MV(pair[1], x0); context.WritePair(inst, pair); }
        else context.Write(inst, result);
        return true;
    }
    if (cmp) {
        const auto result = context.ResultRegister(inst);
        context.Read(a0, inst->GetArg<ir::Value>(0)); context.Read(a1, inst->GetArg<ir::Value>(1));
        move_to_fp(ft1, a0, bits); move_to_fp(ft2, a1, bits);
        as.FEQ(result, ft1, ft2, precision); as.SLLI(result, result, 2);
        as.FLT(t2, ft1, ft2, precision); as.OR(result, result, t2);
        as.FCLASS(t2, ft1, precision); as.FCLASS(t3, ft2, precision); as.OR(t2, t2, t3); as.ANDI(t2, t2, 768);
        Label ordered; as.BEQ(t2, x0, &ordered); as.LI(result, 7); as.Bind(&ordered);
        context.Write(inst, result, true); return true;
    }
    const bool scalar = scalar32 || scalar64 || ((minmax || unary || cmp_mask || round) && inst->GetArg<ir::Imm>(4).Get() != 0);
    const u32 lanes = scalar ? 1 : 128 / bits;
    const u64 quiet = bits == 32 ? 0x00400000 : 0x0008000000000000ULL;
    const u64 sign = u64{1} << (bits - 1);
    const u32 relations = cmp_mask ? inst->GetArg<ir::Imm>(3).Get() & 15 : 0;
    const u32 kind = unary || round ? inst->GetArg<ir::Imm>(3).Get() : 0;
    const u32 fma_flags = fma ? inst->GetArg<ir::Imm>(4).Get() & 3 : 0;
    const bool add = op == O::VecFAdd || op == O::VecFAddScalar32 || op == O::VecFAddScalar64;
    const bool sub = op == O::VecFSub || op == O::VecFSubScalar32 || op == O::VecFSubScalar64;
    const bool mul = op == O::VecFMul || op == O::VecFMulScalar32 || op == O::VecFMulScalar64;
    const auto round_mode = kind == 0 ? RMode::RNE : kind == 1 ? RMode::RDN : kind == 2 ? RMode::RUP : RMode::RTZ;
    if (context.Features().vector && op == O::VecFCvtPacked) {
        const u32 conversion = inst->GetArg<ir::Imm>(1).Get();
        if (conversion > 7) throw std::runtime_error("invalid RV64 packed floating conversion");
        const auto result = context.ResultVector(inst);
        const auto source = context.SourceVector(inst->GetArg<ir::Value>(0), v1);
        if (conversion == 4 || conversion == 5 || conversion == 7) { context.SetVectorType(64, 2); as.VMV(result, 0); }
        if (conversion == 0 || conversion == 1) {
            context.SetVectorType(32, conversion == 0 ? 4 : 2);
            if (conversion == 0) as.VFCVT_F_X(result, source);
            else { as.VFWCVT_F_X(v24, source); context.SetVectorType(64, 2); as.VMV(result, v24); }
        } else if (conversion >= 2 && conversion <= 5) {
            const bool wide = conversion >= 4, nearest = conversion == 2 || conversion == 4;
            const u32 width = wide ? 64 : 32;
            context.SetVectorType(width, 128 / width);
            if (wide) as.VMV(v24, source);
            as.LI(a0, wide ? nearest ? 0x41dfffffffe00000ULL : 0x41e0000000000000ULL : 0x4f000000ULL);
            move_to_fp(ft0, a0, width);
            as.VMFGE(v0, source, ft0); as.VMFNE(v4, source, source); as.VMOR(v0, v0, v4);
            if (wide) {
                context.SetVectorType(32, 2);
                if (nearest) as.VFNCVT_X_F(result, v24); else as.VFNCVT_RTZ_X_F(result, v24);
            } else if (nearest) as.VFCVT_X_F(result, source);
            else as.VFCVT_RTZ_X_F(result, source);
            as.LI(a0, 0x80000000); as.VMERGE(result, result, a0);
        } else if (conversion == 6) {
            context.SetVectorType(32, 2); as.VMFNE(v0, source, source); as.VFWCVT_F_F(v24, source);
            context.SetVectorType(64, 2); as.VMV(result, v24);
            as.VZEXTVF2(v26, source); as.LI(a0, 0x7fffff); as.VAND(v24, v26, a0); as.VSLL(v24, v24, 29u);
            as.VSRL(v26, v26, 31u); as.LI(a0, 63); as.VSLL(v26, v26, a0); as.VOR(v24, v24, v26);
            as.LI(a0, 0x7ff8000000000000ULL); as.VOR(v24, v24, a0); as.VMERGE(result, result, v24);
        } else {
            context.SetVectorType(64, 2); as.VMV(v24, source); as.VMFNE(v0, source, source);
            as.VSRL(v26, source, 29u); as.LI(a0, 32); as.VSRL(v28, source, a0);
            context.SetVectorType(32, 2); as.VFNCVT_F_F(result, v24);
            as.VNSRL(v4, v26, 0u); as.VNSRL(v5, v28, 0u);
            as.LI(a0, 0x7fffff); as.VAND(v4, v4, a0); as.LI(a0, 0x80000000); as.VAND(v5, v5, a0);
            as.VOR(v4, v4, v5); as.LI(a0, 0x7fc00000); as.VOR(v4, v4, a0); as.VMERGE(result, result, v4);
        }
        context.WriteVector(inst, result); return true;
    }
    if (context.Features().vector && op != O::VecFCvtPacked) {
        const auto result = context.ResultVector(inst);
        const auto left = context.SourceVector(inst->GetArg<ir::Value>(0), v1);
        const auto right = context.SourceVector(inst->GetArg<ir::Value>(1), v2);
        const auto third = fma ? context.SourceVector(inst->GetArg<ir::Value>(2), v4) : v4;
        if (scalar) { context.SetVectorType(64, 2); as.VMV(result, unary || round ? right : left); }
        context.SetVectorType(bits, lanes);
        if (minmax) {
            if (inst->GetArg<ir::Imm>(3).Get()) as.VMFLT(v0, right, left); else as.VMFLT(v0, left, right);
            // Strict comparison selects the right operand for equality,
            // signed-zero ties and unordered input, preserving its raw bits.
            as.VMERGE(result, right, left);
        } else if (cmp_mask) {
            const bool invert = __builtin_popcount(relations) > 2;
            const u32 selected = invert ? relations ^ 15 : relations;
            bool initialized{};
            const auto append = [&](Vec mask) { if (!initialized) { as.VMMV(v0, mask); initialized = true; } else as.VMOR(v0, v0, mask); };
            if (selected & 1) { as.VMFLT(v5, left, right); append(v5); }
            if (selected & 2) { as.VMFEQ(v5, left, right); append(v5); }
            if (selected & 4) { as.VMFLT(v5, right, left); append(v5); }
            if (selected & 8) { as.VMFNE(v5, left, left); as.VMFNE(v6, right, right); as.VMOR(v5, v5, v6); append(v5); }
            if (!initialized) as.VMXOR(v0, v0, v0);
            if (invert) as.VMNOT(v0, v0);
            as.VMV(result, 0); as.VMERGE(result, result, -1);
        } else if (round) {
            as.VMV(result, left); as.VFABS(v4, left);
            as.LI(a0, bits == 32 ? 0x4b000000 : 0x4330000000000000ULL); as.VMSLTU(v0, v4, a0);
            if (kind) as.FSRMI(u32(round_mode));
            as.VFCVT_X_F(v5, left, VecMask::Yes); as.VFCVT_F_X(result, v5, VecMask::Yes);
            if (kind) context.EnterFloatMode();
            as.VFSGNJ(result, result, left);
            as.VMFNE(v0, left, left); as.LI(a0, quiet); as.VOR(result, left, a0, VecMask::Yes);
        } else {
            if (fma) {
                as.VMV(result, third);
                if (fma_flags == 0) as.VFMACC(result, left, right);
                else if (fma_flags == 1) as.VFNMSAC(result, left, right);
                else if (fma_flags == 2) as.VFMSAC(result, left, right);
                else as.VFNMACC(result, left, right);
            } else if (unary) {
                if (kind == 0 || bits == 64) as.VFSQRT(result, left);
                else {
                    as.LI(a0, 0x3f800000); as.FMV_W_X(ft0, a0);
                    if (kind == 1) as.VFRDIV(result, left, ft0);
                    else { as.VFSQRT(v5, left); as.VFRDIV(result, v5, ft0); }
                }
            } else if (add) as.VFADD(result, left, right);
            else if (sub) as.VFSUB(result, left, right);
            else if (mul) as.VFMUL(result, left, right);
            else as.VFDIV(result, left, right);
            // RISC-V returns a canonical positive NaN. SSE propagates the
            // earliest input NaN's payload; invalid arithmetic uses -QNaN.
            if (!unary || kind == 0 || bits == 64) {
                as.VMFNE(v0, result, result); as.LI(a0, sign); as.VOR(result, result, a0, VecMask::Yes);
            }
            as.LI(a0, quiet);
            const auto propagate = [&](Vec source) { as.VMFNE(v0, source, source); as.VOR(v5, source, a0); as.VMERGE(result, result, v5); };
            if (fma) propagate(third);
            if (!unary) propagate(right);
            propagate(left);
        }
        context.WriteVector(inst, result); return true;
    }
    const auto result = context.ResultPair(inst);
    context.ReadPart(a0, inst->GetArg<ir::Value>(0), 0); context.ReadPart(a1, inst->GetArg<ir::Value>(0), 1);
    if (op != O::VecFCvtPacked) { context.ReadPart(a2, inst->GetArg<ir::Value>(1), 0); context.ReadPart(a3, inst->GetArg<ir::Value>(1), 1); }
    constexpr std::array left_regs{a0, a1}, right_regs{a2, a3};
    const u32 packed_kind = op == O::VecFCvtPacked ? inst->GetArg<ir::Imm>(1).Get() : 0;
    const u32 source_bits = op == O::VecFCvtPacked && (packed_kind == 4 || packed_kind == 5 || packed_kind == 7) ? 64 : bits;
    const u32 output_bits = op == O::VecFCvtPacked && (packed_kind == 1 || packed_kind == 6) ? 64 : bits;
    const u32 actual_lanes = op == O::VecFCvtPacked ? (source_bits == 64 || output_bits == 64 ? 2 : 4) : lanes;
    for (u32 part = 0; part < 2; ++part) as.MV(result[part], scalar ? unary || round ? right_regs[part] : left_regs[part] : x0);
    for (u32 lane = 0; lane < actual_lanes; ++lane) {
        as.SRLI(a4, left_regs[lane * source_bits / 64], lane * source_bits % 64); context.Mask(a4, source_bits);
        if (op == O::VecFCvtPacked) {
            if (packed_kind == 0 || packed_kind == 1) int_to_fp(t2, a4, 32, output_bits);
            else if (packed_kind >= 2 && packed_kind <= 5) fp_to_int(t2, a4, source_bits, 32, packed_kind == 2 || packed_kind == 4);
            else scalar_float_conversion(t2, a4, source_bits);
        } else {
            as.SRLI(a5, right_regs[lane * bits / 64], lane * bits % 64); context.Mask(a5, bits);
            move_to_fp(ft1, a4, bits); move_to_fp(ft2, a5, bits);
            if (fma) { context.ReadPart(a6, inst->GetArg<ir::Value>(2), lane * bits / 64); if (lane * bits % 64) as.SRLI(a6, a6, lane * bits % 64); context.Mask(a6, bits); move_to_fp(ft3, a6, bits); }
            if (minmax) {
                if (inst->GetArg<ir::Imm>(3).Get()) as.FLT(t3, ft2, ft1, precision); else as.FLT(t3, ft1, ft2, precision);
                Label other, done; as.BEQ(t3, x0, &other); as.MV(t2, a4); as.J(&done); as.Bind(&other); as.MV(t2, a5); as.Bind(&done);
            } else if (cmp_mask) {
                as.FCLASS(t3, ft1, precision); as.FCLASS(t4, ft2, precision); as.OR(t3, t3, t4); as.ANDI(t3, t3, 768);
                Label unordered, matched; as.BNE(t3, x0, &unordered);
                as.FEQ(t2, ft1, ft2, precision); as.FLT(t3, ft1, ft2, precision);
                // Ordered relation index: less=0, equal=1, greater=2.
                as.LI(t4, 2); as.SUB(t4, t4, t2); as.SLLI(t3, t3, 1); as.SUB(t4, t4, t3);
                as.LI(t2, relations); as.SRL(t2, t2, t4); as.ANDI(t2, t2, 1); as.J(&matched);
                as.Bind(&unordered); as.LI(t2, (relations >> 3) & 1); as.Bind(&matched); as.NEG(t2, t2); context.Mask(t2, bits);
            } else if (round) {
                Label unchanged, rounded;
                as.FCLASS(t3, ft1, precision); as.ANDI(t4, t3, 768); as.BNE(t4, x0, &unchanged);
                as.LI(t3, ~sign); as.AND(t3, a4, t3); as.LI(t4, bits == 32 ? 0x4b000000 : 0x4330000000000000ULL);
                as.BGEU(t3, t4, &unchanged);
                if (bits == 32) { as.FCVT_W_S(t2, ft1, round_mode); as.FCVT_S_W(ft0, t2, RMode::RNE); }
                else { as.FCVT_L_D(t2, ft1, round_mode); as.FCVT_D_L(ft0, t2, RMode::RNE); }
                as.FSGNJ(ft0, ft0, ft1, precision); move_from_fp(t2, ft0, bits); as.J(&rounded);
                as.Bind(&unchanged); as.MV(t2, a4);
                as.FCLASS(t3, ft1, precision); as.ANDI(t3, t3, 768); Label not_nan; as.BEQ(t3, x0, &not_nan);
                as.LI(t4, quiet); as.OR(t2, t2, t4); as.Bind(&not_nan); as.Bind(&rounded);
            } else {
                if (fma) {
                    if (fma_flags == 0) as.FMADD(ft0, ft1, ft2, ft3, precision, RMode::RNE);
                    else if (fma_flags == 1) as.FNMSUB(ft0, ft1, ft2, ft3, precision, RMode::RNE);
                    else if (fma_flags == 2) as.FMSUB(ft0, ft1, ft2, ft3, precision, RMode::RNE);
                    else as.FNMADD(ft0, ft1, ft2, ft3, precision, RMode::RNE);
                } else if (unary) {
                    if (kind == 0 || bits == 64) as.FSQRT(ft0, ft1, precision, RMode::RNE);
                    else {
                        as.LI(t2, 0x3f800000); as.FMV_W_X(ft2, t2);
                        if (kind == 1) as.FDIV_S(ft0, ft2, ft1, RMode::RNE);
                        else { as.FSQRT_S(ft0, ft1, RMode::RNE); as.FDIV_S(ft0, ft2, ft0, RMode::RNE); }
                    }
                } else if (add) as.FADD(ft0, ft1, ft2, precision, RMode::RNE);
                else if (sub) as.FSUB(ft0, ft1, ft2, precision, RMode::RNE);
                else if (mul) as.FMUL(ft0, ft1, ft2, precision, RMode::RNE);
                else as.FDIV(ft0, ft1, ft2, precision, RMode::RNE);
                move_from_fp(t2, ft0, bits);
                as.FCLASS(t3, ft0, precision); as.ANDI(t3, t3, 768); Label no_invalid; as.BEQ(t3, x0, &no_invalid);
                if (!unary || kind == 0 || bits == 64) { as.LI(t4, sign); as.OR(t2, t2, t4); }
                as.Bind(&no_invalid);
                const auto propagate = [&](FPR fp, GPR raw) {
                    as.FCLASS(t3, fp, precision); as.ANDI(t3, t3, 768); Label finite; as.BEQ(t3, x0, &finite);
                    as.LI(t4, quiet); as.OR(t2, raw, t4); as.Bind(&finite);
                };
                if (fma) propagate(ft3, a6);
                if (!unary) propagate(ft2, a5);
                propagate(ft1, a4);
            }
        }
        const u32 bit = lane * output_bits, local_bit = bit % 64;
        if (scalar) { as.LI(t3, ~(output_bits == 64 ? UINT64_MAX : u64{UINT32_MAX} << local_bit)); as.AND(result[bit / 64], result[bit / 64], t3); }
        if (local_bit) as.SLLI(t2, t2, local_bit);
        as.OR(result[bit / 64], result[bit / 64], t2);
    }
    context.WritePair(inst, result); return true;
}

}  // namespace swift::runtime::backend::riscv64
