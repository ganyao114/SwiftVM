#pragma once

#include "translator.h"
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/constant.h"

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
    auto uni_type = uni.GetType();
    auto offset = offsetof(State, uniform_buffer_begin) + uni.GetOffset();
    auto reg = context.Get(inst);
}

void JitTranslator::EmitStoreUniform(ir::Inst* inst) {

}

void JitTranslator::EmitLoadLocal(ir::Inst* inst) {

}

void JitTranslator::EmitStoreLocal(ir::Inst* inst) {

}

void JitTranslator::EmitLoadMemory(ir::Inst* inst) {

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
    auto operand = inst->GetArg<ir::Operand>(1);

    auto operand_left = operand.GetLeft();
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