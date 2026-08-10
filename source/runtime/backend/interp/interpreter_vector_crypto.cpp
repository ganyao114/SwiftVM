#include "interpreter.h"

#include <array>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace swift::runtime::backend::interp {

namespace {

u8 GfMul(u8 a, u8 b) {
    u8 out = 0;
    while (b) {
        if (b & 1) out ^= a;
        const bool high = (a & 0x80) != 0;
        a <<= 1;
        if (high) a ^= 0x1b;
        b >>= 1;
    }
    return out;
}

u8 Rotl8(u8 value, unsigned count) {
    return static_cast<u8>((value << count) | (value >> (8 - count)));
}

u8 AesSbox(u8 value) {
    u8 inverse = 0;
    if (value != 0) {
        inverse = 1;
        u8 base = value;
        unsigned exponent = 254;
        while (exponent) {
            if (exponent & 1) inverse = GfMul(inverse, base);
            base = GfMul(base, base);
            exponent >>= 1;
        }
    }
    return static_cast<u8>(inverse ^ Rotl8(inverse, 1) ^ Rotl8(inverse, 2) ^
                           Rotl8(inverse, 3) ^ Rotl8(inverse, 4) ^ 0x63);
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#endif
u8 AesInvSbox(u8 value) {
    // The interpreter is a correctness reference, not the hot path.  Keeping
    // this inverse expressed through the forward S-box avoids a second opaque
    // 256-byte table whose byte order could drift from the JIT mapping.
    for (unsigned candidate = 0; candidate != 256; ++candidate) {
        if (AesSbox(static_cast<u8>(candidate)) == value) return static_cast<u8>(candidate);
    }
    PANIC("AES S-box inverse missing");
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using AesBlock = std::array<u8, 16>;

AesBlock ToAesBlock(unsigned __int128 value) {
    AesBlock out{};
    std::memcpy(out.data(), &value, out.size());
    return out;
}

unsigned __int128 FromAesBlock(const AesBlock& value) {
    unsigned __int128 out{};
    std::memcpy(&out, value.data(), value.size());
    return out;
}

void AesShiftRows(AesBlock& value, bool inverse) {
    const AesBlock input = value;
    for (unsigned column = 0; column < 4; ++column) {
        for (unsigned row = 0; row < 4; ++row) {
            const unsigned source_column = inverse ? (column + 4 - row) % 4
                                                   : (column + row) % 4;
            value[4 * column + row] = input[4 * source_column + row];
        }
    }
}

void AesMixColumns(AesBlock& value, bool inverse) {
    for (unsigned column = 0; column < 4; ++column) {
        const unsigned off = 4 * column;
        const u8 a0 = value[off], a1 = value[off + 1], a2 = value[off + 2], a3 = value[off + 3];
        if (inverse) {
            value[off] = GfMul(a0, 14) ^ GfMul(a1, 11) ^ GfMul(a2, 13) ^ GfMul(a3, 9);
            value[off + 1] = GfMul(a0, 9) ^ GfMul(a1, 14) ^ GfMul(a2, 11) ^ GfMul(a3, 13);
            value[off + 2] = GfMul(a0, 13) ^ GfMul(a1, 9) ^ GfMul(a2, 14) ^ GfMul(a3, 11);
            value[off + 3] = GfMul(a0, 11) ^ GfMul(a1, 13) ^ GfMul(a2, 9) ^ GfMul(a3, 14);
        } else {
            value[off] = GfMul(a0, 2) ^ GfMul(a1, 3) ^ a2 ^ a3;
            value[off + 1] = a0 ^ GfMul(a1, 2) ^ GfMul(a2, 3) ^ a3;
            value[off + 2] = a0 ^ a1 ^ GfMul(a2, 2) ^ GfMul(a3, 3);
            value[off + 3] = GfMul(a0, 3) ^ a1 ^ a2 ^ GfMul(a3, 2);
        }
    }
}

}  // namespace

void Interpreter::RunVecAesEnc(ir::Inst* inst, InterpStack& stack) {
    auto data = ToAesBlock(ReadVec(stack, inst->GetArg<ir::Value>(0)));
    const auto key = ToAesBlock(ReadVec(stack, inst->GetArg<ir::Value>(1)));
    for (auto& byte : data) byte = AesSbox(byte);
    AesShiftRows(data, false);
    AesMixColumns(data, false);
    for (unsigned i = 0; i < data.size(); ++i) data[i] ^= key[i];
    WriteVec(stack, inst, FromAesBlock(data));
}

void Interpreter::RunVecAesEncLast(ir::Inst* inst, InterpStack& stack) {
    auto data = ToAesBlock(ReadVec(stack, inst->GetArg<ir::Value>(0)));
    const auto key = ToAesBlock(ReadVec(stack, inst->GetArg<ir::Value>(1)));
    for (auto& byte : data) byte = AesSbox(byte);
    AesShiftRows(data, false);
    for (unsigned i = 0; i < data.size(); ++i) data[i] ^= key[i];
    WriteVec(stack, inst, FromAesBlock(data));
}

void Interpreter::RunVecAesDec(ir::Inst* inst, InterpStack& stack) {
    auto data = ToAesBlock(ReadVec(stack, inst->GetArg<ir::Value>(0)));
    const auto key = ToAesBlock(ReadVec(stack, inst->GetArg<ir::Value>(1)));
    for (auto& byte : data) byte = AesInvSbox(byte);
    AesShiftRows(data, true);
    AesMixColumns(data, true);
    for (unsigned i = 0; i < data.size(); ++i) data[i] ^= key[i];
    WriteVec(stack, inst, FromAesBlock(data));
}

void Interpreter::RunVecAesDecLast(ir::Inst* inst, InterpStack& stack) {
    auto data = ToAesBlock(ReadVec(stack, inst->GetArg<ir::Value>(0)));
    const auto key = ToAesBlock(ReadVec(stack, inst->GetArg<ir::Value>(1)));
    for (auto& byte : data) byte = AesInvSbox(byte);
    AesShiftRows(data, true);
    for (unsigned i = 0; i < data.size(); ++i) data[i] ^= key[i];
    WriteVec(stack, inst, FromAesBlock(data));
}

void Interpreter::RunVecAesEncFast(ir::Inst* inst, InterpStack& stack) {
    RunVecAesEnc(inst, stack);
}

void Interpreter::RunVecAesEncLastFast(ir::Inst* inst, InterpStack& stack) {
    RunVecAesEncLast(inst, stack);
}

void Interpreter::RunVecAesDecFast(ir::Inst* inst, InterpStack& stack) {
    RunVecAesDec(inst, stack);
}

void Interpreter::RunVecAesDecLastFast(ir::Inst* inst, InterpStack& stack) {
    RunVecAesDecLast(inst, stack);
}

void Interpreter::RunVecAesKeygenAssist(ir::Inst* inst, InterpStack& stack) {
    const auto source = ToAesBlock(ReadVec(stack, inst->GetArg<ir::Value>(0)));
    AesBlock result{};
    const u8 rcon = static_cast<u8>(inst->GetArg<ir::Imm>(1).Get());
    auto emit_word = [&](unsigned dst_word, unsigned src_word, bool rotate) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            result[4 * dst_word + byte] = AesSbox(source[4 * src_word + ((byte + (rotate ? 1 : 0)) & 3)]);
        }
        if (rotate) result[4 * dst_word] ^= rcon;
    };
    emit_word(0, 1, false);
    emit_word(1, 1, true);
    emit_word(2, 3, false);
    emit_word(3, 3, true);
    WriteVec(stack, inst, FromAesBlock(result));
}

void Interpreter::RunVecPclMul(ir::Inst* inst, InterpStack& stack) {
    const auto left = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto right = ReadVec(stack, inst->GetArg<ir::Value>(1));
    const u32 control = inst->GetArg<ir::Imm>(2).Get();
    const u64 a = static_cast<u64>(left >> ((control & 1) ? 64 : 0));
    const u64 b = static_cast<u64>(right >> ((control & 0x10) ? 64 : 0));
    u128 result = 0;
    for (unsigned bit = 0; bit < 64; ++bit) {
        if ((b >> bit) & 1) result ^= u128(a) << bit;
    }
    WriteVec(stack, inst, result);
}

void Interpreter::RunVecSha256Msg1(ir::Inst* inst, InterpStack& stack) {
#if defined(__aarch64__)
    const auto destination = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto source = ReadVec(stack, inst->GetArg<ir::Value>(1));
    uint32x4_t dst{}, src{};
    std::memcpy(&dst, &destination, sizeof(dst));
    std::memcpy(&src, &source, sizeof(src));
    const auto result = vsha256su0q_u32(dst, src);
    u128 out{};
    std::memcpy(&out, &result, sizeof(out));
    WriteVec(stack, inst, out);
#else
    PANIC("SHA256 interpreter requires AArch64 SHA2");
#endif
}

void Interpreter::RunVecSha256Msg2(ir::Inst* inst, InterpStack& stack) {
#if defined(__aarch64__)
    const auto destination = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto source = ReadVec(stack, inst->GetArg<ir::Value>(1));
    uint32x4_t dst{}, src{};
    std::memcpy(&dst, &destination, sizeof(dst));
    std::memcpy(&src, &source, sizeof(src));
    const auto src1 = vextq_u32(dst, dst, 3);
    const auto duplicate = vdupq_n_u32(vgetq_lane_u32(dst, 3));
    const auto src2 = vreinterpretq_u32_u64(
            vzip2q_u64(vreinterpretq_u64_u32(duplicate), vreinterpretq_u64_u32(src)));
    const auto result = vsha256su1q_u32(vdupq_n_u32(0), src1, src2);
    u128 out{};
    std::memcpy(&out, &result, sizeof(out));
    WriteVec(stack, inst, out);
#else
    PANIC("SHA256 interpreter requires AArch64 SHA2");
#endif
}

void Interpreter::RunVecSha256Rnds2(ir::Inst* inst, InterpStack& stack) {
#if defined(__aarch64__)
    const auto destination = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto source = ReadVec(stack, inst->GetArg<ir::Value>(1));
    const auto xmm0 = ReadVec(stack, inst->GetArg<ir::Value>(2));
    uint32x4_t dst{}, src{}, key_source{};
    std::memcpy(&dst, &destination, sizeof(dst));
    std::memcpy(&src, &source, sizeof(src));
    std::memcpy(&key_source, &xmm0, sizeof(key_source));
    const auto abcd = vrev64q_u32(vreinterpretq_u32_u64(
            vzip2q_u64(vreinterpretq_u64_u32(src), vreinterpretq_u64_u32(dst))));
    const auto efgh = vrev64q_u32(vreinterpretq_u32_u64(
            vzip1q_u64(vreinterpretq_u64_u32(src), vreinterpretq_u64_u32(dst))));
    const auto key = vreinterpretq_u32_u64(
            vdupq_n_u64(vgetq_lane_u64(vreinterpretq_u64_u32(key_source), 0)));
    const auto a = vsha256hq_u32(abcd, efgh, key);
    const auto b = vsha256h2q_u32(efgh, abcd, key);
    const auto result = vrev64q_u32(vreinterpretq_u32_u64(
            vzip2q_u64(vreinterpretq_u64_u32(b), vreinterpretq_u64_u32(a))));
    u128 out{};
    std::memcpy(&out, &result, sizeof(out));
    WriteVec(stack, inst, out);
#else
    PANIC("SHA256 interpreter requires AArch64 SHA2");
#endif
}

}  // namespace swift::runtime::backend::interp
