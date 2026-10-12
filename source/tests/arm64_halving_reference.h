#pragma once
#include <array>
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

// Direct mathematical sums/differences, independent of packed bit identities.
namespace halving_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
using Vector = std::array<u64,2>;
inline Vector Evaluate(Vector left,Vector right,u32 bits,u32 operation,bool q) {
    const u64 mask = (u64{1}<<bits)-1,sign = u64{1}<<(bits-1);
    Vector result{};
    for (u32 lane=0;lane<(q ? 128u : 64u)/bits;++lane) {
        const u64 raw_a = (left[lane*bits/64]>>(lane*bits%64))&mask;
        const u64 raw_b = (right[lane*bits/64]>>(lane*bits%64))&mask;
        Big a = raw_a,b = raw_b;
        if (!(operation&1)) { if (raw_a&sign) a -= Big{1}<<bits;if (raw_b&sign) b -= Big{1}<<bits; }
        Big sum = a;if (operation>=4) sum -= b;else sum += b;
        if (operation==2 || operation==3) ++sum;
        const bool negative_odd = sum<0 && bool(sum%2);
        sum /= 2;if (negative_odd) --sum;
        const u64 value = (sum&Big{mask}).convert_to<u64>();
        result[lane*bits/64] |= value<<(lane*bits%64);
    }
    return result;
}
}  // namespace halving_reference
