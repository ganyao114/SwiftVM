#include "interpreter.h"

#include <algorithm>
#include <limits>

namespace swift::runtime::backend::interp {

using ir::ValueType;

#include "interpreter_internal.h"

namespace {

template <typename Op>
unsigned __int128 Vec4Binary(unsigned __int128 a, unsigned __int128 b, Op op) {
    unsigned __int128 result = 0;
    for (int i = 0; i < 4; ++i) {
        const u32 la = static_cast<u32>(a >> (i * 32));
        const u32 lb = static_cast<u32>(b >> (i * 32));
        result |= static_cast<unsigned __int128>(op(la, lb)) << (i * 32);
    }
    return result;
}

template <typename Op>
unsigned __int128 VecLaneBinary(unsigned __int128 a, unsigned __int128 b, u32 lane_bits, Op op) {
    ASSERT(lane_bits == 8 || lane_bits == 16 || lane_bits == 32 || lane_bits == 64);
    const u64 lane_mask = MaskBits(lane_bits);
    unsigned __int128 result = 0;
    for (u32 bit = 0; bit < 128; bit += lane_bits) {
        const u64 left = static_cast<u64>(a >> bit) & lane_mask;
        const u64 right = static_cast<u64>(b >> bit) & lane_mask;
        result |= static_cast<unsigned __int128>(op(left, right) & lane_mask) << bit;
    }
    return result;
}

s64 SignedLane(u64 value, u32 lane_bits) {
    if (lane_bits == 64) {
        return static_cast<s64>(value);
    }
    return static_cast<s64>((value ^ (u64(1) << (lane_bits - 1))) - (u64(1) << (lane_bits - 1)));
}

}  // namespace

void Interpreter::RunVec4Add(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             Vec4Binary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                        ReadVec(stack, inst->GetArg<ir::Value>(1)),
                        [](u32 a, u32 b) { return a + b; }));
}

void Interpreter::RunVec4Sub(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             Vec4Binary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                        ReadVec(stack, inst->GetArg<ir::Value>(1)),
                        [](u32 a, u32 b) { return a - b; }));
}

void Interpreter::RunVec4Mul(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             Vec4Binary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                        ReadVec(stack, inst->GetArg<ir::Value>(1)),
                        [](u32 a, u32 b) { return a * b; }));
}

void Interpreter::RunVec4And(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             ReadVec(stack, inst->GetArg<ir::Value>(0)) &
                     ReadVec(stack, inst->GetArg<ir::Value>(1)));
}

void Interpreter::RunVec4Or(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             ReadVec(stack, inst->GetArg<ir::Value>(0)) |
                     ReadVec(stack, inst->GetArg<ir::Value>(1)));
}

void Interpreter::RunVecXor(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             ReadVec(stack, inst->GetArg<ir::Value>(0)) ^
                     ReadVec(stack, inst->GetArg<ir::Value>(1)));
}

void Interpreter::RunVecAnd(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             ReadVec(stack, inst->GetArg<ir::Value>(0)) &
                     ReadVec(stack, inst->GetArg<ir::Value>(1)));
}

void Interpreter::RunVecOr(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             ReadVec(stack, inst->GetArg<ir::Value>(0)) |
                     ReadVec(stack, inst->GetArg<ir::Value>(1)));
}

void Interpreter::RunVecAndNot(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             ReadVec(stack, inst->GetArg<ir::Value>(0)) &
                     ~ReadVec(stack, inst->GetArg<ir::Value>(1)));
}

void Interpreter::RunVecAdd(ir::Inst* inst, InterpStack& stack) {
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    WriteVec(stack,
             inst,
             VecLaneBinary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                           ReadVec(stack, inst->GetArg<ir::Value>(1)),
                           lane_bits,
                           [](u64 a, u64 b) { return a + b; }));
}

void Interpreter::RunVecSub(ir::Inst* inst, InterpStack& stack) {
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    WriteVec(stack,
             inst,
             VecLaneBinary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                           ReadVec(stack, inst->GetArg<ir::Value>(1)),
                           lane_bits,
                           [](u64 a, u64 b) { return a - b; }));
}

void Interpreter::RunVecCmpEq(ir::Inst* inst, InterpStack& stack) {
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    WriteVec(stack,
             inst,
             VecLaneBinary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                           ReadVec(stack, inst->GetArg<ir::Value>(1)),
                           lane_bits,
                           [](u64 a, u64 b) { return a == b ? ~u64(0) : 0; }));
}

void Interpreter::RunVecCmpGt(ir::Inst* inst, InterpStack& stack) {
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    WriteVec(stack,
             inst,
             VecLaneBinary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                           ReadVec(stack, inst->GetArg<ir::Value>(1)),
                           lane_bits,
                           [lane_bits](u64 a, u64 b) {
                               return SignedLane(a, lane_bits) > SignedLane(b, lane_bits) ? ~u64(0)
                                                                                          : 0;
                           }));
}

void Interpreter::RunVecAvg(ir::Inst* inst, InterpStack& stack) {
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    ASSERT(lane_bits == 8 || lane_bits == 16);
    WriteVec(stack,
             inst,
             VecLaneBinary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                           ReadVec(stack, inst->GetArg<ir::Value>(1)),
                           lane_bits,
                           [](u64 a, u64 b) { return (a + b + 1) >> 1; }));
}

void Interpreter::RunVecMin(ir::Inst* inst, InterpStack& stack) {
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool is_signed = inst->GetArg<ir::Imm>(3).Get() != 0;
    WriteVec(stack,
             inst,
             VecLaneBinary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                           ReadVec(stack, inst->GetArg<ir::Value>(1)),
                           lane_bits,
                           [lane_bits, is_signed](u64 a, u64 b) {
                               if (is_signed) {
                                   return SignedLane(a, lane_bits) < SignedLane(b, lane_bits) ? a
                                                                                              : b;
                               }
                               return std::min(a, b);
                           }));
}

void Interpreter::RunVecMax(ir::Inst* inst, InterpStack& stack) {
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool is_signed = inst->GetArg<ir::Imm>(3).Get() != 0;
    WriteVec(stack,
             inst,
             VecLaneBinary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                           ReadVec(stack, inst->GetArg<ir::Value>(1)),
                           lane_bits,
                           [lane_bits, is_signed](u64 a, u64 b) {
                               if (is_signed) {
                                   return SignedLane(a, lane_bits) > SignedLane(b, lane_bits) ? a
                                                                                              : b;
                               }
                               return std::max(a, b);
                           }));
}

void Interpreter::RunVecMul(ir::Inst* inst, InterpStack& stack) {
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    WriteVec(stack,
             inst,
             VecLaneBinary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                           ReadVec(stack, inst->GetArg<ir::Value>(1)),
                           lane_bits,
                           [](u64 a, u64 b) { return a * b; }));
}

void Interpreter::RunVecMulHigh16(ir::Inst* inst, InterpStack& stack) {
    const bool signed_lanes = inst->GetArg<ir::Imm>(2).Get() != 0;
    WriteVec(stack,
             inst,
             VecLaneBinary(
                     ReadVec(stack, inst->GetArg<ir::Value>(0)),
                     ReadVec(stack, inst->GetArg<ir::Value>(1)),
                     16,
                     [signed_lanes](u64 a, u64 b) {
                         const u32 product = signed_lanes
                                 ? static_cast<u32>(static_cast<s32>(
                                           static_cast<s16>(a) * static_cast<s16>(b)))
                                 : static_cast<u32>(static_cast<u16>(a)) *
                                           static_cast<u32>(static_cast<u16>(b));
                         return static_cast<u64>(product >> 16);
                     }));
}

// Widening multiply of the even lanes; mirrors EmitVecMulWiden.  Source lane 2i
// sits at bit 2i*src_bits, which is bit i*dst_bits -- the same offset as
// destination lane i -- so one loop over the destination lanes reads both.
//
// The signed product cannot overflow the destination: two values in
// [-2^(n-1), 2^(n-1)) multiply into [-2^(2n-2), 2^(2n-2)], which fits a
// 2n-bit signed lane, so the s64 arithmetic below is exact for src_bits <= 32.
void Interpreter::RunVecMulWiden(ir::Inst* inst, InterpStack& stack) {
    const u32 src_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool is_signed = inst->GetArg<ir::Imm>(3).Get() != 0;
    ASSERT(src_bits == 8 || src_bits == 16 || src_bits == 32);
    const u32 dst_bits = src_bits * 2;
    const u64 src_mask = MaskBits(src_bits);
    const u64 dst_mask = MaskBits(dst_bits);
    const u128 a = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u128 b = ReadVec(stack, inst->GetArg<ir::Value>(1));
    u128 result = 0;
    for (u32 bit = 0; bit < 128; bit += dst_bits) {
        const u64 left = static_cast<u64>(a >> bit) & src_mask;
        const u64 right = static_cast<u64>(b >> bit) & src_mask;
        const u64 product =
                is_signed ? static_cast<u64>(SignedLane(left, src_bits) *
                                             SignedLane(right, src_bits))
                          : left * right;
        result |= static_cast<u128>(product & dst_mask) << bit;
    }
    WriteVec(stack, inst, result);
}


void Interpreter::RunVecSatAdd(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool signed_lanes = inst->GetArg<ir::Imm>(3).Get() != 0;
    const u64 mask = (u64(1) << bits) - 1;
    WriteVec(stack,
             inst,
             VecLaneBinary(
                     ReadVec(stack, inst->GetArg<ir::Value>(0)),
                     ReadVec(stack, inst->GetArg<ir::Value>(1)),
                     bits,
                     [bits, signed_lanes, mask](u64 a, u64 b) {
                         if (!signed_lanes) return std::min<u64>(a + b, mask);
                         const s64 minimum = -(s64(1) << (bits - 1));
                         const s64 maximum = (s64(1) << (bits - 1)) - 1;
                         return static_cast<u64>(
                                        std::clamp(SignedLane(a, bits) + SignedLane(b, bits),
                                                   minimum,
                                                   maximum)) &
                                mask;
                     }));
}

void Interpreter::RunVecSatSub(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = inst->GetArg<ir::Imm>(2).Get();
    const bool signed_lanes = inst->GetArg<ir::Imm>(3).Get() != 0;
    const u64 mask = (u64(1) << bits) - 1;
    WriteVec(stack,
             inst,
             VecLaneBinary(
                     ReadVec(stack, inst->GetArg<ir::Value>(0)),
                     ReadVec(stack, inst->GetArg<ir::Value>(1)),
                     bits,
                     [bits, signed_lanes, mask](u64 a, u64 b) {
                         if (!signed_lanes) return a < b ? u64(0) : a - b;
                         const s64 minimum = -(s64(1) << (bits - 1));
                         const s64 maximum = (s64(1) << (bits - 1)) - 1;
                         return static_cast<u64>(
                                        std::clamp(SignedLane(a, bits) - SignedLane(b, bits),
                                                   minimum,
                                                   maximum)) &
                                mask;
                     }));
}

void Interpreter::RunVecPack(ir::Inst* inst, InterpStack& stack) {
    const u32 source_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool unsigned_destination = inst->GetArg<ir::Imm>(3).Get() != 0;
    const u32 destination_bits = source_bits / 2;
    const u32 lanes_per_source = 128 / source_bits;
    const u64 source_mask = (u64(1) << source_bits) - 1;
    const s64 destination_minimum =
            unsigned_destination ? 0 : -(s64(1) << (destination_bits - 1));
    const s64 destination_maximum = unsigned_destination
            ? (s64(1) << destination_bits) - 1
            : (s64(1) << (destination_bits - 1)) - 1;
    const u128 sources[] = {ReadVec(stack, inst->GetArg<ir::Value>(0)),
                            ReadVec(stack, inst->GetArg<ir::Value>(1))};
    u128 result = 0;
    for (u32 source = 0; source < 2; ++source) {
        for (u32 lane = 0; lane < lanes_per_source; ++lane) {
            const u64 raw =
                    static_cast<u64>(sources[source] >> (lane * source_bits)) & source_mask;
            const s64 narrowed = std::clamp(
                    SignedLane(raw, source_bits), destination_minimum, destination_maximum);
            const u64 encoded = static_cast<u64>(narrowed) &
                                ((u64(1) << destination_bits) - 1);
            const u32 output_lane = source * lanes_per_source + lane;
            result |= static_cast<u128>(encoded) << (output_lane * destination_bits);
        }
    }
    WriteVec(stack, inst, result);
}

void Interpreter::RunVecAbsDiffSum8(ir::Inst* inst, InterpStack& stack) {
    const auto left = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto right = ReadVec(stack, inst->GetArg<ir::Value>(1));
    u128 result = 0;
    for (u32 half = 0; half < 2; ++half) {
        u64 sum = 0;
        for (u32 byte = 0; byte < 8; ++byte) {
            const u32 bit = half * 64 + byte * 8;
            const int a = static_cast<u8>(left >> bit);
            const int b = static_cast<u8>(right >> bit);
            sum += static_cast<u64>(a > b ? a - b : b - a);
        }
        result |= static_cast<u128>(sum) << (half * 64);
    }
    WriteVec(stack, inst, result);
}

void Interpreter::RunVecMadd16(ir::Inst* inst, InterpStack& stack) {
    const auto left = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto right = ReadVec(stack, inst->GetArg<ir::Value>(1));
    u128 result = 0;
    for (u32 lane = 0; lane < 4; ++lane) {
        const u32 bit = lane * 32;
        const s32 a0 = static_cast<s16>(left >> bit);
        const s32 a1 = static_cast<s16>(left >> (bit + 16));
        const s32 b0 = static_cast<s16>(right >> bit);
        const s32 b1 = static_cast<s16>(right >> (bit + 16));
        const u32 sum = static_cast<u32>(static_cast<s64>(a0) * b0 + static_cast<s64>(a1) * b1);
        result |= static_cast<u128>(sum) << bit;
    }
    WriteVec(stack, inst, result);
}

void Interpreter::RunVecShiftLeft(ir::Inst* inst, InterpStack& stack) {
    const auto value = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u64 count = ReadScalar(stack, inst->GetArg<ir::Value>(1));
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    WriteVec(stack, inst, VecLaneBinary(value, 0, lane_bits, [count, lane_bits](u64 lane, u64) {
                 return count >= lane_bits ? 0 : lane << count;
             }));
}

void Interpreter::RunVecShiftRight(ir::Inst* inst, InterpStack& stack) {
    const auto value = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u64 count = ReadScalar(stack, inst->GetArg<ir::Value>(1));
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    WriteVec(stack, inst, VecLaneBinary(value, 0, lane_bits, [count, lane_bits](u64 lane, u64) {
                 return count >= lane_bits ? 0 : lane >> count;
             }));
}

void Interpreter::RunVecShiftRightArithmetic(ir::Inst* inst, InterpStack& stack) {
    const auto value = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u64 count = ReadScalar(stack, inst->GetArg<ir::Value>(1));
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 clamped = static_cast<u32>(std::min(count, u64(lane_bits - 1)));
    WriteVec(stack, inst, VecLaneBinary(value, 0, lane_bits, [clamped, lane_bits](u64 lane, u64) {
                 return static_cast<u64>(SignedLane(lane, lane_bits) >> clamped);
             }));
}

void Interpreter::RunVecShiftLeftImm(ir::Inst* inst, InterpStack& stack) {
    const auto value = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u64 count = inst->GetArg<ir::Imm>(1).Get();
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    WriteVec(stack, inst, VecLaneBinary(value, 0, lane_bits, [count, lane_bits](u64 lane, u64) {
                 return count >= lane_bits ? 0 : lane << count;
             }));
}

void Interpreter::RunVecShiftRightImm(ir::Inst* inst, InterpStack& stack) {
    const auto value = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u64 count = inst->GetArg<ir::Imm>(1).Get();
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    WriteVec(stack, inst, VecLaneBinary(value, 0, lane_bits, [count, lane_bits](u64 lane, u64) {
                 return count >= lane_bits ? 0 : lane >> count;
             }));
}

void Interpreter::RunVecShiftRightArithmeticImm(ir::Inst* inst, InterpStack& stack) {
    const auto value = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u64 raw_count = inst->GetArg<ir::Imm>(1).Get();
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    const u32 count = static_cast<u32>(std::min(raw_count, u64(lane_bits - 1)));
    WriteVec(stack, inst, VecLaneBinary(value, 0, lane_bits, [count, lane_bits](u64 lane, u64) {
                 return static_cast<u64>(SignedLane(lane, lane_bits) >> count);
             }));
}


void Interpreter::RunVecByteShift(ir::Inst* inst, InterpStack& stack) {
    const auto value = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u32 count = inst->GetArg<ir::Imm>(2).Get();
    const bool left = inst->GetArg<ir::Imm>(3).Get() != 0;
    ASSERT(count > 0 && count < 16);
    WriteVec(stack, inst, left ? value << (count * 8) : value >> (count * 8));
}

void Interpreter::RunVecShuffle32(ir::Inst* inst, InterpStack& stack) {
    const auto src = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u32 control = inst->GetArg<ir::Imm>(1).Get();
    u128 result = 0;
    for (u32 lane = 0; lane < 4; ++lane) {
        const u32 selected = (control >> (lane * 2)) & 3;
        result |= static_cast<u128>(static_cast<u32>(src >> (selected * 32))) << (lane * 32);
    }
    WriteVec(stack, inst, result);
}


void Interpreter::RunVecShuffle32TwoSrc(ir::Inst* inst, InterpStack& stack) {
    const auto left = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto right = ReadVec(stack, inst->GetArg<ir::Value>(1));
    const u32 control = inst->GetArg<ir::Imm>(2).Get();
    u128 result = 0;
    for (u32 lane = 0; lane < 4; ++lane) {
        const u32 selected = (control >> (lane * 2)) & 3;
        const auto source = lane < 2 ? left : right;
        result |= static_cast<u128>(static_cast<u32>(source >> (selected * 32)))
                  << (lane * 32);
    }
    WriteVec(stack, inst, result);
}

void Interpreter::RunVecLoadConst(ir::Inst* inst, InterpStack& stack) {
    const u64 low = inst->GetArg<ir::Imm>(0).Get();
    const u64 high = inst->GetArg<ir::Imm>(1).Get();
    WriteVec(stack, inst, static_cast<u128>(low) | (static_cast<u128>(high) << 64));
}

void Interpreter::RunVecShuffle32Indexed(ir::Inst* inst, InterpStack& stack) {
    const auto src = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto indexes = ReadVec(stack, inst->GetArg<ir::Value>(1));
    u128 result = 0;
    for (u32 byte = 0; byte < 16; ++byte) {
        const u8 index = static_cast<u8>(indexes >> (byte * 8));
        if (index < 16) {
            result |= static_cast<u128>(static_cast<u8>(src >> (index * 8)))
                      << (byte * 8);
        }
    }
    WriteVec(stack, inst, result);
}

void Interpreter::RunVecSharedZero(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack, inst, 0);
}

void Interpreter::RunVecShuffle16(ir::Inst* inst, InterpStack& stack) {
    const auto src = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u32 control = inst->GetArg<ir::Imm>(1).Get();
    const bool high = inst->GetArg<ir::Imm>(2).Get() != 0;
    const u32 base = high ? 4 : 0;
    u128 result = src;
    const u128 half_mask = static_cast<u128>(~u64(0)) << (base * 16);
    result &= ~half_mask;
    for (u32 lane = 0; lane < 4; ++lane) {
        const u32 selected = base + ((control >> (lane * 2)) & 3);
        const u16 value = static_cast<u16>(src >> (selected * 16));
        result |= static_cast<u128>(value) << ((base + lane) * 16);
    }
    WriteVec(stack, inst, result);
}

void Interpreter::RunVecZip(ir::Inst* inst, InterpStack& stack) {
    const auto left = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto right = ReadVec(stack, inst->GetArg<ir::Value>(1));
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool high = inst->GetArg<ir::Imm>(3).Get() != 0;
    ASSERT(lane_bits == 8 || lane_bits == 16 || lane_bits == 32 || lane_bits == 64);
    const u32 half_lanes = 64 / lane_bits;
    const u32 source_base = high ? half_lanes : 0;
    const u64 lane_mask = MaskBits(lane_bits);
    u128 result = 0;
    for (u32 lane = 0; lane < half_lanes; ++lane) {
        const u32 source_bit = (source_base + lane) * lane_bits;
        const u64 a = static_cast<u64>(left >> source_bit) & lane_mask;
        const u64 b = static_cast<u64>(right >> source_bit) & lane_mask;
        result |= static_cast<u128>(a) << ((lane * 2) * lane_bits);
        result |= static_cast<u128>(b) << ((lane * 2 + 1) * lane_bits);
    }
    WriteVec(stack, inst, result);
}

void Interpreter::RunVecDupPairs32(ir::Inst* inst, InterpStack& stack) {
    const auto src = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u32 odd = inst->GetArg<ir::Imm>(1).Get() ? 1 : 0;
    u128 result = 0;
    for (u32 pair = 0; pair < 2; ++pair) {
        const u32 value = static_cast<u32>(src >> ((pair * 2 + odd) * 32));
        result |= static_cast<u128>(value) << (pair * 64);
        result |= static_cast<u128>(value) << (pair * 64 + 32);
    }
    WriteVec(stack, inst, result);
}

void Interpreter::RunVecDup64(ir::Inst* inst, InterpStack& stack) {
    const u64 src = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    WriteVec(stack, inst, static_cast<u128>(src) | (static_cast<u128>(src) << 64));
}

void Interpreter::RunVecExtract64(ir::Inst* inst, InterpStack& stack) {
    const auto src = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u32 lane = inst->GetArg<ir::Imm>(1).Get() & 1;
    WriteScalar(stack, inst, static_cast<u64>(src >> (lane * 64)));
}

void Interpreter::RunVecExtract16(ir::Inst* inst, InterpStack& stack) {
    const auto src = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u32 lane = inst->GetArg<ir::Imm>(1).Get() & 7;
    WriteScalar(stack, inst, static_cast<u16>(src >> (lane * 16)));
}

void Interpreter::RunVecInsert16(ir::Inst* inst, InterpStack& stack) {
    const auto dest = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u16 value = static_cast<u16>(ReadScalar(stack, inst->GetArg<ir::Value>(1)));
    const u32 lane = inst->GetArg<ir::Imm>(2).Get() & 7;
    const u128 mask = static_cast<u128>(0xFFFF) << (lane * 16);
    WriteVec(stack, inst, (dest & ~mask) | (static_cast<u128>(value) << (lane * 16)));
}

void Interpreter::RunVecMovMask(ir::Inst* inst, InterpStack& stack) {
    const auto src = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u32 lane_bits = inst->GetArg<ir::Imm>(1).Get();
    ASSERT(lane_bits == 8 || lane_bits == 32 || lane_bits == 64);
    u64 result = 0;
    for (u32 lane = 0; lane < 128 / lane_bits; ++lane) {
        result |= static_cast<u64>((src >> (lane * lane_bits + lane_bits - 1)) & 1) << lane;
    }
    WriteScalar(stack, inst, result);
}

void Interpreter::RunVecTableLookup8(ir::Inst* inst, InterpStack& stack) {
    const auto table = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto control = ReadVec(stack, inst->GetArg<ir::Value>(1));
    u128 result = 0;
    for (u32 byte = 0; byte < 16; ++byte) {
        const u8 index = static_cast<u8>(control >> (byte * 8));
        if ((index & 0x80) == 0) {
            result |= static_cast<u128>(static_cast<u8>(table >> ((index & 0x0F) * 8)))
                      << (byte * 8);
        }
    }
    WriteVec(stack, inst, result);
}

// De-interleave, the dual of RunVecZip: keep every other lane of {left, right},
// starting at lane 0 (UZP1) or lane 1 (UZP2). Mirrors EmitVecUnzip.
void Interpreter::RunVecUnzip(ir::Inst* inst, InterpStack& stack) {
    const auto left = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const auto right = ReadVec(stack, inst->GetArg<ir::Value>(1));
    const u32 lane_bits = inst->GetArg<ir::Imm>(2).Get();
    const bool odd = inst->GetArg<ir::Imm>(3).Get() != 0;
    ASSERT(lane_bits == 8 || lane_bits == 16 || lane_bits == 32 || lane_bits == 64);
    const u32 half_lanes = 64 / lane_bits;
    const u32 first = odd ? 1 : 0;
    const u64 lane_mask = MaskBits(lane_bits);
    u128 result = 0;
    for (u32 lane = 0; lane < half_lanes; ++lane) {
        const u32 source_bit = (lane * 2 + first) * lane_bits;
        const u64 a = static_cast<u64>(left >> source_bit) & lane_mask;
        const u64 b = static_cast<u64>(right >> source_bit) & lane_mask;
        result |= static_cast<u128>(a) << (lane * lane_bits);
        result |= static_cast<u128>(b) << ((half_lanes + lane) * lane_bits);
    }
    WriteVec(stack, inst, result);
}


}  // namespace swift::runtime::backend::interp
