#pragma once
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

namespace mixed_sat_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
struct Result { u64 value;bool saturated; };
inline Result Evaluate(u64 source,u64 accumulator,u32 bits,u32 operation) {
    const u64 sign = u64{1}<<(bits-1),mask = bits==64 ? ~u64{0} : (u64{1}<<bits)-1;
    auto signed_value = [&](u64 value) { return value&sign ? Big{value}-(Big{1}<<bits) : Big{value}; };
    const Big signed_source = signed_value(source);
    Big value;
    if (operation==0) value = signed_value(accumulator)+Big{source};
    else if (operation==1) value = Big{accumulator}+signed_source;
    else if (operation==2) value = signed_source<0 ? Big(-signed_source) : signed_source;
    else value = -signed_source;
    const Big low = operation==1 ? Big{0} : -(Big{1}<<(bits-1));
    const Big high = operation==1 ? (Big{1}<<bits)-1 : (Big{1}<<(bits-1))-1;
    const bool saturated = value<low || value>high;
    if (value<low) value=low;if (value>high) value=high;
    return {(value & Big{mask}).convert_to<u64>(),saturated};
}
}  // namespace mixed_sat_reference
