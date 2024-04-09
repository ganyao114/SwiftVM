//
// Created by 甘尧 on 2024/2/23.
//

#include "interpreter.h"

namespace swift::runtime::backend::interp {

Interpreter::Interpreter(State &state, ir::Block* block) : state(state), block(block) {}

HaltReason Interpreter::Run() {
    auto stack_size = block->GetVStackSize();
    auto stack_buffer = alloca(stack_size);
    InterpStack stack{(u64*) stack_buffer, stack_size / 8};
    HaltReason hr{HaltReason::None};
    for (auto &instr : block->GetInstList()) {
        if (hr = Run(&instr, stack); hr != HaltReason::None) {
            break;
        }
    }
    return hr;
}

HaltReason Interpreter::Run(ir::Inst* inst, InterpStack &stack) {
    switch (inst->GetOp()) {
#define INST(name, ...) case ir::OpCode::name: { \
        Run##name(inst, stack);    \
    }
#include "runtime/ir/ir.inc"
#undef INST
        default:
            break;
    }
    return state.halt_reason;
}

void Interpreter::RunAdc(ir::Inst* inst, InterpStack& stack) {
    auto left = GetReg<u64>(stack, inst->GetArg<ir::Value>(0));
}

void Interpreter::RunAdd(ir::Inst* inst, InterpStack& stack) {
    auto left = GetReg<u64>(stack, inst->GetArg<ir::Value>(0));
    for (auto pseudo : inst->GetPseudoOperations()) {

    }
}

}
