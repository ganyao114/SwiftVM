#pragma once
#include <array>
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

namespace absolute_narrow_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
using Vector = std::array<u64,2>;
inline u64 Element(Vector vector,u32 bits,u32 lane) {
    return (vector[lane*bits/64]>>(lane*bits%64)) & (bits==64 ? ~u64{0} : (u64{1}<<bits)-1);
}
inline Vector Evaluate(Vector left,Vector right,Vector accumulator,u32 bits,u32 operation,bool q) {
    const bool sign = !(operation&1),accumulate = operation>=2;
    const Big modulus = Big{1}<<bits;Vector result{};
    auto unpack = [&](Vector vector,u32 lane) {
        const u64 value = Element(vector,bits,lane);
        return sign && (value&(u64{1}<<(bits-1))) ? Big{value}-(Big{1}<<bits) : Big{value};
    };
    for (u32 lane=0;lane<(q ? 128 : 64)/bits;++lane) {
        Big difference = unpack(left,lane)-unpack(right,lane);
        if (difference<0) difference = -difference;
        if (accumulate) difference += Element(accumulator,bits,lane);
        difference %= modulus;
        result[lane*bits/64] |= difference.convert_to<u64>()<<(lane*bits%64);
    }
    return result;
}
}  // namespace absolute_narrow_reference
