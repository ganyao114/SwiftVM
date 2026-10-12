#include "helpers.h"

#include <bit>
#include <array>
#include <cstring>
#include <limits>
#include <emmintrin.h>
#include "runtime/backend/atomic_fallback.h"
extern "C" {
#include "softfloat.h"
}

namespace swift::runtime::backend::x64 {

HostPairResult AtomicCompareExchange128(void* address, u64 expected_low, u64 expected_high,
                                       u64 desired_low, u64 desired_high) {
    // Writer admission drains native/interpreter readers before either half
    // is accessed. Do not acquire a reader lease around this helper: the
    // writer would wait for itself. Mismatch must never attempt a store.
    UnalignedAtomicGuard writer;
    std::array<u64, 2> observed{};
    std::memcpy(observed.data(), address, 16);
    if (observed[0] == expected_low && observed[1] == expected_high) {
        // memcpy may become two MOVQs: a write fault in the second page
        // would then leave the first eight bytes changed. Force one SSE2
        // store so a precise x64 page fault leaves the complete pair intact.
        // VM readers are excluded by the writer guard; MOVDQU itself is not
        // an atomic publication for unrelated host threads.
        const __m128i desired = _mm_set_epi64x(s64(desired_high), s64(desired_low));
        asm volatile("movdqu %1, (%0)" : : "r"(address), "x"(desired) : "memory");
    }
    return {observed[0], observed[1]};
}

void Sha256Message(void* output, const void* destination, const void* source, u32 second) {
    std::array<u32, 4> data{}, other{}, result{};
    std::memcpy(data.data(), destination, 16); std::memcpy(other.data(), source, 16);
    const auto sigma0 = [](u32 value) { return std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3); };
    const auto sigma1 = [](u32 value) { return std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10); };
    if (second) {
        result[0] = data[0] + sigma1(other[2]); result[1] = data[1] + sigma1(other[3]);
        result[2] = data[2] + sigma1(result[0]); result[3] = data[3] + sigma1(result[1]);
    } else {
        for (u32 lane = 0; lane < 4; ++lane) result[lane] = data[lane] + sigma0(lane == 3 ? other[0] : data[lane + 1]);
    }
    std::memcpy(output, result.data(), 16);
}

void Sha256Rounds(void* output, const void* destination, const void* source, const void* key) {
    std::array<u32, 4> data{}, other{}, round_keys{}, result{};
    std::memcpy(data.data(), destination, 16); std::memcpy(other.data(), source, 16); std::memcpy(round_keys.data(), key, 16);
    u32 a = other[3], b = other[2], c = data[3], d = data[2];
    u32 e = other[1], f = other[0], g = data[1], h = data[0];
    for (u32 round = 0; round < 2; ++round) {
        const u32 first = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                          ((e & f) ^ (~e & g)) + round_keys[round];
        const u32 second = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + first; d = c; c = b; b = a; a = first + second;
    }
    result = {f, e, b, a}; std::memcpy(output, result.data(), 16);
}

const u32* Crc32cTable() {
    static constexpr auto table = [] {
        std::array<u32, 256> result{};
        for (u32 index = 0; index < 256; ++index) {
            u32 value = index;
            for (u32 bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0x82F63B78u : 0u);
            result[index] = value;
        }
        return result;
    }();
    return table.data();
}

namespace {
constexpr u8 CryptoMultiply(u8 a, u8 b) {
    u8 result{};
    for (u32 bit = 0; bit < 8; ++bit) {
        if (b & 1) result ^= a;
        a = u8((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
        b >>= 1;
    }
    return result;
}
constexpr auto CryptoSubstitutions() {
    std::array<std::array<u8, 256>, 2> tables{};
    for (u32 value = 0; value < 256; ++value) {
        u8 inverse = value ? 1 : 0, factor = u8(value);
        for (u32 exponent = 254; exponent; exponent >>= 1) {
            if (exponent & 1) inverse = CryptoMultiply(inverse, factor);
            factor = CryptoMultiply(factor, factor);
        }
        u8 substituted = inverse ^ 0x63;
        for (u32 rotate = 1; rotate < 5; ++rotate)
            substituted ^= u8((inverse << rotate) | (inverse >> (8 - rotate)));
        tables[0][value] = substituted;
        tables[1][substituted] = u8(value);
    }
    return tables;
}
constexpr auto crypto_substitutions = CryptoSubstitutions();
}

void AesRound(void* output, const void* input, const void* key, u32 mode) {
    // Copy both operands before storing: recycled SSA homes may alias output.
    std::array<u8, 16> data{}, round_key{}, shifted{}, mixed{};
    std::memcpy(data.data(), input, 16); std::memcpy(round_key.data(), key, 16);
    const bool inverse = (mode & 2) != 0, last = (mode & 1) != 0;
    for (u32 column = 0; column < 4; ++column) for (u32 row = 0; row < 4; ++row) {
        const auto source_column = (column + (inverse ? 4 - row : row)) % 4;
        shifted[column * 4 + row] = crypto_substitutions[inverse][data[source_column * 4 + row]];
    }
    if (!last) {
        constexpr u8 forward[] = {2, 3, 1, 1}, backward[] = {14, 11, 13, 9};
        const auto* coefficients = inverse ? backward : forward;
        for (u32 column = 0; column < 4; ++column) for (u32 row = 0; row < 4; ++row)
            for (u32 term = 0; term < 4; ++term)
                mixed[column * 4 + row] ^= CryptoMultiply(shifted[column * 4 + term], coefficients[(term + 4 - row) % 4]);
        shifted = mixed;
    }
    for (u32 byte = 0; byte < 16; ++byte) shifted[byte] ^= round_key[byte];
    std::memcpy(output, shifted.data(), 16);
}

void AesKeygen(void* output, const void* input, u32 rcon) {
    std::array<u8, 16> data{}, result{};
    std::memcpy(data.data(), input, 16);
    for (u32 word = 0; word < 4; ++word) for (u32 byte = 0; byte < 4; ++byte) {
        result[word * 4 + byte] = crypto_substitutions[0][data[(word < 2 ? 1 : 3) * 4 + ((byte + (word & 1)) % 4)]];
        if ((word & 1) && byte == 0) result[word * 4 + byte] ^= u8(rcon);
    }
    std::memcpy(output, result.data(), 16);
}

void PolynomialMultiply(void* output, u64 left, u64 right) {
    unsigned __int128 result{};
    for (u32 bit = 0; bit < 64; ++bit)
        if ((right >> bit) & 1) result ^= static_cast<unsigned __int128>(left) << bit;
    std::memcpy(output, &result, 16);
}

u64 RoundIntegral(u64 raw, u32 bits, u32 mode) {
    const u32 fraction_bits = bits == 32 ? 23 : 52;
    const u32 bias = bits == 32 ? 127 : 1023;
    const u64 sign = u64{1} << (bits - 1);
    const u64 exponent_mask = bits == 32 ? 0x7f800000 : 0x7ff0000000000000ULL;
    const u64 fraction_mask = (u64{1} << fraction_bits) - 1;
    if (bits == 32) raw &= UINT32_MAX;
    const u64 magnitude = raw & ~sign;
    const bool negative = (raw & sign) != 0;
    if ((raw & exponent_mask) == exponent_mask)
        return raw | ((raw & fraction_mask) ? u64{1} << (fraction_bits - 1) : 0);
    const s32 exponent = s32((raw & exponent_mask) >> fraction_bits) - s32(bias);
    if (exponent >= s32(fraction_bits) || !magnitude) return raw;
    if (exponent < 0) {
        const u64 half = u64(bias - 1) << fraction_bits;
        const bool increase = mode == 0 ? magnitude > half : mode == 1 ? negative : mode == 2 ? !negative : false;
        return (raw & sign) | (increase ? u64(bias) << fraction_bits : 0);
    }
    const u32 discarded_bits = fraction_bits - exponent;
    const u64 unit = u64{1} << discarded_bits;
    const u64 discarded = raw & (unit - 1);
    const u64 truncated = raw & ~(unit - 1);
    const bool increase = mode == 0 ? discarded > unit / 2 || (discarded == unit / 2 && (truncated & unit)) :
                          mode == 1 ? negative && discarded : mode == 2 ? !negative && discarded : false;
    return truncated + (increase ? unit : 0);
}

u64 FusedMultiplyAdd(u64 a, u64 b, u64 c, u32 bits, u32 flags) {
    const u64 exponent = bits == 32 ? 0x7f800000 : 0x7ff0000000000000ULL;
    const u64 fraction = bits == 32 ? 0x007fffff : 0x000fffffffffffffULL;
    const u64 quiet = bits == 32 ? 0x00400000 : 0x0008000000000000ULL;
    const u64 sign = u64{1} << (bits - 1);
    const auto nan = [&](u64 value) { return (value & exponent) == exponent && (value & fraction); };
    for (auto value : {a, b, c}) if (nan(value)) return value | quiet;
    if (flags & 1) a ^= sign;
    if (flags & 2) c ^= sign;
    softfloat_state state{softfloat_tininess_afterRounding, softfloat_round_odd, 0, 80};
    const auto widen = [&](u64 value) {
        return bits == 32 ? f32_to_f128(&state, float32_t{u32(value)}) :
                           extF80_to_f128(&state, f64_to_extF80(&state, float64_t{value}));
    };
    // The product of two doubles has <=106 significant bits and fits f128's
    // exponent range exactly. Jamming the wide addition to odd preserves the
    // final nearest-even rounding, including halfway/subnormal cases.
    const auto product = f128_mul(&state, widen(a), widen(b));
    const auto sum = f128_add(&state, product, widen(c));
    state.roundingMode = softfloat_round_near_even;
    const u64 result = bits == 32 ? f128_to_f32(&state, sum).v : f128_to_f64(&state, sum).v;
    return nan(result) ? exponent | quiet | sign : result;
}

u64 Condition(u64 flags, ir::Cond condition) {
    const bool n = (flags >> 31) & 1, z = (flags >> 30) & 1;
    const bool c = (flags >> 29) & 1, v = (flags >> 28) & 1;
    switch (condition) {
        case ir::Cond::EQ: return z;
        case ir::Cond::NE: return !z;
        case ir::Cond::CS: return c;
        case ir::Cond::CC: return !c;
        case ir::Cond::MI: return n;
        case ir::Cond::PL: return !n;
        case ir::Cond::VS: return v;
        case ir::Cond::VC: return !v;
        case ir::Cond::HI: return c && !z;
        case ir::Cond::LS: return !c || z;
        case ir::Cond::GE: return n == v;
        case ir::Cond::LT: return n != v;
        case ir::Cond::GT: return !z && n == v;
        case ir::Cond::LE: return z || n != v;
        case ir::Cond::AL: case ir::Cond::NV: return 1;
    }
    return 0;
}

namespace {
u64 Mask(ir::Flags requested) {
    u64 mask{};
    if (True(requested & ir::Flags::Negate)) mask |= u64{1} << 31;
    if (True(requested & ir::Flags::Zero)) mask |= u64{1} << 30;
    if (True(requested & ir::Flags::Carry)) mask |= u64{1} << 29;
    if (True(requested & ir::Flags::Overflow)) mask |= u64{1} << 28;
    if (True(requested & ir::Flags::AuxiliaryCarry)) mask |= u64{1} << 26;
    return mask;
}
}

u64 TestFlags(u64 flags, ir::Flags requested) {
    const auto nzcv = Mask(requested & ir::Flags::NZCV);
    bool result = nzcv && (flags & nzcv);
    bool first = !nzcv;
    if (True(requested & ir::Flags::Parity)) {
        const bool parity = !(std::popcount(static_cast<u8>(flags)) & 1);
        result = first ? parity : result && parity;
        first = false;
    }
    if (True(requested & ir::Flags::AuxiliaryCarry)) {
        const bool auxiliary = (flags >> 26) & 1;
        result = first ? auxiliary : result && auxiliary;
    }
    return result;
}

void SaveFlags(State* state, u64 result, u64 native_flags, ir::Flags requested,
               ir::OpCode producer, u32 bits) {
    using O = ir::OpCode;
    u64 mask = Mask(requested & ir::Flags::NZ);
    u64 value = ((result >> (bits - 1)) & 1) << 31;
    value |= u64{result == 0} << 30;
    const bool subtract = producer == O::Sub || producer == O::Sbb || producer == O::Neg;
    const bool arithmetic = subtract || producer == O::Add || producer == O::Adc;
    const bool logical = producer == O::And || producer == O::Or || producer == O::Xor || producer == O::AndNot;
    const bool multiply = producer == O::Mul && bits < 64;
    if (arithmetic || logical || multiply) {
        mask |= Mask(requested & ir::Flags::CV);
        if (arithmetic || multiply) {
            value |= u64{bool(native_flags & 1) != subtract} << 29;
            value |= ((native_flags >> 11) & 1) << 28;
            if (arithmetic) {
                mask |= Mask(requested & ir::Flags::AuxiliaryCarry);
                value |= ((native_flags >> 4) & 1) << 26;
            }
        }
    }
    if (True(requested & ir::Flags::Parity)) { mask |= 255; value |= result & 255; }
    state->host_cpu_flags = (state->host_cpu_flags & ~mask) | (value & mask);
}

void ClearFlags(State* state, ir::Flags requested) {
    auto mask = Mask(requested);
    if (True(requested & ir::Flags::Parity)) mask |= 255;
    state->host_cpu_flags &= ~mask;
    if (True(requested & ir::Flags::Parity)) state->host_cpu_flags |= 1;
}

bool CheckMemory(State* state, u64 address, u64 size) {
    if (!size || address > state->guest_addr_mask || size - 1 > state->guest_addr_mask - address ||
        address >= state->guest_addr_limit || size > state->guest_addr_limit - address) {
        state->halt_reason = HaltReason::PageFatal;
        return false;
    }
    try {
        if (state->interp_range_check && !state->interp_range_check(state->interp_range_check_ctx, address, size))
            state->halt_reason = HaltReason::PageFatal;
    } catch (...) {
        state->halt_reason = HaltReason::IllegalCode;
    }
    return state->halt_reason == HaltReason::None;
}

void BeginMemory(State* state) {
    for (;;) {
        riscv64::anonymous_memory_readers.fetch_add(1, std::memory_order_seq_cst);
        if (!unaligned_atomic_lock.load(std::memory_order_seq_cst)) break;
        riscv64::anonymous_memory_readers.fetch_sub(1, std::memory_order_seq_cst);
        while (unaligned_atomic_lock.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    state->spill_area[kRiscvMemoryOwnedSlot] = 1;
}

bool CheckAndBeginMemory(State* state, u64 address, u64 size) {
    if (!CheckMemory(state, address, size)) return false;
    BeginMemory(state);
    return true;
}

void EndMemory(State* state) {
    if (state->spill_area[kRiscvMemoryOwnedSlot]) {
        state->spill_area[kRiscvMemoryOwnedSlot] = 0;
        riscv64::anonymous_memory_readers.fetch_sub(1, std::memory_order_seq_cst);
    }
}

u64 SignedDivide(u64 left, u64 right, u32 bits) {
    const auto extend = [bits](u64 value) -> s64 {
        if (bits == 64) return static_cast<s64>(value);
        const auto sign = u64{1} << (bits - 1);
        return static_cast<s64>((value ^ sign) - sign);
    };
    const auto l = extend(left), r = extend(right);
    if (!r) return 0;
    if (l == std::numeric_limits<s64>::min() && r == -1) return left;
    return l / r;
}

u64 UnsignedDivide(u64 left, u64 right) { return right ? left / right : 0; }

u64 CountBits(u64 value) { return std::popcount(value); }

u64 CompareCondition(u64 packed, ir::Cond condition) {
    const bool less = (packed & 7) == 1;
    const bool equal = (packed & 7) == 4;
    const bool unordered = (packed & 7) == 7;
    return Condition((u64{less} << 31) | (u64{equal} << 30) |
                     (u64{!less} << 29) | (u64{unordered} << 28), condition);
}

void PublishCompareFlags(State* state, u64 packed, u64 compact) {
    const auto carry = (packed & 1) ^ !!compact;
    constexpr u64 mask = (u64{15} << 28) | (u64{1} << 26) | 255;
    const auto value = (carry << 29) | (((packed >> 2) & 1) << 30) | (((packed >> 1) & 1) ^ 1);
    state->host_cpu_flags = (state->host_cpu_flags & ~mask) | value;
}

}  // namespace swift::runtime::backend::x64
