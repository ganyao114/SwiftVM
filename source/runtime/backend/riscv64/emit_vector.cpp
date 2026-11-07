#include "emit.h"

#include <algorithm>
#include "runtime/arch/x86/vector_crypto.h"

namespace swift::runtime::backend::riscv64::detail {
namespace {
using u64 = std::uint64_t;
using O = OpCode;
constexpr std::array left{a0, a1}, right{a2, a3}, vector_output{a6, a7};

u64 LaneMask(unsigned bits) { return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }

unsigned VectorImmediate(OpContext& ctx, unsigned index) {
    return static_cast<unsigned>(ctx.Arg(index).Payload());
}

void CheckWidth(Writer& writer, unsigned bits) {
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
        writer.Reject(CompileError::UnsupportedOperand);
}

void Extract(Writer& writer,
             GPR target,
             const std::array<GPR, 2>& source,
             unsigned index,
             unsigned bits,
             bool sign = false) {
    const auto bit = index * bits;
    writer.as.SRLI(target, source[bit / 64], bit % 64);
    if (sign)
        writer.SignExtend(target, bits);
    else
        writer.Mask(target, bits);
}

void Insert(Writer& writer, GPR value, unsigned index, unsigned bits) {
    writer.Mask(value, bits);
    const auto bit = index * bits;
    if (bit % 64) writer.as.SLLI(value, value, bit % 64);
    writer.as.OR(vector_output[bit / 64], vector_output[bit / 64], value);
}

void VectorBegin(Writer& writer, OpContext& ctx, bool two = true) {
    writer.ReadPair(ctx.Arg(0), a0, a1);
    if (two) writer.ReadPair(ctx.Arg(1), a2, a3);
    writer.as.MV(a6, x0);
    writer.as.MV(a7, x0);
}

void VectorFinish(Writer& writer, OpContext& ctx) {
    writer.as.MV(ctx.destination, a6);
    writer.as.MV(ctx.high, a7);
}

void VectorClamp(Writer& writer, GPR value, unsigned bits, bool unsigned_result) {
    auto& as = writer.as;
    as.LI(t3, unsigned_result ? 0 : u64{0} - (u64{1} << (bits - 1)));
    as.LI(t4, unsigned_result ? LaneMask(bits) : (u64{1} << (bits - 1)) - 1);
    Label lower, upper;
    as.BGE(value, t3, &lower);
    as.MV(value, t3);
    as.Bind(&lower);
    as.BGE(t4, value, &upper);
    as.MV(value, t4);
    as.Bind(&upper);
}

template <O opcode> void VectorBitwise(Writer& writer, OpContext& ctx) {
    VectorBegin(writer, ctx);
    for (unsigned part = 0; part < 2; ++part) {
        if constexpr (opcode == O::VecAnd || opcode == O::Vec4And)
            writer.as.AND(vector_output[part], left[part], right[part]);
        else if constexpr (opcode == O::VecOr || opcode == O::Vec4Or)
            writer.as.OR(vector_output[part], left[part], right[part]);
        else if constexpr (opcode == O::VecXor)
            writer.as.XOR(vector_output[part], left[part], right[part]);
        else {
            writer.as.NOT(t0, right[part]);
            writer.as.AND(vector_output[part], left[part], t0);
        }
    }
    VectorFinish(writer, ctx);
}

template <O opcode> void VectorArithmetic(Writer& writer, OpContext& ctx) {
    constexpr bool legacy = opcode == O::Vec4Add || opcode == O::Vec4Sub || opcode == O::Vec4Mul;
    const unsigned bits = legacy ? 32 : VectorImmediate(ctx, 2);
    CheckWidth(writer, bits);
    constexpr bool minmax = opcode == O::VecMin || opcode == O::VecMax;
    constexpr bool saturate = opcode == O::VecSatAdd || opcode == O::VecSatSub;
    const bool sign = opcode == O::VecCmpGt || ((minmax || saturate) && VectorImmediate(ctx, 3));
    VectorBegin(writer, ctx);
    auto& as = writer.as;
    for (unsigned lane = 0; lane < 128 / bits; ++lane) {
        Extract(writer, t0, left, lane, bits, sign);
        Extract(writer, t1, right, lane, bits, sign);
        if constexpr (opcode == O::VecAdd || opcode == O::Vec4Add || opcode == O::VecSatAdd)
            as.ADD(t2, t0, t1);
        else if constexpr (opcode == O::VecSub || opcode == O::Vec4Sub || opcode == O::VecSatSub)
            as.SUB(t2, t0, t1);
        else if constexpr (opcode == O::VecMul || opcode == O::Vec4Mul)
            as.MUL(t2, t0, t1);
        else if constexpr (opcode == O::VecCmpEq) {
            as.XOR(t2, t0, t1);
            as.SEQZ(t2, t2);
            as.NEG(t2, t2);
        } else if constexpr (opcode == O::VecCmpGt) {
            as.SLT(t2, t1, t0);
            as.NEG(t2, t2);
        } else if constexpr (opcode == O::VecAvg) {
            // Ceil((a+b)/2), without overflowing a 64-bit lane.
            as.XOR(t2, t0, t1);
            as.ANDI(t3, t2, 1);
            as.SRLI(t2, t2, 1);
            as.AND(t4, t0, t1);
            as.ADD(t2, t2, t4);
            as.ADD(t2, t2, t3);
        } else if constexpr (minmax) {
            if (sign)
                as.SLT(t3, t0, t1);
            else
                as.SLTU(t3, t0, t1);
            Label other, done;
            if constexpr (opcode == O::VecMin)
                as.BEQ(t3, x0, &other);
            else
                as.BNE(t3, x0, &other);
            as.MV(t2, t0);
            as.J(&done);
            as.Bind(&other);
            as.MV(t2, t1);
            as.Bind(&done);
        }
        if constexpr (saturate) {
            if (bits < 64) {
                if (sign)
                    VectorClamp(writer, t2, bits, false);
                else if constexpr (opcode == O::VecSatAdd)
                    VectorClamp(writer, t2, bits, true);
                else {
                    Label nonnegative;
                    as.BGEU(t0, t1, &nonnegative);
                    as.MV(t2, x0);
                    as.Bind(&nonnegative);
                }
            } else if (sign) {
                // A wrapped 64-bit result cannot be clamped by comparisons
                // against the limits; the operand/result signs expose overflow.
                if constexpr (opcode == O::VecSatAdd) {
                    as.XOR(t3, t0, t2);
                    as.XOR(t4, t1, t2);
                } else {
                    as.XOR(t3, t0, t1);
                    as.XOR(t4, t0, t2);
                }
                as.AND(t3, t3, t4);
                Label ordinary;
                as.BGE(t3, x0, &ordinary);
                as.SRAI(t3, t0, 63);
                as.LI(t4, UINT64_C(0x7fffffffffffffff));
                as.XOR(t2, t3, t4);
                as.Bind(&ordinary);
            } else {
                Label ordinary;
                if constexpr (opcode == O::VecSatAdd) {
                    as.BGEU(t2, t0, &ordinary);
                    as.LI(t2, UINT64_MAX);
                } else {
                    as.BGEU(t0, t1, &ordinary);
                    as.MV(t2, x0);
                }
                as.Bind(&ordinary);
            }
        }
        Insert(writer, t2, lane, bits);
    }
    VectorFinish(writer, ctx);
}

template <O opcode> void VectorWiden(Writer& writer, OpContext& ctx) {
    const unsigned bits = VectorImmediate(ctx, 2);
    if (bits != 8 && bits != 16 && bits != 32) writer.Reject(CompileError::UnsupportedOperand);
    const bool sign = opcode == O::VecSignedMulWidenLow || VectorImmediate(ctx, 3);
    VectorBegin(writer, ctx);
    for (unsigned lane = 0; lane < 64 / bits; ++lane) {
        const unsigned index = opcode == O::VecMulWiden ? lane * 2 : lane;
        Extract(writer, t0, left, index, bits, sign);
        Extract(writer, t1, right, index, bits, sign);
        writer.as.MUL(t2, t0, t1);
        Insert(writer, t2, lane, bits * 2);
    }
    VectorFinish(writer, ctx);
}

template <O opcode> void VectorShift(Writer& writer, OpContext& ctx) {
    constexpr bool immediate = opcode == O::VecShiftLeftImm || opcode == O::VecShiftRightImm ||
                               opcode == O::VecShiftRightArithmeticImm;
    constexpr bool arithmetic =
            opcode == O::VecShiftRightArithmeticImm || opcode == O::VecShiftRightArithmetic;
    constexpr bool leftward = opcode == O::VecShiftLeftImm || opcode == O::VecShiftLeft;
    const unsigned bits = VectorImmediate(ctx, 2);
    CheckWidth(writer, bits);
    VectorBegin(writer, ctx, false);
    const bool vector_count = !immediate && writer.View(ctx.Arg(1)) == Type::V128;
    if constexpr (!immediate) {
        if (vector_count)
            writer.ReadPair(ctx.Arg(1), a2, a3);
        else
            writer.Read(ctx.Arg(1), a2);
    }
    auto& as = writer.as;
    for (unsigned lane = 0; lane < 128 / bits; ++lane) {
        Extract(writer, t0, left, lane, bits, arithmetic);
        if constexpr (immediate) {
            const auto count = std::min<unsigned>(VectorImmediate(ctx, 1), bits);
            if constexpr (arithmetic)
                as.SRAI(t2, t0, std::min(count, bits - 1));
            else if (count == bits)
                as.MV(t2, x0);
            else if constexpr (leftward)
                as.SLLI(t2, t0, count);
            else
                as.SRLI(t2, t0, count);
        } else {
            if (vector_count)
                Extract(writer, t1, right, lane, bits);
            else
                as.MV(t1, a2);
            as.LI(t3, bits);
            Label ordinary, done;
            as.BLTU(t1, t3, &ordinary);
            if constexpr (arithmetic)
                as.SRAI(t2, t0, 63);
            else
                as.MV(t2, x0);
            as.J(&done);
            as.Bind(&ordinary);
            if constexpr (leftward)
                as.SLL(t2, t0, t1);
            else if constexpr (arithmetic)
                as.SRA(t2, t0, t1);
            else
                as.SRL(t2, t0, t1);
            as.Bind(&done);
        }
        Insert(writer, t2, lane, bits);
    }
    VectorFinish(writer, ctx);
}

template <O opcode> void VectorShuffle(Writer& writer, OpContext& ctx) {
    constexpr bool two =
            opcode == O::VecShuffle32TwoSrc || opcode == O::VecZip || opcode == O::VecUnzip;
    const unsigned bits = opcode == O::VecShuffle16                      ? 16
                          : opcode == O::VecZip || opcode == O::VecUnzip ? VectorImmediate(ctx, 2)
                                                                         : 32;
    CheckWidth(writer, bits);
    if constexpr (opcode == O::VecDupPairs32)
        if (VectorImmediate(ctx, 1) > 1) writer.Reject(CompileError::UnsupportedOperand);
    VectorBegin(writer, ctx, two);
    const unsigned lanes = 128 / bits;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        unsigned index{}, operand{};
        if constexpr (opcode == O::VecShuffle32 || opcode == O::VecShuffle32TwoSrc) {
            index = (VectorImmediate(ctx, two ? 2 : 1) >> (lane * 2)) & 3;
            operand = two && lane >= 2;
        } else if constexpr (opcode == O::VecShuffle16) {
            const unsigned base = VectorImmediate(ctx, 2) ? 4 : 0;
            index = lane >= base && lane < base + 4
                            ? base + ((VectorImmediate(ctx, 1) >> ((lane - base) * 2)) & 3)
                            : lane;
        } else if constexpr (opcode == O::VecDupPairs32)
            index = (lane & 2) + VectorImmediate(ctx, 1);
        else if constexpr (opcode == O::VecZip) {
            operand = lane & 1;
            index = lane / 2 + (VectorImmediate(ctx, 3) ? lanes / 2 : 0);
        } else {
            const unsigned source = lane * 2 + bool(VectorImmediate(ctx, 3));
            operand = source / lanes;
            index = source % lanes;
        }
        Extract(writer, t0, operand ? right : left, index, bits);
        Insert(writer, t0, lane, bits);
    }
    VectorFinish(writer, ctx);
}

template <O opcode> void VectorLookup(Writer& writer, OpContext& ctx) {
    VectorBegin(writer, ctx);
    auto& as = writer.as;
    for (unsigned lane = 0; lane < 16; ++lane) {
        Extract(writer, t0, right, lane, 8);
        Label zero, upper, done;
        if constexpr (opcode == O::VecTableLookup8) {
            as.ANDI(t1, t0, 128);
            as.BNE(t1, x0, &zero);
            as.ANDI(t0, t0, 15);
        } else {
            as.LI(t1, 16);
            as.BGEU(t0, t1, &zero);
        }
        as.SLLI(t0, t0, 3);
        as.LI(t1, 64);
        as.BGEU(t0, t1, &upper);
        as.SRL(t2, a0, t0);
        as.J(&done);
        as.Bind(&upper);
        as.SRL(t2, a1, t0);
        as.J(&done);
        as.Bind(&zero);
        as.MV(t2, x0);
        as.Bind(&done);
        Insert(writer, t2, lane, 8);
    }
    VectorFinish(writer, ctx);
}

void VectorWindow(Writer& writer, const std::array<GPR, 4>& source, unsigned shift) {
    const unsigned whole = shift / 64, part = shift % 64;
    for (unsigned half = 0; half < 2; ++half) {
        writer.as.SRLI(vector_output[half], source[whole + half], part);
        if (part) {
            writer.as.SLLI(t0, source[whole + half + 1], 64 - part);
            writer.as.OR(vector_output[half], vector_output[half], t0);
        }
    }
}

template <O opcode> void VectorCrypto(Writer& writer, OpContext& ctx) {
    namespace crypto = ::swift::runtime::x86::crypto;
    constexpr auto operation = [] {
        if constexpr (opcode == O::VecAesEnc || opcode == O::VecAesEncFast)
            return crypto::Operation::AesEncrypt;
        else if constexpr (opcode == O::VecAesEncLast || opcode == O::VecAesEncLastFast)
            return crypto::Operation::AesEncryptLast;
        else if constexpr (opcode == O::VecAesDec || opcode == O::VecAesDecFast)
            return crypto::Operation::AesDecrypt;
        else if constexpr (opcode == O::VecAesDecLast || opcode == O::VecAesDecLastFast)
            return crypto::Operation::AesDecryptLast;
        else if constexpr (opcode == O::VecAesImc)
            return crypto::Operation::AesInverseMixColumns;
        else if constexpr (opcode == O::VecAesKeygenAssist)
            return crypto::Operation::AesKeygenAssist;
        else if constexpr (opcode == O::VecPclMul)
            return crypto::Operation::PolynomialMultiply;
        else if constexpr (opcode == O::VecSha256Msg1)
            return crypto::Operation::Sha256Message1;
        else if constexpr (opcode == O::VecSha256Msg2)
            return crypto::Operation::Sha256Message2;
        else
            return crypto::Operation::Sha256Rounds2;
    }();
    writer.ReadPair(ctx.Arg(0), a1, a2);
    if constexpr (opcode == O::VecAesImc || opcode == O::VecAesKeygenAssist) {
        writer.as.MV(a3, x0);
        writer.as.MV(a4, x0);
    } else
        writer.ReadPair(ctx.Arg(1), a3, a4);
    if constexpr (opcode == O::VecSha256Rnds2)
        writer.ReadPair(ctx.Arg(2), a5, a6);
    else {
        writer.as.MV(a5, x0);
        writer.as.MV(a6, x0);
    }
    writer.as.LI(a0, static_cast<u64>(operation));
    constexpr bool immediate = opcode == O::VecAesKeygenAssist || opcode == O::VecPclMul;
    writer.as.LI(a7, immediate ? VectorImmediate(ctx, opcode == O::VecPclMul ? 2 : 1) : 0);
    writer.Call(reinterpret_cast<std::uintptr_t>(&crypto::Execute));
    writer.as.MV(ctx.destination, a0);
    writer.as.MV(ctx.high, a1);
}
}  // namespace

#define RV_BITWISE(name)                                                                           \
    void Writer::Handle(Op<O::name>, OpContext& ctx) { VectorBitwise<O::name>(*this, ctx); }
RV_BITWISE(Vec4And)
RV_BITWISE(Vec4Or)
RV_BITWISE(VecAnd)
RV_BITWISE(VecOr)
RV_BITWISE(VecXor)
RV_BITWISE(VecAndNot)
#undef RV_BITWISE

#define RV_ARITHMETIC(name)                                                                        \
    void Writer::Handle(Op<O::name>, OpContext& ctx) { VectorArithmetic<O::name>(*this, ctx); }
RV_ARITHMETIC(Vec4Add)
RV_ARITHMETIC(Vec4Sub)
RV_ARITHMETIC(Vec4Mul)
RV_ARITHMETIC(VecAdd)
RV_ARITHMETIC(VecSub)
RV_ARITHMETIC(VecMul)
RV_ARITHMETIC(VecCmpEq)
RV_ARITHMETIC(VecCmpGt)
RV_ARITHMETIC(VecAvg)
RV_ARITHMETIC(VecMin)
RV_ARITHMETIC(VecMax)
RV_ARITHMETIC(VecSatAdd)
RV_ARITHMETIC(VecSatSub)
#undef RV_ARITHMETIC

void Writer::Handle(Op<O::VecMulHigh16>, OpContext& ctx) {
    VectorBegin(*this, ctx);
    for (unsigned lane = 0; lane < 8; ++lane) {
        Extract(*this, t0, left, lane, 16, VectorImmediate(ctx, 2));
        Extract(*this, t1, right, lane, 16, VectorImmediate(ctx, 2));
        as.MUL(t2, t0, t1);
        as.SRLI(t2, t2, 16);
        Insert(*this, t2, lane, 16);
    }
    VectorFinish(*this, ctx);
}

void Writer::Handle(Op<O::VecMulWiden>, OpContext& ctx) { VectorWiden<O::VecMulWiden>(*this, ctx); }

void Writer::Handle(Op<O::VecSignedMulWidenLow>, OpContext& ctx) {
    VectorWiden<O::VecSignedMulWidenLow>(*this, ctx);
}

void Writer::Handle(Op<O::VecPack>, OpContext& ctx) {
    const unsigned bits = VectorImmediate(ctx, 2), width = bits / 2;
    if (bits != 16 && bits != 32 && bits != 64) Reject(CompileError::UnsupportedOperand);
    VectorBegin(*this, ctx);
    for (unsigned lane = 0; lane < 256 / bits; ++lane) {
        Extract(*this, t2, lane < 128 / bits ? left : right, lane % (128 / bits), bits, true);
        VectorClamp(*this, t2, width, VectorImmediate(ctx, 3));
        Insert(*this, t2, lane, width);
    }
    VectorFinish(*this, ctx);
}

void Writer::Handle(Op<O::VecExtend>, OpContext& ctx) {
    const unsigned source = VectorImmediate(ctx, 1), destination = VectorImmediate(ctx, 2);
    if ((source != 8 && source != 16 && source != 32) ||
        (destination != 16 && destination != 32 && destination != 64) || source >= destination)
        Reject(CompileError::UnsupportedOperand);
    VectorBegin(*this, ctx, false);
    for (unsigned lane = 0; lane < 128 / destination; ++lane) {
        Extract(*this, t0, left, lane, source, VectorImmediate(ctx, 3));
        Insert(*this, t0, lane, destination);
    }
    VectorFinish(*this, ctx);
}

void Writer::Handle(Op<O::VecAbsDiffSum8>, OpContext& ctx) {
    VectorBegin(*this, ctx);
    for (unsigned half = 0; half < 2; ++half) {
        for (unsigned lane = 0; lane < 8; ++lane) {
            Extract(*this, t0, left, half * 8 + lane, 8);
            Extract(*this, t1, right, half * 8 + lane, 8);
            as.SUB(t2, t0, t1);
            as.SRAI(t3, t2, 63);
            as.XOR(t2, t2, t3);
            as.SUB(t2, t2, t3);
            as.ADD(vector_output[half], vector_output[half], t2);
        }
    }
    VectorFinish(*this, ctx);
}

void Writer::Handle(Op<O::VecMadd16>, OpContext& ctx) {
    VectorBegin(*this, ctx);
    for (unsigned lane = 0; lane < 4; ++lane) {
        Extract(*this, t0, left, lane * 2, 16, true);
        Extract(*this, t1, right, lane * 2, 16, true);
        as.MUL(t4, t0, t1);
        Extract(*this, t0, left, lane * 2 + 1, 16, true);
        Extract(*this, t1, right, lane * 2 + 1, 16, true);
        as.MUL(t2, t0, t1);
        as.ADD(t2, t2, t4);
        Insert(*this, t2, lane, 32);
    }
    VectorFinish(*this, ctx);
}

#define RV_SHIFT(name)                                                                             \
    void Writer::Handle(Op<O::name>, OpContext& ctx) { VectorShift<O::name>(*this, ctx); }
RV_SHIFT(VecShiftLeft)
RV_SHIFT(VecShiftRight)
RV_SHIFT(VecShiftRightArithmetic)
RV_SHIFT(VecShiftLeftImm)
RV_SHIFT(VecShiftRightImm)
RV_SHIFT(VecShiftRightArithmeticImm)
#undef RV_SHIFT

void Writer::Handle(Op<O::VecByteShift>, OpContext& ctx) {
    VectorBegin(*this, ctx);
    const unsigned count = VectorImmediate(ctx, 2);
    if (count > 16) Reject(CompileError::UnsupportedOperand);
    if (VectorImmediate(ctx, 3))
        VectorWindow(*this, {a2, a3, a0, a1}, 128 - count * 8);
    else
        VectorWindow(*this, {a0, a1, a2, a3}, count * 8);
    VectorFinish(*this, ctx);
}

void Writer::Handle(Op<O::VecExtractBytes>, OpContext& ctx) {
    VectorBegin(*this, ctx);
    const unsigned count = VectorImmediate(ctx, 2);
    if (count > 16) Reject(CompileError::UnsupportedOperand);
    VectorWindow(*this, {a0, a1, a2, a3}, count * 8);
    VectorFinish(*this, ctx);
}

#define RV_SHUFFLE(name)                                                                           \
    void Writer::Handle(Op<O::name>, OpContext& ctx) { VectorShuffle<O::name>(*this, ctx); }
RV_SHUFFLE(VecShuffle32)
RV_SHUFFLE(VecShuffle32TwoSrc)
RV_SHUFFLE(VecShuffle16)
RV_SHUFFLE(VecZip)
RV_SHUFFLE(VecUnzip)
RV_SHUFFLE(VecDupPairs32)
#undef RV_SHUFFLE

void Writer::Handle(Op<O::VecShuffle32Indexed>, OpContext& ctx) {
    VectorLookup<O::VecShuffle32Indexed>(*this, ctx);
}

void Writer::Handle(Op<O::VecTableLookup8>, OpContext& ctx) {
    VectorLookup<O::VecTableLookup8>(*this, ctx);
}

void Writer::Handle(Op<O::VecLoadConst>, OpContext& ctx) {
    as.LI(ctx.destination, ctx.Arg(0).Payload());
    as.LI(ctx.high, ctx.Arg(1).Payload());
}

void Writer::Handle(Op<O::VecSharedZero>, OpContext& ctx) {
    as.MV(ctx.destination, x0);
    as.MV(ctx.high, x0);
}

void Writer::Handle(Op<O::VecDup64>, OpContext& ctx) {
    Read(ctx.Arg(0), a0, true);
    as.MV(ctx.destination, a0);
    as.MV(ctx.high, a0);
}

void Writer::Handle(Op<O::VecExtract64>, OpContext& ctx) {
    ReadPair(ctx.Arg(0), a0, a1);
    const auto index = VectorImmediate(ctx, 1);
    if (index > 1) Reject(CompileError::UnsupportedOperand);
    as.MV(ctx.destination, left[index]);
}

void Writer::Handle(Op<O::VecExtract16>, OpContext& ctx) {
    ReadPair(ctx.Arg(0), a0, a1);
    const auto index = VectorImmediate(ctx, 1);
    if (index > 7) Reject(CompileError::UnsupportedOperand);
    Extract(*this, ctx.destination, left, index, 16);
}

void Writer::Handle(Op<O::VecInsert16>, OpContext& ctx) {
    ReadPair(ctx.Arg(0), a0, a1);
    Read(ctx.Arg(1), t0);
    const auto index = VectorImmediate(ctx, 2);
    if (index > 7) Reject(CompileError::UnsupportedOperand);
    const auto bit = index * 16;
    as.MV(a6, a0);
    as.MV(a7, a1);
    as.LI(t1, ~(u64{65535} << (bit % 64)));
    as.AND(vector_output[bit / 64], vector_output[bit / 64], t1);
    Insert(*this, t0, index, 16);
    VectorFinish(*this, ctx);
}

void Writer::Handle(Op<O::VecMovMask>, OpContext& ctx) {
    const auto bits = VectorImmediate(ctx, 1);
    CheckWidth(*this, bits);
    VectorBegin(*this, ctx, false);
    for (unsigned lane = 0; lane < 128 / bits; ++lane) {
        Extract(*this, t0, left, lane, bits);
        as.SRLI(t0, t0, bits - 1);
        as.SLLI(t0, t0, lane);
        as.OR(a6, a6, t0);
    }
    as.MV(ctx.destination, a6);
}

#define RV_CRYPTO(name)                                                                            \
    void Writer::Handle(Op<O::name>, OpContext& ctx) { VectorCrypto<O::name>(*this, ctx); }
RV_CRYPTO(VecAesEnc)
RV_CRYPTO(VecAesEncLast)
RV_CRYPTO(VecAesDec)
RV_CRYPTO(VecAesDecLast)
RV_CRYPTO(VecAesEncFast)
RV_CRYPTO(VecAesEncLastFast)
RV_CRYPTO(VecAesDecFast)
RV_CRYPTO(VecAesDecLastFast)
RV_CRYPTO(VecAesImc)
RV_CRYPTO(VecAesKeygenAssist)
RV_CRYPTO(VecPclMul)
RV_CRYPTO(VecSha256Msg1)
RV_CRYPTO(VecSha256Msg2)
RV_CRYPTO(VecSha256Rnds2)
#undef RV_CRYPTO
}  // namespace swift::runtime::backend::riscv64::detail
