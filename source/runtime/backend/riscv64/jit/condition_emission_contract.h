#pragma once

#include <stdexcept>
#include <type_traits>
#include "runtime/ir/instr.h"
#include "runtime/ir/terminal.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::riscv64 {

inline void VerifyRVCondition(ir::Cond condition) {
    if (static_cast<u8>(condition) > static_cast<u8>(ir::Cond::NV))
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
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            VerifyRVCondition(term.cond);
            VerifyRVTerminalConditions(term.then_);
            VerifyRVTerminalConditions(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
            VerifyRVTerminalConditions(term.then_);
            VerifyRVTerminalConditions(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            for (const auto& item : term.cases) VerifyRVTerminalConditions(item.then);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            VerifyRVTerminalConditions(term.else_);
        }
    });
}

} // namespace swift::runtime::backend::riscv64
