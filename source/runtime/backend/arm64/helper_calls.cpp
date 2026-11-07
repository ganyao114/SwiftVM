#include <cstdint>
#include <cstdlib>

extern "C" std::uint64_t SvmX87Execute(void*, std::uint64_t, std::uint64_t) noexcept;

namespace {
using u64 = std::uint64_t;
using u128 = unsigned __int128;

struct DivisionResult {
    u64 quotient, remainder;
};

}  // namespace

// The private entry stubs below adapt the writer's fixed homes to the normal
// host ABI once, rather than emitting a register-save sequence at each call.
extern "C" __attribute__((used, noinline, visibility("hidden"))) DivisionResult
SvmArm64WideDivide(u64 high, u64 low, u64 divisor, unsigned config, u64* valid) noexcept {
    *valid = 0;
    const auto bits = config & 255u;
    const bool sign = config & 256u;
    const u64 mask = bits == 64 ? ~u64{} : (u64{1} << bits) - 1;
    const u128 wide_mask = bits == 64 ? ~u128{} : (u128{1} << (bits * 2)) - 1;
    high &= mask;
    low &= mask;
    divisor &= mask;
    if (!divisor) return {};
    const u128 dividend = (u128{high} << bits) | low;
    const bool negative_dividend = sign && (high >> (bits - 1));
    const bool negative_divisor = sign && (divisor >> (bits - 1));
    const u128 magnitude = negative_dividend ? (-dividend & wide_mask) : dividend;
    const u64 denominator = negative_divisor ? (-divisor & mask) : divisor;
    const auto quotient = magnitude / denominator;
    const bool negative_quotient = negative_dividend != negative_divisor;
    const u64 limit = sign ? (u64{1} << (bits - 1)) - !negative_quotient : mask;
    if (quotient > limit) return {};
    const u64 remainder = static_cast<u64>(magnitude % denominator);
    *valid = 1;
    return {(negative_quotient ? -static_cast<u64>(quotient) : static_cast<u64>(quotient)) & mask,
            (negative_dividend ? -remainder : remainder) & mask};
}

#if defined(__aarch64__)
extern "C" {
void SvmArm64Div8U() noexcept;
void SvmArm64Div16U() noexcept;
void SvmArm64Div32U() noexcept;
void SvmArm64Div64U() noexcept;
void SvmArm64Div8S() noexcept;
void SvmArm64Div16S() noexcept;
void SvmArm64Div32S() noexcept;
void SvmArm64Div64S() noexcept;
void SvmArm64QueryCpuid() noexcept;
void SvmArm64ExecuteX87() noexcept;
}
#else
static void SvmArm64QueryCpuid() { std::abort(); }

static void SvmArm64ExecuteX87() { std::abort(); }
#endif

namespace swift::runtime::backend::arm64 {
std::uintptr_t DivisionHelper(unsigned bits, bool sign) noexcept {
#if defined(__aarch64__)
    void (*entry)() = bits == 8    ? (sign ? SvmArm64Div8S : SvmArm64Div8U)
                      : bits == 16 ? (sign ? SvmArm64Div16S : SvmArm64Div16U)
                      : bits == 32 ? (sign ? SvmArm64Div32S : SvmArm64Div32U)
                                   : (sign ? SvmArm64Div64S : SvmArm64Div64U);
    return reinterpret_cast<std::uintptr_t>(entry);
#else
    // Cross-host word-generation tests never execute ARM64 words.
    (void)bits;
    (void)sign;
    return reinterpret_cast<std::uintptr_t>(&SvmArm64WideDivide);
#endif
}

std::uintptr_t CpuidHelper() noexcept {
    return reinterpret_cast<std::uintptr_t>(&SvmArm64QueryCpuid);
}

std::uintptr_t X87Helper() noexcept {
    return reinterpret_cast<std::uintptr_t>(&SvmArm64ExecuteX87);
}
}  // namespace swift::runtime::backend::arm64
