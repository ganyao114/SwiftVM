#pragma once

#include "runtime/common/types.h"

namespace swift::runtime::ir {

// AtomicRMW returns the value observed before the update.  Carry is an
// explicit IR operand for ADC/SBB: host NZCV is backend state, not a stable
// input to an exclusive retry loop.
enum class AtomicRMWOp : u8 {
    Add,
    Sub,
    And,
    Or,
    Xor,
    Neg,
    AddCarry,
    SubBorrow,
};

}  // namespace swift::runtime::ir
