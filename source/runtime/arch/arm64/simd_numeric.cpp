#include <array>
#include <bit>
#include <cstring>
#include "aarch64/instructions-aarch64.h"
#include "numeric.h"
#include "simd_math.h"

namespace swift::runtime::arm64 {
namespace {
using namespace vixl::aarch64;
using Pair = std::array<u64, 2>;

Pair Read(const GuestState& s, unsigned r) {
    Pair out;
    std::memcpy(out.data(), s.q[r], 16);
    return out;
}

void Write(GuestState& s, unsigned r, const Pair& value) { std::memcpy(s.q[r], value.data(), 16); }
}  // namespace

u64 NumericDot(u64 state, u64 encoded, u64) {
    auto& s = *reinterpret_cast<GuestState*>(state);
    const u32 word = encoded;
    const bool indexed = (encoded >> 40) & 1;
    const bool signed_bytes = !(word & (1u << 29));
    const unsigned rn = (word >> 5) & 31, rm = (word >> 16) & 31, rd = word & 31;
    std::array<u8, 16> left, right;
    std::memcpy(left.data(), s.q[rn], 16);
    std::memcpy(right.data(), s.q[rm], 16);
    const unsigned group = ((word >> 11) & 1) * 2 + ((word >> 21) & 1);
    Pair output{};
    const unsigned lanes = (word & NEON_Q) ? 4 : 2;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        u32 value;
        std::memcpy(&value, s.q[rd] + lane * 4, 4);
        for (unsigned byte = 0; byte < 4; ++byte) {
            const unsigned index = indexed ? group * 4 + byte : lane * 4 + byte;
            value += signed_bytes ? u32(s32(std::int8_t(left[lane * 4 + byte])) *
                                        s32(std::int8_t(right[index])))
                                  : u32(left[lane * 4 + byte]) * right[index];
        }
        std::memcpy(reinterpret_cast<u8*>(output.data()) + lane * 4, &value, 4);
    }
    Write(s, rd, output);
    return 0;
}

u64 NumericSatMultiply(u64 state, u64 encoded, u64) {
    auto& s = *reinterpret_cast<GuestState*>(state);
    u32 word = encoded;
    auto* i = reinterpret_cast<const Instruction*>(&word);
    const u32 flags = encoded >> 40;
    const bool scalar = flags & 1, indexed = flags & 2, extra = flags & 4;
    const u32 mask = indexed ? (scalar ? NEONScalarByIndexedElementMask : NEONByIndexedElementMask)
                     : extra ? (scalar ? NEONScalar3SameExtraMask : NEON3SameExtraMask)
                             : (scalar ? NEONScalar3SameMask : NEON3SameMask);
    const u32 op = i->Mask(mask), scalar_bits = scalar ? NEON_Q | NEONScalar : 0;
    const u32 plain = (indexed ? NEON_SQDMULH_byelement : NEON_SQDMULH) | scalar_bits;
    const u32 round = (indexed ? NEON_SQRDMULH_byelement : NEON_SQRDMULH) | scalar_bits;
    const u32 add = (indexed ? NEON_SQRDMLAH_byelement : NEON_SQRDMLAH) | scalar_bits;
    const u32 mode = op == plain ? 0 : op == round ? 1 : op == add ? 2 : 3;
    const u32 bits = 8u << i->GetNEONSize();
    auto left = Read(s, i->GetRn()), right = Read(s, i->GetRm());
    const auto old = Read(s, i->GetRd());
    if (indexed) {
        const u32 rm = bits == 16 ? i->GetRmLow16() : i->GetRm();
        const u32 index = bits == 16 ? (i->GetNEONH() << 2) | (i->GetNEONL() << 1) | i->GetNEONM()
                                     : (i->GetNEONH() << 1) | i->GetNEONL();
        const auto source = Read(s, rm);
        const u64 lane =
                (source[index * bits / 64] >> (index * bits % 64)) & ((u64{1} << bits) - 1);
        right = {};
        for (u32 j = 0; j < 128 / bits; ++j) right[j * bits / 64] |= lane << (j * bits % 64);
    }
    const auto result = EvaluateSaturatingMultiply(
            left, right, old, {bits, mode, scalar ? 1u : (i->GetNEONQ() ? 128u : 64u) / bits});
    Write(s, i->GetRd(), result.value);
    if (result.saturated) s.fpsr |= 1u << 27;
    return 0;
}

u64 NumericSatShift(u64 state, u64 encoded, u64) {
    auto& s = *reinterpret_cast<GuestState*>(state);
    u32 word = encoded;
    auto* i = reinterpret_cast<const Instruction*>(&word);
    SaturatingShiftMode mode;
    switch (i->Mask(NEONShiftImmediateMask)) {
#define MODE(code, name)                                                                           \
    case code:                                                                                     \
        mode = SaturatingShiftMode::name;                                                          \
        break;
        MODE(NEON_SQSHL_imm, SQSHL)
        MODE(NEON_UQSHL_imm, UQSHL)
        MODE(NEON_SQSHLU, SQSHLU)
        MODE(NEON_SQSHRN, SQSHRN)
        MODE(NEON_UQSHRN, UQSHRN)
        MODE(NEON_SQSHRUN, SQSHRUN)
        MODE(NEON_SQRSHRN, SQRSHRN)
        MODE(NEON_UQRSHRN, UQRSHRN)
        MODE(NEON_SQRSHRUN, SQRSHRUN)
#undef MODE
        default:
            s.helper_status = 1;
            return 0;
    }
    const bool narrow = u32(mode) >= 3, q = i->GetNEONQ();
    const u32 bits = 8u << (int(std::bit_width(u32(i->GetImmNEONImmh()))) - 1);
    const u32 immediate = i->GetImmNEONImmhImmb(),
              shift = narrow ? 2 * bits - immediate : immediate - bits;
    const auto result = EvaluateSaturatingShift(Read(s, i->GetRn()),
                                                {bits,
                                                 shift,
                                                 (narrow ? 64u
                                                  : q    ? 128u
                                                         : 64u) /
                                                         bits,
                                                 mode});
    auto value = result.value;
    if (narrow && q) {
        const auto original = Read(s, i->GetRd());
        value = {original[0], value[0]};
    }
    Write(s, i->GetRd(), value);
    if (result.saturated) s.fpsr |= 1u << 27;
    return 0;
}

u64 NumericUnsignedEstimate(u64 state, u64 encoded, u64) {
    auto& s = *reinterpret_cast<GuestState*>(state);
    u32 word = encoded;
    auto* i = reinterpret_cast<const Instruction*>(&word);
    const auto source = Read(s, i->GetRn());
    Pair result{};
    for (u32 j = 0; j < (i->GetNEONQ() ? 4u : 2u); ++j) {
        const u32 input = source[j / 2] >> (32 * (j % 2));
        result[j / 2] |= u64(EvaluateUnsignedEstimateLane(
                                 input, i->Mask(NEON2RegMiscFPMask) == NEON_URSQRTE))
                         << (32 * (j % 2));
    }
    Write(s, i->GetRd(), result);
    return 0;
}

u64 NumericCrc32(u64 state, u64 encoded, u64) {
    auto& s = *reinterpret_cast<GuestState*>(state);
    const u32 word = encoded;
    const unsigned rn = (word >> 5) & 31, rm = (word >> 16) & 31, rd = word & 31;
    u32 result = rn == 31 ? 0 : u32(s.gprs[rn]);
    u64 value = rm == 31 ? 0 : s.gprs[rm];
    const unsigned count = 1u << ((word >> 10) & 3);
    for (unsigned byte = 0; byte < count; ++byte) {
        result ^= u8(value);
        value >>= 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            result = (result >> 1) ^ (0xedb88320u & (0u - (result & 1)));
    }
    if (rd != 31) s.gprs[rd] = result;
    return result;
}
}  // namespace swift::runtime::arm64
