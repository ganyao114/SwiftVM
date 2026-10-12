#pragma once
#include <array>
#include <boost/multiprecision/cpp_int.hpp>
#include "base/types.h"

namespace pairwise_widen_reference {
using namespace swift;
using Big = boost::multiprecision::cpp_int;
using Vector = std::array<u64,2>;
inline u64 Element(Vector vector,u32 bits,u32 lane) {
    return (vector[lane*bits/64]>>(lane*bits%64)) & (bits==64 ? ~u64{0} : (u64{1}<<bits)-1);
}
inline Vector Evaluate(Vector source,Vector accumulator,u32 bits,u32 operation,bool q) {
    const bool shift = operation==4,sign = !(operation&1);
    const u32 output_bits = bits*2,active = (shift || q ? 128 : 64)/output_bits;
    const Big modulus = Big{1}<<output_bits;
    Vector result{};
    for (u32 lane=0;lane<active;++lane) {
        Big value;
        if (shift) value = Big{Element(source,bits,lane+(q ? 64/bits : 0))}<<bits;
        else {
            auto unpack = [&](u32 index) {
                const u64 value = Element(source,bits,index);
                return sign && (value&(u64{1}<<(bits-1))) ? Big{value}-(Big{1}<<bits) : Big{value};
            };
            value = unpack(lane*2)+unpack(lane*2+1);
            if (operation>=2) value += Element(accumulator,output_bits,lane);
        }
        value %= modulus;if (value<0) value += modulus;
        result[lane*output_bits/64] |= value.convert_to<u64>()<<(lane*output_bits%64);
    }
    return result;
}
}  // namespace pairwise_widen_reference
