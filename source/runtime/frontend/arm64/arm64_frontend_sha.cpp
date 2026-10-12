#include <array>
#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

void A64Decoder::VisitCrypto2RegSHA(const Instruction* instr) {
    const u32 op = instr->GetInstructionBits() & 0xFFFFFC00u;
    if (op != 0x5E280800 && op != 0x5E281800 && op != 0x5E282800) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    if (op == 0x5E282800) {
        auto old = ReadVRegister(instr->GetRd(), ir::ValueType::V128);
        auto source = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
        WriteVRegister(instr->GetRd(), __ VecSha256Msg1(old, source).SetType(ir::ValueType::V128));
        return;
    }
    auto words = [&](u32 reg) {
        std::array<ir::Value, 4> result;
        auto low = ReadVHalf(reg, false), high = ReadVHalf(reg, true);
        for (u32 i = 0; i < 4; ++i) {
            auto shifted = __ LsrImm<ir::U64>(i < 2 ? low : high, ir::Imm{(i % 2) * 32}).SetType(ir::ValueType::U64);
            result[i] = __ And<ir::U32>(shifted, SingleOperand(ir::Imm{u32{0xFFFFFFFF}})).SetType(ir::ValueType::U32);
        }
        return result;
    };
    const auto source = words(instr->GetRn());
    auto rotate = [&](ir::Value word, u32 right) {
        return __ RorImm<ir::U32>(word, ir::Imm{right}).SetType(ir::ValueType::U32);
    };
    if (op == 0x5E280800) {
        WriteVHalves(instr->GetRd(), Widen(rotate(source[0], 2)), ImmValue(0, ir::ValueType::U64));
        return;
    }
    const auto old = words(instr->GetRd());
    std::array<ir::Value, 4> result;
    for (u32 i = 0; i < 4; ++i) {
        auto other = i < 3 ? source[i + 1] : result[0];
        result[i] = rotate(__ Xor<ir::U32>(old[i], SingleOperand(other)).SetType(ir::ValueType::U32), 31);
    }
    auto pair = [&](u32 i) {
        auto upper = __ LslImm<ir::U64>(Widen(result[i + 1]), ir::Imm{32u}).SetType(ir::ValueType::U64);
        return __ Or<ir::U64>(Widen(result[i]), SingleOperand(upper)).SetType(ir::ValueType::U64);
    };
    WriteVHalves(instr->GetRd(), pair(0), pair(2));
}

void A64Decoder::VisitCrypto3RegSHA(const Instruction* instr) {
    const u32 op = instr->GetInstructionBits() & 0xFFE0FC00u;
    if (op < 0x5E000000 || op > 0x5E006000 || ((op - 0x5E000000) & 0xFFF)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    auto old = ReadVRegister(instr->GetRd(), ir::ValueType::V128);
    auto source = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
    auto message = ReadVRegister(instr->GetRm(), ir::ValueType::V128);
    if (op == 0x5E006000) {
        auto addend = __ VecExtractBytes(source, message, ir::Imm{4u}).SetType(ir::ValueType::V128);
        auto partial = __ VecAdd(old, addend, ir::Imm{32u}).SetType(ir::ValueType::V128);
        WriteVRegister(instr->GetRd(), __ VecSha256Msg2(partial, message).SetType(ir::ValueType::V128));
        return;
    }
    if (op == 0x5E004000 || op == 0x5E005000) {
        auto abcd = op == 0x5E004000 ? old : source;
        auto efgh = op == 0x5E004000 ? source : old;
        auto reversed_a = __ VecShuffle32(abcd, ir::Imm{0x1Bu}).SetType(ir::ValueType::V128);
        auto reversed_e = __ VecShuffle32(efgh, ir::Imm{0x1Bu}).SetType(ir::ValueType::V128);
        auto packed_source = __ VecShuffle32TwoSrc(reversed_e, reversed_a, ir::Imm{0xEEu}).SetType(ir::ValueType::V128);
        auto packed_destination = __ VecShuffle32TwoSrc(reversed_e, reversed_a, ir::Imm{0x44u}).SetType(ir::ValueType::V128);
        auto first = __ VecSha256Rnds2(packed_destination, packed_source, message).SetType(ir::ValueType::V128);
        // After two rounds the other packed half equals the original source.
        auto second_keys = __ VecShuffle32(message, ir::Imm{0x0Eu}).SetType(ir::ValueType::V128);
        auto second = __ VecSha256Rnds2(packed_source, first, second_keys).SetType(ir::ValueType::V128);
        auto result = __ VecShuffle32TwoSrc(second, first, ir::Imm{op == 0x5E004000 ? 0xBBu : 0x11u})
                          .SetType(ir::ValueType::V128);
        WriteVRegister(instr->GetRd(), result);
        return;
    }
    auto words = [&](ir::Value vector) {
        std::array<ir::Value, 4> result;
        auto low = __ VecExtract64(vector, ir::Imm{0u}).SetType(ir::ValueType::U64);
        auto high = __ VecExtract64(vector, ir::Imm{1u}).SetType(ir::ValueType::U64);
        for (u32 i = 0; i < 4; ++i) {
            auto shifted = __ LsrImm<ir::U64>(i < 2 ? low : high, ir::Imm{(i % 2) * 32}).SetType(ir::ValueType::U64);
            result[i] = __ And<ir::U32>(shifted, SingleOperand(ir::Imm{u32{0xFFFFFFFF}})).SetType(ir::ValueType::U32);
        }
        return result;
    };
    auto d = words(old);
    const auto n = words(source), m = words(message);
    auto xor32 = [&](ir::Value a, ir::Value b) { return __ Xor<ir::U32>(a, SingleOperand(b)).SetType(ir::ValueType::U32); };
    auto and32 = [&](ir::Value a, ir::Value b) { return __ And<ir::U32>(a, SingleOperand(b)).SetType(ir::ValueType::U32); };
    auto add32 = [&](ir::Value a, ir::Value b) { return __ Add<ir::U32>(a, SingleOperand(b)).SetType(ir::ValueType::U32); };
    if (op == 0x5E003000) {
        const auto initial = d;
        for (u32 i = 0; i < 4; ++i) d[i] = xor32(xor32(initial[i], i < 2 ? initial[i + 2] : n[i - 2]), m[i]);
    } else {
        auto e = n[0];
        for (u32 round = 0; round < 4; ++round) {
            ir::Value function;
            if (op == 0x5E000000) {
                function = xor32(d[3], and32(d[1], xor32(d[2], d[3])));
            } else if (op == 0x5E001000) {
                function = xor32(xor32(d[1], d[2]), d[3]);
            } else {
                auto first = and32(d[1], d[2]);
                auto other = and32(d[3], __ Or<ir::U32>(d[1], SingleOperand(d[2])).SetType(ir::ValueType::U32));
                function = __ Or<ir::U32>(first, SingleOperand(other)).SetType(ir::ValueType::U32);
            }
            auto a = __ RorImm<ir::U32>(d[0], ir::Imm{27u}).SetType(ir::ValueType::U32);
            auto result = add32(add32(add32(a, function), e), m[round]);
            e = d[3];
            d = {result, d[0], __ RorImm<ir::U32>(d[1], ir::Imm{2u}).SetType(ir::ValueType::U32), d[2]};
        }
    }
    auto pair = [&](u32 i) {
        auto upper = __ LslImm<ir::U64>(Widen(d[i + 1]), ir::Imm{32u}).SetType(ir::ValueType::U64);
        return __ Or<ir::U64>(Widen(d[i]), SingleOperand(upper)).SetType(ir::ValueType::U64);
    };
    WriteVHalves(instr->GetRd(), pair(0), pair(2));
}

#undef __
}  // namespace swift::arm64
