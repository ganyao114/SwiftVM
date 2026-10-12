#pragma once

#include <type_traits>
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
    SignedMax,
    SignedMin,
    UnsignedMax,
    UnsignedMin,
};

// Compare two's-complement bit patterns without an implementation-defined
// unsigned-to-signed conversion. T is the actual access width, not u64.
template<class T>
constexpr T AtomicExtremum(AtomicRMWOp operation, T old, T operand) {
    static_assert(std::is_unsigned_v<T>);
    constexpr T sign = T(T(1) << (sizeof(T) * 8 - 1));
    const bool signed_compare = operation == AtomicRMWOp::SignedMax || operation == AtomicRMWOp::SignedMin;
    const T old_key = signed_compare ? T(old ^ sign) : old;
    const T operand_key = signed_compare ? T(operand ^ sign) : operand;
    const bool maximum = operation == AtomicRMWOp::SignedMax || operation == AtomicRMWOp::UnsignedMax;
    return (maximum ? old_key < operand_key : operand_key < old_key) ? operand : old;
}

}  // namespace swift::runtime::ir
