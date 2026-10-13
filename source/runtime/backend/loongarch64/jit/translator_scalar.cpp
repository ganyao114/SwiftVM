#include "translator.h"

namespace swift::runtime::backend::loongarch64 {
namespace {

u32 Bits(ir::ValueType type) { return ir::GetValueSizeByte(type) * 8; }

bool Scalar(ir::Inst* inst) {
    return !ir::IsFloatValueType(inst->ReturnType()) && Bits(inst->ReturnType()) <= 64;
}

void ReadBinary(JitContext& context, ir::Inst* inst) {
    context.Read(t0, inst->GetArg<ir::Value>(0));
    context.Operand(t1, inst->GetArg<ir::Operand>(1));
}

void CopyValue(JitContext& context, ir::Inst* inst, ir::Value source) {
    // BitCast/GetResult preserve the entire raw slot, even through U8/U32
    // return annotations. Scalar consumers subsequently apply their views;
    // a later vector bitcast must still recover the untouched upper bits.
    context.FlushValues(); context.DiscardValues();
    context.ReadRawPart(t0, source, 0); context.ReadRawPart(t1, source, 1);
    context.WriteRawPart(inst, t0, 0); context.WriteRawPart(inst, t1, 1);
}

bool Immediate(const ir::Operand& operand, s64& result) {
    if (!operand.GetLeft().IsImm() ||
        (!operand.GetRight().Null() && operand.GetOp().type != ir::OperandOp::None))
        return false;
    result = static_cast<s64>(operand.GetLeft().imm.Get());
    return true;
}

enum class Shift { Left, Right, Arithmetic, Rotate };

void ShiftImmediate(JitContext& context, ir::Inst* inst, Shift kind) {
    auto& as = context.GetMasm();
    const auto value = inst->GetArg<ir::Value>(0);
    const auto bits = Bits(inst->ReturnType());
    const u32 shift = inst->GetArg<ir::Imm>(1).Get() & (bits - 1);
    context.Read(t0, value);
    if (kind == Shift::Arithmetic) context.Sign(t0, Bits(value.Type()));
    else context.Mask(t0, bits);
    if (kind == Shift::Left) as.slli_d(t0, t0, shift);
    else if (kind == Shift::Right) as.srli_d(t0, t0, shift);
    else if (kind == Shift::Arithmetic) as.srai_d(t0, t0, shift);
    else if (bits == 64) as.rotri_d(t0, t0, shift);
    else if (bits == 32) as.rotri_w(t0, t0, shift);
    else if (shift) {
        as.srli_d(t1, t0, shift);
        as.slli_d(t0, t0, bits - shift);
        as.or_(t0, t0, t1);
    }
    context.Write(inst, t0);
}

void ShiftValue(JitContext& context, ir::Inst* inst, Shift kind) {
    auto& as = context.GetMasm();
    const auto value = inst->GetArg<ir::Value>(0);
    const auto bits = Bits(inst->ReturnType());
    context.Read(t0, value);
    context.Read(t1, inst->GetArg<ir::Value>(1));
    as.andi(t1, t1, bits - 1);
    if (kind == Shift::Arithmetic) context.Sign(t0, Bits(value.Type()));
    else context.Mask(t0, bits);
    if (kind == Shift::Left) as.sll_d(t0, t0, t1);
    else if (kind == Shift::Right) as.srl_d(t0, t0, t1);
    else if (kind == Shift::Arithmetic) as.sra_d(t0, t0, t1);
    else if (bits == 64) as.rotr_d(t0, t0, t1);
    else if (bits == 32) as.rotr_w(t0, t0, t1);
    else {
        as.sub_d(t2, zero, t1);
        as.andi(t2, t2, bits - 1);
        as.sll_d(t2, t0, t2);
        as.srl_d(t0, t0, t1);
        as.or_(t0, t0, t2);
    }
    context.Write(inst, t0);
}

void SelectValue(JitContext& context, ir::Inst* inst) {
    auto& as = context.GetMasm();
    // t0 is an all-ones/all-zero predicate. Reading both candidates keeps
    // value-cache state identical for both runtime outcomes.
    if (ir::IsFloatValueType(inst->ReturnType())) {
        // Vector selects, including V8/V32/V64 annotations, have raw 16-byte
        // slot semantics. The scalar cache deliberately normalizes views.
        context.FlushValues(); context.DiscardValues();
        for (u32 part = 0; part < 2; ++part) {
            context.ReadRawPart(t1, inst->GetArg<ir::Value>(1), part);
            context.ReadRawPart(t2, inst->GetArg<ir::Value>(2), part);
            as.xor_(t1, t1, t2); as.and_(t1, t1, t0); as.xor_(t1, t1, t2);
            context.WriteRawPart(inst, t1, part);
        }
        return;
    }
    const u32 parts = (u32(ir::GetValueSizeByte(inst->ReturnType())) + 7) / 8;
    for (u32 part = 0; part < parts; ++part) {
        context.ReadPart(t1, inst->GetArg<ir::Value>(1), part);
        context.ReadPart(t2, inst->GetArg<ir::Value>(2), part);
        as.xor_(t1, t1, t2);
        as.and_(t1, t1, t0);
        as.xor_(t1, t1, t2);
        context.WritePart(inst, t1, part);
    }
}

}  // namespace

void JitTranslator::EmitZero(ir::Inst* inst) {
    if (Bits(inst->ReturnType()) > 128) { EmitHelper(inst); return; }
    const u32 parts = (u32(ir::GetValueSizeByte(inst->ReturnType())) + 7) / 8;
    for (u32 part = 0; part < parts; ++part)
        context.WritePart(inst, zero, part);
}

void JitTranslator::EmitLoadImm(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    if (context.Rematerialized(inst)) return;
    context.GetMasm().LI(t0, inst->GetArg<ir::Imm>(0).Get());
    context.Write(inst, t0);
}

void JitTranslator::EmitBitCast(ir::Inst* inst) {
    if (Bits(inst->ReturnType()) > 128) { EmitHelper(inst); return; }
    CopyValue(context, inst, inst->GetArg<ir::Value>(0));
}

void JitTranslator::EmitGetResult(ir::Inst* inst) {
    if (Bits(inst->ReturnType()) > 128) { EmitHelper(inst); return; }
    CopyValue(context, inst, inst->GetArg<ir::Value>(0));
}

void JitTranslator::EmitGetOperand(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    context.Operand(t0, inst->GetArg<ir::Operand>(0));
    context.Write(inst, t0);
}

void JitTranslator::EmitAdd(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0));
    const auto operand = inst->GetArg<ir::Operand>(1);
    s64 immediate{};
    if (Immediate(operand, immediate) && immediate >= -2048 && immediate <= 2047)
        as.addi_d(t0, t0, static_cast<s32>(immediate));
    else { context.Operand(t1, operand); as.add_d(t0, t0, t1); }
    context.Write(inst, t0);
}

void JitTranslator::EmitSub(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0));
    const auto operand = inst->GetArg<ir::Operand>(1);
    s64 immediate{};
    if (Immediate(operand, immediate) && immediate >= -2047 && immediate <= 2048)
        as.addi_d(t0, t0, static_cast<s32>(-immediate));
    else { context.Operand(t1, operand); as.sub_d(t0, t0, t1); }
    context.Write(inst, t0);
}

#define SCALAR_BINARY(name, mnemonic) \
    void JitTranslator::Emit##name(ir::Inst* inst) { \
        if (!Scalar(inst)) { EmitHelper(inst); return; } \
        ReadBinary(context, inst); \
        context.GetMasm().mnemonic(t0, t0, t1); \
        context.Write(inst, t0); \
    }
SCALAR_BINARY(Mul, mul_d)
SCALAR_BINARY(And, and_)
SCALAR_BINARY(Or, or_)
SCALAR_BINARY(Xor, xor_)
#undef SCALAR_BINARY

void JitTranslator::EmitAdc(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    auto& as = context.GetMasm();
    ReadBinary(context, inst);
    context.ReadFlags(t2);
    as.bstrpick_d(t2, t2, 29, 29);
    as.add_d(t0, t0, t1);
    as.add_d(t0, t0, t2);
    context.Write(inst, t0);
}

void JitTranslator::EmitSbb(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    auto& as = context.GetMasm();
    ReadBinary(context, inst);
    context.ReadFlags(t2);
    as.bstrpick_d(t2, t2, 29, 29);
    as.xori(t2, t2, 1);  // The virtual carry has ARM's !borrow polarity.
    as.sub_d(t0, t0, t1);
    as.sub_d(t0, t0, t2);
    context.Write(inst, t0);
}

void JitTranslator::EmitAndNot(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    ReadBinary(context, inst);
    auto& as = context.GetMasm();
    as.nor(t1, t1, zero);
    as.and_(t0, t0, t1);
    context.Write(inst, t0);
}

void JitTranslator::EmitNot(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    auto& as = context.GetMasm();
    if (inst->ArgAt(1).IsVoid()) {
        context.Read(t0, inst->GetArg<ir::Value>(0));
        as.sltui(t0, t0, 1);
    } else {
        context.Operand(t0, inst->GetArg<ir::Operand>(1));
        as.nor(t0, t0, zero);
    }
    context.Write(inst, t0);
}

void JitTranslator::EmitNeg(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    context.Read(t0, inst->GetArg<ir::Value>(0));
    context.GetMasm().sub_d(t0, zero, t0);
    context.Write(inst, t0);
}

void JitTranslator::EmitMulSub(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0));
    context.Read(t1, inst->GetArg<ir::Value>(1));
    context.Read(t2, inst->GetArg<ir::Value>(2));
    as.mul_d(t0, t0, t1);
    as.sub_d(t0, t2, t0);
    context.Write(inst, t0);
}

void JitTranslator::EmitMulHigh(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0));
    context.Read(t1, inst->GetArg<ir::Value>(1));
    if (inst->GetArg<ir::Imm>(2).Get()) as.mulh_d(t0, t0, t1);
    else as.mulh_du(t0, t0, t1);
    context.Write(inst, t0);
}

void JitTranslator::EmitDiv(ir::Inst* inst) {
    if (!Scalar(inst)) { EmitHelper(inst); return; }
    auto& as = context.GetMasm();
    const auto type = inst->GetArg<ir::Value>(0).Type();
    ReadBinary(context, inst);
    context.Mask(t0, Bits(type)); context.Mask(t1, Bits(type));
    Label nonzero, done;
    as.BNE(t1, zero, &nonzero);
    as.MV(t0, zero); as.J(&done);
    as.Bind(&nonzero);
    if (ir::IsSignValueType(type)) {
        context.Sign(t0, Bits(type)); context.Sign(t1, Bits(type));
        as.div_d(t0, t0, t1);
    } else as.div_du(t0, t0, t1);
    as.Bind(&done);
    context.Write(inst, t0);
}

void JitTranslator::EmitSignedDiv64(ir::Inst* inst) {
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0));
    context.Read(t1, inst->GetArg<ir::Value>(1));
    Label nonzero, done;
    as.BNE(t1, zero, &nonzero);
    as.MV(t0, zero); as.J(&done);
    as.Bind(&nonzero); as.div_d(t0, t0, t1); as.Bind(&done);
    context.Write(inst, t0);
}

#define SCALAR_EXTEND(name, action) \
    void JitTranslator::Emit##name(ir::Inst* inst) { \
        context.Read(t0, inst->GetArg<ir::Value>(0)); \
        action; \
        context.Write(inst, t0); \
    }
SCALAR_EXTEND(ZeroExtend32, context.Mask(t0, 32))
SCALAR_EXTEND(ZeroExtend32To64, context.Mask(t0, 32))
SCALAR_EXTEND(ZeroExtend64, (void)0)
SCALAR_EXTEND(SignExtend, context.Sign(t0, Bits(inst->GetArg<ir::Value>(0).Type())))
#undef SCALAR_EXTEND

#define SCALAR_SHIFT(name, kind, function) \
    void JitTranslator::Emit##name(ir::Inst* inst) { \
        if (!Scalar(inst)) { EmitHelper(inst); return; } \
        function(context, inst, Shift::kind); \
    }
SCALAR_SHIFT(LslImm, Left, ShiftImmediate)
SCALAR_SHIFT(LsrImm, Right, ShiftImmediate)
SCALAR_SHIFT(AsrImm, Arithmetic, ShiftImmediate)
SCALAR_SHIFT(RorImm, Rotate, ShiftImmediate)
SCALAR_SHIFT(LslValue, Left, ShiftValue)
SCALAR_SHIFT(LsrValue, Right, ShiftValue)
SCALAR_SHIFT(AsrValue, Arithmetic, ShiftValue)
SCALAR_SHIFT(RorValue, Rotate, ShiftValue)
#undef SCALAR_SHIFT

void JitTranslator::EmitSelect(ir::Inst* inst) {
    if (Bits(inst->ReturnType()) > 128) { EmitHelper(inst); return; }
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0));
    as.sltu(t0, zero, t0); as.sub_d(t0, zero, t0);
    SelectValue(context, inst);
}

void JitTranslator::EmitSelectZero(ir::Inst* inst) {
    if (Bits(inst->ReturnType()) > 128) { EmitHelper(inst); return; }
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0));
    as.sltui(t0, t0, 1); as.sub_d(t0, zero, t0);
    SelectValue(context, inst);
}

void JitTranslator::EmitCondSelect(ir::Inst* inst) {
    if (Bits(inst->ReturnType()) > 128) { EmitHelper(inst); return; }
    EmitCondition(inst->GetArg<ir::Cond>(0), t0);
    context.GetMasm().sub_d(t0, zero, t0);
    SelectValue(context, inst);
}

void JitTranslator::EmitTestZero(ir::Inst* inst) {
    context.Read(t0, inst->GetArg<ir::Value>(0));
    context.GetMasm().sltui(t0, t0, 1);
    context.Write(inst, t0);
}

void JitTranslator::EmitTestNotZero(ir::Inst* inst) {
    context.Read(t0, inst->GetArg<ir::Value>(0));
    context.GetMasm().sltu(t0, zero, t0);
    context.Write(inst, t0);
}

void JitTranslator::EmitTestBit(ir::Inst* inst) {
    context.Read(t0, inst->GetArg<ir::Value>(0));
    const u32 bit = inst->GetArg<ir::Imm>(1).Get() & 63;
    context.GetMasm().bstrpick_d(t0, t0, bit, bit);
    context.Write(inst, t0);
}

}  // namespace swift::runtime::backend::loongarch64
