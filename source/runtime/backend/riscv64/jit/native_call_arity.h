#pragma once

#include <stdexcept>
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::riscv64 {

// The native host-call trampoline supports at most eight nonnull integer
// DataClass arguments. Numeric zero is an argument; only Void is padding.
// Reject before native emission instead of falling back to the interpreter,
// whose host-call ABI has the same eight-argument bound.
inline void VerifyNativeCallArityForOpcode(const ir::Inst& inst, ir::OpCode op) {
    if (op != ir::OpCode::CallLocation && op != ir::OpCode::CallDynamic) return;
    unsigned count{};
    for (const auto& param : inst.GetArg<ir::Params>(1))
        if (!param.data.Null() && ++count > 8)
            throw std::runtime_error("RV64 native call has more than eight nonnull arguments");
}

// Preserve the standalone entry point; internal preflight can reuse its
// already observed opcode without querying the instruction a second time.
inline void VerifyNativeCallArity(const ir::Inst& inst) {
    VerifyNativeCallArityForOpcode(inst, inst.GetOp());
}

}  // namespace swift::runtime::backend::riscv64
