#include "emit.h"

#include <algorithm>
#include "runtime/arch/x86/vector_crypto.h"

namespace swift::runtime::backend::loongarch64::detail {
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

void LoadVR(Writer& writer, VR dst, CellView cell) {
    if (cell.Kind() == OperandKind::Value &&
        TypeBits(writer.source.Operations()[cell.Payload()].type) == 128) {
        const auto& home = writer.allocation.homes[cell.Payload()];
        if (!home.spilled) {
            writer.as.VINSGR2VR_D(dst, GPR{home.reg}, 0);
            writer.as.VINSGR2VR_D(dst, GPR{home.high_reg}, 1);
            return;
        }
        if (home.stack_offset <= 2032) {
            writer.as.VLD(dst, sp, static_cast<int>(home.stack_offset));
            return;
        }
    }
    writer.ReadPair(cell, a0, a1);
    writer.as.VINSGR2VR_D(dst, a0, 0);
    writer.as.VINSGR2VR_D(dst, a1, 1);
}

void StoreVR(Writer& writer, OpContext& ctx, VR src) {
    const auto& home = writer.allocation.homes[ctx.node.value];
    if (home.spilled && home.stack_offset <= 2032) {
        writer.as.VST(src, sp, static_cast<int>(home.stack_offset));
        ctx.saved = true;
        return;
    }
    writer.as.VPICKVE2GR_DU(ctx.destination, src, 0);
    writer.as.VPICKVE2GR_DU(ctx.high, src, 1);
}

void EmitByteWindowLSX(Writer& writer,
                       OpContext& ctx,
                       CellView low_arg,
                       CellView high_arg,
                       unsigned right_bytes) {
    if (right_bytes == 0) {
        LoadVR(writer, vr0, low_arg);
        StoreVR(writer, ctx, vr0);
        return;
    }
    if (right_bytes == 16) {
        LoadVR(writer, vr0, high_arg);
        StoreVR(writer, ctx, vr0);
        return;
    }
    LoadVR(writer, vr0, low_arg);
    LoadVR(writer, vr1, high_arg);
    writer.as.VBSRL_V(vr0, vr0, right_bytes);
    writer.as.VBSLL_V(vr1, vr1, 16 - right_bytes);
    writer.as.VOR_V(vr0, vr0, vr1);
    StoreVR(writer, ctx, vr0);
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
    if (writer.options.lsx) {
        LoadVR(writer, vr0, ctx.Arg(0));
        LoadVR(writer, vr1, ctx.Arg(1));
        if constexpr (opcode == O::VecAnd || opcode == O::Vec4And)
            writer.as.VAND_V(vr0, vr0, vr1);
        else if constexpr (opcode == O::VecOr || opcode == O::Vec4Or)
            writer.as.VOR_V(vr0, vr0, vr1);
        else if constexpr (opcode == O::VecXor)
            writer.as.VXOR_V(vr0, vr0, vr1);
        else
            writer.as.VANDN_V(vr0, vr1, vr0);
        StoreVR(writer, ctx, vr0);
        return;
    }
    writer.ReadPair(ctx.Arg(0), a0, a1);
    writer.ReadPair(ctx.Arg(1), a2, a3);
    const std::array dst{ctx.destination, ctx.high};
    for (unsigned part = 0; part < 2; ++part) {
        if constexpr (opcode == O::VecAnd || opcode == O::Vec4And)
            writer.as.AND(dst[part], left[part], right[part]);
        else if constexpr (opcode == O::VecOr || opcode == O::Vec4Or)
            writer.as.OR(dst[part], left[part], right[part]);
        else if constexpr (opcode == O::VecXor)
            writer.as.XOR(dst[part], left[part], right[part]);
        else
            writer.as.ANDN(dst[part], left[part], right[part]);
    }
}

template <O opcode> void VectorArithmetic(Writer& writer, OpContext& ctx) {
    constexpr bool legacy = opcode == O::Vec4Add || opcode == O::Vec4Sub || opcode == O::Vec4Mul;
    const unsigned bits = legacy ? 32 : VectorImmediate(ctx, 2);
    CheckWidth(writer, bits);
    constexpr bool minmax = opcode == O::VecMin || opcode == O::VecMax;
    constexpr bool saturate = opcode == O::VecSatAdd || opcode == O::VecSatSub;
    const bool sign = opcode == O::VecCmpGt || ((minmax || saturate) && VectorImmediate(ctx, 3));
    if (writer.options.lsx) {
        auto& as = writer.as;
        LoadVR(writer, vr0, ctx.Arg(0));
        LoadVR(writer, vr1, ctx.Arg(1));
        if constexpr (opcode == O::VecAdd || opcode == O::Vec4Add)
            as.VADD(bits, vr0, vr0, vr1);
        else if constexpr (opcode == O::VecSub || opcode == O::Vec4Sub)
            as.VSUB(bits, vr0, vr0, vr1);
        else if constexpr (opcode == O::VecMul || opcode == O::Vec4Mul)
            as.VMUL(bits, vr0, vr0, vr1);
        else if constexpr (opcode == O::VecCmpEq)
            as.VSEQ(bits, vr0, vr0, vr1);
        else if constexpr (opcode == O::VecCmpGt)
            as.VSLT(bits, true, vr0, vr1, vr0);
        else if constexpr (opcode == O::VecAvg)
            as.VAVGR_U(bits, vr0, vr0, vr1);
        else if constexpr (opcode == O::VecMin)
            as.VMIN(bits, sign, vr0, vr0, vr1);
        else if constexpr (opcode == O::VecMax)
            as.VMAX(bits, sign, vr0, vr0, vr1);
        else if constexpr (opcode == O::VecSatAdd)
            as.VSADD(bits, sign, vr0, vr0, vr1);
        else
            as.VSSUB(bits, sign, vr0, vr0, vr1);
        StoreVR(writer, ctx, vr0);
        return;
    }
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
    if (writer.options.lsx) {
        LoadVR(writer, vr0, ctx.Arg(0));
        LoadVR(writer, vr1, ctx.Arg(1));
        if constexpr (opcode == O::VecMulWiden) {
            writer.as.VMULWEV(bits, sign, vr0, vr0, vr1);
        } else {
            writer.as.VSLLWIL_ZERO(bits, true, vr0, vr0);
            writer.as.VSLLWIL_ZERO(bits, true, vr1, vr1);
            writer.as.VMUL(bits * 2, vr0, vr0, vr1);
        }
        StoreVR(writer, ctx, vr0);
        return;
    }
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
    const bool vector_count = !immediate && writer.View(ctx.Arg(1)) == Type::V128;
    if (writer.options.lsx) {
        auto& as = writer.as;
        if constexpr (immediate) {
            const unsigned count = VectorImmediate(ctx, 1);
            if (count == 0) {
                LoadVR(writer, vr0, ctx.Arg(0));
            } else if (count >= bits) {
                if constexpr (arithmetic) {
                    LoadVR(writer, vr0, ctx.Arg(0));
                    as.VSRAI(bits, vr0, vr0, bits - 1);
                } else {
                    as.VXOR_V(vr0, vr0, vr0);
                }
            } else {
                LoadVR(writer, vr0, ctx.Arg(0));
                if constexpr (leftward)
                    as.VSLLI(bits, vr0, vr0, count);
                else if constexpr (arithmetic)
                    as.VSRAI(bits, vr0, vr0, count);
                else
                    as.VSRLI(bits, vr0, vr0, count);
            }
            StoreVR(writer, ctx, vr0);
            return;
        }
        LoadVR(writer, vr0, ctx.Arg(0));
        if (vector_count) {
            LoadVR(writer, vr1, ctx.Arg(1));
            if (bits <= 16) {
                as.VSLTI_U(bits, vr3, vr1, bits);
            } else {
                as.LI(t0, bits);
                as.VREPLGR2VR(bits, vr2, t0);
                as.VSLT(bits, false, vr3, vr1, vr2);
            }
            if constexpr (arithmetic) {
                as.VSRA(bits, vr2, vr0, vr1);
                as.VSRAI(bits, vr4, vr0, bits - 1);
                as.VAND_V(vr2, vr2, vr3);
                as.VANDN_V(vr4, vr3, vr4);
                as.VOR_V(vr0, vr2, vr4);
            } else {
                if constexpr (leftward)
                    as.VSLL(bits, vr0, vr0, vr1);
                else
                    as.VSRL(bits, vr0, vr0, vr1);
                as.VAND_V(vr0, vr0, vr3);
            }
        } else {
            writer.Read(ctx.Arg(1), a2);
            as.SLTUI(t0, a2, static_cast<int>(bits));
            if constexpr (arithmetic) {
                as.LI(t1, bits - 1);
                as.MASKEQZ(a2, a2, t0);
                as.MASKNEZ(t1, t1, t0);
                as.OR(a2, a2, t1);
                as.VREPLGR2VR(bits, vr1, a2);
                as.VSRA(bits, vr0, vr0, vr1);
            } else {
                as.NEG(t0, t0);
                as.VREPLGR2VR(bits, vr1, a2);
                if constexpr (leftward)
                    as.VSLL(bits, vr0, vr0, vr1);
                else
                    as.VSRL(bits, vr0, vr0, vr1);
                as.VREPLGR2VR(64, vr2, t0);
                as.VAND_V(vr0, vr0, vr2);
            }
        }
        StoreVR(writer, ctx, vr0);
        return;
    }
    VectorBegin(writer, ctx, false);
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
    if (writer.options.lsx) {
        auto& as = writer.as;
        LoadVR(writer, vr0, ctx.Arg(0));
        if constexpr (two) LoadVR(writer, vr1, ctx.Arg(1));
        if constexpr (opcode == O::VecShuffle32) {
            as.VSHUF4I_W(vr0, vr0, VectorImmediate(ctx, 1) & 0xffu);
        } else if constexpr (opcode == O::VecDupPairs32) {
            as.VSHUF4I_W(vr0, vr0, VectorImmediate(ctx, 1) ? 0xf5u : 0xa0u);
        } else if constexpr (opcode == O::VecShuffle32TwoSrc) {
            const unsigned imm = VectorImmediate(ctx, 2) & 0xffu;
            as.VSHUF4I_W(vr0, vr0, imm & 0x0fu);
            as.VSHUF4I_W(vr1, vr1, (imm >> 4) & 0x0fu);
            as.VILVL(64, vr0, vr1, vr0);
        } else if constexpr (opcode == O::VecShuffle16) {
            const unsigned imm = VectorImmediate(ctx, 1) & 0xffu;
            if (VectorImmediate(ctx, 2)) {
                as.VBSRL_V(vr1, vr0, 8);
                as.VSHUF4I_H(vr1, vr1, imm);
                as.VILVL(64, vr0, vr1, vr0);
            } else {
                as.VSHUF4I_H(vr1, vr0, imm);
                as.VBSRL_V(vr0, vr0, 8);
                as.VILVL(64, vr0, vr0, vr1);
            }
        } else if constexpr (opcode == O::VecZip) {
            if (VectorImmediate(ctx, 3))
                as.VILVH(bits, vr0, vr1, vr0);
            else
                as.VILVL(bits, vr0, vr1, vr0);
        } else {
            if (VectorImmediate(ctx, 3))
                as.VPICKOD(bits, vr0, vr1, vr0);
            else
                as.VPICKEV(bits, vr0, vr1, vr0);
        }
        StoreVR(writer, ctx, vr0);
        return;
    }
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
    if (writer.options.lsx) {
        auto& as = writer.as;
        LoadVR(writer, vr0, ctx.Arg(0));
        LoadVR(writer, vr1, ctx.Arg(1));
        if constexpr (opcode == O::VecTableLookup8) {
            as.VSLTI_B(vr3, vr1, 0);
            as.VANDI_B(vr2, vr1, 15);
            as.VSHUF_B(vr2, vr0, vr0, vr2);
            as.VANDN_V(vr0, vr3, vr2);
        } else {
            as.VSLTI_BU(vr3, vr1, 16);
            as.VSHUF_B(vr2, vr0, vr0, vr1);
            as.VAND_V(vr0, vr2, vr3);
        }
        StoreVR(writer, ctx, vr0);
        return;
    }
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
    writer.Call(reinterpret_cast<std::uintptr_t>(&crypto::Execute), HostSymbol::Crypto);
    writer.as.MV(ctx.destination, a0);
    writer.as.MV(ctx.high, a1);
}
}  // namespace

#define LA_BITWISE(name)                                                                           \
    void Writer::Handle(Op<O::name>, OpContext& ctx) { VectorBitwise<O::name>(*this, ctx); }
LA_BITWISE(Vec4And)
LA_BITWISE(Vec4Or)
LA_BITWISE(VecAnd)
LA_BITWISE(VecOr)
LA_BITWISE(VecXor)
LA_BITWISE(VecAndNot)
#undef LA_BITWISE

#define LA_ARITHMETIC(name)                                                                        \
    void Writer::Handle(Op<O::name>, OpContext& ctx) { VectorArithmetic<O::name>(*this, ctx); }
LA_ARITHMETIC(Vec4Add)
LA_ARITHMETIC(Vec4Sub)
LA_ARITHMETIC(Vec4Mul)
LA_ARITHMETIC(VecAdd)
LA_ARITHMETIC(VecSub)
LA_ARITHMETIC(VecMul)
LA_ARITHMETIC(VecCmpEq)
LA_ARITHMETIC(VecCmpGt)
LA_ARITHMETIC(VecAvg)
LA_ARITHMETIC(VecMin)
LA_ARITHMETIC(VecMax)
LA_ARITHMETIC(VecSatAdd)
LA_ARITHMETIC(VecSatSub)
#undef LA_ARITHMETIC

void Writer::Handle(Op<O::VecMulHigh16>, OpContext& ctx) {
    if (options.lsx) {
        LoadVR(*this, vr0, ctx.Arg(0));
        LoadVR(*this, vr1, ctx.Arg(1));
        if (VectorImmediate(ctx, 2))
            as.VMUH_H(vr0, vr0, vr1);
        else
            as.VMUH_HU(vr0, vr0, vr1);
        StoreVR(*this, ctx, vr0);
        return;
    }
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
    if (options.lsx) {
        LoadVR(*this, vr0, ctx.Arg(0));
        LoadVR(*this, vr1, ctx.Arg(1));
        as.VSSRANI_ZERO(bits, bool(VectorImmediate(ctx, 3)), vr1, vr0);
        StoreVR(*this, ctx, vr1);
        return;
    }
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
    if (options.lsx) {
        const bool is_signed = bool(VectorImmediate(ctx, 3));
        LoadVR(*this, vr0, ctx.Arg(0));
        for (unsigned cur = source; cur < destination; cur *= 2)
            as.VSLLWIL_ZERO(cur, is_signed, vr0, vr0);
        StoreVR(*this, ctx, vr0);
        return;
    }
    VectorBegin(*this, ctx, false);
    for (unsigned lane = 0; lane < 128 / destination; ++lane) {
        Extract(*this, t0, left, lane, source, VectorImmediate(ctx, 3));
        Insert(*this, t0, lane, destination);
    }
    VectorFinish(*this, ctx);
}

void Writer::Handle(Op<O::VecAbsDiffSum8>, OpContext& ctx) {
    if (options.lsx) {
        LoadVR(*this, vr0, ctx.Arg(0));
        LoadVR(*this, vr1, ctx.Arg(1));
        as.VABSD_BU(vr0, vr0, vr1);
        as.VHADDW_HU_BU(vr0, vr0, vr0);
        as.VHADDW_WU_HU(vr0, vr0, vr0);
        as.VHADDW_DU_WU(vr0, vr0, vr0);
        StoreVR(*this, ctx, vr0);
        return;
    }
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
    if (options.lsx) {
        LoadVR(*this, vr0, ctx.Arg(0));
        LoadVR(*this, vr1, ctx.Arg(1));
        as.VMULWEV(16, true, vr2, vr0, vr1);
        as.VMADDWOD_W_H(vr2, vr0, vr1);
        StoreVR(*this, ctx, vr2);
        return;
    }
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

#define LA_SHIFT(name)                                                                             \
    void Writer::Handle(Op<O::name>, OpContext& ctx) { VectorShift<O::name>(*this, ctx); }
LA_SHIFT(VecShiftLeft)
LA_SHIFT(VecShiftRight)
LA_SHIFT(VecShiftRightArithmetic)
LA_SHIFT(VecShiftLeftImm)
LA_SHIFT(VecShiftRightImm)
LA_SHIFT(VecShiftRightArithmeticImm)
#undef LA_SHIFT

void Writer::Handle(Op<O::VecByteShift>, OpContext& ctx) {
    const unsigned count = VectorImmediate(ctx, 2);
    if (count > 16) Reject(CompileError::UnsupportedOperand);
    if (options.lsx) {
        if (VectorImmediate(ctx, 3))
            EmitByteWindowLSX(*this, ctx, ctx.Arg(1), ctx.Arg(0), 16 - count);
        else
            EmitByteWindowLSX(*this, ctx, ctx.Arg(0), ctx.Arg(1), count);
        return;
    }
    VectorBegin(*this, ctx);
    if (VectorImmediate(ctx, 3))
        VectorWindow(*this, {a2, a3, a0, a1}, 128 - count * 8);
    else
        VectorWindow(*this, {a0, a1, a2, a3}, count * 8);
    VectorFinish(*this, ctx);
}

void Writer::Handle(Op<O::VecExtractBytes>, OpContext& ctx) {
    const unsigned count = VectorImmediate(ctx, 2);
    if (count > 16) Reject(CompileError::UnsupportedOperand);
    if (options.lsx) {
        EmitByteWindowLSX(*this, ctx, ctx.Arg(0), ctx.Arg(1), count);
        return;
    }
    VectorBegin(*this, ctx);
    VectorWindow(*this, {a0, a1, a2, a3}, count * 8);
    VectorFinish(*this, ctx);
}

#define LA_SHUFFLE(name)                                                                           \
    void Writer::Handle(Op<O::name>, OpContext& ctx) { VectorShuffle<O::name>(*this, ctx); }
LA_SHUFFLE(VecShuffle32)
LA_SHUFFLE(VecShuffle32TwoSrc)
LA_SHUFFLE(VecShuffle16)
LA_SHUFFLE(VecZip)
LA_SHUFFLE(VecUnzip)
LA_SHUFFLE(VecDupPairs32)
#undef LA_SHUFFLE

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

void Writer::Handle(Op<O::VecScalar>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    as.MV(ctx.destination, a0);
    as.MV(ctx.high, x0);
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
    if (options.lsx) {
        LoadVR(*this, vr0, ctx.Arg(0));
        as.VMSKLTZ(bits, vr0, vr0);
        as.VPICKVE2GR_DU(ctx.destination, vr0, 0);
        return;
    }
    VectorBegin(*this, ctx, false);
    for (unsigned lane = 0; lane < 128 / bits; ++lane) {
        Extract(*this, t0, left, lane, bits);
        as.SRLI(t0, t0, bits - 1);
        as.SLLI(t0, t0, lane);
        as.OR(a6, a6, t0);
    }
    as.MV(ctx.destination, a6);
}

#define LA_CRYPTO(name)                                                                            \
    void Writer::Handle(Op<O::name>, OpContext& ctx) { VectorCrypto<O::name>(*this, ctx); }
LA_CRYPTO(VecAesEnc)
LA_CRYPTO(VecAesEncLast)
LA_CRYPTO(VecAesDec)
LA_CRYPTO(VecAesDecLast)
LA_CRYPTO(VecAesEncFast)
LA_CRYPTO(VecAesEncLastFast)
LA_CRYPTO(VecAesDecFast)
LA_CRYPTO(VecAesDecLastFast)
LA_CRYPTO(VecAesImc)
LA_CRYPTO(VecAesKeygenAssist)
LA_CRYPTO(VecPclMul)
LA_CRYPTO(VecSha256Msg1)
LA_CRYPTO(VecSha256Msg2)
LA_CRYPTO(VecSha256Rnds2)
#undef LA_CRYPTO
}  // namespace swift::runtime::backend::loongarch64::detail
