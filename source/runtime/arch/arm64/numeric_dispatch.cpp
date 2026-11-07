#include <array>
#include "numeric.h"

namespace swift::runtime::arm64 {
u64 NumericUnary16(u64, u64, u64);
u64 NumericBinary16(u64, u64, u64);
u64 NumericVectorBinary(u64, u64, u64);
u64 NumericVectorUnary(u64, u64, u64);
u64 NumericCompare00(u64, u64, u64);
u64 NumericCompare01(u64, u64, u64);
u64 NumericCompare10(u64, u64, u64);
u64 NumericCompare11(u64, u64, u64);
u64 NumericScalar16(u64, u64, u64);
u64 NumericPairwise(u64, u64, u64);
u64 NumericInteger(u64, u64, u64);
u64 NumericInteger16(u64, u64, u64);
u64 NumericFixed(u64, u64, u64);
u64 NumericRangeRound(u64, u64, u64);
u64 NumericPrecision(u64, u64, u64);
u64 NumericEstimate(u64, u64, u64);
u64 NumericEstimate16(u64, u64, u64);
u64 NumericReduction00(u64, u64, u64);
u64 NumericReduction01(u64, u64, u64);
u64 NumericReduction10(u64, u64, u64);
u64 NumericReduction11(u64, u64, u64);
u64 NumericFused(u64, u64, u64);
u64 NumericExtended(u64, u64, u64);
u64 NumericIndexed(u64, u64, u64);
u64 NumericDot(u64, u64, u64);
u64 NumericSatMultiply(u64, u64, u64);
u64 NumericSatShift(u64, u64, u64);
u64 NumericUnsignedEstimate(u64, u64, u64);
u64 NumericCrc32(u64, u64, u64);
bool SupportsUnary16(u32);
bool SupportsBinary16(u32);
bool SupportsVectorBinary(u32);
bool SupportsVectorUnary(u32);
bool SupportsCompare00(u32);
bool SupportsCompare01(u32);
bool SupportsCompare10(u32);
bool SupportsCompare11(u32);
bool SupportsScalar16(u32);
bool SupportsPairwise(u32);
bool SupportsInteger(u32);
bool SupportsInteger16(u32);
bool SupportsFixed(u32);
bool SupportsRangeRound(u32);
bool SupportsPrecision(u32);
bool SupportsEstimate(u32);
bool SupportsEstimate16(u32);
bool SupportsReduction00(u32);
bool SupportsReduction01(u32);
bool SupportsReduction10(u32);
bool SupportsReduction11(u32);
bool SupportsFused(u32);
bool SupportsExtended(u32);
bool SupportsIndexed(u32);

bool SupportsNumeric(Numeric family, u32 word) noexcept {
    switch (family) {
        case Numeric::Scalar:
            return IsSupportedScalarFP(word);
        case Numeric::Unary16:
            return SupportsUnary16(word);
        case Numeric::Binary16:
            return SupportsBinary16(word);
        case Numeric::VectorBinary:
            return SupportsVectorBinary(word);
        case Numeric::VectorUnary:
            return SupportsVectorUnary(word);
        case Numeric::Compare00:
            return SupportsCompare00(word);
        case Numeric::Compare01:
            return SupportsCompare01(word);
        case Numeric::Compare10:
            return SupportsCompare10(word);
        case Numeric::Compare11:
            return SupportsCompare11(word);
        case Numeric::Scalar16:
            return SupportsScalar16(word);
        case Numeric::Pairwise:
            return SupportsPairwise(word);
        case Numeric::Integer:
            return SupportsInteger(word);
        case Numeric::Integer16:
            return SupportsInteger16(word);
        case Numeric::Fixed:
            return SupportsFixed(word);
        case Numeric::RangeRound:
            return SupportsRangeRound(word);
        case Numeric::Precision:
            return SupportsPrecision(word);
        case Numeric::Estimate:
            return SupportsEstimate(word);
        case Numeric::Estimate16:
            return SupportsEstimate16(word);
        case Numeric::Reduction00:
            return SupportsReduction00(word);
        case Numeric::Reduction01:
            return SupportsReduction01(word);
        case Numeric::Reduction10:
            return SupportsReduction10(word);
        case Numeric::Reduction11:
            return SupportsReduction11(word);
        case Numeric::Fused:
            return SupportsFused(word);
        case Numeric::Extended:
            return SupportsExtended(word);
        case Numeric::Indexed:
            return SupportsIndexed(word);
        default:
            return false;
    }
}

u64 ExecuteNumeric(u64 state, u64 encoded, u64 pc) {
    using Callback = u64 (*)(u64, u64, u64);
    static constexpr std::array<Callback, static_cast<unsigned>(Numeric::Count)> callbacks{
            ExecuteScalarFP,         NumericUnary16,     NumericBinary16,    NumericVectorBinary,
            NumericVectorUnary,      NumericCompare00,   NumericCompare01,   NumericCompare10,
            NumericCompare11,        NumericScalar16,    NumericPairwise,    NumericInteger,
            NumericInteger16,        NumericFixed,       NumericRangeRound,  NumericPrecision,
            NumericEstimate,         NumericEstimate16,  NumericReduction00, NumericReduction01,
            NumericReduction10,      NumericReduction11, NumericFused,       NumericExtended,
            NumericIndexed,          NumericDot,         NumericSatMultiply, NumericSatShift,
            NumericUnsignedEstimate, NumericCrc32};
    auto& context = *reinterpret_cast<GuestState*>(state);
    context.helper_status = 0;
    const auto family = (encoded >> 32) & 255;
    if (family >= callbacks.size()) {
        context.helper_status = 1;
        return 0;
    }
    return callbacks[family](
            state, (encoded & UINT64_C(0xffffff0000000000)) | std::uint32_t(encoded), pc);
}
}  // namespace swift::runtime::arm64
