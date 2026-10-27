#pragma once

#include <stdexcept>
#include <type_traits>
#include "runtime/ir/instr.h"
#include "runtime/ir/terminal.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::riscv64 {

inline void VerifyRVCondition(ir::Cond condition) {
    if (!ir::IsValidCondition(condition))
        throw std::runtime_error("RV64 condition is outside EQ..NV");
}

// Only called for the four selected opcodes with their actual cached opcode.
inline void VerifyRVInstructionConditionForOpcode(const ir::Inst& inst, ir::OpCode op) {
    const u8 index = op == ir::OpCode::FCmpCondSet ? 1 : 0;
    if (inst.ArgAt(index).GetType() != ir::ArgType::Cond)
        throw std::runtime_error("RV64 condition operand is not a Cond");
    VerifyRVCondition(inst.GetArg<ir::Cond>(index));
}

inline void VerifyRVTerminalConditions(const ir::Terminal& terminal) {
    if (!ir::CheckTerminal(terminal, [](ir::Value) {}))
        throw std::runtime_error("RV64 terminal has invalid arguments");
}

} // namespace swift::runtime::backend::riscv64
