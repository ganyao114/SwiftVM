#include "assembler.h"

#include <bit>

namespace swift::runtime::backend::loongarch64 {
namespace {
constexpr std::int32_t Signed(std::uint32_t value, unsigned bits) {
    const auto sign = std::uint32_t{1} << (bits - 1);
    return std::bit_cast<std::int32_t>((value ^ sign) - sign);
}
}  // namespace

void Assembler::LI(GPR rd, std::uint64_t value) {
    const auto small = std::bit_cast<std::int64_t>(value);
    if (small >= -2048 && small <= 2047) {
        addi_d(rd, zero, static_cast<int>(small));
        return;
    }
    if (value <= 4095) {
        ori(rd, zero, static_cast<int>(value));
        return;
    }
    lu12i_w(rd, Signed(static_cast<std::uint32_t>((value >> 12) & 0xfffff), 20));
    if (value & 4095) ori(rd, rd, static_cast<int>(value & 4095));
    const auto sign32 = value & (std::uint64_t{1} << 31) ? UINT64_C(0xffffffff) : 0;
    if ((value >> 32) != sign32) {
        lu32i_d(rd, Signed(static_cast<std::uint32_t>((value >> 32) & 0xfffff), 20));
        const auto sign52 = value & (std::uint64_t{1} << 51) ? 4095u : 0u;
        if ((value >> 52) != sign52) lu52i_d(rd, rd, Signed(value >> 52, 12));
    }
}

void Assembler::AUIPC(GPR rd, std::uint32_t high) { pcaddu12i(rd, Signed(high, 20)); }

void Assembler::FCLASS(GPR rd, FPR src, Precision bits, FPR temporary) {
    // LA FCLASS writes its native classification bits into an FPR. The
    // caller supplies that temporary and receives the unchanged bit mask.
    if (bits == Precision::S)
        fclass_s(temporary, src);
    else
        fclass_d(temporary, src);
    movfr2gr_d(rd, temporary);
}

void Assembler::FEQ(GPR rd, FPR lhs, FPR rhs, Precision bits, unsigned condition) {
    if (bits == Precision::S)
        fcmp_ceq_s(condition, lhs, rhs);
    else
        fcmp_ceq_d(condition, lhs, rhs);
    movcf2gr(rd, condition);
}

void Assembler::FLT(GPR rd, FPR lhs, FPR rhs, Precision bits, unsigned condition) {
    if (bits == Precision::S)
        fcmp_clt_s(condition, lhs, rhs);
    else
        fcmp_clt_d(condition, lhs, rhs);
    movcf2gr(rd, condition);
}

void Assembler::FloatToInteger(
        GPR rd, FPR src, unsigned integer_bits, Precision bits, RMode rounding, FPR temporary) {
    const bool word = integer_bits == 32, single = bits == Precision::S;
#define LA_CONVERT(native)                                                                         \
    if (word) {                                                                                    \
        if (single)                                                                                \
            native##_w_s(temporary, src);                                                          \
        else                                                                                       \
            native##_w_d(temporary, src);                                                          \
    } else {                                                                                       \
        if (single)                                                                                \
            native##_l_s(temporary, src);                                                          \
        else                                                                                       \
            native##_l_d(temporary, src);                                                          \
    }
    switch (rounding) {
        case RMode::RNE: {
            LA_CONVERT(ftintrne) break;
        }
        case RMode::RTZ: {
            LA_CONVERT(ftintrz) break;
        }
        case RMode::RDN: {
            LA_CONVERT(ftintrm) break;
        }
        case RMode::RUP: {
            LA_CONVERT(ftintrp) break;
        }
        case RMode::DYN: {
            LA_CONVERT(ftint) break;
        }
    }
#undef LA_CONVERT
    if (word)
        movfr2gr_s(rd, temporary);
    else
        movfr2gr_d(rd, temporary);
}

void Assembler::IntegerToFloat(
        FPR rd, GPR src, unsigned integer_bits, Precision bits, FPR temporary) {
    if (integer_bits == 32) {
        movgr2fr_w(temporary, src);
        if (bits == Precision::S)
            ffint_s_w(rd, temporary);
        else
            ffint_d_w(rd, temporary);
    } else {
        movgr2fr_d(temporary, src);
        if (bits == Precision::S)
            ffint_s_l(rd, temporary);
        else
            ffint_d_l(rd, temporary);
    }
}
}  // namespace swift::runtime::backend::loongarch64
