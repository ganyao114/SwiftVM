#include "call_helpers.h"

#include <cstring>

#include "runtime/arch/x86/x87.h"

namespace swift::runtime::backend::riscv64 {
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

CallPair WideDivide(u64 high, u64 low, u64 divisor, unsigned config, u64* valid) noexcept {
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

}  // namespace swift::runtime::backend::riscv64
