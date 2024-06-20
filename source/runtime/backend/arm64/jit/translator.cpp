#pragma once

#include "runtime/backend/arm64/constant.h"
#include "runtime/backend/context.h"
#include "translator.h"
#include "runtime/backend/arm64/defines.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

JitTranslator::JitTranslator(JitContext& ctx) : context(ctx), masm(ctx.GetMasm()) {}

void JitTranslator::Translate() {

}

void JitTranslator::Translate(ir::Inst* inst) {
    ASSERT(inst);
#define INST(name, ...) case ir::OpCode::name: \
    Emit##name(inst);                          \
    break;

    switch (inst->GetOp()) {
#include "runtime/ir/ir.inc"
        default:
            ASSERT_MSG(false, "Instr unk op: {}", inst->GetOp());
    }

#undef INST
}

void JitTranslator::EmitLoadUniform(ir::Inst* inst) {
    auto uni = inst->GetArg<ir::Uniform>(0);
    auto offset = offsetof(State, uniform_buffer_begin) + uni.GetOffset();
    auto reg = context.Get(inst);
    if (std::holds_alternative<Register>(reg)) {
        auto gpr = std::get<Register>(reg);
        switch (GetValueSizeByte(inst->ReturnType())) {
            case 1:
                __ Ldrb(gpr, MemOperand(state, offset));
                break;
            case 2:
                __ Ldrh(gpr, MemOperand(state, offset));
                break;
            case 4:
                __ Ldr(gpr.W(), MemOperand(state, offset));
                break;
            case 8:
                __ Ldr(gpr, MemOperand(state, offset));
                break;
        }
    } else {
        auto fpr = std::get<VRegister>(reg);
        switch (GetValueSizeByte(inst->ReturnType())) {
            case 1:
                __ Ldr(fpr.B(), MemOperand(state, offset));
                break;
            case 2:
                __ Ldr(fpr.H(), MemOperand(state, offset));
                break;
            case 4:
                __ Ldr(fpr.S(), MemOperand(state, offset));
                break;
            case 8:
                __ Ldr(fpr.D(), MemOperand(state, offset));
                break;
            case 16:
                __ Ldr(fpr.Q(), MemOperand(state, offset));
                break;
        }
    }
}

void JitTranslator::EmitStoreUniform(ir::Inst* inst) {
    auto uni = inst->GetArg<ir::Uniform>(0);
    auto offset = offsetof(State, uniform_buffer_begin) + uni.GetOffset();
    auto value_reg = context.Get(inst->GetArg<ir::Value>(1));
    if (std::holds_alternative<Register>(value_reg)) {
        auto gpr = std::get<Register>(value_reg);
        switch (GetValueSizeByte(inst->ReturnType())) {
            case 1:
                __ Strb(gpr, MemOperand(state, offset));
                break;
            case 2:
                __ Strh(gpr, MemOperand(state, offset));
                break;
            case 4:
                __ Str(gpr.W(), MemOperand(state, offset));
                break;
            case 8:
                __ Str(gpr, MemOperand(state, offset));
                break;
        }
    } else {
        auto fpr = std::get<VRegister>(value_reg);
        switch (GetValueSizeByte(inst->ReturnType())) {
            case 1:
                __ Str(fpr.B(), MemOperand(state, offset));
                break;
            case 2:
                __ Str(fpr.H(), MemOperand(state, offset));
                break;
            case 4:
                __ Str(fpr.S(), MemOperand(state, offset));
                break;
            case 8:
                __ Str(fpr.D(), MemOperand(state, offset));
                break;
            case 16:
                __ Str(fpr.Q(), MemOperand(state, offset));
                break;
        }
    }
}

void JitTranslator::EmitLoadLocal(ir::Inst* inst) {
    // TODO
    ASSERT_MSG(false, "Unimplemented!");
}

void JitTranslator::EmitStoreLocal(ir::Inst* inst) {
    // TODO
    ASSERT_MSG(false, "Unimplemented!");
}

void JitTranslator::EmitLoadMemory(ir::Inst* inst) {
    auto uni = inst->GetArg<ir::Uniform>(0);
    auto offset = offsetof(State, uniform_buffer_begin) + uni.GetOffset();
    auto value_reg = context.Get(inst->GetArg<ir::Value>(1));


}

void JitTranslator::EmitStoreMemory(ir::Inst* inst) {

}

void JitTranslator::EmitLoadMemoryTSO(ir::Inst* inst) {

}

void JitTranslator::EmitStoreMemoryTSO(ir::Inst* inst) {

}

Operand JitTranslator::EmitOperand(ir::Operand& ir_op) {
    context.V(ir_op.GetLeft().value).B();
    return Operand();
}

MemOperand JitTranslator::EmitMemOperand(ir::Operand& ir_op) {
    return MemOperand();
}

void JitTranslator::EmitAdd(ir::Inst* inst) {
    auto left = inst->GetArg<ir::Value>(0);
    auto right = inst->GetArg<ir::Operand>(1);

}

void JitTranslator::EmitAdc(ir::Inst* inst) {

}

void JitTranslator::EmitAndImm(ir::Inst* inst) {
    auto imm = inst->GetArg<ir::Imm>(1);

}

void JitTranslator::EmitAndValue(ir::Inst* inst) {

}

#undef masm

}