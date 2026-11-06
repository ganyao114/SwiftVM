#include "sse42_string.h"

#include <array>
#include <bit>

namespace swift::runtime::x86::sse42 {
namespace {
using Elements = std::array<std::int32_t, 16>;
Elements Unpack(Block128 value, bool words, bool sign) {
    Elements result{};
    const unsigned bits = words ? 16 : 8, count = 128 / bits;
    const auto mask = (1u << bits) - 1;
    for (unsigned i = 0; i < count; ++i) {
        const auto raw = static_cast<std::uint32_t>((i * bits < 64 ? value.low : value.high) >> (i * bits % 64)) & mask;
        const auto bias = sign ? 1u << (bits - 1) : 0;
        result[i] = static_cast<std::int32_t>(raw ^ bias) - static_cast<std::int32_t>(bias);
    }
    return result;
}
unsigned Length(const Elements& input, unsigned count) {
    unsigned length{};
    while (length < count && input[length]) ++length;
    return length;
}
unsigned ExplicitLength(std::uint64_t input, bool wide, unsigned count) {
    std::uint64_t magnitude;
    if (wide) magnitude = input >> 63 ? std::uint64_t{} - input : input;
    else {
        const auto word = static_cast<std::uint32_t>(input);
        magnitude = word >> 31 ? std::uint32_t{} - word : word;
    }
    return magnitude < count ? static_cast<unsigned>(magnitude) : count;
}
std::uint64_t Compare(const Elements& a, const Elements& b, unsigned length_a,
                      unsigned length_b, unsigned count, std::uint8_t control) {
    const unsigned aggregation = (control >> 2) & 3;
    const auto compare = [&](unsigned i, unsigned j) {
        const bool valid_a = i < length_a, valid_b = j < length_b;
        if (!valid_a && !valid_b) return aggregation >= 2;
        if (!valid_a) return aggregation == 3;
        if (!valid_b) return false;
        if (aggregation == 1) return (i & 1) ? b[j] <= a[i] : b[j] >= a[i];
        return a[i] == b[j];
    };
    std::uint32_t result{};
    for (unsigned j = 0; j < count; ++j) {
        bool match = aggregation == 3;
        if (aggregation == 0) {
            for (unsigned i = 0; i < count && !match; ++i) match = compare(i, j);
        } else if (aggregation == 1) {
            for (unsigned i = 0; i + 1 < count && !match; i += 2) match = compare(i, j) && compare(i + 1, j);
        } else if (aggregation == 2) match = compare(j, j);
        else for (unsigned i = 0; i + j < count && match; ++i) match = compare(i, i + j);
        if (match) result |= 1u << j;
    }
    const unsigned polarity = (control >> 4) & 3;
    if (polarity == 1) result = ~result;
    else if (polarity == 3) result ^= (1u << length_b) - 1;
    result &= (1u << count) - 1;
    const unsigned index = !result ? count : (control & 64) ? 31u - std::countl_zero(result) : std::countr_zero(result);
    return result | (std::uint64_t{index} << IndexShift) |
            (std::uint64_t{length_a < count} << SignBit) | (std::uint64_t{length_b < count} << ZeroBit) |
            (std::uint64_t{result != 0} << CarryBit) | (std::uint64_t{result & 1} << OverflowBit);
}
} // namespace

std::uint64_t CompareImplicit(Block128 left, Block128 right, std::uint8_t control) noexcept {
    const unsigned count = control & 1 ? 8 : 16;
    const auto a = Unpack(left, control & 1, control & 2), b = Unpack(right, control & 1, control & 2);
    return Compare(a, b, Length(a, count), Length(b, count), count, control);
}
std::uint64_t CompareExplicit(Block128 left, Block128 right, Block128 lengths, std::uint16_t control) noexcept {
    const unsigned count = control & 1 ? 8 : 16;
    const auto a = Unpack(left, control & 1, control & 2), b = Unpack(right, control & 1, control & 2);
    return Compare(a, b, ExplicitLength(lengths.low, control & 256, count),
            ExplicitLength(lengths.high, control & 256, count), count, static_cast<std::uint8_t>(control));
}
} // namespace swift::runtime::x86::sse42
