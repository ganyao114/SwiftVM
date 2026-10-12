#pragma once
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

// Mathematical integer model: neither runtime IR nor host masked shifts.
namespace scalar_variable_shift_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
struct Result { u64 value; bool saturated; };
// Operation order: SSHL, USHL, SRSHL, URSHL, SQSHL, UQSHL, SQRSHL, UQRSHL.
inline Result Evaluate(u64 value, u64 raw_count, u32 bits, u32 operation) {
    const Big modulus = Big{1} << bits;
    const Big mask = modulus - 1;
    Big integer = Big{value} & mask;
    const bool is_unsigned = operation & 1;
    if (!is_unsigned && bool(integer & (Big{1} << (bits - 1)))) integer -= modulus;
    const s32 count = s32(raw_count & 255) - ((raw_count & 128) ? 256 : 0);
    bool saturated = false;
    if (count >= 0) {
        integer *= Big{1} << count;
        if (operation >= 4) {
            const Big minimum = is_unsigned ? Big{0} : -(Big{1} << (bits - 1));
            const Big maximum = is_unsigned ? mask : (Big{1} << (bits - 1)) - 1;
            if (integer < minimum) { integer = minimum; saturated = true; }
            if (integer > maximum) { integer = maximum; saturated = true; }
        }
    } else {
        const Big denominator = Big{1} << -count;
        if (operation & 2) integer += denominator / 2;
        // cpp_int division truncates; mathematical right shift requires floor.
        const Big remainder = integer % denominator;
        integer /= denominator;
        if (remainder < 0) --integer;
    }
    return {(integer & mask).convert_to<u64>(), saturated};
}
}  // namespace scalar_variable_shift_reference
