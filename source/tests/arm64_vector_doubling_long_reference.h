#pragma once
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

namespace vector_doubling_long_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
struct Result { u64 value; bool product_saturated, accumulator_saturated; };
inline u64 Mask(u32 bits) { return bits == 64 ? ~u64{0} : (u64{1} << bits) - 1; }
inline Big Signed(u64 value, u32 bits) {
    Big integer = value & Mask(bits);
    if (value & (u64{1} << (bits-1))) integer -= Big{1} << bits;
    return integer;
}
inline Result Evaluate(u64 lhs, u64 rhs, u64 accumulator, u32 bits, u32 operation) {
    const u32 width = 2 * bits;
    const Big limit = Big{1} << (width-1), minimum = -limit, maximum = limit-1;
    Big product = 2 * Signed(lhs,bits) * Signed(rhs,bits);
    const bool product_saturated = product < minimum || product > maximum;
    if (product < minimum) product = minimum;
    if (product > maximum) product = maximum;
    Big result = product;
    if (operation == 1) result = Signed(accumulator,width) + product;
    if (operation == 2) result = Signed(accumulator,width) - product;
    const bool accumulator_saturated = result < minimum || result > maximum;
    if (result < minimum) result = minimum;
    if (result > maximum) result = maximum;
    if (result < 0) result += Big{1} << width;
    return {result.convert_to<u64>(),product_saturated,accumulator_saturated};
}
}  // namespace vector_doubling_long_reference
