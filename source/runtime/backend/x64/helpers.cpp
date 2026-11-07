#include "helpers.h"

#include <cstring>
#include <initializer_list>
#define LITTLEENDIAN 1
#define SOFTFLOAT_FAST_INT64 1
#define FEXCORE_PRESERVE_ALL_ATTR
extern "C" {
#include <SoftFloat-3e/softfloat.h>
}
#undef FEXCORE_PRESERVE_ALL_ATTR
#undef SOFTFLOAT_FAST_INT64
#undef LITTLEENDIAN

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

u64 RoundIntegral(u64 raw, unsigned bits, unsigned mode, unsigned mxcsr) noexcept {
    if ((mxcsr & 64) &&
        ((raw & (bits == 32 ? u64{0x7f800000} : UINT64_C(0x7ff0000000000000))) == 0))
        raw &= u64{1} << (bits - 1);
    const unsigned frac = bits == 32 ? 23 : 52, bias = bits == 32 ? 127 : 1023;
    const u64 sign = u64{1} << (bits - 1),
              em = bits == 32 ? 0x7f800000 : UINT64_C(0x7ff0000000000000),
              fm = (u64{1} << frac) - 1;
    if (bits == 32) raw &= UINT32_MAX;
    u64 mag = raw & ~sign;
    bool neg = raw & sign;
    if ((raw & em) == em) return raw | ((raw & fm) ? u64{1} << (frac - 1) : 0);
    int e = int((raw & em) >> frac) - bias;
    if (e >= int(frac) || !mag) return raw;
    if (e < 0) {
        const u64 half = u64(bias - 1) << frac;
        bool up = mode == 0 ? mag > half : mode == 1 ? neg : mode == 2 ? !neg : false;
        return (raw & sign) | (up ? u64(bias) << frac : 0);
    }
    const unsigned shift = frac - e;
    const u64 unit = u64{1} << shift, discard = raw & (unit - 1), trunc = raw & ~(unit - 1);
    bool up = mode == 0   ? discard > unit / 2 || (discard == unit / 2 && (trunc & unit))
              : mode == 1 ? neg && discard
              : mode == 2 ? !neg && discard
                          : false;
    return trunc + (up ? unit : 0);
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

u64 FusedMultiplyAdd(
        u64 a, u64 b, u64 c, unsigned bits, unsigned flags, unsigned control) noexcept {
    const u64 em = bits == 32 ? u64{0x7f800000} : UINT64_C(0x7ff0000000000000),
              fm = bits == 32 ? u64{0x7fffff} : UINT64_C(0x000fffffffffffff),
              quiet = u64{1} << (bits == 32 ? 22 : 51), sign = u64{1} << (bits - 1);
    const auto nan = [&](u64 v) { return (v & em) == em && (v & fm); };
    for (auto v : {a, b, c})
        if (nan(v)) return v | quiet;
    if (control & 64) {
        if (!(a & em)) a &= sign;
        if (!(b & em)) b &= sign;
        if (!(c & em)) c &= sign;
    }
    if (flags & 1) a ^= sign;
    if (flags & 2) c ^= sign;
    softfloat_state state{softfloat_tininess_afterRounding, softfloat_round_odd, 0, 80};
    const auto widen = [&](u64 v) {
        return bits == 32 ? f32_to_f128(&state, float32_t{std::uint32_t(v)})
                          : extF80_to_f128(&state, f64_to_extF80(&state, float64_t{v}));
    };
    auto product = f128_mul(&state, widen(a), widen(b));
    auto sum = f128_add(&state, product, widen(c));
    constexpr unsigned rounding[]{softfloat_round_near_even,
                                  softfloat_round_min,
                                  softfloat_round_max,
                                  softfloat_round_minMag};
    state.roundingMode = rounding[(control >> 13) & 3];
    u64 r = bits == 32 ? f128_to_f32(&state, sum).v : f128_to_f64(&state, sum).v;
    if (nan(r)) r = em | quiet | sign;
    if ((control & 0x8000) && !(r & em)) r &= sign;
    return r;
}
}  // namespace swift::runtime::backend::x64
