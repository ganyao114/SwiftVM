#include "arm64_frontend_internal.h"

namespace swift::arm64 {
#define __ assembler->

bool A64Decoder::TryNEONSaturatingMultiplyHigh(const Instruction* instr, bool scalar,
                                              bool indexed, bool extra) {
    const u32 mask = indexed
        ? (scalar ? NEONScalarByIndexedElementMask : NEONByIndexedElementMask)
        : extra ? (scalar ? NEONScalar3SameExtraMask : NEON3SameExtraMask)
                : (scalar ? NEONScalar3SameMask : NEON3SameMask);
    const u32 op = instr->Mask(mask);
    const u32 scalar_bits = scalar ? NEON_Q | NEONScalar : 0;
    const u32 plain = (indexed ? NEON_SQDMULH_byelement : NEON_SQDMULH) | scalar_bits;
    const u32 rounded = (indexed ? NEON_SQRDMULH_byelement : NEON_SQRDMULH) | scalar_bits;
    const u32 add = (indexed ? NEON_SQRDMLAH_byelement : NEON_SQRDMLAH) | scalar_bits;
    const u32 sub = (indexed ? NEON_SQRDMLSH_byelement : NEON_SQRDMLSH) | scalar_bits;
    u32 mode;
    if ((!extra || indexed) && op == plain) mode = 0;
    else if ((!extra || indexed) && op == rounded) mode = 1;
    else if ((extra || indexed) && op == add) mode = 2;
    else if ((extra || indexed) && op == sub) mode = 3;
    else return false;
    const u32 bits = 8u << instr->GetNEONSize();
    if (bits != 16 && bits != 32) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }

    // Snapshot every input before the result can overwrite an aliased register.
    auto lhs = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
    ir::Value rhs;
    if (indexed) {
        const u32 rm = bits == 16 ? instr->GetRmLow16() : instr->GetRm();
        const u32 index = bits == 16
            ? (instr->GetNEONH() << 2) | (instr->GetNEONL() << 1) | instr->GetNEONM()
            : (instr->GetNEONH() << 1) | instr->GetNEONL();
        auto element = ReadVHalf(rm, index * bits >= 64);
        if (index * bits % 64)
            element = __ LsrImm<ir::U64>(element, ir::Imm{index * bits % 64}).SetType(ir::ValueType::U64);
        element = __ And<ir::U64>(element, SingleOperand(ir::Imm{(u64{1} << bits) - 1}))
                      .SetType(ir::ValueType::U64);
        for (u32 shift = bits; shift < 64; shift *= 2) {
            auto copy = __ LslImm<ir::U64>(element, ir::Imm{shift}).SetType(ir::ValueType::U64);
            element = __ Or<ir::U64>(element, SingleOperand(copy)).SetType(ir::ValueType::U64);
        }
        rhs = __ VecDup64(element).SetType(ir::ValueType::V128);
    } else {
        rhs = ReadVRegister(instr->GetRm(), ir::ValueType::V128);
    }
    auto accumulator = ReadVRegister(instr->GetRd(), ir::ValueType::V128);
    const u32 active = scalar ? 1 : (instr->GetNEONQ() ? 128 : 64) / bits;
    auto result = __ VecSatMulHigh(lhs, rhs, accumulator, ir::Imm{bits}, ir::Imm{(active << 2) | mode})
                     .SetType(ir::ValueType::V128);
    auto saturated = __ VecSatMulQC(result).SetType(ir::ValueType::U8);
    const ir::Uniform status{u32(offsetof(ThreadContext64, fpsr)), ir::ValueType::U32};
    auto previous = __ LoadUniform<ir::U32>(status);
    auto qc = __ Select(saturated, ImmValue(1u << 27, ir::ValueType::U32),
                        ImmValue(0, ir::ValueType::U32)).SetType(ir::ValueType::U32);
    __ StoreUniform(status, __ Or<ir::U32>(previous, SingleOperand(qc)).SetType(ir::ValueType::U32));
    WriteVRegister(instr->GetRd(), result);
    return true;
}

void A64Decoder::VisitNEONScalar3SameExtra(const Instruction* instr) {
    if (!TryNEONSaturatingMultiplyHigh(instr, true, false, true))
        Interrupt(InterruptReason::FALLBACK, current_pc);
}

#undef __
}  // namespace swift::arm64
