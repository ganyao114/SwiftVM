#include "emit.h"

namespace swift::runtime::backend::riscv64::detail {
namespace {
using O = OpCode;
using u64 = std::uint64_t;
constexpr std::array first{a0, a1}, second{a2, a3}, third{a4, a5}, result{a6, a7};

unsigned Immediate(OpContext& ctx, unsigned index) {
    return static_cast<unsigned>(ctx.Arg(index).Payload());
}

void CheckFloatWidth(Writer& writer, unsigned bits) {
    if (bits != 32 && bits != 64) writer.Reject(CompileError::UnsupportedOperand);
}

void ToFloat(Assembler& as, FPR destination, GPR input, unsigned bits) {
    if (bits == 32)
        as.FMV_W_X(destination, input);
    else
        as.FMV_D_X(destination, input);
}

void FromFloat(Writer& writer, GPR destination, FPR input, unsigned bits) {
    if (bits == 32) {
        writer.as.FMV_X_W(destination, input);
        writer.Mask(destination, 32);
    } else
        writer.as.FMV_X_D(destination, input);
}

void Lane(Writer& writer,
          GPR destination,
          const std::array<GPR, 2>& input,
          unsigned index,
          unsigned bits) {
    const unsigned bit = index * bits;
    writer.as.SRLI(destination, input[bit / 64], bit % 64);
    writer.Mask(destination, bits);
}

void Put(Writer& writer, GPR value, unsigned index, unsigned bits, bool merge = false) {
    const unsigned bit = index * bits;
    writer.Mask(value, bits);
    if (merge) {
        const auto mask = bits == 64 ? UINT64_MAX : u64{UINT32_MAX} << (bit % 64);
        writer.as.LI(t4, ~mask);
        writer.as.AND(result[bit / 64], result[bit / 64], t4);
    }
    if (bit % 64) writer.as.SLLI(value, value, bit % 64);
    writer.as.OR(result[bit / 64], result[bit / 64], value);
}

void Finish(Writer& writer, OpContext& ctx) {
    writer.as.MV(ctx.destination, a6);
    writer.as.MV(ctx.high, a7);
}

// SVM models numerical DAZ/FTZ explicitly; RV64 has no independent hardware
// controls for them. Only t4/t5 are scratch, retaining all source lane bits.
void Flush(Writer& writer, GPR raw, unsigned bits, unsigned control_bit, bool force = false) {
    if (!force && writer.options.mxcsr_offset == UINT32_MAX) return;
    auto& as = writer.as;
    Label complete;
    if (!force) {
        writer.Load(t4, s1, static_cast<std::int32_t>(writer.options.mxcsr_offset), 32);
        as.SRLI(t4, t4, control_bit);
        as.ANDI(t4, t4, 1);
        as.BEQ(t4, x0, &complete);
    }
    as.LI(t5, bits == 32 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000));
    as.AND(t4, raw, t5);
    as.BNE(t4, x0, &complete);
    as.LI(t5, u64{1} << (bits - 1));
    as.AND(raw, raw, t5);
    as.Bind(&complete);
}

void PropagateNaN(Writer& writer, GPR destination, FPR input, GPR raw, unsigned bits) {
    auto& as = writer.as;
    as.FCLASS(t4, input, bits == 32 ? Precision::S : Precision::D);
    as.ANDI(t4, t4, 768);
    Label finite;
    as.BEQ(t4, x0, &finite);
    as.LI(t5, u64{1} << (bits == 32 ? 22 : 51));
    as.OR(destination, raw, t5);
    as.Bind(&finite);
}

void RepairArithmeticNaN(Writer& writer, unsigned bits, bool binary, bool fused) {
    auto& as = writer.as;
    as.FCLASS(t4, ft0, bits == 32 ? Precision::S : Precision::D);
    as.ANDI(t4, t4, 768);
    Label numeric;
    as.BEQ(t4, x0, &numeric);
    // Invalid arithmetic with finite inputs has the guest's negative QNaN.
    as.LI(t5, u64{1} << (bits - 1));
    as.OR(t3, t3, t5);
    as.Bind(&numeric);
    // RISC-V canonicalizes NaNs. Recover the earliest input's sign/payload
    // rather than exposing the host's canonical NaN as the guest result.
    if (fused) PropagateNaN(writer, t3, ft3, t2, bits);
    if (binary) PropagateNaN(writer, t3, ft2, t1, bits);
    PropagateNaN(writer, t3, ft1, t0, bits);
}

void FloatToInteger(Writer& writer, unsigned float_bits, unsigned integer_bits, bool truncate) {
    auto& as = writer.as;
    CheckFloatWidth(writer, float_bits);
    if (integer_bits != 32 && integer_bits != 64) writer.Reject(CompileError::UnsupportedOperand);
    // The truncating IR contract consumes the raw input; other conversions
    // use the numerical DAZ control, as the existing ARM64 consumer does.
    if (!truncate) Flush(writer, t0, float_bits, 6);
    ToFloat(as, ft1, t0, float_bits);
    const auto rounding = truncate ? RMode::RTZ : RMode::DYN;
    as.FSFLAGSI(t6, 0);
    if (integer_bits == 32) {
        if (float_bits == 32)
            as.FCVT_W_S(t3, ft1, rounding);
        else
            as.FCVT_W_D(t3, ft1, rounding);
    } else {
        if (float_bits == 32)
            as.FCVT_L_S(t3, ft1, rounding);
        else
            as.FCVT_L_D(t3, ft1, rounding);
    }
    as.FRFLAGS(t4);
    as.FSFLAGS(t6);
    as.ANDI(t4, t4, 16);  // Invalid includes NaN and either signed overflow.
    Label valid;
    as.BEQ(t4, x0, &valid);
    as.LI(t3, u64{1} << (integer_bits - 1));
    as.Bind(&valid);
    writer.Mask(t3, integer_bits);
}

void IntegerToFloat(Writer& writer, unsigned integer_bits, unsigned float_bits) {
    auto& as = writer.as;
    CheckFloatWidth(writer, float_bits);
    if (integer_bits != 16 && integer_bits != 32 && integer_bits != 64)
        writer.Reject(CompileError::UnsupportedOperand);
    writer.SignExtend(t0, integer_bits);
    if (float_bits == 32) {
        if (integer_bits == 64)
            as.FCVT_S_L(ft0, t0, RMode::DYN);
        else
            as.FCVT_S_W(ft0, t0, RMode::DYN);
    } else {
        if (integer_bits == 64)
            as.FCVT_D_L(ft0, t0, RMode::DYN);
        else
            as.FCVT_D_W(ft0, t0, RMode::DYN);
    }
    FromFloat(writer, t3, ft0, float_bits);
    Flush(writer, t3, float_bits, 15);
}

void ConvertFloat(Writer& writer, unsigned source_bits) {
    CheckFloatWidth(writer, source_bits);
    const unsigned destination_bits = 96 - source_bits;
    auto& as = writer.as;
    Flush(writer, t0, source_bits, 6);
    ToFloat(as, ft1, t0, source_bits);
    if (source_bits == 32)
        as.FCVT_D_S(ft0, ft1, RMode::DYN);
    else
        as.FCVT_S_D(ft0, ft1, RMode::DYN);
    FromFloat(writer, t3, ft0, destination_bits);
    as.FCLASS(t4, ft1, source_bits == 32 ? Precision::S : Precision::D);
    as.ANDI(t4, t4, 768);
    Label numeric;
    as.BEQ(t4, x0, &numeric);
    if (source_bits == 32) {
        as.SLLI(t3, t0, 41);
        as.SRLI(t3, t3, 12);
        as.SRLI(t4, t0, 31);
        as.SLLI(t4, t4, 63);
        as.LI(t5, UINT64_C(0x7ff8000000000000));
    } else {
        as.SRLI(t3, t0, 29);
        as.LI(t5, UINT64_C(0x7fffff));
        as.AND(t3, t3, t5);
        as.SRLI(t4, t0, 32);
        as.LI(t5, UINT64_C(0x80000000));
        as.AND(t4, t4, t5);
        as.LI(t5, UINT64_C(0x7fc00000));
    }
    as.OR(t3, t3, t4);
    as.OR(t3, t3, t5);
    as.Bind(&numeric);
    Flush(writer, t3, destination_bits, 15);
}

void RoundIntegral(Writer& writer, unsigned bits, unsigned mode) {
    auto& as = writer.as;
    const auto precision = bits == 32 ? Precision::S : Precision::D;
    constexpr std::array modes{RMode::RNE, RMode::RDN, RMode::RUP, RMode::RTZ};
    if (mode > 3) writer.Reject(CompileError::UnsupportedOperand);
    Label unchanged, done;
    as.LI(t4, ~(u64{1} << (bits - 1)));
    as.AND(t4, t0, t4);
    as.LI(t5, bits == 32 ? UINT64_C(0x4b000000) : UINT64_C(0x4330000000000000));
    as.BGEU(t4, t5, &unchanged);
    if (bits == 32) {
        as.FCVT_W_S(t3, ft1, modes[mode]);
        as.FCVT_S_W(ft0, t3, RMode::RNE);
    } else {
        as.FCVT_L_D(t3, ft1, modes[mode]);
        as.FCVT_D_L(ft0, t3, RMode::RNE);
    }
    as.FSGNJ(ft0, ft0, ft1, precision);
    FromFloat(writer, t3, ft0, bits);
    as.J(&done);
    as.Bind(&unchanged);
    as.MV(t3, t0);
    PropagateNaN(writer, t3, ft1, t0, bits);
    as.Bind(&done);
}

void CompareMask(Writer& writer, unsigned bits, unsigned relations) {
    auto& as = writer.as;
    const auto precision = bits == 32 ? Precision::S : Precision::D;
    as.FCLASS(t4, ft1, precision);
    as.FCLASS(t5, ft2, precision);
    as.OR(t4, t4, t5);
    as.ANDI(t4, t4, 768);
    Label unordered, done;
    as.BNE(t4, x0, &unordered);
    as.FEQ(t3, ft1, ft2, precision);
    as.FLT(t4, ft1, ft2, precision);
    as.LI(t5, 2);
    as.SUB(t5, t5, t3);
    as.SLLI(t4, t4, 1);
    as.SUB(t5, t5, t4);  // less=0, equal=1, greater=2.
    as.LI(t3, relations);
    as.SRL(t3, t3, t5);
    as.ANDI(t3, t3, 1);
    as.J(&done);
    as.Bind(&unordered);
    as.LI(t3, (relations >> 3) & 1);
    as.Bind(&done);
    as.NEG(t3, t3);
}

template <O opcode> void Floating(Writer& writer, OpContext& ctx) {
    constexpr bool single_scalar = opcode == O::VecFAddScalar32 || opcode == O::VecFSubScalar32 ||
                                   opcode == O::VecFMulScalar32 || opcode == O::VecFDivScalar32;
    constexpr bool double_scalar = opcode == O::VecFAddScalar64 || opcode == O::VecFSubScalar64 ||
                                   opcode == O::VecFMulScalar64 || opcode == O::VecFDivScalar64;
    constexpr bool unary = opcode == O::VecFUnary, round = opcode == O::VecFRoundInt;
    constexpr bool minmax = opcode == O::VecFMinMax, mask = opcode == O::VecFCmpMask;
    constexpr bool fused = opcode == O::VecFMulAdd;
    const unsigned bits = single_scalar ? 32 : double_scalar ? 64 : Immediate(ctx, fused ? 3 : 2);
    CheckFloatWidth(writer, bits);
    const unsigned flags = fused ? Immediate(ctx, 4) : 0;
    const unsigned kind = unary || round ? Immediate(ctx, 3) : 0;
    if constexpr (unary)
        if (kind > 2) writer.Reject(CompileError::UnsupportedOperand);
    const bool scalar = single_scalar || double_scalar ||
                        ((unary || round || minmax || mask) && Immediate(ctx, 4)) ||
                        (fused && (flags & 4));
    auto& as = writer.as;
    writer.ReadPair(ctx.Arg(0), a0, a1);
    writer.ReadPair(ctx.Arg(1), a2, a3);
    if constexpr (fused) writer.ReadPair(ctx.Arg(2), a4, a5);
    as.MV(a6, scalar ? a0 : x0);
    as.MV(a7, scalar ? a1 : x0);
    const auto precision = bits == 32 ? Precision::S : Precision::D;
    for (unsigned lane = 0; lane < (scalar ? 1 : 128 / bits); ++lane) {
        Lane(writer, t0, unary || round ? second : first, lane, bits);
        Flush(writer, t0, bits, 6, unary && kind != 0);
        ToFloat(as, ft1, t0, bits);
        if constexpr (!unary && !round) {
            Lane(writer, t1, second, lane, bits);
            Flush(writer, t1, bits, 6);
            ToFloat(as, ft2, t1, bits);
        }
        if constexpr (fused) {
            Lane(writer, t2, third, lane, bits);
            Flush(writer, t2, bits, 6);
            ToFloat(as, ft3, t2, bits);
        }
        if constexpr (minmax) {
            if (Immediate(ctx, 3))
                as.FLT(t4, ft2, ft1, precision);
            else
                as.FLT(t4, ft1, ft2, precision);
            Label other, done;
            as.BEQ(t4, x0, &other);
            as.MV(t3, t0);
            as.J(&done);
            as.Bind(&other);
            as.MV(t3, t1);
            as.Bind(&done);
        } else if constexpr (mask)
            CompareMask(writer, bits, Immediate(ctx, 3) & 15);
        else if constexpr (round)
            RoundIntegral(writer, bits, kind);
        else {
            if constexpr (fused) {
                const bool negate_addend =
                        bool(flags & 2) ^ (bool(flags & 8) && ((lane & 1) == ((flags >> 4) & 1)));
                if (flags & 1) {
                    if (negate_addend)
                        as.FNMADD(ft0, ft1, ft2, ft3, precision, RMode::DYN);
                    else
                        as.FNMSUB(ft0, ft1, ft2, ft3, precision, RMode::DYN);
                } else {
                    if (negate_addend)
                        as.FMSUB(ft0, ft1, ft2, ft3, precision, RMode::DYN);
                    else
                        as.FMADD(ft0, ft1, ft2, ft3, precision, RMode::DYN);
                }
            } else if constexpr (unary) {
                if (kind == 0)
                    as.FSQRT(ft0, ft1, precision, RMode::DYN);
                else {
                    as.LI(t3, bits == 32 ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000));
                    ToFloat(as, ft2, t3, bits);
                    if (kind == 1)
                        as.FDIV(ft0, ft2, ft1, precision, RMode::DYN);
                    else {
                        as.FSQRT(ft0, ft1, precision, RMode::DYN);
                        as.FDIV(ft0, ft2, ft0, precision, RMode::DYN);
                    }
                }
            } else if constexpr (opcode == O::VecFAdd || opcode == O::VecFAddScalar32 ||
                                 opcode == O::VecFAddScalar64)
                as.FADD(ft0, ft1, ft2, precision, RMode::DYN);
            else if constexpr (opcode == O::VecFSub || opcode == O::VecFSubScalar32 ||
                               opcode == O::VecFSubScalar64)
                as.FSUB(ft0, ft1, ft2, precision, RMode::DYN);
            else if constexpr (opcode == O::VecFMul || opcode == O::VecFMulScalar32 ||
                               opcode == O::VecFMulScalar64)
                as.FMUL(ft0, ft1, ft2, precision, RMode::DYN);
            else
                as.FDIV(ft0, ft1, ft2, precision, RMode::DYN);
            FromFloat(writer, t3, ft0, bits);
            RepairArithmeticNaN(writer, bits, !unary, fused);
        }
        if constexpr (!mask && !minmax) Flush(writer, t3, bits, 15, unary && kind != 0);
        Put(writer, t3, lane, bits, scalar);
    }
    Finish(writer, ctx);
}
}  // namespace

#define RV_FLOAT(name)                                                                             \
    void Writer::Handle(Op<O::name>, OpContext& ctx) { Floating<O::name>(*this, ctx); }
RV_FLOAT(VecFAddScalar32)
RV_FLOAT(VecFSubScalar32)
RV_FLOAT(VecFMulScalar32)
RV_FLOAT(VecFDivScalar32)
RV_FLOAT(VecFAddScalar64)
RV_FLOAT(VecFSubScalar64)
RV_FLOAT(VecFMulScalar64)
RV_FLOAT(VecFDivScalar64)
RV_FLOAT(VecFAdd)
RV_FLOAT(VecFSub)
RV_FLOAT(VecFMul)
RV_FLOAT(VecFDiv)
RV_FLOAT(VecFMinMax)
RV_FLOAT(VecFUnary)
RV_FLOAT(VecFCmpMask)
RV_FLOAT(VecFMulAdd)
RV_FLOAT(VecFRoundInt)
#undef RV_FLOAT

void Writer::Handle(Op<O::VecFCmp>, OpContext& ctx) {
    const unsigned bits = Immediate(ctx, 2);
    CheckFloatWidth(*this, bits);
    Read(ctx.Arg(0), t0, true);
    Read(ctx.Arg(1), t1, true);
    Mask(t0, bits);
    Mask(t1, bits);
    Flush(*this, t0, bits, 6);
    Flush(*this, t1, bits, 6);
    ToFloat(as, ft1, t0, bits);
    ToFloat(as, ft2, t1, bits);
    const auto precision = bits == 32 ? Precision::S : Precision::D;
    as.FEQ(t3, ft1, ft2, precision);
    as.SLLI(t3, t3, 2);
    as.FLT(t4, ft1, ft2, precision);
    as.OR(t3, t3, t4);
    as.FCLASS(t4, ft1, precision);
    as.FCLASS(t5, ft2, precision);
    as.OR(t4, t4, t5);
    as.ANDI(t4, t4, 768);
    Label ordered;
    as.BEQ(t4, x0, &ordered);
    as.LI(t3, 7);
    as.Bind(&ordered);
    as.MV(ctx.destination, t3);
}

void Writer::Handle(Op<O::VecFCvtIntToFloat>, OpContext& ctx) {
    Read(ctx.Arg(0), t0, true);
    IntegerToFloat(*this, Immediate(ctx, 1), Immediate(ctx, 2));
    as.MV(ctx.destination, t3);
    if (ctx.operation.type == Type::V128) as.MV(ctx.high, x0);
}

void Writer::Handle(Op<O::VecFCvtFloatToInt>, OpContext& ctx) {
    Read(ctx.Arg(0), t0, true);
    FloatToInteger(*this, Immediate(ctx, 1), Immediate(ctx, 2), Immediate(ctx, 3) != 0);
    as.MV(ctx.destination, t3);
}

void Writer::Handle(Op<O::VecFCvtScalar>, OpContext& ctx) {
    CheckFloatWidth(*this, Immediate(ctx, 1));
    Read(ctx.Arg(0), t0, true);
    Mask(t0, Immediate(ctx, 1));
    ConvertFloat(*this, Immediate(ctx, 1));
    as.MV(ctx.destination, t3);
    if (ctx.operation.type == Type::V128) as.MV(ctx.high, x0);
}

void Writer::Handle(Op<O::VecFCvtPacked>, OpContext& ctx) {
    const unsigned kind = Immediate(ctx, 1);
    if (kind > 7) Reject(CompileError::UnsupportedOperand);
    const unsigned source_bits = kind == 4 || kind == 5 || kind == 7 ? 64 : 32;
    const unsigned destination_bits = kind == 1 || kind == 6 ? 64 : 32;
    ReadPair(ctx.Arg(0), a0, a1);
    as.MV(a6, x0);
    as.MV(a7, x0);
    const unsigned count = source_bits == 64 || destination_bits == 64 ? 2 : 4;
    for (unsigned lane = 0; lane < count; ++lane) {
        Lane(*this, t0, first, lane, source_bits);
        if (kind <= 1)
            IntegerToFloat(*this, 32, destination_bits);
        else if (kind <= 5)
            FloatToInteger(*this, source_bits, 32, kind == 3 || kind == 5);
        else
            ConvertFloat(*this, source_bits);
        Put(*this, t3, lane, destination_bits);
    }
    Finish(*this, ctx);
}
}  // namespace swift::runtime::backend::riscv64::detail
