#pragma once
#include "arm64_fmulx_reference.h"
#include "arm64_vector_fma_reference.h"

namespace indexed_fp_reference {
using namespace swift;
using Result = fmulx_reference::Result;
// These integer/rational oracles are independent of production FP helpers.
// Operation order is FMUL, FMULX, FMLA, FMLS.
inline Result Evaluate(u64 a, u64 b, u64 c, u32 bits, u32 operation, u32 fpcr) {
    if (operation >= 2) {
        const auto result = vector_fma_reference::Evaluate(a,b,c,bits,operation == 3 ? 1 : 0,fpcr);
        return {result.value,result.exceptions};
    }
    if (operation == 1) return fmulx_reference::Evaluate(a,b,bits,fpcr);
    const fmulx_reference::Format f(bits);
    u32 flags = 0;
    if (fpcr & (1u << (bits == 16 ? 19 : 24))) for (u64* source : {&a,&b}) {
        if (!(*source & f.exponent_mask) && (*source & f.fraction_mask)) {
            *source &= f.sign;
            if (bits != 16) flags |= 128;
        }
    }
    if (!f.NaN(a) && !f.NaN(b) &&
        (((a & ~f.sign) == f.exponent_mask && !(b & ~f.sign)) ||
         ((b & ~f.sign) == f.exponent_mask && !(a & ~f.sign))))
        return {f.exponent_mask | f.quiet,flags | 1};
    auto result = fmulx_reference::Evaluate(a,b,bits,fpcr);
    result.exceptions |= flags;
    return result;
}
}  // namespace indexed_fp_reference
