#include "translator.h"

#include <stdexcept>

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

bool JitTranslator::EmitVectorInteger(ir::Inst* inst) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::Vec4Mul: case O::VecMul: case O::VecMulHigh16: case O::VecMulWiden:
        case O::VecCmpEq: case O::VecCmpGt: case O::VecMin: case O::VecMax:
        case O::VecSatAdd: case O::VecSatSub: case O::VecPack: break;
        default: return false;
    }
    const u32 bits = op == O::Vec4Mul ? 32 : op == O::VecMulHigh16 ? 16 : inst->GetArg<ir::Imm>(2).Get();
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) throw std::runtime_error("invalid RV64 vector lane width");
    const bool signed_lanes = op == O::VecCmpGt ||
            (op == O::VecMulHigh16 ? inst->GetArg<ir::Imm>(2).Get() != 0 :
             op == O::VecMin || op == O::VecMax || op == O::VecSatAdd || op == O::VecSatSub || op == O::VecMulWiden
                     ? inst->GetArg<ir::Imm>(3).Get() != 0 : false);
    const bool pack = op == O::VecPack, widen = op == O::VecMulWiden;
    if ((pack && bits == 8) || (widen && bits == 64)) throw std::runtime_error("invalid RV64 vector narrowing/widening width");
    auto& as = context.GetMasm();
    if (context.Features().vector) {
        const auto result = context.ResultVector(inst);
        const auto left = context.SourceVector(inst->GetArg<ir::Value>(0), v1);
        const auto right = context.SourceVector(inst->GetArg<ir::Value>(1), v2);
        if (pack || widen) {
            // Narrowing instructions have a double-width source group, so
            // copy to even scratch registers regardless of cached source id.
            context.SetVectorType(64, 2); as.VMV(v24, left); as.VMV(v26, right);
            if (pack) {
                context.SetVectorType(bits, 128 / bits);
                const bool unsigned_destination = inst->GetArg<ir::Imm>(3).Get() != 0;
                if (unsigned_destination) { as.VMAX(v24, v24, x0); as.VMAX(v26, v26, x0); }
                context.SetVectorType(bits / 2, 128 / bits);
                if (unsigned_destination) { as.VNCLIPU(v4, v24, 0u); as.VNCLIPU(v5, v26, 0u); }
                else { as.VNCLIP(v4, v24, 0u); as.VNCLIP(v5, v26, 0u); }
                context.SetVectorType(64, 2); as.VMV(result, v4); as.VSLIDEUP(result, v5, 1u);
            } else {
                context.SetVectorType(bits, 64 / bits);
                as.VNSRL(v4, v24, 0u); as.VNSRL(v5, v26, 0u);
                if (signed_lanes) as.VWMUL(v28, v4, v5); else as.VWMULU(v28, v4, v5);
                context.SetVectorType(64, 2); as.VMV(result, v28);
            }
        } else {
            context.SetVectorType(bits, 128 / bits);
            switch (op) {
                case O::Vec4Mul: case O::VecMul: as.VMUL(result, left, right); break;
                case O::VecMulHigh16:
                    if (signed_lanes) as.VMULH(result, left, right); else as.VMULHU(result, left, right); break;
                case O::VecCmpEq: case O::VecCmpGt:
                    if (op == O::VecCmpEq) as.VMSEQ(v0, left, right); else as.VMSGT(v0, left, right);
                    as.VMV(result, 0); as.VMERGE(result, result, -1); break;
                case O::VecMin:
                    if (signed_lanes) as.VMIN(result, left, right); else as.VMINU(result, left, right); break;
                case O::VecMax:
                    if (signed_lanes) as.VMAX(result, left, right); else as.VMAXU(result, left, right); break;
                case O::VecSatAdd:
                    if (signed_lanes) as.VSADD(result, left, right); else as.VSADDU(result, left, right); break;
                case O::VecSatSub:
                    if (signed_lanes) as.VSSUB(result, left, right); else as.VSSUBU(result, left, right); break;
                default: UNREACHABLE();
            }
        }
        context.WriteVector(inst, result); return true;
    }
    const auto result = context.ResultPair(inst);
    constexpr std::array inputs{a0, a1, a2, a3};
    if (context.PairCoalescingEnabled()) {
        context.ReadPart(a0, inst->GetArg<ir::Value>(0), 0); context.ReadPart(a1, inst->GetArg<ir::Value>(0), 1);
        context.ReadPart(a2, inst->GetArg<ir::Value>(1), 0); context.ReadPart(a3, inst->GetArg<ir::Value>(1), 1);
    }
    const u32 output_bits = pack ? bits / 2 : widen ? bits * 2 : bits;
    const auto lane = [&](GPR target, ir::Value input, u32 bit, bool sign) {
        const auto source = context.PairCoalescingEnabled()
                ? inputs[(input.Def() == inst->GetArg<ir::Value>(0).Def() ? 0 : 2) + bit / 64]
                : context.SourcePart(input, bit / 64, target);
        if (bit % 64) as.SRLI(target, source, bit % 64); else if (source != target) as.MV(target, source);
        if (sign) context.SignExtend(target, bits); else context.Mask(target, bits);
    };
    // Clamp with static limits outside the per-lane arithmetic. RISC-V Zbb
    // uses MIN/MAX; the RV64G path uses nearby branches without helper calls.
    const bool clamp = pack || op == O::VecSatAdd || op == O::VecSatSub;
    const bool clamp_signed = pack || signed_lanes;
    const bool unsigned_destination = pack && inst->GetArg<ir::Imm>(3).Get() != 0;
    const u64 minimum = unsigned_destination || !clamp_signed ? 0 : u64{0} - (u64{1} << (output_bits - 1));
    const u64 maximum = unsigned_destination || !clamp_signed
            ? output_bits == 64 ? UINT64_MAX : (u64{1} << output_bits) - 1
            : (u64{1} << (output_bits - 1)) - 1;
    if (clamp) { as.LI(a4, minimum); as.LI(a5, maximum); }
    for (u32 part = 0; part < 2; ++part) {
        as.MV(result[part], x0);
        for (u32 local_bit = 0; local_bit < 64; local_bit += output_bits) {
            const u32 output_bit = part * 64 + local_bit;
            if (pack) lane(t3, inst->GetArg<ir::Value>(part), local_bit * 2, true);
            else {
                lane(t3, inst->GetArg<ir::Value>(0), output_bit, signed_lanes);
                lane(t4, inst->GetArg<ir::Value>(1), output_bit, signed_lanes);
            }
            if (op == O::Vec4Mul || op == O::VecMul || widen || op == O::VecMulHigh16) {
                as.MUL(t2, t3, t4);
                if (op == O::VecMulHigh16) as.SRLI(t2, t2, 16);
            } else if (op == O::VecCmpEq) { as.XOR(t2, t3, t4); as.SEQZ(t2, t2); as.NEG(t2, t2); }
            else if (op == O::VecCmpGt) { as.SLT(t2, t4, t3); as.NEG(t2, t2); }
            else if (op == O::VecMin || op == O::VecMax) {
                if (context.Features().zbb) {
                    if (op == O::VecMin) { if (signed_lanes) as.MIN(t2, t3, t4); else as.MINU(t2, t3, t4); }
                    else { if (signed_lanes) as.MAX(t2, t3, t4); else as.MAXU(t2, t3, t4); }
                } else {
                    Label selected, done;
                    const auto first = op == O::VecMin ? t3 : t4, second = op == O::VecMin ? t4 : t3;
                    if (signed_lanes) as.BLT(first, second, &selected); else as.BLTU(first, second, &selected);
                    as.MV(t2, t4); as.J(&done); as.Bind(&selected); as.MV(t2, t3); as.Bind(&done);
                }
            } else {
                if (pack) as.MV(t2, t3);
                else if (op == O::VecSatAdd) as.ADD(t2, t3, t4); else as.SUB(t2, t3, t4);
                if (!pack && bits == 64) {
                    Label unchanged;
                    if (signed_lanes) {
                        as.XOR(a6, t2, t3); as.XOR(a7, t3, t4);
                        if (op == O::VecSatAdd) as.NOT(a7, a7);
                        as.AND(a6, a6, a7); as.BGE(a6, x0, &unchanged);
                        as.SRAI(a6, t3, 63); as.XOR(t2, a5, a6);
                    } else if (op == O::VecSatAdd) { as.BGEU(t2, t3, &unchanged); as.MV(t2, a5); }
                    else { as.BGEU(t3, t4, &unchanged); as.MV(t2, x0); }
                    as.Bind(&unchanged);
                } else if (!clamp_signed && op == O::VecSatSub) {
                    Label nonnegative; as.BGEU(t3, t4, &nonnegative); as.MV(t2, x0); as.Bind(&nonnegative);
                } else if (context.Features().zbb) {
                    if (clamp_signed) { as.MAX(t2, t2, a4); as.MIN(t2, t2, a5); }
                    else as.MINU(t2, t2, a5);
                } else {
                    Label low, high, done;
                    if (clamp_signed) as.BLT(t2, a4, &low);
                    if (clamp_signed) as.BLT(a5, t2, &high); else as.BLTU(a5, t2, &high);
                    as.J(&done); as.Bind(&low); as.MV(t2, a4); as.J(&done);
                    as.Bind(&high); as.MV(t2, a5); as.Bind(&done);
                }
            }
            context.Mask(t2, output_bits);
            if (local_bit) as.SLLI(t2, t2, local_bit);
            as.OR(result[part], result[part], t2);
        }
    }
    context.WritePair(inst, result); return true;
}

}  // namespace swift::runtime::backend::riscv64
