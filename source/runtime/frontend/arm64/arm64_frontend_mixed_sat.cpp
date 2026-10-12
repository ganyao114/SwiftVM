#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryNEONMixedSaturating(const Instruction* instr,bool scalar) {
    const u32 op = instr->GetInstructionBits() & NEON2RegMiscMask & ~u32(NEON_Q|NEONScalar);
    const bool mixed = op == NEON_SUQADD || op == NEON_USQADD;
    const bool unary = !scalar && (op == NEON_SQABS || op == NEON_SQNEG);
    if (!mixed && !unary) return false;
    const u32 bits = 8u<<instr->GetNEONSize();const bool q = instr->GetNEONQ();
    if (!scalar && bits==64 && !q) { Interrupt(InterruptReason::ILL_CODE,current_pc);return true; }
    const u64 mask = bits==64 ? ~u64{0} : (u64{1}<<bits)-1,sign = u64{1}<<(bits-1);
    const u32 active = scalar ? 1 : (q ? 128 : 64)/bits;
    auto source_low = ReadVHalf(instr->GetRn(),false),source_high = ReadVHalf(instr->GetRn(),true);
    auto old_low = mixed ? ReadVHalf(instr->GetRd(),false) : ImmValue(0,ir::ValueType::U64);
    auto old_high = mixed ? ReadVHalf(instr->GetRd(),true) : old_low;
    auto zero = ImmValue(0,ir::ValueType::U64),low = zero,high = zero;
    auto any_saturation = ImmValue(0,ir::ValueType::U8);
    auto invert = [&](ir::Value value) { return __ Xor(value,SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64); };
    auto unsigned_less = [&](ir::Value a,ir::Value b) {
        auto difference = __ Sub(a,SingleOperand(b)).SetType(ir::ValueType::U64);
        auto first = __ And(invert(a),SingleOperand(b)).SetType(ir::ValueType::U64);
        auto changed = __ Xor(a,SingleOperand(b)).SetType(ir::ValueType::U64);
        auto second = __ And(invert(changed),SingleOperand(difference)).SetType(ir::ValueType::U64);
        auto borrow = __ Or(first,SingleOperand(second)).SetType(ir::ValueType::U64);
        return __ TestBit(borrow,ir::Imm{bits-1}).SetType(ir::ValueType::U8);
    };
    for (u32 lane=0;lane<active;++lane) {
        const u32 position = lane*bits;
        auto source = __ LsrImm(position<64 ? source_low : source_high,ir::Imm{position%64}).SetType(ir::ValueType::U64);
        source = __ And(source,SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        ir::Value result,saturated;
        if (mixed) {
            auto old = __ LsrImm(position<64 ? old_low : old_high,ir::Imm{position%64}).SetType(ir::ValueType::U64);
            old = __ And(old,SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
            // Bias the accumulator into the source's signedness. The mixed
            // operation then becomes ordinary same-signedness saturating add.
            old = __ Xor(old,SingleOperand(ir::Imm{sign})).SetType(ir::ValueType::U64);
            result = __ Add(old,SingleOperand(source)).SetType(ir::ValueType::U64);
            result = __ And(result,SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
            ir::Value limit;
            if (op == NEON_SUQADD) {
                saturated = unsigned_less(result,old);limit = ImmValue(mask,ir::ValueType::U64);
            } else {
                auto first = __ Xor(old,SingleOperand(result)).SetType(ir::ValueType::U64);
                auto second = __ Xor(source,SingleOperand(result)).SetType(ir::ValueType::U64);
                auto changed = __ And(first,SingleOperand(second)).SetType(ir::ValueType::U64);
                saturated = __ TestBit(changed,ir::Imm{bits-1}).SetType(ir::ValueType::U8);
                auto negative = __ TestBit(old,ir::Imm{bits-1}).SetType(ir::ValueType::U8);
                limit = __ Select(negative,ImmValue(sign,ir::ValueType::U64),ImmValue(sign-1,ir::ValueType::U64)).SetType(ir::ValueType::U64);
            }
            result = __ Select(saturated,limit,result).SetType(ir::ValueType::U64);
            result = __ Xor(result,SingleOperand(ir::Imm{sign})).SetType(ir::ValueType::U64);
        } else {
            auto difference = __ Xor(source,SingleOperand(ir::Imm{sign})).SetType(ir::ValueType::U64);
            saturated = __ TestZero(difference).SetType(ir::ValueType::U8);
            result = __ Sub(zero,SingleOperand(source)).SetType(ir::ValueType::U64);
            if (op == NEON_SQABS) {
                auto negative = __ TestBit(source,ir::Imm{bits-1}).SetType(ir::ValueType::U8);
                result = __ Select(negative,result,source).SetType(ir::ValueType::U64);
            }
            result = __ Select(saturated,ImmValue(sign-1,ir::ValueType::U64),result).SetType(ir::ValueType::U64);
        }
        any_saturation = __ Or(any_saturation,SingleOperand(saturated)).SetType(ir::ValueType::U8);
        result = __ And(result,SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        result = __ LslImm(result,ir::Imm{position%64}).SetType(ir::ValueType::U64);
        auto& half = position<64 ? low : high;half = __ Or(half,SingleOperand(result)).SetType(ir::ValueType::U64);
    }
    ir::Uniform status{u32(offsetof(ThreadContext64,fpsr)),ir::ValueType::U32};
    auto previous = __ LoadUniform<ir::U32>(status);
    auto qc = __ Select(any_saturation,ImmValue(1u<<27,ir::ValueType::U32),ImmValue(0,ir::ValueType::U32)).SetType(ir::ValueType::U32);
    __ StoreUniform(status,__ Or(previous,SingleOperand(qc)).SetType(ir::ValueType::U32));
    WriteVHalves(instr->GetRd(),low,high);return true;
}

#undef __
}  // namespace swift::arm64
