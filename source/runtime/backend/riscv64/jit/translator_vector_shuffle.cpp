#include "translator.h"

#include <stdexcept>

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

bool JitTranslator::EmitVectorShuffle(ir::Inst* inst) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::VecByteShift: case O::VecShuffle32: case O::VecShuffle32TwoSrc:
        case O::VecShuffle32Indexed: case O::VecShuffle16: case O::VecExtractBytes:
        case O::VecZip: case O::VecUnzip: case O::VecDupPairs32: case O::VecMovMask:
        case O::VecTableLookup8: case O::VecAbsDiffSum8: case O::VecMadd16: break;
        default: return false;
    }
    auto& as = context.GetMasm();
    const auto input = inst->GetArg<ir::Value>(0);
    const bool two = op == O::VecShuffle32TwoSrc || op == O::VecShuffle32Indexed ||
            op == O::VecExtractBytes || op == O::VecZip || op == O::VecUnzip ||
            op == O::VecTableLookup8 || op == O::VecAbsDiffSum8 || op == O::VecMadd16;
    const bool dynamic = op == O::VecTableLookup8 || op == O::VecShuffle32Indexed;
    const bool shift = op == O::VecByteShift || op == O::VecExtractBytes;
    const bool mask = op == O::VecMovMask;
    const bool sad = op == O::VecAbsDiffSum8, madd = op == O::VecMadd16;
    u32 bits = op == O::VecShuffle16 ? 16 : op == O::VecShuffle32 || op == O::VecShuffle32TwoSrc ||
            op == O::VecDupPairs32 ? 32 : op == O::VecZip || op == O::VecUnzip
            ? inst->GetArg<ir::Imm>(2).Get() : mask ? inst->GetArg<ir::Imm>(1).Get() : 8;
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
        throw std::runtime_error("invalid RV64 shuffle lane width");
    const u32 count = shift ? inst->GetArg<ir::Imm>(2).Get() : 0;
    if (shift && (count == 0 || count >= 16)) throw std::runtime_error("invalid RV64 vector byte shift");
    const bool left_shift = op == O::VecByteShift && inst->GetArg<ir::Imm>(3).Get() != 0;
    const u32 lanes = 128 / bits, half = lanes / 2;
    std::array<u32, 16> indices{}, sources{};
    if (!dynamic && !shift && !mask && !sad && !madd) {
        for (u32 lane = 0; lane < lanes; ++lane) {
            if (op == O::VecShuffle32 || op == O::VecShuffle32TwoSrc) {
                const u32 control = inst->GetArg<ir::Imm>(two ? 2 : 1).Get();
                indices[lane] = (control >> (lane * 2)) & 3;
                sources[lane] = two && lane >= 2;
            } else if (op == O::VecShuffle16) {
                const u32 base = inst->GetArg<ir::Imm>(2).Get() ? 4 : 0;
                indices[lane] = lane >= base && lane < base + 4
                        ? base + ((inst->GetArg<ir::Imm>(1).Get() >> ((lane - base) * 2)) & 3) : lane;
            } else if (op == O::VecDupPairs32) {
                indices[lane] = (lane & ~1u) + (inst->GetArg<ir::Imm>(1).Get() != 0);
            } else if (op == O::VecZip) {
                indices[lane] = lane / 2 + (inst->GetArg<ir::Imm>(3).Get() ? half : 0);
                sources[lane] = lane & 1;
            } else {
                indices[lane] = (lane % half) * 2 + (inst->GetArg<ir::Imm>(3).Get() != 0);
                sources[lane] = lane >= half;
            }
        }
    }
    if (context.Features().vector) {
        const auto scalar_result = mask ? context.ResultRegister(inst) : x0;
        const auto pair_result = sad ? context.ResultPair(inst) : std::array<GPR, 2>{x0, x0};
        const auto result = !mask && !sad ? context.ResultVector(inst) : v3;
        const auto source = context.SourceVector(input, v1);
        const auto other = two ? context.SourceVector(inst->GetArg<ir::Value>(1), v2) : v2;
        if (mask) {
            context.SetVectorType(bits, lanes);
            as.VMSLT(v0, source, x0);
            context.SetVectorType(64, 2); as.VMV_XS(scalar_result, v0);
            context.Mask(scalar_result, lanes); context.Write(inst, scalar_result, true); return true;
        }
        if (sad) {
            context.SetVectorType(8, 16);
            as.VMAXU(v4, source, other); as.VMINU(v5, source, other); as.VSUB(v4, v4, v5);
            as.VMV(v5, 0); as.VSLIDEDOWN(v6, v4, 8u);
            context.SetVectorType(8, 8);
            as.VWREDSUMU(v7, v4, v5);
            context.SetVectorType(16, 1); as.VMV_XS(pair_result[0], v7);
            context.SetVectorType(8, 8); as.VWREDSUMU(v7, v6, v5);
            context.SetVectorType(16, 1); as.VMV_XS(pair_result[1], v7);
            context.WritePair(inst, pair_result); return true;
        }
        if (madd) {
            context.SetVectorType(64, 2); as.VMV(v24, source); as.VMV(v26, other);
            context.SetVectorType(16, 4); as.VNSRL(v4, v24, 0u); as.VNSRL(v5, v26, 0u);
            as.VWMUL(v28, v4, v5);
            context.SetVectorType(32, 4); as.VSRL(v24, v24, 16u); as.VSRL(v26, v26, 16u);
            context.SetVectorType(16, 4); as.VNSRL(v4, v24, 0u); as.VNSRL(v5, v26, 0u);
            as.VWMUL(v30, v4, v5);
            context.SetVectorType(32, 4); as.VADD(result, v28, v30);
        } else if (shift) {
            context.SetVectorType(8, 16);
            if (op == O::VecExtractBytes) {
                as.VSLIDEDOWN(result, source, count); as.VSLIDEUP(result, other, 16 - count);
            } else {
                as.VMV(result, 0);
                if (left_shift) as.VSLIDEUP(result, source, count);
                else {
                    // Slide-down can read past VL up to VLMAX. Mask out those
                    // lanes explicitly so a VLEN=256 host cannot leak tails.
                    as.VID(v4); as.VMSLTU(v0, v4, int(16 - count));
                    as.VSLIDEDOWN(result, source, count, VecMask::Yes);
                }
            }
        } else if (dynamic) {
            context.SetVectorType(8, 16);
            if (op == O::VecTableLookup8) {
                as.VAND(v4, other, 15); as.VMSGE(v0, other, 0);
            } else { as.VMV(v4, other); as.VMSLTU(v0, other, 16); }
            as.VMV(result, 0); as.VRGATHER(result, source, v4, VecMask::Yes);
        } else {
            if (op == O::VecZip || op == O::VecUnzip || op == O::VecDupPairs32) {
                context.SetVectorType(bits, lanes); as.VID(v4);
                if (op == O::VecZip) {
                    as.VSRL(v4, v4, 1u);
                    if (inst->GetArg<ir::Imm>(3).Get()) as.VADD(v4, v4, int(half));
                } else if (op == O::VecUnzip) {
                    as.VAND(v4, v4, int(half - 1)); as.VSLL(v4, v4, 1u);
                    if (inst->GetArg<ir::Imm>(3).Get()) as.VADD(v4, v4, 1);
                } else {
                    as.VAND(v4, v4, -2);
                    if (inst->GetArg<ir::Imm>(1).Get()) as.VADD(v4, v4, 1);
                }
            } else {
                std::array<u64, 2> packed{};
                for (u32 lane = 0; lane < lanes; ++lane) packed[lane * bits / 64] |= u64(indices[lane]) << (lane * bits % 64);
                context.SetVectorType(64, 2); as.LI(a0, packed[0]); as.LI(a1, packed[1]);
                as.VMV(v5, a1); as.VSLIDE1UP(v4, v5, a0);
                context.SetVectorType(bits, lanes);
            }
            if (two) {
                as.VRGATHER(v5, source, v4); as.VRGATHER(v6, other, v4); as.VID(v4);
                if (op == O::VecZip) { as.VAND(v4, v4, 1); as.VMSNE(v0, v4, x0); }
                else as.VMSGEU(v0, v4, int(half));
                as.VMERGE(result, v5, v6);
            } else as.VRGATHER(result, source, v4);
        }
        context.WriteVector(inst, result); return true;
    }
    const auto scalar_result = mask ? context.ResultRegister(inst) : x0;
    const auto result = !mask ? context.ResultPair(inst) : std::array<GPR, 2>{x0, x0};
    context.ReadPart(a0, input, 0); context.ReadPart(a1, input, 1);
    if (two) { context.ReadPart(a2, inst->GetArg<ir::Value>(1), 0); context.ReadPart(a3, inst->GetArg<ir::Value>(1), 1); }
    constexpr std::array input_regs{a0, a1, a2, a3};
    if (mask) {
        as.MV(scalar_result, x0);
        for (u32 lane = 0; lane < lanes; ++lane) {
            as.SRLI(t2, input_regs[lane * bits / 64], lane * bits % 64 + bits - 1);
            as.ANDI(t2, t2, 1); if (lane) as.SLLI(t2, t2, lane); as.OR(scalar_result, scalar_result, t2);
        }
        context.Write(inst, scalar_result, true); return true;
    }
    if (shift) {
        const u32 amount = count * 8;
        if (op == O::VecExtractBytes) {
            if (amount == 64) { as.MV(result[0], a1); as.MV(result[1], a2); }
            else if (amount < 64) {
                as.SRLI(result[0], a0, amount); as.SLLI(t2, a1, 64 - amount); as.OR(result[0], result[0], t2);
                as.SRLI(result[1], a1, amount); as.SLLI(t2, a2, 64 - amount); as.OR(result[1], result[1], t2);
            } else {
                as.SRLI(result[0], a1, amount - 64); as.SLLI(t2, a2, 128 - amount); as.OR(result[0], result[0], t2);
                as.SRLI(result[1], a2, amount - 64); as.SLLI(t2, a3, 128 - amount); as.OR(result[1], result[1], t2);
            }
        } else if (amount >= 64) {
            as.MV(result[left_shift ? 0 : 1], x0);
            if (amount == 64) as.MV(result[left_shift ? 1 : 0], left_shift ? a0 : a1);
            else if (left_shift) as.SLLI(result[1], a0, amount - 64);
            else as.SRLI(result[0], a1, amount - 64);
        } else if (left_shift) {
            as.SLLI(result[0], a0, amount); as.SLLI(result[1], a1, amount);
            as.SRLI(t2, a0, 64 - amount); as.OR(result[1], result[1], t2);
        } else {
            as.SRLI(result[0], a0, amount); as.SRLI(result[1], a1, amount);
            as.SLLI(t2, a1, 64 - amount); as.OR(result[0], result[0], t2);
        }
    } else {
        for (u32 part = 0; part < 2; ++part) {
            as.MV(result[part], x0);
            for (u32 bit = 0; bit < 64; bit += madd ? 32 : bits) {
                const u32 lane = (part * 64 + bit) / bits;
                if (dynamic) {
                    as.SRLI(t3, input_regs[2 + part], bit); as.ANDI(t3, t3, 255);
                    Label zero, upper, done;
                    if (op == O::VecTableLookup8) { as.ANDI(t4, t3, 128); as.BNE(t4, x0, &zero); as.ANDI(t3, t3, 15); }
                    else { as.LI(t4, 16); as.BGEU(t3, t4, &zero); }
                    as.SLLI(t3, t3, 3); as.LI(t4, 64); as.BGEU(t3, t4, &upper);
                    as.SRL(t2, a0, t3); as.J(&done); as.Bind(&upper); as.SRL(t2, a1, t3); as.J(&done);
                    as.Bind(&zero); as.MV(t2, x0); as.Bind(&done); as.ANDI(t2, t2, 255);
                } else if (sad || madd) {
                    const auto extract = [&](GPR reg, GPR src, u32 offset, u32 width, bool sign) {
                        as.SRLI(reg, src, offset);
                        if (sign) context.SignExtend(reg, width); else context.Mask(reg, width);
                    };
                    extract(t3, input_regs[part], bit, madd ? 16 : 8, madd);
                    extract(t4, input_regs[2 + part], bit, madd ? 16 : 8, madd);
                    if (sad) {
                        as.SUB(t2, t3, t4); as.SRAI(t3, t2, 63); as.XOR(t2, t2, t3); as.SUB(t2, t2, t3);
                    } else {
                        as.MUL(t2, t3, t4);
                        extract(t3, input_regs[part], bit + 16, 16, true);
                        extract(t4, input_regs[2 + part], bit + 16, 16, true);
                        as.MUL(t3, t3, t4); as.ADD(t2, t2, t3); context.Mask(t2, 32);
                    }
                } else {
                    const u32 source_bit = indices[lane] * bits;
                    as.SRLI(t2, input_regs[sources[lane] * 2 + source_bit / 64], source_bit % 64);
                    context.Mask(t2, bits);
                }
                if (sad) as.ADD(result[part], result[part], t2);
                else { if (bit) as.SLLI(t2, t2, bit); as.OR(result[part], result[part], t2); }
            }
        }
    }
    context.WritePair(inst, result); return true;
}

}  // namespace swift::runtime::backend::riscv64
