#pragma once
#include <array>
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

namespace high_narrow_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
using Vector = std::array<u64,2>;
inline Vector Evaluate(Vector left,Vector right,Vector destination,u32 bits,u32 operation,bool upper) {
    const u32 out_bits = bits/2;
    const Big modulus = Big{1} << bits;
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1}<<bits)-1;
    u64 packed = 0;
    for (u32 lane=0;lane<128/bits;++lane) {
        const Big a = (left[lane*bits/64]>>(lane*bits%64))&mask;
        const Big b = (right[lane*bits/64]>>(lane*bits%64))&mask;
        Big sum = a;
        if (operation&2) sum -= b;else sum += b;
        if (operation&1) sum += Big{1}<<(out_bits-1);
        sum %= modulus;if (sum<0) sum += modulus;
        const Big high = sum/(Big{1}<<out_bits);
        packed |= high.convert_to<u64>()<<(lane*out_bits);
    }
    return upper ? Vector{destination[0],packed} : Vector{packed,0};
}
}  // namespace high_narrow_reference
