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

// Token namespace: the spellings allowed in ir.inc, including optional forms.
namespace tokens {
    using enum ArgSignature;
    constexpr ArgSignature BOOL = ArgSignature::Value;
    constexpr ArgSignature U32 = ArgSignature::Value;
    constexpr ArgSignature U64 = ArgSignature::Value;
}
// Bring the tokens into unqualified lookup for the ir.inc expansion below.
// Scoped to meta only; it does NOT leak into files that include this header.
using namespace tokens;

// Fixed policies use the return token. Other result sources are declared
// separately in ir.inc; an absent hint is distinct from an explicit VOID.
namespace results {
    constexpr ValueType Void = ValueType::VOID;
    constexpr ValueType Value = ValueType::VOID;
    constexpr ValueType BOOL = ValueType::U8;
    constexpr ValueType U32 = ValueType::U32;
    constexpr ValueType U64 = ValueType::U64;
}

// An Inst stores at most this many physical Arg slots. Must equal Inst::max_args.
inline constexpr int kMaxPhysSlots = 5;

// Pack helper: logical arg list -> fixed array. An empty pack = a zero-arg inst.
template <ArgSignature... Args>
inline constexpr std::array<ArgSignature, sizeof...(Args)> ArgList = {Args...};

// Physical slot count for a logical arg list (Operand spans 3 slots, else 1).
template <std::size_t N>
constexpr int SlotCount(const std::array<ArgSignature, N>& args) {
    int count = 0;
    for (auto a : args) {
        count += PhysicalSlots(a);
    }
    return count;
}

constexpr bool ValidArgCount(std::span<const ArgSignature> args, std::size_t actual) {
    if (actual > args.size()) return false;
    for (auto i = actual; i < args.size(); ++i)
        if (!IsOptional(args[i])) return false;
    return true;
}

constexpr bool AcceptsArguments(std::span<const ArgSignature> signature,
                                std::span<const ArgType> actual) {
    if (!ValidArgCount(signature, actual.size())) return false;
    for (std::size_t i = 0; i < actual.size(); ++i)
        if (!AcceptsArgument(signature[i], actual[i])) return false;
    return true;
}

template <std::size_t N>
constexpr bool ValidSignature(const std::array<ArgSignature, N>& args) {
    bool optional = false;
    for (auto arg : args) {
        if (arg == ArgSignature::Void || (optional && !IsOptional(arg))) return false;
        optional |= IsOptional(arg);
    }
    return true;
}

// True if an instruction produces a Value (today only Void-ness/Value-ness is consumed).
constexpr bool HasValue(ArgType ret) { return ret == ArgType::Value; }

// Branch bindings may carry a type view for label identity, including the
// void-metadata BindLabel used by Function lowering. They never produce data.
constexpr bool IsControlToken(OpCode op) {
    return op == OpCode::Goto || op == OpCode::NotGoto || op == OpCode::BindLabel;
}

// Arg0 is deliberately limited to a direct value, immediate, uniform or local.
// Nested operands, call targets and parameter lists never choose result types.
constexpr bool CanSupplyResultType(ArgSignature arg) {
    return arg == ArgSignature::Value || arg == ArgSignature::Imm ||
           arg == ArgSignature::Uniform || arg == ArgSignature::Local;
}

template <ResultTypePolicy policy>
inline constexpr bool HasDefaultResultType =
        policy != ResultTypePolicy::Explicit && policy != ResultTypePolicy::Call;

// Project ir.inc into per-instruction constexpr signature and result facts.
template <OpCode> struct InstructionSignature;

#define INST(name, ret, policy, ...)                                                                \
    inline constexpr ArgType kRet_##name = StorageType(ret);                                        \
    inline constexpr ValueType kResult_##name = results::ret;                                       \
    inline constexpr auto kResultPolicy_##name = ResultTypePolicy::policy;                          \
    inline constexpr auto kArgs_##name = ArgList<__VA_ARGS__>;                                      \
    template <> struct InstructionSignature<OpCode::name> {                                        \
        static constexpr const auto& arguments = kArgs_##name;                                    \
    };                                                                                            \
    inline constexpr int kPhys_##name = SlotCount(kArgs_##name);                                    \
    static_assert(kPhys_##name <= kMaxPhysSlots,                                                    \
                  #name " needs more than Inst::max_args physical slots");                          \
    static_assert(ValidSignature(kArgs_##name), #name " has an invalid optional argument suffix");  \
    static_assert((kRet_##name == ArgType::Void) ==                                                 \
                  (kResultPolicy_##name == ResultTypePolicy::Void));                                \
    static_assert((kResult_##name != ValueType::VOID) ==                                            \
                  (kResultPolicy_##name == ResultTypePolicy::Fixed));                               \
    static_assert(kResultPolicy_##name != ResultTypePolicy::Arg0 ||                                 \
                  (!kArgs_##name.empty() && CanSupplyResultType(kArgs_##name[0])));
#include "ir.inc"
#undef INST

}  // namespace swift::runtime::ir::meta
