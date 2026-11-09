#pragma once

#include "runtime/arch/arm32/location.h"
#include "runtime/arch/arm32/profile.h"
#include "runtime/ir/types.h"

#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace swift::arm32 {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using u128 = unsigned __int128;
using size_t = std::size_t;
using Cond = runtime::ir::Condition;
using Location = runtime::arm32::Location;
using LocationDescriptor = Location;
using ITState = runtime::arm32::ITState;
using ArchVersion = runtime::arm32::ArchVersion;
using Profile = runtime::arm32::Profile;

enum class Reg : u8 {
    R0,
    R1,
    R2,
    R3,
    R4,
    R5,
    R6,
    R7,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
    SP = R13,
    LR = R14,
    PC = R15,
    INVALID_REG = 99
};
enum class ExtReg : u8 {
    S0,
    S1,
    S2,
    S3,
    S4,
    S5,
    S6,
    S7,
    S8,
    S9,
    S10,
    S11,
    S12,
    S13,
    S14,
    S15,
    S16,
    S17,
    S18,
    S19,
    S20,
    S21,
    S22,
    S23,
    S24,
    S25,
    S26,
    S27,
    S28,
    S29,
    S30,
    S31,
    D0,
    D1,
    D2,
    D3,
    D4,
    D5,
    D6,
    D7,
    D8,
    D9,
    D10,
    D11,
    D12,
    D13,
    D14,
    D15,
    D16,
    D17,
    D18,
    D19,
    D20,
    D21,
    D22,
    D23,
    D24,
    D25,
    D26,
    D27,
    D28,
    D29,
    D30,
    D31,
    Q0,
    Q1,
    Q2,
    Q3,
    Q4,
    Q5,
    Q6,
    Q7,
    Q8,
    Q9,
    Q10,
    Q11,
    Q12,
    Q13,
    Q14,
    Q15
};
enum class CoprocReg : u8 { C0, C1, C2, C3, C4, C5, C6, C7, C8, C9, C10, C11, C12, C13, C14, C15 };
enum class ShiftType : u8 { LSL, LSR, ASR, ROR };
enum class SignExtendRotation : u8 { ROR_0, ROR_8, ROR_16, ROR_24 };
using RegList = u16;

constexpr bool IsSingleExtReg(ExtReg reg) { return reg <= ExtReg::S31; }

constexpr bool IsDoubleExtReg(ExtReg reg) { return reg >= ExtReg::D0 && reg <= ExtReg::D31; }

constexpr bool IsQuadExtReg(ExtReg reg) { return reg >= ExtReg::Q0 && reg <= ExtReg::Q15; }

constexpr size_t RegNumber(Reg reg) { return static_cast<size_t>(reg); }

constexpr size_t RegNumber(ExtReg reg) {
    return static_cast<size_t>(reg) - (IsSingleExtReg(reg) ? 0 : IsDoubleExtReg(reg) ? 32 : 64);
}

constexpr Reg operator+(Reg reg, size_t count) { return static_cast<Reg>(RegNumber(reg) + count); }

constexpr ExtReg operator+(ExtReg reg, size_t count) {
    return static_cast<ExtReg>(static_cast<size_t>(reg) + count);
}

constexpr ExtReg ToExtRegQ(size_t base, bool bit) {
    return ExtReg::Q0 + ((base >> 1) + (bit ? 8 : 0));
}

constexpr ExtReg ToExtRegD(size_t base, bool bit) { return ExtReg::D0 + (base + (bit ? 16 : 0)); }

constexpr ExtReg ToExtRegS(size_t base, bool bit) {
    return ExtReg::S0 + ((base << 1) + unsigned(bit));
}

constexpr ExtReg ToExtReg(bool sz, size_t base, bool bit) {
    return sz ? ToExtRegD(base, bit) : ToExtRegS(base, bit);
}

constexpr ExtReg ToVector(bool q, size_t base, bool bit) {
    return q ? ToExtRegQ(base, bit) : ToExtRegD(base, bit);
}

template <size_t N> class Imm {
    static_assert(N && N <= 32);

public:
    static constexpr size_t bit_size = N;

    explicit constexpr Imm(u32 bits) : value(bits & (UINT32_MAX >> (32 - N))) {}

    template <class T = u32> constexpr T ZeroExtend() const { return static_cast<T>(value); }

    template <class T = s32> constexpr T SignExtend() const {
        return static_cast<T>(
                static_cast<s32>((value ^ (u32{1} << (N - 1))) - (u32{1} << (N - 1))));
    }

    template <size_t B> constexpr bool Bit() const {
        static_assert(B < N);
        return (value >> B) & 1;
    }

    template <size_t B, size_t E, class T = u32> constexpr T Bits() const {
        static_assert(B <= E && E < N);
        return static_cast<T>((value >> B) & (UINT32_MAX >> (31 - E + B)));
    }

    constexpr auto operator<=>(const Imm&) const = default;
    constexpr bool operator==(const Imm&) const = default;

    constexpr auto operator<=>(u32 other) const { return value <=> other; }

    constexpr bool operator==(u32 other) const { return value == other; }

private:
    u32 value;
};

template <size_t N, size_t... M> constexpr auto concatenate(Imm<N> first, Imm<M>... rest) {
    if constexpr (sizeof...(rest) == 0)
        return first;
    else {
        const auto tail = concatenate(rest...);
        return Imm<N + (M + ...)>{(first.ZeroExtend() << decltype(tail)::bit_size) |
                                  tail.ZeroExtend()};
    }
}

namespace bits {
template <class T> constexpr bool get_bit(unsigned bit, T value) { return (value >> bit) & 1; }

template <size_t B, class T> constexpr bool get_bit(T value) { return (value >> B) & 1; }

template <size_t B, size_t E, class T> constexpr auto get_bits(T value) {
    using U = std::make_unsigned_t<T>;
    return (static_cast<U>(value) >> B) & (~U{0} >> (sizeof(T) * 8 - 1 - E + B));
}

template <size_t N, class T> constexpr T sign_extend(T value) {
    return static_cast<T>((value ^ (T{1} << (N - 1))) - (T{1} << (N - 1)));
}

template <class T> constexpr unsigned count_ones(T value) {
    return std::popcount(static_cast<std::make_unsigned_t<T>>(value));
}

template <class T> constexpr unsigned lowest_set_bit(T value) {
    return std::countr_zero(static_cast<std::make_unsigned_t<T>>(value));
}

template <class T> constexpr int highest_set_bit(T value) {
    return int(std::bit_width(static_cast<std::make_unsigned_t<T>>(value))) - 1;
}

template <class T> constexpr T ones(unsigned count) {
    return count == sizeof(T) * 8 ? ~T{0} : (T{1} << count) - 1;
}

template <class T> constexpr T rotate_right(T value, unsigned count) {
    return std::rotr(value, count);
}

template <class From, class To> constexpr To replicate_element(From value) {
    To result{};
    for (size_t n = 0; n < sizeof(To); n += sizeof(From)) result |= To(value) << (n * 8);
    return result;
}
}  // namespace bits
}  // namespace swift::arm32
