#pragma once

#include "runtime/ir/opcodes.h"

namespace swift::runtime::ir {

// These consumers read a low-W view without overwriting its source register.
// Keep allocation and emission checks on the same opcode contract.
constexpr bool IsReadOnlyLow32Consumer(OpCode op) {
    return op == OpCode::StoreMemory || op == OpCode::StoreMemoryTSO ||
           op == OpCode::StoreUniform || op == OpCode::SetHostGPR;
}

}  // namespace swift::runtime::ir
