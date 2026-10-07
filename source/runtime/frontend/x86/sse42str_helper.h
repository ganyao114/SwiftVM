#pragma once

#include "runtime/common/sse42str_call_abi.h"

namespace swift::x86 {

VAddr Sse42StrVectorHelperAddress(u8 imm);
extern "C" u64 SwiftSse42StrEvalImplicit(u64 a_lo, u64 a_hi, u64 b_lo, u64 b_hi, u64 imm8);

}  // namespace swift::x86
