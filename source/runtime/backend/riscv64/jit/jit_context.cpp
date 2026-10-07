#include "jit_context.h"

#include "runtime/backend/context.h"
#include "runtime/backend/riscv64/defines.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;

void JitContext::EnsureSpace() {
    auto& buffer = masm.GetCodeBuffer();
    if (buffer.GetRemainingBytes() < 1024)
        buffer.Grow(buffer.GetSizeInBytes() + 4096);
}

void JitContext::Address(GPR result, GPR base, s64 offset) {
    if (offset >= -2048 && offset <= 2047) {
        masm.ADDI(result, base, static_cast<s32>(offset));
    } else {
        // t6 is reserved for addresses, never an SSA or operand register.
        ASSERT(base != t6);
        masm.LI(t6, static_cast<u64>(offset));
        masm.ADD(result, base, t6);
    }
}

void JitContext::Load(GPR result, GPR base, s64 offset, u32 size) {
    Address(t6, base, offset);
    switch (size) {
        case 1: masm.LBU(result, 0, t6); break;
        case 2: masm.LHU(result, 0, t6); break;
        case 4: masm.LWU(result, 0, t6); break;
        case 8: masm.LD(result, 0, t6); break;
        default: PANIC("unsupported RV64 scalar load size {}", size);
    }
}

void JitContext::Store(GPR value, GPR base, s64 offset, u32 size) {
    ASSERT(value != t6);
    Address(t6, base, offset);
    switch (size) {
        case 1: masm.SB(value, 0, t6); break;
        case 2: masm.SH(value, 0, t6); break;
        case 4: masm.SW(value, 0, t6); break;
        case 8: masm.SD(value, 0, t6); break;
        default: PANIC("unsupported RV64 scalar store size {}", size);
    }
}

void JitContext::Read(GPR result, ir::Value value) {
    Load(result, values, static_cast<s64>(value.Def()->Id()) * kValueStride);
}

void JitContext::Data(GPR result, const ir::DataClass& data) {
    if (data.IsValue()) Read(result, data.value);
    else if (data.IsImm()) masm.LI(result, data.imm.Get());
    else masm.MV(result, x0);
}

void JitContext::Operand(GPR result, const ir::Operand& operand) {
    ASSERT(result != t3 && result != t4 && result != t6);
    Data(result, operand.GetLeft());
    if (operand.GetRight().Null() || operand.GetOp().type == ir::OperandOp::None) return;
    Data(t3, operand.GetRight());
    switch (operand.GetOp().type) {
        case ir::OperandOp::Plus: masm.ADD(result, result, t3); break;
        case ir::OperandOp::PlusExt:
            masm.SLLI(t3, t3, operand.GetOp().shift_ext);
            masm.ADD(result, result, t3);
            break;
        case ir::OperandOp::LSL: masm.SLL(result, result, t3); break;
        case ir::OperandOp::LSR:
            if (operand.GetLeft().IsValue())
                Mask(result, ir::GetValueSizeByte(operand.GetLeft().value.Type()) * 8);
            masm.SRL(result, result, t3);
            break;
        default: PANIC("unsupported RV64 operand");
    }
}

void JitContext::Mask(GPR value, u32 bits) {
    ASSERT(bits > 0 && bits <= 64);
    if (bits < 64) {
        // Biscuit::ZEXTW emits Zba ADD.UW; shifts require only RV64I.
        masm.SLLI(value, value, 64 - bits);
        masm.SRLI(value, value, 64 - bits);
    }
}

void JitContext::SignExtend(GPR value, u32 bits) {
    ASSERT(bits > 0 && bits <= 64);
    if (bits < 64) {
        masm.SLLI(value, value, 64 - bits);
        masm.SRAI(value, value, 64 - bits);
    }
}

void JitContext::Write(ir::Inst* inst, GPR value) {
    if (inst->ReturnType() == ir::ValueType::VOID) return;
    Mask(value, ir::GetValueSizeByte(inst->ReturnType()) * 8);
    Store(value, values, static_cast<s64>(inst->Id()) * kValueStride);
}

void JitContext::Jump(Label& label) {
    // PC-relative pair avoids JAL's +/-1 MiB limit and survives copying to RX.
    masm.LILabel(t5, &label);
    masm.JR(t5);
}

void JitContext::BranchZero(GPR value, Label& label, bool zero) {
    // Only this local skip uses the +/-4 KiB conditional branch encoding.
    Label skip;
    if (zero) masm.BNE(value, x0, &skip);
    else masm.BEQ(value, x0, &skip);
    Jump(label);
    masm.Bind(&skip);
}

void JitContext::Condition(GPR result, ir::Cond condition) {
    Load(t0, state, state_offset_host_flags);
    masm.SRLI(t1, t0, kNegateBit);
    masm.ANDI(t1, t1, 1);
    masm.SRLI(t2, t0, kZeroBit);
    masm.ANDI(t2, t2, 1);
    masm.SRLI(t3, t0, kCarryBit);
    masm.ANDI(t3, t3, 1);
    masm.SRLI(t4, t0, kOverflowBit);
    masm.ANDI(t4, t4, 1);
    switch (condition) {
        case ir::Cond::EQ: masm.MV(result, t2); break;
        case ir::Cond::NE: masm.XORI(result, t2, 1); break;
        case ir::Cond::CS: masm.MV(result, t3); break;
        case ir::Cond::CC: masm.XORI(result, t3, 1); break;
        case ir::Cond::MI: masm.MV(result, t1); break;
        case ir::Cond::PL: masm.XORI(result, t1, 1); break;
        case ir::Cond::VS: masm.MV(result, t4); break;
        case ir::Cond::VC: masm.XORI(result, t4, 1); break;
        case ir::Cond::HI:
            masm.XORI(t2, t2, 1); masm.AND(result, t3, t2); break;
        case ir::Cond::LS:
            masm.XORI(t3, t3, 1); masm.OR(result, t3, t2); break;
        case ir::Cond::GE:
            masm.XOR(result, t1, t4); masm.XORI(result, result, 1); break;
        case ir::Cond::LT: masm.XOR(result, t1, t4); break;
        case ir::Cond::GT:
            masm.XOR(result, t1, t4); masm.OR(result, result, t2);
            masm.XORI(result, result, 1); break;
        case ir::Cond::LE:
            masm.XOR(result, t1, t4); masm.OR(result, result, t2); break;
        case ir::Cond::AL:
        case ir::Cond::NV: masm.LI(result, 1); break;
    }
}

u32 JitContext::CurrentBufferSize() {
    return static_cast<u32>(masm.GetCodeBuffer().GetSizeInBytes());
}

void JitContext::Flush(const CodeBuffer& buffer) {
    ASSERT(buffer.size >= CurrentBufferSize());
    std::memcpy(buffer.rw_data, masm.GetCodeBuffer().GetOffsetPointer(0), CurrentBufferSize());
    buffer.Flush();
}

}  // namespace swift::runtime::backend::riscv64
