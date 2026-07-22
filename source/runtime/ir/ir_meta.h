//
// Compile-time projection of ir.inc: the single source of truth for IR
// instruction signatures. Every consumer derives from the constexpr facts
// generated here; inconsistencies surface as build errors, not runtime bugs.
//

#pragma once

#include <array>
#include <cstddef>
#include "runtime/ir/ir_types.h"

namespace swift::runtime::ir::meta {

// Token namespace: the spellings allowed in ir.inc, each resolving to an ArgType.
namespace tokens {
    using enum ArgType;                       // arg tokens map 1:1 to ArgType enumerators
    constexpr ArgType BOOL = ArgType::Value;  // return-type synonyms used in ir.inc
    constexpr ArgType U32 = ArgType::Value;
    constexpr ArgType U64 = ArgType::Value;
}
// Bring the tokens into unqualified lookup for the ir.inc expansion below.
// Scoped to meta only; it does NOT leak into files that include this header.
using namespace tokens;

// An Inst stores at most this many physical Arg slots. Must equal Inst::max_args.
inline constexpr int kMaxPhysSlots = 5;

// Pack helper: logical arg list -> fixed array. An empty pack = a zero-arg inst.
template <ArgType... Args>
inline constexpr std::array<ArgType, sizeof...(Args)> ArgList = {Args...};

// Physical slot count for a logical arg list (Operand spans 3 slots, else 1).
template <std::size_t N>
constexpr int SlotCount(const std::array<ArgType, N>& args) {
    int count = 0;
    for (auto a : args) {
        count += PhysicalSlots(a);
    }
    return count;
}

// True if an instruction produces a Value (today only Void-ness/Value-ness is consumed).
constexpr bool HasValue(ArgType ret) { return ret == ArgType::Value; }

// Project ir.inc into per-instruction constexpr signature facts.
#define INST(name, ret, ...)                                                       \
    inline constexpr ArgType kRet_##name = ret;                                    \
    inline constexpr auto kArgs_##name = ArgList<__VA_ARGS__>;                     \
    inline constexpr int kPhys_##name = SlotCount(kArgs_##name);                   \
    static_assert(kPhys_##name <= kMaxPhysSlots,                                   \
                  #name " needs more than Inst::max_args physical slots");
#include "ir.inc"
#undef INST

}  // namespace swift::runtime::ir::meta
