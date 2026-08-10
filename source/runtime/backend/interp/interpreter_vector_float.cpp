#include "interpreter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace swift::runtime::backend::interp {

using ir::ValueType;

#include "interpreter_internal.h"

namespace {

enum class FloatBinaryKind : u8 { Add, Sub, Mul, Div };

template <typename T, typename Op>
unsigned __int128 VecFloatBinary(unsigned __int128 a,
                                 unsigned __int128 b,
                                 u32 lane_bits,
                                 Op op,
                                 FloatBinaryKind kind) {
    ASSERT(lane_bits == 32 || lane_bits == 64);
    unsigned __int128 result = 0;
    const u32 lanes = 128 / lane_bits;
    const u64 lane_mask = lane_bits == 32 ? 0xFFFFFFFFull : ~0ull;
    for (u32 lane = 0; lane < lanes; ++lane) {
        const u64 abits = static_cast<u64>(a >> (lane * lane_bits)) & lane_mask;
        const u64 bbits = static_cast<u64>(b >> (lane * lane_bits)) & lane_mask;
        T av{};
        T bv{};
        std::memcpy(&av, &abits, sizeof(T));
        std::memcpy(&bv, &bbits, sizeof(T));
        const T rv = op(av, bv);
        u64 rbits = 0;
        std::memcpy(&rbits, &rv, sizeof(T));
        // x86 SSE arithmetic propagates the first NaN operand, preserving
        // its sign/payload while setting the quiet bit. Host FPUs are free
        // to choose a different NaN sign, so normalize the raw lane here.
        const u64 exponent_mask = lane_bits == 32 ? 0x7F800000u
                                                    : 0x7FF0000000000000ull;
        const u64 fraction_mask = lane_bits == 32 ? 0x007FFFFFu
                                                    : 0x000FFFFFFFFFFFFFull;
        const u64 quiet_mask = lane_bits == 32 ? 0x00400000u
                                                 : 0x0008000000000000ull;
        const u64 sign_mask = lane_bits == 32 ? 0x80000000u : 0x8000000000000000ull;
        const bool a_nan = (abits & exponent_mask) == exponent_mask &&
                           (abits & fraction_mask) != 0;
        const bool b_nan = (bbits & exponent_mask) == exponent_mask &&
                           (bbits & fraction_mask) != 0;
        const bool a_inf = (abits & exponent_mask) == exponent_mask &&
                           (abits & fraction_mask) == 0;
        const bool b_inf = (bbits & exponent_mask) == exponent_mask &&
                           (bbits & fraction_mask) == 0;
        const bool a_zero = (abits & ~sign_mask) == 0;
        const bool b_zero = (bbits & ~sign_mask) == 0;
        // Keep the intermediate result canonical for the exact non-NaN edge
        // cases that exposed host-FPU differences in the interpreter.  The
        // operation has already been evaluated in T; these bit fixes only
        // enforce x86's infinity sign and round-to-nearest zero behavior.
        if (!a_nan && !b_nan) {
            if (kind == FloatBinaryKind::Sub && abits == bbits && !a_inf) {
                // x - x is +0 under the default MXCSR round-to-nearest mode.
                rbits = 0;
            } else if (kind == FloatBinaryKind::Mul && (a_inf || b_inf) && !(a_zero || b_zero)) {
                rbits = ((abits ^ bbits) & sign_mask) | exponent_mask;
            } else if (kind == FloatBinaryKind::Div && (a_inf || b_inf) && !b_inf && !b_zero) {
                rbits = ((abits ^ bbits) & sign_mask) | exponent_mask;
            } else if ((kind == FloatBinaryKind::Add || kind == FloatBinaryKind::Sub) &&
                       (a_inf || b_inf)) {
                // Addition/subtraction with one infinity returns that
                // infinity (subtraction flips the RHS sign).
                if (a_inf && !b_inf)
                    rbits = abits;
                else if (!a_inf && b_inf)
                    rbits = kind == FloatBinaryKind::Sub ? (bbits ^ sign_mask) : bbits;
            } else if ((kind == FloatBinaryKind::Mul || kind == FloatBinaryKind::Div) &&
                       (rbits & ~sign_mask) == 0) {
                // Signed zero for multiply/divide is the XOR of operand signs.
                rbits = (abits ^ bbits) & sign_mask;
            }
        }
        if (a_nan)
            rbits = abits | quiet_mask;
        else if (b_nan)
            rbits = bbits | quiet_mask;
        else if ((rbits & exponent_mask) == exponent_mask && (rbits & fraction_mask) != 0)
            rbits = lane_bits == 32 ? 0xFFC00000u : 0xFFF8000000000000ull;
        result |= static_cast<unsigned __int128>(rbits & lane_mask) << (lane * lane_bits);
    }
    return result;
}

u64 FloatCompareFlags(u64 a, u64 b, u32 lane_bits) {
    if (lane_bits == 32) {
        const u32 abits = static_cast<u32>(a);
        const u32 bbits = static_cast<u32>(b);
        float av{}, bv{};
        std::memcpy(&av, &abits, sizeof(av));
        std::memcpy(&bv, &bbits, sizeof(bv));
        if (std::isnan(av) || std::isnan(bv)) return 7;
        if (av == bv) return 4;
        return av < bv ? 1 : 0;
    }
    double av{}, bv{};
    std::memcpy(&av, &a, sizeof(av));
    std::memcpy(&bv, &b, sizeof(bv));
    if (std::isnan(av) || std::isnan(bv)) return 7;
    if (av == bv) return 4;
    return av < bv ? 1 : 0;
}

u64 FloatToIntIndefinite(u64 raw, u32 src_bits, u32 dst_bits) {
    long double value{};
    if (src_bits == 32) {
        const u32 bits = static_cast<u32>(raw);
        float v{};
        std::memcpy(&v, &bits, sizeof(v));
        value = v;
    } else {
        double v{};
        std::memcpy(&v, &raw, sizeof(v));
        value = v;
    }
    if (dst_bits == 32) {
        constexpr long double kMin = -2147483648.0L;
        constexpr long double kMaxExclusive = 2147483648.0L;
        if (std::isnan(static_cast<double>(value)) || value < kMin || value >= kMaxExclusive)
            return 0x80000000u;
        return static_cast<u32>(static_cast<s32>(value));
    }
    constexpr long double kMin = -9223372036854775808.0L;
    constexpr long double kMaxExclusive = 9223372036854775808.0L;
    if (std::isnan(static_cast<double>(value)) || value < kMin || value >= kMaxExclusive)
        return 0x8000000000000000ull;
    return static_cast<u64>(static_cast<s64>(value));
}

template <typename Op>
unsigned __int128 VecFloatScalar32(unsigned __int128 a, unsigned __int128 b, Op op) {
    const u32 left_bits = static_cast<u32>(a);
    const u32 right_bits = static_cast<u32>(b);
    float left;
    float right;
    std::memcpy(&left, &left_bits, sizeof(left));
    std::memcpy(&right, &right_bits, sizeof(right));
    const float value = op(left, right);
    u32 result_bits;
    std::memcpy(&result_bits, &value, sizeof(result_bits));
    const bool left_nan = (left_bits & 0x7F800000u) == 0x7F800000u &&
                          (left_bits & 0x007FFFFFu) != 0;
    const bool right_nan = (right_bits & 0x7F800000u) == 0x7F800000u &&
                           (right_bits & 0x007FFFFFu) != 0;
    const bool result_nan = (result_bits & 0x7F800000u) == 0x7F800000u &&
                            (result_bits & 0x007FFFFFu) != 0;
    if (left_nan)
        result_bits = left_bits | 0x00400000u;
    else if (right_nan)
        result_bits = right_bits | 0x00400000u;
    else if (result_nan)
        result_bits = 0xFFC00000u;
    return (a & ~static_cast<unsigned __int128>(0xFFFFFFFFu)) | result_bits;
}


template <typename Op>
unsigned __int128 VecFloatScalar64(unsigned __int128 a, unsigned __int128 b, Op op) {
    const u64 left_bits = static_cast<u64>(a);
    const u64 right_bits = static_cast<u64>(b);
    double left;
    double right;
    std::memcpy(&left, &left_bits, sizeof(left));
    std::memcpy(&right, &right_bits, sizeof(right));
    const double value = op(left, right);
    u64 result_bits;
    std::memcpy(&result_bits, &value, sizeof(result_bits));
    const bool left_nan = (left_bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
                          (left_bits & 0x000FFFFFFFFFFFFFull) != 0;
    const bool right_nan =
            (right_bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
            (right_bits & 0x000FFFFFFFFFFFFFull) != 0;
    const bool result_nan =
            (result_bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
            (result_bits & 0x000FFFFFFFFFFFFFull) != 0;
    if (left_nan)
        result_bits = left_bits | 0x0008000000000000ull;
    else if (right_nan)
        result_bits = right_bits | 0x0008000000000000ull;
    else if (result_nan)
        result_bits = 0xFFF8000000000000ull;
    return (a & (static_cast<unsigned __int128>(UINT64_MAX) << 64)) | result_bits;
}

}  // namespace

void Interpreter::RunVecFAdd(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const auto a = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto b = ReadVec(stack, inst->GetArg<ir::Value>(1));
    WriteVec(stack,
             inst,
             bits == 32 ? VecFloatBinary<float>(a, b, bits, [](float x, float y) { return x + y; }, FloatBinaryKind::Add)
                        : VecFloatBinary<double>(a, b, bits, [](double x, double y) { return x + y; }, FloatBinaryKind::Add));
}

void Interpreter::RunVecFSub(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const auto a = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto b = ReadVec(stack, inst->GetArg<ir::Value>(1));
    WriteVec(stack,
             inst,
             bits == 32 ? VecFloatBinary<float>(a, b, bits, [](float x, float y) { return x - y; }, FloatBinaryKind::Sub)
                        : VecFloatBinary<double>(a, b, bits, [](double x, double y) { return x - y; }, FloatBinaryKind::Sub));
}

void Interpreter::RunVecFMul(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const auto a = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto b = ReadVec(stack, inst->GetArg<ir::Value>(1));
    WriteVec(stack,
             inst,
             bits == 32 ? VecFloatBinary<float>(a, b, bits, [](float x, float y) { return x * y; }, FloatBinaryKind::Mul)
                        : VecFloatBinary<double>(a, b, bits, [](double x, double y) { return x * y; }, FloatBinaryKind::Mul));
}

void Interpreter::RunVecFDiv(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const auto a = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto b = ReadVec(stack, inst->GetArg<ir::Value>(1));
    WriteVec(stack,
             inst,
             bits == 32 ? VecFloatBinary<float>(a, b, bits, [](float x, float y) { return x / y; }, FloatBinaryKind::Div)
                        : VecFloatBinary<double>(a, b, bits, [](double x, double y) { return x / y; }, FloatBinaryKind::Div));
}

void Interpreter::RunVecFMinMax(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool maximum = inst->GetArg<ir::Imm>(3).Get() != 0;
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    const u128 left = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u128 right = ReadVec(stack, inst->GetArg<ir::Value>(1));
    u128 result = scalar ? left : 0;
    const u32 lanes = scalar ? 1 : 128 / bits;
    const u64 lane_mask = bits == 64 ? UINT64_MAX : UINT32_MAX;
    for (u32 lane = 0; lane < lanes; ++lane) {
        const u64 a_bits = static_cast<u64>(left >> (lane * bits)) & lane_mask;
        const u64 b_bits = static_cast<u64>(right >> (lane * bits)) & lane_mask;
        bool choose_left;
        if (bits == 32) {
            float a;
            float b;
            const u32 aa = u32(a_bits);
            const u32 bb = u32(b_bits);
            std::memcpy(&a, &aa, sizeof(a));
            std::memcpy(&b, &bb, sizeof(b));
            choose_left = !std::isnan(a) && !std::isnan(b) &&
                          (maximum ? a > b : a < b);
        } else {
            double a;
            double b;
            std::memcpy(&a, &a_bits, sizeof(a));
            std::memcpy(&b, &b_bits, sizeof(b));
            choose_left = !std::isnan(a) && !std::isnan(b) &&
                          (maximum ? a > b : a < b);
        }
        const u128 mask = static_cast<u128>(lane_mask) << (lane * bits);
        const u64 selected = choose_left ? a_bits : b_bits;
        result = (result & ~mask) | (static_cast<u128>(selected) << (lane * bits));
    }
    WriteVec(stack, inst, result);
}

void Interpreter::RunVecFUnary(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 kind = inst->GetArg<ir::Imm>(3).Get();
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    const u128 source = ReadVec(stack, inst->GetArg<ir::Value>(0));
    u128 result = scalar ? ReadVec(stack, inst->GetArg<ir::Value>(1)) : 0;
    const u32 lanes = scalar ? 1 : 128 / bits;
    const u64 lane_mask = bits == 64 ? UINT64_MAX : UINT32_MAX;
    for (u32 lane = 0; lane < lanes; ++lane) {
        const u64 raw = static_cast<u64>(source >> (lane * bits)) & lane_mask;
        u64 output;
        // x86 SQRT of a negative operand raises #I and delivers the QNaN
        // *indefinite*, whose sign bit is SET (0xFFC00000 / 0xFFF8...). Both
        // std::sqrt and ARM's FSQRT deliver a positive default NaN instead, so
        // the sign has to be forced. `value < 0` is false for NaN (which must
        // keep propagating) and for -0.0 (whose sqrt is legitimately -0.0),
        // which is exactly the wanted predicate. Verified against real x86 via
        // Rosetta: sqrtps(-4) = ffc00000, sqrtpd(-4) = fff8000000000000.
        // Kept in step with JitTranslator::EmitVecFUnary — a divergence here is
        // worse than either behaviour, since the two backends are differentially
        // tested against each other.
        if (bits == 32) {
            float value;
            const u32 input = u32(raw);
            std::memcpy(&value, &input, sizeof(value));
            const float converted = kind == 0 ? std::sqrt(value)
                                    : kind == 1 ? 1.0f / value
                                                : 1.0f / std::sqrt(value);
            u32 encoded;
            std::memcpy(&encoded, &converted, sizeof(encoded));
            if (kind == 0 && value < 0.0f) {
                encoded = 0xFFC00000u;
            }
            output = encoded;
        } else {
            double value;
            std::memcpy(&value, &raw, sizeof(value));
            const double converted = std::sqrt(value);
            std::memcpy(&output, &converted, sizeof(output));
            if (value < 0.0) {
                output = UINT64_C(0xFFF8000000000000);
            }
        }
        const u128 mask = static_cast<u128>(lane_mask) << (lane * bits);
        result = (result & ~mask) | (static_cast<u128>(output) << (lane * bits));
    }
    WriteVec(stack, inst, result);
}

void Interpreter::RunVecFCmp(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u64 a = static_cast<u64>(ReadVec(stack, inst->GetArg<ir::Value>(0)));
    const u64 b = static_cast<u64>(ReadVec(stack, inst->GetArg<ir::Value>(1)));
    WriteScalar(stack, inst, FloatCompareFlags(a, b, bits));
}

// The predicate is a relation set -- bit 0 = less, 1 = equal, 2 = greater,
// 3 = unordered; see the comment on VecFCmpMask in ir/ir.inc.  The four
// outcomes are mutually exclusive and exhaustive, so the lane's mask is simply
// "is the outcome that occurred a member of the set", and all 16 sets (0 =
// never, 15 = always) fall out of that with no per-predicate case at all.
void Interpreter::RunVecFCmpMask(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 relations = inst->GetArg<ir::Imm>(3).Get() & 15;
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    const u128 left = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u128 right = ReadVec(stack, inst->GetArg<ir::Value>(1));
    u128 result = scalar ? left : 0;
    const u32 lanes = scalar ? 1 : 128 / bits;
    const u64 lane_mask = bits == 64 ? UINT64_MAX : UINT32_MAX;
    for (u32 lane = 0; lane < lanes; ++lane) {
        const u64 a_bits = static_cast<u64>(left >> (lane * bits)) & lane_mask;
        const u64 b_bits = static_cast<u64>(right >> (lane * bits)) & lane_mask;
        bool eq;
        bool lt;
        bool gt;
        bool unordered;
        if (bits == 32) {
            float a;
            float b;
            const u32 aa = u32(a_bits);
            const u32 bb = u32(b_bits);
            std::memcpy(&a, &aa, sizeof(a));
            std::memcpy(&b, &bb, sizeof(b));
            unordered = std::isnan(a) || std::isnan(b);
            eq = !unordered && a == b;
            lt = !unordered && a < b;
            gt = !unordered && a > b;
        } else {
            double a;
            double b;
            std::memcpy(&a, &a_bits, sizeof(a));
            std::memcpy(&b, &b_bits, sizeof(b));
            unordered = std::isnan(a) || std::isnan(b);
            eq = !unordered && a == b;
            lt = !unordered && a < b;
            gt = !unordered && a > b;
        }
        const u32 outcome = unordered ? 8u : lt ? 1u : eq ? 2u : gt ? 4u : 0u;
        const bool matched = (relations & outcome) != 0;
        const u128 mask = static_cast<u128>(lane_mask) << (lane * bits);
        result = (result & ~mask) |
                 (matched ? static_cast<u128>(lane_mask) << (lane * bits) : 0);
    }
    WriteVec(stack, inst, result);
}


void Interpreter::RunVecFCvtIntToFloat(ir::Inst* inst, InterpStack& stack) {
    const u32 src_bits = inst->GetArg<ir::Imm>(1).Get();
    const u32 dst_bits = inst->GetArg<ir::Imm>(2).Get();
    const u64 raw = static_cast<u64>(ReadScalar(stack, inst->GetArg<ir::Value>(0)));
    if (dst_bits == 32) {
        const long double value = src_bits == 32 ? static_cast<long double>(static_cast<s32>(raw))
                                                 : static_cast<long double>(static_cast<s64>(raw));
        const float converted = static_cast<float>(value);
        u32 bits = 0;
        std::memcpy(&bits, &converted, sizeof(bits));
        WriteScalar(stack, inst, bits);
    } else {
        const long double value = src_bits == 32 ? static_cast<long double>(static_cast<s32>(raw))
                                                 : static_cast<long double>(static_cast<s64>(raw));
        const double converted = static_cast<double>(value);
        u64 bits = 0;
        std::memcpy(&bits, &converted, sizeof(bits));
        WriteScalar(stack, inst, bits);
    }
}

void Interpreter::RunVecFCvtFloatToInt(ir::Inst* inst, InterpStack& stack) {
    const u32 src_bits = inst->GetArg<ir::Imm>(1).Get();
    const u32 dst_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool round_nearest = inst->GetArg<ir::Imm>(3).Get() != 0;
    const u64 raw = static_cast<u64>(ReadScalar(stack, inst->GetArg<ir::Value>(0)));
    if (!round_nearest) {
        WriteScalar(stack, inst, FloatToIntIndefinite(raw, src_bits, dst_bits));
        return;
    }
    long double value;
    if (src_bits == 32) {
        const u32 encoded = u32(raw);
        float decoded;
        std::memcpy(&decoded, &encoded, sizeof(decoded));
        value = std::nearbyint(static_cast<long double>(decoded));
    } else {
        double decoded;
        std::memcpy(&decoded, &raw, sizeof(decoded));
        value = std::nearbyint(static_cast<long double>(decoded));
    }
    const long double minimum =
            dst_bits == 32 ? -2147483648.0L : -9223372036854775808.0L;
    const long double maximum =
            dst_bits == 32 ? 2147483648.0L : 9223372036854775808.0L;
    if (std::isnan(static_cast<double>(value)) || value < minimum || value >= maximum) {
        WriteScalar(stack, inst, dst_bits == 32 ? 0x80000000u : 0x8000000000000000ull);
    } else {
        WriteScalar(stack,
                    inst,
                    dst_bits == 32 ? u64(u32(static_cast<s32>(value)))
                                   : u64(static_cast<s64>(value)));
    }
}

void Interpreter::RunVecFCvtScalar(ir::Inst* inst, InterpStack& stack) {
    const u32 src_bits = inst->GetArg<ir::Imm>(1).Get();
    const u64 raw = static_cast<u64>(ReadScalar(stack, inst->GetArg<ir::Value>(0)));
    if (src_bits == 32) {
        const u32 source = static_cast<u32>(raw);
        float value{};
        std::memcpy(&value, &source, sizeof(value));
        const double converted = static_cast<double>(value);
        u64 bits = 0;
        std::memcpy(&bits, &converted, sizeof(bits));
        WriteScalar(stack, inst, bits);
    } else {
        double value{};
        std::memcpy(&value, &raw, sizeof(value));
        const float converted = static_cast<float>(value);
        u32 bits = 0;
        std::memcpy(&bits, &converted, sizeof(bits));
        WriteScalar(stack, inst, bits);
    }
}

void Interpreter::RunVecFCvtPacked(ir::Inst* inst, InterpStack& stack) {
    const u128 source = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u32 kind = inst->GetArg<ir::Imm>(1).Get();
    u128 result = 0;
    auto put32 = [&](u32 lane, u32 value) {
        result |= static_cast<u128>(value) << (lane * 32);
    };
    auto put64 = [&](u32 lane, u64 value) {
        result |= static_cast<u128>(value) << (lane * 64);
    };
    if (kind == 0 || kind == 1) {
        const u32 lanes = kind == 0 ? 4 : 2;
        for (u32 lane = 0; lane < lanes; ++lane) {
            const s32 value = static_cast<s32>(source >> (lane * 32));
            if (kind == 0) {
                const float converted = static_cast<float>(value);
                u32 encoded;
                std::memcpy(&encoded, &converted, sizeof(encoded));
                put32(lane, encoded);
            } else {
                const double converted = static_cast<double>(value);
                u64 encoded;
                std::memcpy(&encoded, &converted, sizeof(encoded));
                put64(lane, encoded);
            }
        }
    } else if (kind >= 2 && kind <= 5) {
        const bool source_double = kind >= 4;
        const bool truncate = kind == 3 || kind == 5;
        const u32 lanes = source_double ? 2 : 4;
        for (u32 lane = 0; lane < lanes; ++lane) {
            long double value;
            if (source_double) {
                const u64 raw = static_cast<u64>(source >> (lane * 64));
                double decoded;
                std::memcpy(&decoded, &raw, sizeof(decoded));
                value = decoded;
            } else {
                const u32 raw = static_cast<u32>(source >> (lane * 32));
                float decoded;
                std::memcpy(&decoded, &raw, sizeof(decoded));
                value = decoded;
            }
            value = truncate ? std::trunc(value) : std::nearbyint(value);
            u32 encoded = 0x80000000u;
            if (!std::isnan(static_cast<double>(value)) &&
                value >= -2147483648.0L && value < 2147483648.0L) {
                encoded = static_cast<u32>(static_cast<s32>(value));
            }
            put32(lane, encoded);
        }
    } else if (kind == 6) {
        for (u32 lane = 0; lane < 2; ++lane) {
            const u32 raw = static_cast<u32>(source >> (lane * 32));
            float decoded;
            std::memcpy(&decoded, &raw, sizeof(decoded));
            const double converted = decoded;
            u64 encoded;
            std::memcpy(&encoded, &converted, sizeof(encoded));
            put64(lane, encoded);
        }
    } else {
        for (u32 lane = 0; lane < 2; ++lane) {
            const u64 raw = static_cast<u64>(source >> (lane * 64));
            double decoded;
            std::memcpy(&decoded, &raw, sizeof(decoded));
            const float converted = static_cast<float>(decoded);
            u32 encoded;
            std::memcpy(&encoded, &converted, sizeof(encoded));
            put32(lane, encoded);
        }
    }
    WriteVec(stack, inst, result);
}


void Interpreter::RunVecFAddScalar32(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             VecFloatScalar32(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                              ReadScalarBits(stack, inst->GetArg<ir::Value>(1)),
                              [](float a, float b) { return a + b; }));
}

void Interpreter::RunVecFSubScalar32(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             VecFloatScalar32(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                              ReadScalarBits(stack, inst->GetArg<ir::Value>(1)),
                              [](float a, float b) { return a - b; }));
}

void Interpreter::RunVecFMulScalar32(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             VecFloatScalar32(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                              ReadScalarBits(stack, inst->GetArg<ir::Value>(1)),
                              [](float a, float b) { return a * b; }));
}

void Interpreter::RunVecFDivScalar32(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             VecFloatScalar32(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                              ReadScalarBits(stack, inst->GetArg<ir::Value>(1)),
                              [](float a, float b) { return a / b; }));
}






void Interpreter::RunVecFAddScalar64(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             VecFloatScalar64(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                              ReadScalarBits(stack, inst->GetArg<ir::Value>(1)),
                              [](double a, double b) { return a + b; }));
}

void Interpreter::RunVecFSubScalar64(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             VecFloatScalar64(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                              ReadScalarBits(stack, inst->GetArg<ir::Value>(1)),
                              [](double a, double b) { return a - b; }));
}

void Interpreter::RunVecFMulScalar64(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             VecFloatScalar64(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                              ReadScalarBits(stack, inst->GetArg<ir::Value>(1)),
                              [](double a, double b) { return a * b; }));
}

void Interpreter::RunVecFDivScalar64(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             VecFloatScalar64(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                              ReadScalarBits(stack, inst->GetArg<ir::Value>(1)),
                              [](double a, double b) { return a / b; }));
}

// Fused multiply-add: dst = +-(a*b) +- c with a SINGLE rounding (ir.inc).
//
// std::fma is not a style preference here.  Written `av * bv + cv`, C++ is
// free to round the product before the addition (and on a host without a
// fused unit it must), which is exactly the double rounding this opcode
// exists to avoid -- and since the JIT lowers to FMLA, an unfused
// interpreter would differ from it on every input whose exact product falls
// on a tie.  That is the worse failure the two-backend rule guards against:
// not "unlike hardware" but "the two backends disagree".
//
// Mirrors JitTranslator::EmitVecFMulAdd, including its x86 NaN rules: a NaN
// source is returned quieted with the earliest source winning (order a, b,
// c), and an invalid operation with no NaN source -- Inf*0, or an infinite
// product added to the opposite infinity -- yields the QNaN indefinite, whose
// sign bit is SET.
void Interpreter::RunVecFMulAdd(ir::Inst* inst, InterpStack& stack) {
    const u128 a = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u128 b = ReadVec(stack, inst->GetArg<ir::Value>(1));
    const u128 c = ReadVec(stack, inst->GetArg<ir::Value>(2));
    const u32 bits = inst->GetArg<ir::Imm>(3).Get();
    const u32 flags = inst->GetArg<ir::Imm>(4).Get();
    ASSERT(bits == 32 || bits == 64);
    const bool negate_product = (flags & 1u) != 0;
    const bool negate_addend = (flags & 2u) != 0;
    const u32 lanes = 128 / bits;
    const u64 lane_mask = bits == 32 ? 0xFFFFFFFFull : ~0ull;
    const u64 exponent_mask = bits == 32 ? 0x7F800000ull : 0x7FF0000000000000ull;
    const u64 fraction_mask = bits == 32 ? 0x007FFFFFull : 0x000FFFFFFFFFFFFFull;
    const u64 quiet_mask = bits == 32 ? 0x00400000ull : 0x0008000000000000ull;
    const u64 indefinite = bits == 32 ? 0xFFC00000ull : 0xFFF8000000000000ull;
    const auto is_nan = [&](u64 x) {
        return (x & exponent_mask) == exponent_mask && (x & fraction_mask) != 0;
    };
    u128 result = 0;
    for (u32 lane = 0; lane < lanes; ++lane) {
        const u32 shift = lane * bits;
        const u64 a_bits = static_cast<u64>(a >> shift) & lane_mask;
        const u64 b_bits = static_cast<u64>(b >> shift) & lane_mask;
        const u64 c_bits = static_cast<u64>(c >> shift) & lane_mask;
        u64 r_bits = 0;
        if (bits == 32) {
            float av{};
            float bv{};
            float cv{};
            const u32 ai = u32(a_bits);
            const u32 bi = u32(b_bits);
            const u32 ci = u32(c_bits);
            std::memcpy(&av, &ai, sizeof(av));
            std::memcpy(&bv, &bi, sizeof(bv));
            std::memcpy(&cv, &ci, sizeof(cv));
            if (negate_product) av = -av;
            if (negate_addend) cv = -cv;
            const float rv = std::fma(av, bv, cv);
            u32 encoded;
            std::memcpy(&encoded, &rv, sizeof(encoded));
            r_bits = encoded;
        } else {
            double av{};
            double bv{};
            double cv{};
            std::memcpy(&av, &a_bits, sizeof(av));
            std::memcpy(&bv, &b_bits, sizeof(bv));
            std::memcpy(&cv, &c_bits, sizeof(cv));
            if (negate_product) av = -av;
            if (negate_addend) cv = -cv;
            const double rv = std::fma(av, bv, cv);
            std::memcpy(&r_bits, &rv, sizeof(r_bits));
        }
        if (is_nan(a_bits)) {
            r_bits = a_bits | quiet_mask;
        } else if (is_nan(b_bits)) {
            r_bits = b_bits | quiet_mask;
        } else if (is_nan(c_bits)) {
            r_bits = c_bits | quiet_mask;
        } else if (is_nan(r_bits)) {
            r_bits = indefinite;
        }
        result |= static_cast<u128>(r_bits & lane_mask) << shift;
    }
    WriteVec(stack, inst, result);
}

// VecFRoundInt -- round each lane to an integral floating-point value.
//
// Kept in step with JitTranslator::EmitVecFRoundInt (FRINTN/FRINTM/FRINTP/
// FRINTZ).  Two things are done by hand rather than by libm, both so that the
// answer cannot depend on host state:
//
//  * NaN is handled BEFORE the arithmetic.  std::trunc / std::floor of a
//    signalling NaN is not specified to quiet it, and on some hosts lowers to
//    an instruction that raises.  Returning `raw | quiet_bit` reproduces what
//    both FRINT* and x86 ROUND* do: the operand back, quieted, sign and
//    payload untouched.
//  * Nearest-ties-to-even is spelled out instead of calling std::nearbyint,
//    which follows the HOST rounding mode.  Nothing in this runtime calls
//    fesetround today, but FRINTN is unconditionally ties-to-even, and a
//    divergence between the two back ends is worse than either behaviour.
void Interpreter::RunVecFRoundInt(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 mode = inst->GetArg<ir::Imm>(3).Get();
    const bool scalar = inst->GetArg<ir::Imm>(4).Get() != 0;
    const u128 source = ReadVec(stack, inst->GetArg<ir::Value>(0));
    u128 result = scalar ? ReadVec(stack, inst->GetArg<ir::Value>(1)) : 0;
    const u32 lanes = scalar ? 1 : 128 / bits;
    const u64 lane_mask = MaskBits(bits);

    // Ties-to-even without touching the host rounding mode.  |x| >= 2^52
    // (2^23 for f32) is already integral, and there `x - trunc(x)` is exactly
    // zero, so the general path below returns it unchanged.
    const auto round_even = [](double x) {
        const double t = std::trunc(x);
        const double frac = std::fabs(x - t);
        double adjust = 0.0;
        if (frac > 0.5) {
            adjust = 1.0;
        } else if (frac == 0.5) {
            // Ties go to the even neighbour: step away from zero only when
            // truncation left an odd integer.
            adjust = std::fmod(t, 2.0) != 0.0 ? 1.0 : 0.0;
        }
        // The step must be AWAY FROM ZERO, so it carries x's sign: -1.5 must
        // become -2.0, not (-1.0 + 1.0) = -0.0.  The trailing copysign then
        // restores the sign of a zero result (-0.3 truncates to -0.0).
        return std::copysign(t + std::copysign(adjust, x), x);
    };

    for (u32 lane = 0; lane < lanes; ++lane) {
        const u64 raw = static_cast<u64>(source >> (lane * bits)) & lane_mask;
        u64 output;
        if (bits == 32) {
            constexpr u32 kExponent = 0x7F800000u;
            constexpr u32 kMantissa = 0x007FFFFFu;
            constexpr u32 kQuiet = 0x00400000u;
            const auto input = static_cast<u32>(raw);
            if ((input & kExponent) == kExponent && (input & kMantissa) != 0) {
                output = input | kQuiet;
            } else {
                float value;
                std::memcpy(&value, &input, sizeof(value));
                const double wide = static_cast<double>(value);
                const double rounded = mode == 0   ? round_even(wide)
                                       : mode == 1 ? std::floor(wide)
                                       : mode == 2 ? std::ceil(wide)
                                                   : std::trunc(wide);
                // An f32 rounded to an integer is exactly representable in f32
                // (its magnitude never grows past the next power of two), so
                // the double detour cannot lose anything.
                const auto narrowed = static_cast<float>(rounded);
                u32 encoded;
                std::memcpy(&encoded, &narrowed, sizeof(encoded));
                output = encoded;
            }
        } else {
            constexpr u64 kExponent = UINT64_C(0x7FF0000000000000);
            constexpr u64 kMantissa = UINT64_C(0x000FFFFFFFFFFFFF);
            constexpr u64 kQuiet = UINT64_C(0x0008000000000000);
            if ((raw & kExponent) == kExponent && (raw & kMantissa) != 0) {
                output = raw | kQuiet;
            } else {
                double value;
                std::memcpy(&value, &raw, sizeof(value));
                const double rounded = mode == 0   ? round_even(value)
                                       : mode == 1 ? std::floor(value)
                                       : mode == 2 ? std::ceil(value)
                                                   : std::trunc(value);
                std::memcpy(&output, &rounded, sizeof(output));
            }
        }
        const u128 mask = static_cast<u128>(lane_mask) << (lane * bits);
        result = (result & ~mask) | (static_cast<u128>(output) << (lane * bits));
    }
    WriteVec(stack, inst, result);
}

}  // namespace swift::runtime::backend::interp
