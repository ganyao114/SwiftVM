#include "translator.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace swift::runtime::backend::loongarch64 {
namespace {

const VReg v0{0}, v1{1}, v2{2};
using BinaryInstruction = void (Xbyak_loongarch64::CodeGenerator::*)(
        const VReg&, const VReg&, const VReg&);

u32 LaneIndex(u64 bits) {
    switch (bits) {
        case 8: return 0;
        case 16: return 1;
        case 32: return 2;
        case 64: return 3;
        default: throw std::invalid_argument("invalid LoongArch vector lane width");
    }
}

void ReadVector(JitContext& context, ir::Value value, VReg reg) {
    context.PublishInput(value.Def());
    context.Address(t0, values, context.HomeOffset(value.Def()));
    context.GetMasm().vld(reg, t0, 0);
}

void WriteVector(JitContext& context, ir::Inst* inst, VReg reg) {
    // The producer writes the complete raw container, including narrow IR
    // annotations. The scalar cache must not later spill an obsolete half.
    context.DiscardValues();
    context.Address(t0, values, context.HomeOffset(inst));
    context.GetMasm().vst(reg, t0, 0);
    context.ReloadDefinition(inst);
}

void ReadPair(JitContext& context, ir::Value value, GPR low, GPR high) {
    // FlushValues() is required before these raw-home reads. ReadPart masks
    // scalar-width annotations; vector operations consume the whole container.
    context.ReadRawPart(low, value, 0);
    context.ReadRawPart(high, value, 1);
}

void WritePair(JitContext& context, ir::Inst* inst, GPR low, GPR high) {
    context.DiscardValues();
    context.WriteRawPart(inst, low, 0);
    context.WriteRawPart(inst, high, 1);
}

template <typename Operation>
void BinaryVector(JitContext& context, ir::Inst* inst, Operation operation) {
    context.FlushValues();
    ReadVector(context, inst->GetArg<ir::Value>(0), v0);
    ReadVector(context, inst->GetArg<ir::Value>(1), v1);
    operation(context.GetMasm());
    WriteVector(context, inst, v2);
}

void BinaryLane(JitContext& context, ir::Inst* inst, u64 bits,
                const std::array<BinaryInstruction, 4>& instructions) {
    const auto instruction = instructions[LaneIndex(bits)];
    BinaryVector(context, inst, [&](Assembler& as) { (as.*instruction)(v2, v0, v1); });
}

// LSX opcodes missing from the pinned Xbyak mnemonic set. All constants are
// independently checked with GNU binutils 2.44, using vd=0/vj=1/vk=2.
void RawBinary(Assembler& as, u32 opcode, VReg destination, VReg left, VReg right) {
    as.dd(opcode | (right.getIdx() << 10) | (left.getIdx() << 5) | destination.getIdx());
}

void BinaryRawLane(JitContext& context, ir::Inst* inst, u64 bits,
                   u32 opcode, bool reverse = false) {
    opcode += LaneIndex(bits) * 0x8000;
    BinaryVector(context, inst, [&](Assembler& as) {
        RawBinary(as, opcode, v2, reverse ? v1 : v0, reverse ? v0 : v1);
    });
}

template <typename ScalarOperation, typename VectorOperation>
void Bitwise(JitContext& context, ir::Inst* inst, ScalarOperation scalar, VectorOperation vector) {
    if (context.Features().lsx) {
        BinaryVector(context, inst, vector);
        return;
    }
    context.FlushValues();
    ReadPair(context, inst->GetArg<ir::Value>(0), t0, t1);
    ReadPair(context, inst->GetArg<ir::Value>(1), t2, t3);
    auto& as = context.GetMasm();
    scalar(as, t4, t0, t2);
    scalar(as, t5, t1, t3);
    WritePair(context, inst, t4, t5);
}

void ImmediateShift(JitContext& context, ir::Inst* inst, bool left, bool arithmetic) {
    const auto bits = inst->GetArg<ir::Imm>(2).Get();
    const auto width = LaneIndex(bits);
    const auto raw_count = inst->GetArg<ir::Imm>(1).Get();
    const auto count = arithmetic ? std::min(raw_count, bits - 1) : raw_count;
    context.FlushValues();
    ReadVector(context, inst->GetArg<ir::Value>(0), v0);
    auto& as = context.GetMasm();
    if (!arithmetic && count >= bits) as.vxor_v(v2, v2, v2);
    else {
#define SHIFT_CASE(index, suffix) \
        case index: \
            if (left) as.vslli_##suffix(v2, v0, u32(count)); \
            else if (arithmetic) as.vsrai_##suffix(v2, v0, u32(count)); \
            else as.vsrli_##suffix(v2, v0, u32(count)); \
            break;
        switch (width) {
            SHIFT_CASE(0, b)
            SHIFT_CASE(1, h)
            SHIFT_CASE(2, w)
            SHIFT_CASE(3, d)
        }
#undef SHIFT_CASE
    }
    WriteVector(context, inst, v2);
}

void VariableShift(JitContext& context, ir::Inst* inst, bool left, bool arithmetic) {
    const auto bits = inst->GetArg<ir::Imm>(2).Get();
    const auto width = LaneIndex(bits);
    context.FlushValues();
    ReadVector(context, inst->GetArg<ir::Value>(0), v0);
    context.Read(t0, inst->GetArg<ir::Value>(1));
    auto& as = context.GetMasm();
    Label in_range, finished;
    as.sltui(t1, t0, s32(bits));
    as.BNE(t1, zero, &in_range);
    if (arithmetic) as.LI(t0, bits - 1);
    else { as.vxor_v(v2, v2, v2); as.J(&finished); }
    as.Bind(&in_range);
    switch (width) {
        case 0: as.vreplgr2vr_b(v1, t0); break;
        case 1: as.vreplgr2vr_h(v1, t0); break;
        case 2: as.vreplgr2vr_w(v1, t0); break;
        case 3: as.vreplgr2vr_d(v1, t0); break;
    }
    RawBinary(as, (left ? 0x70e80000 : arithmetic ? 0x70ec0000 : 0x70ea0000) + width * 0x8000,
              v2, v0, v1);
    as.Bind(&finished);
    WriteVector(context, inst, v2);
}

void ByteShiftPair(JitContext& context, ir::Inst* inst, bool left, bool extract) {
    const auto count = inst->GetArg<ir::Imm>(2).Get();
    if (!count || count >= 16) throw std::invalid_argument("invalid LoongArch vector byte shift");
    const u32 amount = count * 8;
    context.FlushValues();
    ReadPair(context, inst->GetArg<ir::Value>(0), t0, t1);
    if (extract) ReadPair(context, inst->GetArg<ir::Value>(1), t2, t3);
    auto& as = context.GetMasm();
    if (extract) {
        if (amount == 64) { as.MV(t4, t1); as.MV(t5, t2); }
        else if (amount < 64) {
            as.srli_d(t4, t0, amount); as.slli_d(t6, t1, 64 - amount); as.or_(t4, t4, t6);
            as.srli_d(t5, t1, amount); as.slli_d(t6, t2, 64 - amount); as.or_(t5, t5, t6);
        } else {
            as.srli_d(t4, t1, amount - 64); as.slli_d(t6, t2, 128 - amount); as.or_(t4, t4, t6);
            as.srli_d(t5, t2, amount - 64); as.slli_d(t6, t3, 128 - amount); as.or_(t5, t5, t6);
        }
    } else if (amount >= 64) {
        as.MV(left ? t4 : t5, zero);
        if (amount == 64) as.MV(left ? t5 : t4, left ? t0 : t1);
        else if (left) as.slli_d(t5, t0, amount - 64);
        else as.srli_d(t4, t1, amount - 64);
    } else if (left) {
        as.slli_d(t4, t0, amount); as.slli_d(t5, t1, amount);
        as.srli_d(t6, t0, 64 - amount); as.or_(t5, t5, t6);
    } else {
        as.srli_d(t4, t0, amount); as.srli_d(t5, t1, amount);
        as.slli_d(t6, t1, 64 - amount); as.or_(t4, t4, t6);
    }
    WritePair(context, inst, t4, t5);
}

void StaticShuffle(JitContext& context, ir::Inst* inst, u32 bits,
                   const std::array<u32, 16>& indices,
                   const std::array<u32, 16>& sources, bool two) {
    (void)LaneIndex(bits);
    context.FlushValues();
    ReadPair(context, inst->GetArg<ir::Value>(0), t0, t1);
    if (two) ReadPair(context, inst->GetArg<ir::Value>(1), t2, t3);
    const std::array input{t0, t1, t2, t3};
    auto& as = context.GetMasm();
    as.MV(t4, zero); as.MV(t5, zero);
    for (u32 lane = 0; lane < 128 / bits; ++lane) {
        const auto source_bit = indices[lane] * bits;
        const auto destination_bit = lane * bits;
        as.srli_d(t6, input[sources[lane] * 2 + source_bit / 64], source_bit % 64);
        context.Mask(t6, bits);
        if (destination_bit % 64) as.slli_d(t6, t6, destination_bit % 64);
        as.or_(destination_bit < 64 ? t4 : t5, destination_bit < 64 ? t4 : t5, t6);
    }
    WritePair(context, inst, t4, t5);
}

void DynamicByteShuffle(JitContext& context, ir::Inst* inst, bool table_lookup) {
    context.FlushValues();
    ReadPair(context, inst->GetArg<ir::Value>(0), t0, t1);
    ReadPair(context, inst->GetArg<ir::Value>(1), t2, t3);
    auto& as = context.GetMasm();
    as.MV(t4, zero); as.MV(t5, zero);
    for (u32 lane = 0; lane < 16; ++lane) {
        as.srli_d(t6, lane < 8 ? t2 : t3, (lane % 8) * 8);
        as.andi(t6, t6, 255);
        Label zero_value, lower, assembled;
        if (table_lookup) {
            as.andi(t7, t6, 128); as.BNE(t7, zero, &zero_value); as.andi(t6, t6, 15);
        } else {
            as.sltui(t7, t6, 16); as.BEQ(t7, zero, &zero_value);
        }
        as.sltui(t7, t6, 8); as.slli_d(t6, t6, 3); as.BNE(t7, zero, &lower);
        as.srl_d(t7, t1, t6); as.J(&assembled);
        as.Bind(&lower); as.srl_d(t7, t0, t6); as.J(&assembled);
        as.Bind(&zero_value); as.MV(t7, zero);
        as.Bind(&assembled); as.andi(t7, t7, 255);
        if (lane % 8) as.slli_d(t7, t7, (lane % 8) * 8);
        as.or_(lane < 8 ? t4 : t5, lane < 8 ? t4 : t5, t7);
    }
    WritePair(context, inst, t4, t5);
}

}  // namespace

#define LANE_METHOD(name, mnemonic, width) \
    void JitTranslator::Emit##name(ir::Inst* inst) { \
        if (!context.Features().lsx) { EmitHelper(inst); return; } \
        BinaryLane(context, inst, width, \
            {&Assembler::mnemonic##_b, &Assembler::mnemonic##_h, \
             &Assembler::mnemonic##_w, &Assembler::mnemonic##_d}); \
    }
LANE_METHOD(Vec4Add, vadd, 32)
LANE_METHOD(Vec4Mul, vmul, 32)
LANE_METHOD(VecAdd, vadd, inst->GetArg<ir::Imm>(2).Get())
LANE_METHOD(VecMul, vmul, inst->GetArg<ir::Imm>(2).Get())
LANE_METHOD(VecCmpEq, vseq, inst->GetArg<ir::Imm>(2).Get())
#undef LANE_METHOD

void JitTranslator::EmitVec4Sub(ir::Inst* inst) {
    if (!context.Features().lsx) { EmitHelper(inst); return; }
    BinaryRawLane(context, inst, 32, 0x700c0000);
}
void JitTranslator::EmitVecSub(ir::Inst* inst) {
    if (!context.Features().lsx) { EmitHelper(inst); return; }
    BinaryRawLane(context, inst, inst->GetArg<ir::Imm>(2).Get(), 0x700c0000);
}
void JitTranslator::EmitVecCmpGt(ir::Inst* inst) {
    if (!context.Features().lsx) { EmitHelper(inst); return; }
    BinaryRawLane(context, inst, inst->GetArg<ir::Imm>(2).Get(), 0x70060000, true);
}
void JitTranslator::EmitVecAvg(ir::Inst* inst) {
    const auto bits = inst->GetArg<ir::Imm>(2).Get();
    if (bits != 8 && bits != 16) throw std::invalid_argument("LoongArch VecAvg requires byte/halfword lanes");
    if (!context.Features().lsx) { EmitHelper(inst); return; }
    BinaryRawLane(context, inst, bits, 0x706a0000);
}

#define MINMAX_METHOD(name, mnemonic) \
    void JitTranslator::Emit##name(ir::Inst* inst) { \
        if (!context.Features().lsx) { EmitHelper(inst); return; } \
        const auto bits = inst->GetArg<ir::Imm>(2).Get(); \
        if (inst->GetArg<ir::Imm>(3).Get()) \
            BinaryLane(context, inst, bits, {&Assembler::mnemonic##_b, &Assembler::mnemonic##_h, \
                       &Assembler::mnemonic##_w, &Assembler::mnemonic##_d}); \
        else BinaryLane(context, inst, bits, {&Assembler::mnemonic##_bu, &Assembler::mnemonic##_hu, \
                        &Assembler::mnemonic##_wu, &Assembler::mnemonic##_du}); \
    }
MINMAX_METHOD(VecMin, vmin)
MINMAX_METHOD(VecMax, vmax)
#undef MINMAX_METHOD

void JitTranslator::EmitVecSatAdd(ir::Inst* inst) {
    if (!context.Features().lsx) { EmitHelper(inst); return; }
    BinaryRawLane(context, inst, inst->GetArg<ir::Imm>(2).Get(),
                  inst->GetArg<ir::Imm>(3).Get() ? 0x70460000 : 0x704a0000);
}
void JitTranslator::EmitVecSatSub(ir::Inst* inst) {
    if (!context.Features().lsx) { EmitHelper(inst); return; }
    BinaryRawLane(context, inst, inst->GetArg<ir::Imm>(2).Get(),
                  inst->GetArg<ir::Imm>(3).Get() ? 0x70480000 : 0x704c0000);
}
void JitTranslator::EmitVecMulHigh16(ir::Inst* inst) {
    if (!context.Features().lsx) { EmitHelper(inst); return; }
    BinaryRawLane(context, inst, 16, inst->GetArg<ir::Imm>(2).Get() ? 0x70860000 : 0x70880000);
}
void JitTranslator::EmitVecMulWiden(ir::Inst* inst) {
    const auto bits = inst->GetArg<ir::Imm>(2).Get();
    if (bits != 8 && bits != 16 && bits != 32) throw std::invalid_argument("invalid LoongArch widening multiply width");
    if (!context.Features().lsx) { EmitHelper(inst); return; }
    BinaryRawLane(context, inst, bits, inst->GetArg<ir::Imm>(3).Get() ? 0x70900000 : 0x70980000);
}

#define BITWISE_METHOD(name, scalar_instruction, vector_instruction) \
    void JitTranslator::Emit##name(ir::Inst* inst) { \
        Bitwise(context, inst, \
            [](Assembler& as, GPR rd, GPR rj, GPR rk) { as.scalar_instruction(rd, rj, rk); }, \
            [](Assembler& as) { as.vector_instruction(v2, v0, v1); }); \
    }
BITWISE_METHOD(Vec4And, and_, vand_v)
BITWISE_METHOD(VecAnd, and_, vand_v)
BITWISE_METHOD(Vec4Or, or_, vor_v)
BITWISE_METHOD(VecOr, or_, vor_v)
BITWISE_METHOD(VecXor, xor_, vxor_v)
#undef BITWISE_METHOD
void JitTranslator::EmitVecAndNot(ir::Inst* inst) {
    Bitwise(context, inst,
        [](Assembler& as, GPR rd, GPR rj, GPR rk) { as.nor(t6, rk, zero); as.and_(rd, rj, t6); },
        // LSX vandn.v computes ~vj & vk; IR requires lhs & ~rhs.
        [](Assembler& as) { as.vandn_v(v2, v1, v0); });
}

#define SHIFT_METHOD(name, helper, left, arithmetic) \
    void JitTranslator::Emit##name(ir::Inst* inst) { \
        if (!context.Features().lsx) { EmitHelper(inst); return; } \
        helper(context, inst, left, arithmetic); \
    }
SHIFT_METHOD(VecShiftLeftImm, ImmediateShift, true, false)
SHIFT_METHOD(VecShiftRightImm, ImmediateShift, false, false)
SHIFT_METHOD(VecShiftRightArithmeticImm, ImmediateShift, false, true)
SHIFT_METHOD(VecShiftLeft, VariableShift, true, false)
SHIFT_METHOD(VecShiftRight, VariableShift, false, false)
SHIFT_METHOD(VecShiftRightArithmetic, VariableShift, false, true)
#undef SHIFT_METHOD

void JitTranslator::EmitVecLoadConst(ir::Inst* inst) {
    context.FlushValues();
    auto& as = context.GetMasm();
    as.LI(t4, inst->GetArg<ir::Imm>(0).Get()); as.LI(t5, inst->GetArg<ir::Imm>(1).Get());
    WritePair(context, inst, t4, t5);
}
void JitTranslator::EmitVecSharedZero(ir::Inst* inst) {
    context.FlushValues();
    WritePair(context, inst, zero, zero);
}
void JitTranslator::EmitVecDup64(ir::Inst* inst) {
    context.FlushValues();
    context.Read(t4, inst->GetArg<ir::Value>(0));
    WritePair(context, inst, t4, t4);
}
void JitTranslator::EmitVecExtract64(ir::Inst* inst) {
    context.FlushValues();
    ReadPair(context, inst->GetArg<ir::Value>(0), t0, t1);
    context.Write(inst, inst->GetArg<ir::Imm>(1).Get() & 1 ? t1 : t0);
}
void JitTranslator::EmitVecExtract16(ir::Inst* inst) {
    context.FlushValues();
    ReadPair(context, inst->GetArg<ir::Value>(0), t0, t1);
    const u32 lane = inst->GetArg<ir::Imm>(1).Get() & 7;
    context.GetMasm().bstrpick_d(t2, lane < 4 ? t0 : t1, (lane % 4) * 16 + 15, (lane % 4) * 16);
    context.Write(inst, t2);
}
void JitTranslator::EmitVecInsert16(ir::Inst* inst) {
    context.FlushValues();
    ReadPair(context, inst->GetArg<ir::Value>(0), t0, t1);
    context.Read(t2, inst->GetArg<ir::Value>(1));
    const u32 lane = inst->GetArg<ir::Imm>(2).Get() & 7;
    context.GetMasm().bstrins_d(lane < 4 ? t0 : t1, t2, (lane % 4) * 16 + 15, (lane % 4) * 16);
    WritePair(context, inst, t0, t1);
}
void JitTranslator::EmitVecMovMask(ir::Inst* inst) {
    const auto bits = inst->GetArg<ir::Imm>(1).Get();
    if (bits != 8 && bits != 32 && bits != 64) throw std::invalid_argument("invalid LoongArch vector sign mask width");
    context.FlushValues();
    ReadPair(context, inst->GetArg<ir::Value>(0), t0, t1);
    auto& as = context.GetMasm();
    as.MV(t4, zero);
    for (u32 lane = 0; lane < 128 / bits; ++lane) {
        const u32 bit = lane * bits + bits - 1;
        as.bstrpick_d(t2, bit < 64 ? t0 : t1, bit % 64, bit % 64);
        if (lane) as.slli_d(t2, t2, lane);
        as.or_(t4, t4, t2);
    }
    context.Write(inst, t4);
}

void JitTranslator::EmitVecShuffle32(ir::Inst* inst) {
    const auto control = inst->GetArg<ir::Imm>(1).Get() & 255;
    if (context.Features().lsx) {
        context.FlushValues(); ReadVector(context, inst->GetArg<ir::Value>(0), v0);
        context.GetMasm().vshuf4i_w(v2, v0, u32(control)); WriteVector(context, inst, v2);
        return;
    }
    std::array<u32, 16> indices{}, sources{};
    for (u32 lane = 0; lane < 4; ++lane) indices[lane] = (control >> (lane * 2)) & 3;
    StaticShuffle(context, inst, 32, indices, sources, false);
}
void JitTranslator::EmitVecShuffle32TwoSrc(ir::Inst* inst) {
    const auto control = inst->GetArg<ir::Imm>(2).Get();
    std::array<u32, 16> indices{}, sources{};
    for (u32 lane = 0; lane < 4; ++lane) {
        indices[lane] = (control >> (lane * 2)) & 3; sources[lane] = lane >= 2;
    }
    StaticShuffle(context, inst, 32, indices, sources, true);
}
void JitTranslator::EmitVecShuffle16(ir::Inst* inst) {
    const auto control = inst->GetArg<ir::Imm>(1).Get();
    const u32 base = inst->GetArg<ir::Imm>(2).Get() ? 4 : 0;
    std::array<u32, 16> indices{}, sources{};
    for (u32 lane = 0; lane < 8; ++lane)
        indices[lane] = lane >= base && lane < base + 4 ? base + ((control >> ((lane - base) * 2)) & 3) : lane;
    StaticShuffle(context, inst, 16, indices, sources, false);
}
void JitTranslator::EmitVecDupPairs32(ir::Inst* inst) {
    const bool odd = inst->GetArg<ir::Imm>(1).Get() != 0;
    if (context.Features().lsx) {
        context.FlushValues(); ReadVector(context, inst->GetArg<ir::Value>(0), v0);
        context.GetMasm().vshuf4i_w(v2, v0, odd ? 0xf5 : 0xa0); WriteVector(context, inst, v2);
        return;
    }
    std::array<u32, 16> indices{}, sources{};
    for (u32 lane = 0; lane < 4; ++lane) indices[lane] = (lane & ~1u) + odd;
    StaticShuffle(context, inst, 32, indices, sources, false);
}

void JitTranslator::EmitVecZip(ir::Inst* inst) {
    const auto bits = inst->GetArg<ir::Imm>(2).Get();
    const bool high = inst->GetArg<ir::Imm>(3).Get() != 0;
    const auto index = LaneIndex(bits);
    if (context.Features().lsx) {
        const std::array<BinaryInstruction, 4> low{&Assembler::vilvl_b, &Assembler::vilvl_h,
                                                  &Assembler::vilvl_w, &Assembler::vilvl_d};
        const std::array<BinaryInstruction, 4> upper{&Assembler::vilvh_b, &Assembler::vilvh_h,
                                                    &Assembler::vilvh_w, &Assembler::vilvh_d};
        BinaryVector(context, inst, [&](Assembler& as) { (as.*(high ? upper[index] : low[index]))(v2, v1, v0); });
        return;
    }
    std::array<u32, 16> indices{}, sources{};
    for (u32 lane = 0; lane < 128 / bits; ++lane) {
        indices[lane] = lane / 2 + (high ? 64 / bits : 0); sources[lane] = lane & 1;
    }
    StaticShuffle(context, inst, u32(bits), indices, sources, true);
}
void JitTranslator::EmitVecUnzip(ir::Inst* inst) {
    const auto bits = inst->GetArg<ir::Imm>(2).Get();
    const bool odd = inst->GetArg<ir::Imm>(3).Get() != 0;
    const auto index = LaneIndex(bits);
    if (context.Features().lsx) {
        const std::array<BinaryInstruction, 4> even{&Assembler::vpickev_b, &Assembler::vpickev_h,
                                                   &Assembler::vpickev_w, &Assembler::vpickev_d};
        const std::array<BinaryInstruction, 4> upper{&Assembler::vpickod_b, &Assembler::vpickod_h,
                                                    &Assembler::vpickod_w, &Assembler::vpickod_d};
        BinaryVector(context, inst, [&](Assembler& as) { (as.*(odd ? upper[index] : even[index]))(v2, v1, v0); });
        return;
    }
    std::array<u32, 16> indices{}, sources{};
    for (u32 lane = 0; lane < 128 / bits; ++lane) {
        indices[lane] = (lane % (64 / bits)) * 2 + odd; sources[lane] = lane >= 64 / bits;
    }
    StaticShuffle(context, inst, u32(bits), indices, sources, true);
}

void JitTranslator::EmitVecByteShift(ir::Inst* inst) {
    const auto count = inst->GetArg<ir::Imm>(2).Get();
    const bool left = inst->GetArg<ir::Imm>(3).Get() != 0;
    if (!count || count >= 16) throw std::invalid_argument("invalid LoongArch vector byte shift");
    if (!context.Features().lsx) { ByteShiftPair(context, inst, left, false); return; }
    context.FlushValues(); ReadVector(context, inst->GetArg<ir::Value>(0), v0);
    auto& as = context.GetMasm();
    if (left) as.vbsll_v(v2, v0, u32(count)); else as.vbsrl_v(v2, v0, u32(count));
    WriteVector(context, inst, v2);
}
void JitTranslator::EmitVecExtractBytes(ir::Inst* inst) {
    const auto count = inst->GetArg<ir::Imm>(2).Get();
    if (!count || count >= 16) throw std::invalid_argument("invalid LoongArch vector byte shift");
    if (!context.Features().lsx) { ByteShiftPair(context, inst, false, true); return; }
    BinaryVector(context, inst, [&](Assembler& as) {
        as.vbsrl_v(v2, v0, u32(count)); as.vbsll_v(v1, v1, u32(16 - count)); as.vor_v(v2, v2, v1);
    });
}
void JitTranslator::EmitVecShuffle32Indexed(ir::Inst* inst) { DynamicByteShuffle(context, inst, false); }
void JitTranslator::EmitVecTableLookup8(ir::Inst* inst) { DynamicByteShuffle(context, inst, true); }

}  // namespace swift::runtime::backend::loongarch64
