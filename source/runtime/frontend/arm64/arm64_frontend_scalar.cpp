#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

void A64Decoder::VisitNEONScalar3Same(const Instruction* instr) {
    if (TryNEONVariableShift(instr, true)) return;
    if (TryNEONFPCompare(instr, false, false)) return;
    if (TryNEONExtendedFP(instr, false)) return;
    if (TryNEONSaturatingMultiplyHigh(instr, true, false, false)) return;
    const auto op = instr->Mask(NEONScalar3SameMask);
    const bool saturation = op == NEON_SQADD_scalar || op == NEON_UQADD_scalar ||
                            op == NEON_SQSUB_scalar || op == NEON_UQSUB_scalar;
    const bool compare = op == NEON_CMEQ_scalar || op == NEON_CMGT_scalar || op == NEON_CMGE_scalar ||
                         op == NEON_CMHI_scalar || op == NEON_CMHS_scalar || op == NEON_CMTST_scalar;
    const bool shift = op == NEON_SSHL_scalar || op == NEON_USHL_scalar;
    const bool arithmetic = op == NEON_ADD_scalar || op == NEON_SUB_scalar;
    if (!saturation && !compare && !shift && !arithmetic) {
        Interrupt(InterruptReason::FALLBACK, current_pc); return;
    }
    const u32 bits = 8u << instr->GetNEONSize();
    if (!saturation && bits != 64) { Interrupt(InterruptReason::ILL_CODE, current_pc); return; }
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    auto a = ReadVHalf(instr->GetRn(), false), b = ReadVHalf(instr->GetRm(), false);
    a = __ And(a, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
    b = __ And(b, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
    auto zero = ImmValue(0, ir::ValueType::U64);
    auto invert = [&](ir::Value value) {
        return __ Xor(value, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
    };
    auto unsigned_less = [&](ir::Value left, ir::Value right) {
        auto difference = __ Sub(left, SingleOperand(right)).SetType(ir::ValueType::U64);
        auto first = __ And(invert(left), SingleOperand(right)).SetType(ir::ValueType::U64);
        auto changed = __ Xor(left, SingleOperand(right)).SetType(ir::ValueType::U64);
        auto second = __ And(invert(changed), SingleOperand(difference)).SetType(ir::ValueType::U64);
        auto borrow = __ Or(first, SingleOperand(second)).SetType(ir::ValueType::U64);
        return __ TestBit(borrow, ir::Imm{bits - 1}).SetType(ir::ValueType::U8);
    };
    ir::Value result;
    if (arithmetic || saturation) {
        const bool add = op == NEON_ADD_scalar || op == NEON_SQADD_scalar || op == NEON_UQADD_scalar;
        result = (add ? __ Add(a, SingleOperand(b)) : __ Sub(a, SingleOperand(b))).SetType(ir::ValueType::U64);
        result = __ And(result, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        if (saturation) {
            const bool sign = op == NEON_SQADD_scalar || op == NEON_SQSUB_scalar;
            ir::Value overflow, limit;
            if (sign) {
                auto changed = __ Xor(a, SingleOperand(result)).SetType(ir::ValueType::U64);
                auto other = __ Xor(add ? b : a, SingleOperand(add ? result : b)).SetType(ir::ValueType::U64);
                auto crossed = __ And(changed, SingleOperand(other)).SetType(ir::ValueType::U64);
                overflow = __ TestBit(crossed, ir::Imm{bits - 1}).SetType(ir::ValueType::U8);
                auto negative = __ TestBit(a, ir::Imm{bits - 1}).SetType(ir::ValueType::U8);
                limit = __ Select(negative, ImmValue(u64{1} << (bits - 1), ir::ValueType::U64),
                                           ImmValue((u64{1} << (bits - 1)) - 1, ir::ValueType::U64)).SetType(ir::ValueType::U64);
            } else {
                overflow = add ? unsigned_less(result, a) : unsigned_less(a, b);
                limit = ImmValue(add ? mask : 0, ir::ValueType::U64);
            }
            result = __ Select(overflow, limit, result).SetType(ir::ValueType::U64);
            ir::Uniform status{u32(offsetof(ThreadContext64, fpsr)), ir::ValueType::U32};
            auto old = __ LoadUniform<ir::U32>(status);
            auto qc = __ Select(overflow, ImmValue(1u << 27, ir::ValueType::U32),
                                          ImmValue(0, ir::ValueType::U32)).SetType(ir::ValueType::U32);
            __ StoreUniform(status, __ Or(old, SingleOperand(qc)).SetType(ir::ValueType::U32));
        }
    } else if (compare) {
        ir::Value condition;
        if (op == NEON_CMEQ_scalar) {
            auto unequal = __ Xor(a, SingleOperand(b)).SetType(ir::ValueType::U64);
            condition = __ TestZero(unequal).SetType(ir::ValueType::U8);
        } else if (op == NEON_CMTST_scalar) {
            auto common = __ And(a, SingleOperand(b)).SetType(ir::ValueType::U64);
            condition = __ TestNotZero(common).SetType(ir::ValueType::U8);
        } else {
            const bool sign = op == NEON_CMGT_scalar || op == NEON_CMGE_scalar;
            const bool equal = op == NEON_CMGE_scalar || op == NEON_CMHS_scalar;
            auto left = a, right = b;
            if (sign) {
                left = __ Xor(a, SingleOperand(ir::Imm{u64{1} << 63})).SetType(ir::ValueType::U64);
                right = __ Xor(b, SingleOperand(ir::Imm{u64{1} << 63})).SetType(ir::ValueType::U64);
            }
            condition = unsigned_less(equal ? left : right, equal ? right : left);
            if (equal) condition = __ Xor(condition, SingleOperand(ir::Imm{u8{1}})).SetType(ir::ValueType::U8);
        }
        result = __ Select(condition, ImmValue(mask, ir::ValueType::U64), zero).SetType(ir::ValueType::U64);
    } else {
        auto count = SignExtendValue(b, 8);
        auto right = __ TestBit(count, ir::Imm{63u}).SetType(ir::ValueType::U8);
        auto negative = __ Sub(zero, SingleOperand(count)).SetType(ir::ValueType::U64);
        auto amount = __ Select(right, negative, count).SetType(ir::ValueType::U64);
        auto difference = __ Sub(amount, SingleOperand(ir::Imm{u64{64}})).SetType(ir::ValueType::U64);
        auto in_range = __ TestBit(difference, ir::Imm{63u}).SetType(ir::ValueType::U8);
        auto left_result = __ LslValue(a, amount).SetType(ir::ValueType::U64);
        left_result = __ Select(in_range, left_result, zero).SetType(ir::ValueType::U64);
        ir::Value right_result;
        if (op == NEON_SSHL_scalar) {
            auto clamped = __ Select(in_range, amount, ImmValue(63, ir::ValueType::U64)).SetType(ir::ValueType::U64);
            right_result = __ AsrValue(a, clamped).SetType(ir::ValueType::U64);
        } else {
            right_result = __ LsrValue(a, amount).SetType(ir::ValueType::U64);
            right_result = __ Select(in_range, right_result, zero).SetType(ir::ValueType::U64);
        }
        result = __ Select(right, right_result, left_result).SetType(ir::ValueType::U64);
    }
    WriteVHalves(instr->GetRd(), result, zero);
}
}  // namespace swift::arm64
