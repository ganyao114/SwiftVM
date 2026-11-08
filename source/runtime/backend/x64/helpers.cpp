#include "helpers.h"

#include <cstring>
#include "runtime/arch/x86/floating.h"

#include "runtime/arch/x86/x87.h"

namespace swift::runtime::backend::x64 {
namespace {
using u64 = std::uint64_t;
using u128 = unsigned __int128;
namespace x87 = ::swift::runtime::x86;

void CommitMetadata(void* state, u64 pc, u64 dp, u64 metadata) noexcept {
    auto* bytes = static_cast<std::uint8_t*>(state);
    const auto opcode = x87::X87MetadataOpcode(metadata);
    std::memcpy(bytes + x87::X87MetadataOffset(metadata, 0), &pc, sizeof(pc));
    if (x87::X87MetadataHasDP(metadata))
        std::memcpy(bytes + x87::X87MetadataOffset(metadata, 1), &dp, sizeof(dp));
    std::memcpy(bytes + x87::X87MetadataOffset(metadata, 2), &opcode, sizeof(opcode));
}
}  // namespace

Pair WideDivide(u64 high, u64 low, u64 divisor, unsigned config, u64* valid) noexcept {
    const unsigned bits = config & 255;
    const bool sign = config & 256;
    const auto mask = bits == 64 ? ~u64{} : (u64{1} << bits) - 1;
    const auto wide_mask = bits == 64 ? ~u128{} : (u128{1} << (2 * bits)) - 1;
    high &= mask;
    low &= mask;
    divisor &= mask;
    *valid = 0;
    if (!divisor) return {};
    const auto dividend = (u128{high} << bits) | low;
    const bool negative_dividend = sign && (high >> (bits - 1));
    const bool negative_divisor = sign && (divisor >> (bits - 1));
    const auto magnitude = negative_dividend ? (-dividend & wide_mask) : dividend;
    const auto denominator = negative_divisor ? (-divisor & mask) : divisor;
    const auto quotient = magnitude / denominator;
    const bool negative_quotient = negative_dividend != negative_divisor;
    const auto limit = sign ? (u64{1} << (bits - 1)) - !negative_quotient : mask;
    if (quotient > limit) return {};
    const auto remainder = static_cast<u64>(magnitude % denominator);
    *valid = 1;
    return {(negative_quotient ? -static_cast<u64>(quotient) : static_cast<u64>(quotient)) & mask,
            (negative_dividend ? -remainder : remainder) & mask};
}

u64 CheckedX87(void* state, u64 command, u64 raw, u64 pc, u64 dp, u64 metadata) noexcept {
    const auto status = static_cast<x87::X87Result>(SvmX87Execute(state, command, raw));
    if (status == x87::X87Result::RaisedException ||
        (status == x87::X87Result::Ok && x87::X87MetadataCommitsSuccess(metadata)))
        CommitMetadata(state, pc, dp, metadata);
    return static_cast<u64>(status);
}

Pair RoundIntegralStatus(u64 raw, unsigned bits, unsigned mode, unsigned mxcsr) noexcept {
    x87::FloatingCall call{};
    call.operation = static_cast<unsigned>(x87::FloatingOperation::RoundIntegral);
    call.lane_bits = bits;
    call.b[0] = raw;
    call.flags = static_cast<unsigned>(x87::FloatingFlag::Scalar) |
                 (mode & 8 ? static_cast<unsigned>(x87::FloatingFlag::SuppressPrecision) : 0);
    call.immediate = mode & 3;
    call.control = mxcsr;
    x87::ExecuteFloating(&call);
    return {call.result[0], call.exceptions};
}

u64 RoundIntegral(u64 raw, unsigned bits, unsigned mode, unsigned mxcsr) noexcept {
    return RoundIntegralStatus(raw, bits, mode, mxcsr).first;
}

Pair CompareExchange128(void* address, u64 elo, u64 ehi, u64 dlo, u64 dhi) noexcept {
    using W = unsigned __int128;
    auto* p = static_cast<W*>(address);
    W old = __atomic_load_n(p, __ATOMIC_SEQ_CST), expected = (W(ehi) << 64) | elo,
      desired = (W(dhi) << 64) | dlo;
    for (;;) {
        const W before = old;
        W value = old == expected ? desired : old;
        if (__atomic_compare_exchange_n(p, &old, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return {u64(before), u64(before >> 64)};
    }
}

Pair FusedMultiplyAddStatus(
        u64 a, u64 b, u64 c, unsigned bits, unsigned flags, unsigned control) noexcept {
    x87::FloatingCall call{};
    call.operation = static_cast<unsigned>(x87::FloatingOperation::FusedMultiplyAdd);
    call.lane_bits = bits;
    call.a[0] = a;
    call.b[0] = b;
    call.c[0] = c;
    call.flags = static_cast<unsigned>(x87::FloatingFlag::Scalar);
    call.immediate = flags;
    call.control = control;
    x87::ExecuteFloating(&call);
    return {call.result[0], call.exceptions};
}

u64 FusedMultiplyAdd(
        u64 a, u64 b, u64 c, unsigned bits, unsigned flags, unsigned control) noexcept {
    return FusedMultiplyAddStatus(a, b, c, bits, flags, control).first;
}

}  // namespace swift::runtime::backend::x64
