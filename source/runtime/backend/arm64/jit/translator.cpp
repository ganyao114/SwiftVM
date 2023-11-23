#pragma once

#include "translator.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

JitTranslator::JitTranslator(JitContext& ctx) : context(ctx), masm(ctx.GetMasm()) {}

void JitTranslator::Translate(ir::Inst* inst) {
    ASSERT(inst);
#define INST(name, ...) case ir::OpCode::name: \
    Emit##name(inst);                          \
    break;

    switch (inst->GetOp()) {
#include "runtime/ir/ir.inc"
        default:
            abort();
    }

#undef INST
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

}

void JitTranslator::EmitAndValue(ir::Inst* inst) {

}

#undef masm

}