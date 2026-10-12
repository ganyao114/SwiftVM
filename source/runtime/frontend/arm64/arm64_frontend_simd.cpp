#include "arm64_frontend_internal.h"

namespace swift::arm64 {

#define __ assembler->

bool A64Decoder::TryNEONInteger(const Instruction* instr, bool miscellaneous) {
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    const auto op = instr->Mask(miscellaneous ? NEON2RegMiscMask : NEON3SameMask);
    auto vector = [&](u32 reg) { return ReadVRegister(reg, ir::ValueType::V128); };
    auto constant = [&](u64 half) {
        return __ VecLoadConst(ir::Imm{half}, ir::Imm{half}).SetType(ir::ValueType::V128);
    };
    auto commit = [&](ir::Value result) {
        result.SetType(ir::ValueType::V128);
        if (!q) result = __ BitCast(result).SetType(ir::ValueType::V64);
        WriteVRegister(instr->GetRd(), result);
    };
    const auto ones = [&] { return constant(~u64{0}); };
    if (!miscellaneous) {
        switch (instr->Mask(NEON3SameLogicalMask)) {
            case NEON_AND: commit(__ VecAnd(vector(instr->GetRn()), vector(instr->GetRm()))); return true;
            case NEON_ORR: commit(__ VecOr(vector(instr->GetRn()), vector(instr->GetRm()))); return true;
            case NEON_EOR: commit(__ VecXor(vector(instr->GetRn()), vector(instr->GetRm()))); return true;
            case NEON_BIC: commit(__ VecAndNot(vector(instr->GetRn()), vector(instr->GetRm()))); return true;
            case NEON_ORN: {
                auto inverted = __ VecXor(vector(instr->GetRm()), ones()).SetType(ir::ValueType::V128);
                commit(__ VecOr(vector(instr->GetRn()), inverted)); return true;
            }
            case NEON_BSL: case NEON_BIT: case NEON_BIF: {
                const auto logical = instr->Mask(NEON3SameLogicalMask);
                auto old = vector(instr->GetRd()), left = vector(instr->GetRn()), right = vector(instr->GetRm());
                auto mask = logical == NEON_BSL ? old : right;
                if (logical == NEON_BIF) mask = __ VecXor(mask, ones()).SetType(ir::ValueType::V128);
                auto selected = __ VecAnd(left, mask).SetType(ir::ValueType::V128);
                auto retained = __ VecAndNot(logical == NEON_BSL ? right : old, mask).SetType(ir::ValueType::V128);
                commit(__ VecOr(selected, retained)); return true;
            }
            default: break;
        }
    } else if (op == NEON_RBIT_NOT && instr->GetNEONSize() == 0) {
        commit(__ VecXor(vector(instr->GetRn()), ones())); return true;
    }
    if (bits == 64) {
        // VIXL's generic class visitor also admits reserved width/Q fields.
        // Check only recognized integer operations; other opcode classes keep
        // their own size interpretation and unsupported-operation fallback.
        bool invalid = false;
        if (miscellaneous) {
            switch (op) {
                case NEON_CMEQ_zero: case NEON_CMGT_zero: case NEON_CMGE_zero:
                case NEON_CMLT_zero: case NEON_CMLE_zero: case NEON_ABS: case NEON_NEG:
                    invalid = !q; break;
                default: break;
            }
        } else {
            switch (op) {
                case NEON_MUL: case NEON_MLA: case NEON_MLS:
                case NEON_SMAX: case NEON_SMIN: case NEON_UMAX: case NEON_UMIN:
                case NEON_SMAXP: case NEON_SMINP: case NEON_UMAXP: case NEON_UMINP:
                    invalid = true; break;
                case NEON_ADD: case NEON_SUB: case NEON_ADDP:
                case NEON_CMEQ: case NEON_CMGT: case NEON_CMGE:
                case NEON_CMHI: case NEON_CMHS: case NEON_CMTST:
                    invalid = !q; break;
                default: break;
            }
        }
        if (invalid) { Interrupt(InterruptReason::ILL_CODE,current_pc);return true; }
        if (!q) return false;
    }
    auto zero = [&] { return __ VecSharedZero().SetType(ir::ValueType::V128); };
    const bool pairwise = !miscellaneous && (op == NEON_ADDP || op == NEON_SMAXP ||
            op == NEON_SMINP || op == NEON_UMAXP || op == NEON_UMINP);
    if (pairwise && (bits != 64 || op == NEON_ADDP)) {
        if (q) {
            auto left = vector(instr->GetRn()), right = vector(instr->GetRm());
            auto even = __ VecUnzip(left, right, ir::Imm{bits}, ir::Imm{false}).SetType(ir::ValueType::V128);
            auto odd = __ VecUnzip(left, right, ir::Imm{bits}, ir::Imm{true}).SetType(ir::ValueType::V128);
            if (op == NEON_ADDP) commit(__ VecAdd(even, odd, ir::Imm{bits}));
            else if (op == NEON_SMAXP || op == NEON_UMAXP)
                commit(__ VecMax(even, odd, ir::Imm{bits}, ir::Imm{op == NEON_SMAXP}));
            else commit(__ VecMin(even, odd, ir::Imm{bits}, ir::Imm{op == NEON_SMINP}));
        } else {
            const u32 pairs = 32 / bits;
            const u64 mask = (u64{1} << bits) - 1;
            auto left = ReadVHalf(instr->GetRn(), false), right = ReadVHalf(instr->GetRm(), false);
            auto packed = ImmValue(0, ir::ValueType::U64);
            for (u32 index = 0; index < pairs * 2; ++index) {
                auto source = index < pairs ? left : right;
                auto a = __ LsrImm(source, ir::Imm{(index % pairs) * bits * 2}).SetType(ir::ValueType::U64);
                auto b = __ LsrImm(source, ir::Imm{((index % pairs) * 2 + 1) * bits}).SetType(ir::ValueType::U64);
                a = __ And(a, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
                b = __ And(b, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
                const bool sign = op == NEON_SMAXP || op == NEON_SMINP;
                if (sign) { a = SignExtendValue(a, bits); b = SignExtendValue(b, bits); }
                ir::Value lane;
                if (op == NEON_ADDP) lane = __ Add(a, SingleOperand(b)).SetType(ir::ValueType::U64);
                else {
                    auto difference = __ Sub(a, SingleOperand(b)).SetType(ir::ValueType::U64);
                    auto less = __ TestBit(difference, ir::Imm{63}).SetType(ir::ValueType::U8);
                    const bool maximum = op == NEON_SMAXP || op == NEON_UMAXP;
                    lane = __ Select(less, maximum ? b : a, maximum ? a : b).SetType(ir::ValueType::U64);
                }
                lane = __ And(lane, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
                lane = __ LslImm(lane, ir::Imm{index * bits}).SetType(ir::ValueType::U64);
                packed = __ Or(packed, SingleOperand(lane)).SetType(ir::ValueType::U64);
            }
            WriteVRegister(instr->GetRd(), packed);
        }
        return true;
    }
    auto lhs = vector(instr->GetRn());
    auto rhs = miscellaneous ? zero() : vector(instr->GetRm());
    auto compare = [&](ir::Value a, ir::Value b, bool sign, bool equal) {
        if (!sign) {
            u64 bias{};
            for (u32 shift = 0; shift < 64; shift += bits) bias |= u64{1} << (shift + bits - 1);
            auto flip = constant(bias);
            a = __ VecXor(a, flip).SetType(ir::ValueType::V128);
            b = __ VecXor(b, flip).SetType(ir::ValueType::V128);
        }
        auto gt = __ VecCmpGt(a, b, ir::Imm{bits}).SetType(ir::ValueType::V128);
        if (!equal) return gt;
        auto eq = __ VecCmpEq(a, b, ir::Imm{bits}).SetType(ir::ValueType::V128);
        return __ VecOr(gt, eq).SetType(ir::ValueType::V128);
    };
    if (miscellaneous) {
        switch (op) {
            case NEON_CMEQ_zero: commit(__ VecCmpEq(lhs, rhs, ir::Imm{bits})); return true;
            case NEON_CMGT_zero: commit(compare(lhs, rhs, true, false)); return true;
            case NEON_CMGE_zero: commit(compare(lhs, rhs, true, true)); return true;
            case NEON_CMLT_zero: commit(compare(rhs, lhs, true, false)); return true;
            case NEON_CMLE_zero: commit(compare(rhs, lhs, true, true)); return true;
            case NEON_NEG: commit(__ VecSub(rhs, lhs, ir::Imm{bits})); return true;
            case NEON_ABS: {
                auto negative = __ VecSub(rhs, lhs, ir::Imm{bits}).SetType(ir::ValueType::V128);
                auto mask = compare(rhs, lhs, true, false);
                auto selected = __ VecAnd(negative, mask).SetType(ir::ValueType::V128);
                auto retained = __ VecAndNot(lhs, mask).SetType(ir::ValueType::V128);
                commit(__ VecOr(selected, retained)); return true;
            }
            default: return false;
        }
    }
    switch (op) {
        case NEON_ADD: commit(__ VecAdd(lhs, rhs, ir::Imm{bits})); return true;
        case NEON_SUB: commit(__ VecSub(lhs, rhs, ir::Imm{bits})); return true;
        case NEON_MUL: case NEON_MLA: case NEON_MLS: {
            if (bits == 64) return false;
            auto product = __ VecMul(lhs, rhs, ir::Imm{bits}).SetType(ir::ValueType::V128);
            if (op == NEON_MUL) commit(product);
            else {
                auto old = vector(instr->GetRd());
                commit(op == NEON_MLA ? __ VecAdd(old, product, ir::Imm{bits})
                                     : __ VecSub(old, product, ir::Imm{bits}));
            }
            return true;
        }
        case NEON_CMEQ: commit(__ VecCmpEq(lhs, rhs, ir::Imm{bits})); return true;
        case NEON_CMGT: case NEON_CMGE: case NEON_CMHI: case NEON_CMHS:
            commit(compare(lhs, rhs, op == NEON_CMGT || op == NEON_CMGE,
                           op == NEON_CMGE || op == NEON_CMHS)); return true;
        case NEON_CMTST: {
            auto combined = __ VecAnd(lhs, rhs).SetType(ir::ValueType::V128);
            auto eq = __ VecCmpEq(combined, zero(), ir::Imm{bits}).SetType(ir::ValueType::V128);
            commit(__ VecXor(eq, ones())); return true;
        }
        case NEON_SMAX: case NEON_SMIN: case NEON_UMAX: case NEON_UMIN:
            if (bits == 64) return false;
            if (op == NEON_SMAX || op == NEON_UMAX)
                commit(__ VecMax(lhs, rhs, ir::Imm{bits}, ir::Imm{op == NEON_SMAX}));
            else commit(__ VecMin(lhs, rhs, ir::Imm{bits}, ir::Imm{op == NEON_SMIN}));
            return true;
        default: return false;
    }
}

void A64Decoder::VisitNEONPerm(const Instruction* instr) {
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    const u32 lanes = (q ? 128 : 64) / bits;
    const auto op = instr->Mask(NEONPermMask);
    const bool second = op == NEON_UZP2 || op == NEON_ZIP2 || op == NEON_TRN2;
    if (lanes < 2) { Interrupt(InterruptReason::ILL_CODE, current_pc); return; }
    if (q) {
        auto left = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
        auto right = ReadVRegister(instr->GetRm(), ir::ValueType::V128);
        ir::Value result;
        if (op == NEON_UZP1 || op == NEON_UZP2)
            result = __ VecUnzip(left, right, ir::Imm{bits}, ir::Imm{second});
        else if (op == NEON_ZIP1 || op == NEON_ZIP2)
            result = __ VecZip(left, right, ir::Imm{bits}, ir::Imm{second});
        else if (op == NEON_TRN1 || op == NEON_TRN2) {
            auto a = __ VecUnzip(left, left, ir::Imm{bits}, ir::Imm{second}).SetType(ir::ValueType::V128);
            auto b = __ VecUnzip(right, right, ir::Imm{bits}, ir::Imm{second}).SetType(ir::ValueType::V128);
            result = __ VecZip(a, b, ir::Imm{bits}, ir::Imm{false});
        } else { Interrupt(InterruptReason::ILL_CODE, current_pc); return; }
        WriteVRegister(instr->GetRd(), result.SetType(ir::ValueType::V128));
        return;
    }
    auto left = ReadVHalf(instr->GetRn(), false), right = ReadVHalf(instr->GetRm(), false);
    auto packed = ImmValue(0, ir::ValueType::U64);
    const u64 mask = (u64{1} << bits) - 1;
    for (u32 index = 0; index < lanes; ++index) {
        ir::Value source;
        u32 lane{};
        if (op == NEON_UZP1 || op == NEON_UZP2) {
            source = index < lanes / 2 ? left : right;
            lane = (index % (lanes / 2)) * 2 + second;
        } else if (op == NEON_ZIP1 || op == NEON_ZIP2) {
            source = index % 2 ? right : left;
            lane = index / 2 + second * (lanes / 2);
        } else if (op == NEON_TRN1 || op == NEON_TRN2) {
            source = index % 2 ? right : left;
            lane = (index / 2) * 2 + second;
        } else { Interrupt(InterruptReason::ILL_CODE, current_pc); return; }
        auto value = __ LsrImm(source, ir::Imm{lane * bits}).SetType(ir::ValueType::U64);
        value = __ And(value, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        value = __ LslImm(value, ir::Imm{index * bits}).SetType(ir::ValueType::U64);
        packed = __ Or(packed, SingleOperand(value)).SetType(ir::ValueType::U64);
    }
    WriteVRegister(instr->GetRd(), packed);
}

void A64Decoder::VisitNEONTable(const Instruction* instr) {
    const u32 count = ((instr->GetInstructionBits() >> 13) & 3) + 1;
    auto indexes = ReadVRegister(instr->GetRm(), ir::ValueType::V128);
    auto nibble_mask = __ VecLoadConst(ir::Imm{u64{0xf0f0f0f0f0f0f0f0ULL}}, ir::Imm{u64{0xf0f0f0f0f0f0f0f0ULL}})
                               .SetType(ir::ValueType::V128);
    auto upper = __ VecAnd(indexes, nibble_mask).SetType(ir::ValueType::V128);
    auto result = __ VecSharedZero().SetType(ir::ValueType::V128);
    auto valid = result;
    for (u32 table = 0; table < count; ++table) {
        const u64 repeated = 0x0101010101010101ULL * (table * 16);
        auto id = __ VecLoadConst(ir::Imm{repeated}, ir::Imm{repeated}).SetType(ir::ValueType::V128);
        auto mask = __ VecCmpEq(upper, id, ir::Imm{8}).SetType(ir::ValueType::V128);
        auto source = ReadVRegister((instr->GetRn() + table) & 31, ir::ValueType::V128);
        auto looked = __ VecTableLookup8(source, indexes).SetType(ir::ValueType::V128);
        auto selected = __ VecAnd(looked, mask).SetType(ir::ValueType::V128);
        result = __ VecOr(result, selected).SetType(ir::ValueType::V128);
        valid = __ VecOr(valid, mask).SetType(ir::ValueType::V128);
    }
    if (instr->GetInstructionBits() & NEONTableExt) {
        auto retained = __ VecAndNot(ReadVRegister(instr->GetRd(), ir::ValueType::V128), valid)
                                   .SetType(ir::ValueType::V128);
        result = __ VecOr(result, retained).SetType(ir::ValueType::V128);
    }
    if (!instr->GetNEONQ()) result = __ BitCast(result).SetType(ir::ValueType::V64);
    WriteVRegister(instr->GetRd(), result);
}

void A64Decoder::VisitNEONAcrossLanes(const Instruction* instr) {
    const bool fp16 = instr->Mask(NEONAcrossLanesFP16FMask) == NEONAcrossLanesFP16Fixed;
    if (TryNEONFPReduction(instr, fp16, true)) return;
    const auto op = instr->Mask(NEONAcrossLanesMask);
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    if (bits >= 64 || (bits == 32 && !q)) { Interrupt(InterruptReason::ILL_CODE, current_pc); return; }
    const bool add = op == NEON_ADDV || op == NEON_SADDLV || op == NEON_UADDLV;
    const bool sign = op == NEON_SADDLV || op == NEON_SMAXV || op == NEON_SMINV;
    const bool maximum = op == NEON_SMAXV || op == NEON_UMAXV;
    if (!add && !maximum && op != NEON_SMINV && op != NEON_UMINV) {
        Interrupt(InterruptReason::FALLBACK, current_pc); return;
    }
    auto low = ReadVHalf(instr->GetRn(), false), high = ReadVHalf(instr->GetRn(), true);
    auto result = ImmValue(0, ir::ValueType::U64);
    const u64 mask = (u64{1} << bits) - 1;
    for (u32 index = 0; index < (q ? 128u : 64u) / bits; ++index) {
        auto lane = __ LsrImm(index * bits < 64 ? low : high, ir::Imm{index * bits % 64})
                              .SetType(ir::ValueType::U64);
        lane = __ And(lane, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        if (sign) lane = SignExtendValue(lane, bits);
        if (!index) result = lane;
        else if (add) result = __ Add(result, SingleOperand(lane)).SetType(ir::ValueType::U64);
        else {
            // All legal source widths here are <=32, so the difference fits s64.
            auto diff = __ Sub(result, SingleOperand(lane)).SetType(ir::ValueType::U64);
            auto negative = __ LsrImm(diff, ir::Imm{63}).SetType(ir::ValueType::U64);
            auto less = __ TestNotZero(negative).SetType(ir::ValueType::U8);
            result = __ Select(less, maximum ? lane : result, maximum ? result : lane)
                               .SetType(ir::ValueType::U64);
        }
    }
    const u32 out_bits = (op == NEON_SADDLV || op == NEON_UADDLV) ? bits * 2 : bits;
    auto narrow = __ BitCast(result).SetType(ir::GetIRValueType(out_bits / 8));
    WriteVRegister(instr->GetRd(), narrow);
}

void A64Decoder::VisitNEON3Different(const Instruction* instr) {
    if (TryNEONHighNarrow(instr)) return;
    if (TryNEONAbsoluteLong(instr)) return;
    if (TryNEONPolynomial(instr,true)) return;
    if (TryNEONVectorDoublingLong(instr, false)) return;
    const u32 bits = 8u << instr->GetNEONSize();
    const auto op = instr->Mask(NEON3DifferentMask) & ~NEON_Q;
    const bool sign = !(instr->GetInstructionBits() & NEON3SameUBit);
    const bool upper = instr->GetNEONQ();
    const bool multiply = op == NEON_SMULL || op == NEON_UMULL || op == NEON_SMLAL ||
                          op == NEON_UMLAL || op == NEON_SMLSL || op == NEON_UMLSL;
    const bool add = op == NEON_SADDL || op == NEON_UADDL || op == NEON_SADDW || op == NEON_UADDW;
    const bool sub = op == NEON_SSUBL || op == NEON_USUBL || op == NEON_SSUBW || op == NEON_USUBW;
    const bool wide_left = op == NEON_SADDW || op == NEON_UADDW || op == NEON_SSUBW || op == NEON_USUBW;
    const bool accumulate = op == NEON_SMLAL || op == NEON_UMLAL || op == NEON_SMLSL || op == NEON_UMLSL;
    if (!multiply && !add && !sub) { Interrupt(InterruptReason::FALLBACK, current_pc); return; }
    if (bits >= 64) { Interrupt(InterruptReason::ILL_CODE,current_pc);return; }
    const u32 out_bits = bits * 2;
    auto left = ReadVHalf(instr->GetRn(), upper), right = ReadVHalf(instr->GetRm(), upper);
    auto left_low = wide_left ? ReadVHalf(instr->GetRn(), false) : left;
    auto left_high = wide_left ? ReadVHalf(instr->GetRn(), true) : left;
    auto old_low = accumulate ? ReadVHalf(instr->GetRd(), false) : ImmValue(0, ir::ValueType::U64);
    auto old_high = accumulate ? ReadVHalf(instr->GetRd(), true) : old_low;
    const u64 mask = (u64{1} << bits) - 1;
    const u64 out_mask = out_bits == 64 ? ~u64{0} : (u64{1} << out_bits) - 1;
    auto low = ImmValue(0, ir::ValueType::U64), high = low;
    for (u32 index = 0; index < 64 / bits; ++index) {
        auto a = __ LsrImm(wide_left ? (index * out_bits < 64 ? left_low : left_high) : left,
                           ir::Imm{index * (wide_left ? out_bits : bits) % 64}).SetType(ir::ValueType::U64);
        auto b = __ LsrImm(right, ir::Imm{index * bits}).SetType(ir::ValueType::U64);
        a = __ And(a, SingleOperand(ir::Imm{wide_left ? out_mask : mask})).SetType(ir::ValueType::U64);
        b = __ And(b, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        if (sign) {
            if (!wide_left) a = SignExtendValue(a, bits);
            b = SignExtendValue(b, bits);
        }
        auto value = multiply ? __ Mul(a, SingleOperand(b))
                   : add ? __ Add(a, SingleOperand(b)) : __ Sub(a, SingleOperand(b));
        value.SetType(ir::ValueType::U64);
        if (accumulate) {
            auto old = __ LsrImm(index * out_bits < 64 ? old_low : old_high,
                                 ir::Imm{index * out_bits % 64}).SetType(ir::ValueType::U64);
            value = (op == NEON_SMLSL || op == NEON_UMLSL)
                        ? __ Sub(old, SingleOperand(value)) : __ Add(old, SingleOperand(value));
            value.SetType(ir::ValueType::U64);
        }
        value = __ And(value, SingleOperand(ir::Imm{out_mask})).SetType(ir::ValueType::U64);
        value = __ LslImm(value, ir::Imm{index * out_bits % 64}).SetType(ir::ValueType::U64);
        auto& packed = index * out_bits < 64 ? low : high;
        packed = __ Or(packed, SingleOperand(value)).SetType(ir::ValueType::U64);
    }
    WriteVHalves(instr->GetRd(), low, high);
}

bool A64Decoder::TryNEONSaturatingArithmetic(const Instruction* instr) {
    const auto op = instr->Mask(NEON3SameMask);
    if (op != NEON_SQADD && op != NEON_UQADD && op != NEON_SQSUB && op != NEON_UQSUB)
        return false;
    const bool add = op == NEON_SQADD || op == NEON_UQADD;
    const bool sign = op == NEON_SQADD || op == NEON_SQSUB;
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    if (bits == 64 && !q) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    ir::Value saturated;
    if (bits <= 16) {
        auto left = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
        auto right = ReadVRegister(instr->GetRm(), ir::ValueType::V128);
        if (!q) {
            auto mask = __ VecLoadConst(ir::Imm{~u64{0}}, ir::Imm{u64{0}}).SetType(ir::ValueType::V128);
            left = __ VecAnd(left, mask).SetType(ir::ValueType::V128);
            right = __ VecAnd(right, mask).SetType(ir::ValueType::V128);
        }
        auto result = add ? __ VecSatAdd(left, right, ir::Imm{bits}, ir::Imm{sign})
                          : __ VecSatSub(left, right, ir::Imm{bits}, ir::Imm{sign});
        result.SetType(ir::ValueType::V128);
        auto wrapped = add ? __ VecAdd(left, right, ir::Imm{bits})
                           : __ VecSub(left, right, ir::Imm{bits});
        wrapped.SetType(ir::ValueType::V128);
        auto different = __ VecXor(result, wrapped).SetType(ir::ValueType::V128);
        auto low = __ VecExtract64(different, ir::Imm{false}).SetType(ir::ValueType::U64);
        auto high = __ VecExtract64(different, ir::Imm{true}).SetType(ir::ValueType::U64);
        auto any = __ Or(low, SingleOperand(high)).SetType(ir::ValueType::U64);
        saturated = __ TestNotZero(any).SetType(ir::ValueType::U8);
        if (!q) result = __ BitCast(result).SetType(ir::ValueType::V64);
        WriteVRegister(instr->GetRd(), result);
    } else {
        auto left_low = ReadVHalf(instr->GetRn(), false), left_high = ReadVHalf(instr->GetRn(), true);
        auto right_low = ReadVHalf(instr->GetRm(), false), right_high = ReadVHalf(instr->GetRm(), true);
        auto low = ImmValue(0, ir::ValueType::U64), high = low;
        saturated = ImmValue(0, ir::ValueType::U8);
        const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
        for (u32 lane = 0; lane < (q ? 128u : 64u) / bits; ++lane) {
            const u32 position = lane * bits;
            auto a = __ LsrImm(position < 64 ? left_low : left_high,
                               ir::Imm{position % 64}).SetType(ir::ValueType::U64);
            auto b = __ LsrImm(position < 64 ? right_low : right_high,
                               ir::Imm{position % 64}).SetType(ir::ValueType::U64);
            a = __ And(a, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
            b = __ And(b, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
            auto result = add ? __ Add(a, SingleOperand(b)) : __ Sub(a, SingleOperand(b));
            result.SetType(ir::ValueType::U64);
            result = __ And(result, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
            auto invert = [&](ir::Value value) {
                return __ Xor(value, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
            };
            ir::Value overflow, limit;
            if (sign) {
                auto changed = __ Xor(a, SingleOperand(result)).SetType(ir::ValueType::U64);
                auto other = __ Xor(add ? b : a, SingleOperand(add ? result : b))
                                     .SetType(ir::ValueType::U64);
                auto crossed = __ And(changed, SingleOperand(other)).SetType(ir::ValueType::U64);
                overflow = __ TestBit(crossed, ir::Imm{bits - 1}).SetType(ir::ValueType::U8);
                auto negative = __ TestBit(a, ir::Imm{bits - 1}).SetType(ir::ValueType::U8);
                limit = __ Select(negative, ImmValue(u64{1} << (bits - 1), ir::ValueType::U64),
                                           ImmValue((u64{1} << (bits - 1)) - 1, ir::ValueType::U64))
                                  .SetType(ir::ValueType::U64);
            } else {
                // The top carry/borrow bit remains valid even for 64-bit lanes.
                auto first = __ And(add ? a : invert(a), SingleOperand(b)).SetType(ir::ValueType::U64);
                auto related = add ? __ Or(a, SingleOperand(b)) : __ Xor(a, SingleOperand(b));
                related.SetType(ir::ValueType::U64);
                auto second = __ And(add ? related : invert(related),
                                     SingleOperand(add ? invert(result) : result)).SetType(ir::ValueType::U64);
                auto carry = __ Or(first, SingleOperand(second)).SetType(ir::ValueType::U64);
                overflow = __ TestBit(carry, ir::Imm{bits - 1}).SetType(ir::ValueType::U8);
                limit = ImmValue(add ? mask : 0, ir::ValueType::U64);
            }
            saturated = __ Or(saturated, SingleOperand(overflow)).SetType(ir::ValueType::U8);
            result = __ Select(overflow, limit, result).SetType(ir::ValueType::U64);
            result = __ LslImm(result, ir::Imm{position % 64}).SetType(ir::ValueType::U64);
            auto& half = position < 64 ? low : high;
            half = __ Or(half, SingleOperand(result)).SetType(ir::ValueType::U64);
        }
        WriteVHalves(instr->GetRd(), low, high);
    }
    ir::Uniform status{u32(offsetof(ThreadContext64, fpsr)), ir::ValueType::U32};
    auto old = __ LoadUniform<ir::U32>(status);
    auto qc = __ Select(saturated, ImmValue(u64{1} << 27, ir::ValueType::U32),
                                  ImmValue(0, ir::ValueType::U32)).SetType(ir::ValueType::U32);
    __ StoreUniform(status, __ Or<ir::U32>(old, SingleOperand(qc)));
    return true;
}

bool A64Decoder::TryNEONShiftImmediate(const Instruction* instr) {
    const auto op = instr->Mask(NEONShiftImmediateMask);
    const bool widen = op == NEON_SSHLL || op == NEON_USHLL;
    const bool narrow = op == NEON_RSHRN;
    const bool left = op == NEON_SHL || op == NEON_SLI || widen;
    const bool insert = op == NEON_SLI || op == NEON_SRI;
    const bool sign = op == NEON_SSHR || op == NEON_SSRA ||
                      op == NEON_SRSHR || op == NEON_SRSRA;
    const bool rounded = op == NEON_SRSHR || op == NEON_URSHR ||
                         op == NEON_SRSRA || op == NEON_URSRA || narrow;
    const bool accumulate = op == NEON_SSRA || op == NEON_USRA ||
                            op == NEON_SRSRA || op == NEON_URSRA;
    if (!left && !insert && !sign && !rounded && !accumulate && op != NEON_USHR)
        return false;
    const int highest = vixl::HighestSetBitPosition(instr->GetImmNEONImmh());
    if (highest < 0 || highest > 3 || ((widen || narrow) && highest > 2)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const u32 bits = 8u << highest;
    const u32 encoded = instr->GetImmNEONImmhImmb();
    const u32 shift = left ? encoded - bits : 2 * bits - encoded;
    const bool q = instr->GetNEONQ();
    if (widen || narrow) {
        const u32 source_bits = narrow ? bits * 2 : bits;
        const u32 result_bits = narrow ? bits : bits * 2;
        auto input_low = ReadVHalf(instr->GetRn(), widen && q);
        auto input_high = narrow ? ReadVHalf(instr->GetRn(), true) : input_low;
        const u64 result_mask = result_bits == 64 ? ~u64{0} : (u64{1} << result_bits) - 1;
        const u64 source_mask = source_bits == 64 ? ~u64{0} : (u64{1} << source_bits) - 1;
        auto low = ImmValue(0, ir::ValueType::U64), high = low;
        for (u32 lane = 0; lane < (narrow ? 64u : 128u) / result_bits; ++lane) {
            const u32 source_position = lane * source_bits;
            auto value = __ LsrImm(source_position < 64 ? input_low : input_high,
                                   ir::Imm{source_position % 64}).SetType(ir::ValueType::U64);
            value = __ And(value, SingleOperand(ir::Imm{source_mask})).SetType(ir::ValueType::U64);
            if (narrow) {
                auto round = __ LsrImm(value, ir::Imm{shift - 1}).SetType(ir::ValueType::U64);
                round = __ And(round, SingleOperand(ir::Imm{u64{1}})).SetType(ir::ValueType::U64);
                value = __ LsrImm(value, ir::Imm{shift}).SetType(ir::ValueType::U64);
                value = __ Add(value, SingleOperand(round)).SetType(ir::ValueType::U64);
            } else {
                if (op == NEON_SSHLL) value = SignExtendValue(value, bits);
                value = __ LslImm(value, ir::Imm{shift}).SetType(ir::ValueType::U64);
            }
            value = __ And(value, SingleOperand(ir::Imm{result_mask})).SetType(ir::ValueType::U64);
            value = __ LslImm(value, ir::Imm{lane * result_bits % 64}).SetType(ir::ValueType::U64);
            auto& half = lane * result_bits < 64 ? low : high;
            half = __ Or(half, SingleOperand(value)).SetType(ir::ValueType::U64);
        }
        if (narrow && q) WriteVHalves(instr->GetRd(), ReadVHalf(instr->GetRd(), false), low);
        else WriteVHalves(instr->GetRd(), low, high);
        return true;
    }
    if (bits == 64 && !q) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto input = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
    auto result = left ? __ VecShiftLeftImm(input, ir::Imm{shift}, ir::Imm{bits})
               : sign ? __ VecShiftRightArithmeticImm(input, ir::Imm{shift}, ir::Imm{bits})
                      : __ VecShiftRightImm(input, ir::Imm{shift}, ir::Imm{bits});
    result.SetType(ir::ValueType::V128);
    auto constant = [&](u64 lane) {
        u64 half{};
        for (u32 position = 0; position < 64; position += bits) half |= lane << position;
        return __ VecLoadConst(ir::Imm{half}, ir::Imm{half}).SetType(ir::ValueType::V128);
    };
    if (rounded) {
        // Quotient plus the last discarded bit avoids overflowing the source lane.
        auto round = __ VecShiftRightImm(input, ir::Imm{shift - 1}, ir::Imm{bits})
                            .SetType(ir::ValueType::V128);
        round = __ VecAnd(round, constant(1)).SetType(ir::ValueType::V128);
        result = __ VecAdd(result, round, ir::Imm{bits}).SetType(ir::ValueType::V128);
    }
    if (accumulate || insert) {
        auto old = ReadVRegister(instr->GetRd(), ir::ValueType::V128);
        if (accumulate) result = __ VecAdd(old, result, ir::Imm{bits}).SetType(ir::ValueType::V128);
        else {
            const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
            const u64 keep = left ? (shift == 0 ? 0 : (u64{1} << shift) - 1)
                                 : (shift == bits ? mask : mask ^ (mask >> shift));
            old = __ VecAnd(old, constant(keep)).SetType(ir::ValueType::V128);
            result = __ VecOr(old, result).SetType(ir::ValueType::V128);
        }
    }
    if (!q) result = __ BitCast(result).SetType(ir::ValueType::V64);
    WriteVRegister(instr->GetRd(), result);
    return true;
}

bool A64Decoder::TryNEONVariableShift(const Instruction* instr, bool scalar) {
    // Scalar encodings share the vector opcode after removing their class bit.
    const auto op = (instr->GetInstructionBits() & ~u32(NEONScalar)) & NEON3SameMask;
    const bool sign = op == NEON_SSHL || op == NEON_SRSHL || op == NEON_SQSHL || op == NEON_SQRSHL;
    const bool rounded = op == NEON_SRSHL || op == NEON_URSHL || op == NEON_SQRSHL || op == NEON_UQRSHL;
    const bool saturating = op == NEON_SQSHL || op == NEON_UQSHL || op == NEON_SQRSHL || op == NEON_UQRSHL;
    if (!sign && !rounded && !saturating && op != NEON_USHL) return false;
    const u32 bits = 8u << instr->GetNEONSize();
    const bool q = instr->GetNEONQ();
    if ((scalar && !saturating && bits != 64) || (!scalar && bits == 64 && !q)) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    auto input_low = ReadVHalf(instr->GetRn(), false), input_high = ReadVHalf(instr->GetRn(), true);
    auto count_low = ReadVHalf(instr->GetRm(), false), count_high = ReadVHalf(instr->GetRm(), true);
    auto low = ImmValue(0, ir::ValueType::U64), high = low;
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    auto zero = ImmValue(0, ir::ValueType::U64);
    auto any_saturation = ImmValue(0, ir::ValueType::U8);
    const u32 active = scalar ? 1 : (q ? 128u : 64u) / bits;
    for (u32 lane = 0; lane < active; ++lane) {
        const u32 position = lane * bits;
        auto input = __ LsrImm(position < 64 ? input_low : input_high,
                               ir::Imm{position % 64}).SetType(ir::ValueType::U64);
        input = __ And(input, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        auto count = __ LsrImm(position < 64 ? count_low : count_high,
                               ir::Imm{position % 64}).SetType(ir::ValueType::U64);
        count = SignExtendValue(count, 8);
        auto right = __ TestBit(count, ir::Imm{63u}).SetType(ir::ValueType::U8);
        auto negative = __ Sub(zero, SingleOperand(count)).SetType(ir::ValueType::U64);
        auto magnitude = __ Select(right, negative, count).SetType(ir::ValueType::U64);
        auto difference = __ Sub(magnitude, SingleOperand(ir::Imm{u64{bits}})).SetType(ir::ValueType::U64);
        auto in_range = __ TestBit(difference, ir::Imm{63u}).SetType(ir::ValueType::U8);
        auto shifted_left = __ LslValue(input, magnitude).SetType(ir::ValueType::U64);
        shifted_left = __ Select(in_range, shifted_left, zero).SetType(ir::ValueType::U64);
        if (saturating) {
            auto narrowed = __ And(shifted_left, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
            auto original = sign ? SignExtendValue(input, bits) : input;
            auto extended = sign ? SignExtendValue(narrowed, bits) : narrowed;
            auto restored = sign ? __ AsrValue(extended, magnitude) : __ LsrValue(extended, magnitude);
            restored.SetType(ir::ValueType::U64);
            auto mismatch = __ Xor(restored, SingleOperand(original)).SetType(ir::ValueType::U64);
            auto changed = __ TestNotZero(mismatch).SetType(ir::ValueType::U8);
            auto nonzero = __ TestNotZero(input).SetType(ir::ValueType::U8);
            auto overflow = __ Select(in_range, changed, nonzero).SetType(ir::ValueType::U8);
            overflow = __ Select(right, ImmValue(0, ir::ValueType::U8), overflow).SetType(ir::ValueType::U8);
            any_saturation = __ Or(any_saturation, SingleOperand(overflow)).SetType(ir::ValueType::U8);
            auto limit = ImmValue(mask, ir::ValueType::U64);
            if (sign) {
                auto negative_input = __ TestBit(input, ir::Imm{bits - 1}).SetType(ir::ValueType::U8);
                limit = __ Select(negative_input, ImmValue(u64{1} << (bits - 1), ir::ValueType::U64),
                                                ImmValue((u64{1} << (bits - 1)) - 1, ir::ValueType::U64))
                                  .SetType(ir::ValueType::U64);
            }
            shifted_left = __ Select(overflow, limit, narrowed).SetType(ir::ValueType::U64);
        }
        ir::Value shifted_right;
        if (sign) {
            auto signed_input = SignExtendValue(input, bits);
            auto amount = __ Select(in_range, magnitude, ImmValue(bits - 1, ir::ValueType::U64))
                                  .SetType(ir::ValueType::U64);
            shifted_right = __ AsrValue(signed_input, amount).SetType(ir::ValueType::U64);
        } else {
            shifted_right = __ LsrValue(input, magnitude).SetType(ir::ValueType::U64);
            shifted_right = __ Select(in_range, shifted_right, zero).SetType(ir::ValueType::U64);
        }
        if (rounded) {
            auto last_bit = __ Sub(magnitude, SingleOperand(ir::Imm{u64{1}})).SetType(ir::ValueType::U64);
            auto round = __ LsrValue(input, last_bit).SetType(ir::ValueType::U64);
            round = __ And(round, SingleOperand(ir::Imm{u64{1}})).SetType(ir::ValueType::U64);
            shifted_right = __ Add(shifted_right, SingleOperand(round)).SetType(ir::ValueType::U64);
            auto below_next = __ Sub(magnitude, SingleOperand(ir::Imm{u64{bits + 1}})).SetType(ir::ValueType::U64);
            auto round_range = __ TestBit(below_next, ir::Imm{63u}).SetType(ir::ValueType::U8);
            shifted_right = __ Select(round_range, shifted_right, zero).SetType(ir::ValueType::U64);
        }
        auto value = __ Select(right, shifted_right, shifted_left).SetType(ir::ValueType::U64);
        value = __ And(value, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
        value = __ LslImm(value, ir::Imm{position % 64}).SetType(ir::ValueType::U64);
        auto& half = position < 64 ? low : high;
        half = __ Or(half, SingleOperand(value)).SetType(ir::ValueType::U64);
    }
    WriteVHalves(instr->GetRd(), low, high);
    if (saturating) {
        ir::Uniform status{u32(offsetof(ThreadContext64, fpsr)), ir::ValueType::U32};
        auto old = __ LoadUniform<ir::U32>(status);
        auto qc = __ Select(any_saturation, ImmValue(u64{1} << 27, ir::ValueType::U32),
                                          ImmValue(0, ir::ValueType::U32)).SetType(ir::ValueType::U32);
        __ StoreUniform(status, __ Or<ir::U32>(old, SingleOperand(qc)));
    }
    return true;
}

void A64Decoder::NEONMultiMemory(const Instruction* instr, bool post_index) {
    const u32 opcode = (instr->GetInstructionBits() >> 12) & 15;
    u32 registers;
    bool interleaved = false;
    switch (opcode) {
        case 7: registers = 1; break;
        case 10: registers = 2; break;
        case 6: registers = 3; break;
        case 2: registers = 4; break;
        case 8: registers = 2; interleaved = true; break;
        case 4: registers = 3; interleaved = true; break;
        case 0: registers = 4; interleaved = true; break;
        default:
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
    }
    const bool q = instr->GetNEONQ();
    const u32 bytes = q ? 16 : 8;
    const u32 size = (instr->GetInstructionBits() >> 10) & 3;
    if (interleaved && size == 3 && !q) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const bool load = (instr->GetInstructionBits() >> 22) & 1;
    const u8 rn = instr->GetRn(), rt = instr->GetRt();
    __ SetLocation(ir::Lambda{ir::Imm{current_pc}});
    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    auto offset = post_index && instr->GetRm() != 31 ? ReadXRegister(instr->GetRm())
                  : ImmValue(bytes * registers, ir::ValueType::U64);
    auto address = [&](u32 position) {
        return position ? __ Add(base, SingleOperand(ir::Imm{u64{position}})).SetType(ir::ValueType::U64)
                        : base;
    };
    if (!interleaved) {
        const auto type = q ? ir::ValueType::V128 : ir::ValueType::U64;
        for (u32 reg = 0; reg < registers; ++reg) {
            const u8 code = (rt + reg) & 31;
            if (load) WriteVRegister(code, ReadMemory(ir::Lambda{address(reg * bytes)}, type));
            else WriteMemory(address(reg * bytes), ReadVRegister(code, type));
        }
    } else if (registers == 2 && q) {
        const u32 bits = 8u << size;
        if (load) {
            auto first = ReadMemory(ir::Lambda{base}, ir::ValueType::V128);
            auto second = ReadMemory(ir::Lambda{address(16)}, ir::ValueType::V128);
            auto even = __ VecUnzip(first, second, ir::Imm{bits}, ir::Imm{false}).SetType(ir::ValueType::V128);
            auto odd = __ VecUnzip(first, second, ir::Imm{bits}, ir::Imm{true}).SetType(ir::ValueType::V128);
            WriteVRegister(rt, even);
            WriteVRegister((rt + 1) & 31, odd);
        } else {
            auto first = ReadVRegister(rt, ir::ValueType::V128);
            auto second = ReadVRegister((rt + 1) & 31, ir::ValueType::V128);
            auto low = __ VecZip(first, second, ir::Imm{bits}, ir::Imm{false}).SetType(ir::ValueType::V128);
            auto high = __ VecZip(first, second, ir::Imm{bits}, ir::Imm{true}).SetType(ir::ValueType::V128);
            WriteMemory(base, low);
            WriteMemory(address(16), high);
        }
    } else {
        const u32 lane_bytes = 1u << size, bits = lane_bytes * 8;
        const auto lane_type = size == 0 ? ir::ValueType::U8 : size == 1 ? ir::ValueType::U16
                             : size == 2 ? ir::ValueType::U32 : ir::ValueType::U64;
        const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
        ir::Value low[4], high[4];
        for (u32 reg = 0; reg < registers; ++reg) {
            low[reg] = load ? ImmValue(0, ir::ValueType::U64) : ReadVHalf((rt + reg) & 31, false);
            high[reg] = load ? low[reg] : ReadVHalf((rt + reg) & 31, true);
        }
        for (u32 lane = 0; lane < bytes / lane_bytes; ++lane) {
            const u32 position = lane * bits;
            for (u32 reg = 0; reg < registers; ++reg) {
                auto addr = address((lane * registers + reg) * lane_bytes);
                auto& half = position < 64 ? low[reg] : high[reg];
                if (load) {
                    auto value = Widen(ReadMemory(ir::Lambda{addr}, lane_type));
                    value = __ LslImm(value, ir::Imm{position % 64}).SetType(ir::ValueType::U64);
                    half = __ Or(half, SingleOperand(value)).SetType(ir::ValueType::U64);
                } else {
                    auto value = __ LsrImm(half, ir::Imm{position % 64}).SetType(ir::ValueType::U64);
                    value = __ And(value, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
                    value = __ BitCast(value).SetType(lane_type);
                    WriteMemory(addr, value);
                }
            }
        }
        if (load) for (u32 reg = 0; reg < registers; ++reg)
            WriteVHalves((rt + reg) & 31, low[reg], high[reg]);
    }
    if (post_index) WriteXRegister(rn, __ Add(base, SingleOperand(offset)).SetType(ir::ValueType::U64),
                                  Reg31IsStackPointer);
}

void A64Decoder::VisitNEONLoadStoreSingleStruct(const Instruction* instr) {
    NEONSingleMemory(instr, false);
}

void A64Decoder::VisitNEONLoadStoreSingleStructPostIndex(const Instruction* instr) {
    NEONSingleMemory(instr, true);
}

void A64Decoder::NEONSingleMemory(const Instruction* instr, bool post_index) {
    const u32 word = instr->GetInstructionBits();
    const u32 count = 1 + ((word >> 21) & 1) + 2 * ((word >> 13) & 1);
    const u32 scale = (word >> 14) & 3;
    const u32 size = instr->GetNEONLSSize();
    const bool load = (word >> 22) & 1;
    const bool replicate = scale == 3;
    u32 lane_size;
    if (replicate) {
        if (!load || instr->GetNEONS()) {
            Interrupt(InterruptReason::ILL_CODE, current_pc);
            return;
        }
        lane_size = size;
    } else if (scale == 0) lane_size = 0;
    else if (scale == 1 && !(size & 1)) lane_size = 1;
    else if (scale == 2 && !(size & 2) && (!(size & 1) || !instr->GetNEONS()))
        lane_size = 2 + (size & 1);
    else {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return;
    }
    const u32 bytes = 1u << lane_size, bits = bytes * 8;
    const u32 lane = replicate ? 0 : instr->GetNEONLSIndex(lane_size);
    const u32 position = lane * bits;
    const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
    const auto type = lane_size == 0 ? ir::ValueType::U8 : lane_size == 1 ? ir::ValueType::U16
                    : lane_size == 2 ? ir::ValueType::U32 : ir::ValueType::U64;
    const u8 rn = instr->GetRn(), rt = instr->GetRt();
    __ SetLocation(ir::Lambda{ir::Imm{current_pc}});
    auto base = ReadXRegister(rn, Reg31IsStackPointer);
    auto offset = post_index && instr->GetRm() != 31 ? ReadXRegister(instr->GetRm())
                  : ImmValue(bytes * count, ir::ValueType::U64);
    for (u32 reg = 0; reg < count; ++reg) {
        const u8 code = (rt + reg) & 31;
        auto address = reg ? __ Add(base, SingleOperand(ir::Imm{u64{reg * bytes}}))
                                   .SetType(ir::ValueType::U64) : base;
        if (load) {
            auto value = Widen(ReadMemory(ir::Lambda{address}, type));
            if (replicate) {
                for (u32 shift = bits; shift < 64; shift *= 2) {
                    auto copy = __ LslImm(value, ir::Imm{shift}).SetType(ir::ValueType::U64);
                    value = __ Or(value, SingleOperand(copy)).SetType(ir::ValueType::U64);
                }
                WriteVHalves(code, value, instr->GetNEONQ() ? value : ImmValue(0, ir::ValueType::U64));
            } else {
                auto old = ReadVHalf(code, position >= 64);
                old = __ And(old, SingleOperand(ir::Imm{~(mask << (position % 64))}))
                            .SetType(ir::ValueType::U64);
                value = __ LslImm(value, ir::Imm{position % 64}).SetType(ir::ValueType::U64);
                value = __ Or(old, SingleOperand(value)).SetType(ir::ValueType::U64);
                const auto uniform_offset = u32(offsetof(ThreadContext64, v) + code * sizeof(u128)) +
                                            (position >= 64 ? 8u : 0u);
                // Lane loads preserve every untouched bit, including the high half at Q=0.
                __ StoreUniform(ir::Uniform{uniform_offset, ir::ValueType::U64}, value);
            }
        } else {
            auto value = ReadVHalf(code, position >= 64);
            value = __ LsrImm(value, ir::Imm{position % 64}).SetType(ir::ValueType::U64);
            value = __ And(value, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
            value = __ BitCast(value).SetType(type);
            WriteMemory(address, value);
        }
    }
    if (post_index) WriteXRegister(rn, __ Add(base, SingleOperand(offset)).SetType(ir::ValueType::U64),
                                  Reg31IsStackPointer);
}

bool A64Decoder::TryNEONBitMisc(const Instruction* instr) {
    const auto op = instr->Mask(NEON2RegMiscMask);
    const u32 size = instr->GetNEONSize(), bits = 8u << size;
    const bool q = instr->GetNEONQ();
    const bool narrow = op == NEON_XTN || op == NEON_SQXTN ||
                        op == NEON_UQXTN || op == NEON_SQXTUN;
    const bool count = op == NEON_CLS || op == NEON_CLZ;
    const u32 group_bits = op == NEON_REV64 ? 64 : op == NEON_REV32 ? 32 :
                           op == NEON_REV16 ? 16 : 0;
    const bool reverse_bits = op == NEON_RBIT_NOT && size == 1;
    if (op == NEON_RBIT_NOT && size > 1) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    if (!narrow && !count && !group_bits && op != NEON_CNT && !reverse_bits) return false;
    if ((narrow || count) ? size == 3 : group_bits ? bits >= group_bits :
        op == NEON_CNT && size != 0) {
        Interrupt(InterruptReason::ILL_CODE, current_pc);
        return true;
    }
    const auto input_low = ReadVHalf(instr->GetRn(), false);
    const auto input_high = ReadVHalf(instr->GetRn(), true);
    auto zero = ImmValue(0, ir::ValueType::U64);
    auto low = zero, high = zero;
    if (op == NEON_CNT || reverse_bits) {
        auto apply = [&](ir::Value value) {
            if (op == NEON_CNT) {
                auto shifted = __ LsrImm(value, ir::Imm{1}).SetType(ir::ValueType::U64);
                shifted = __ And(shifted, SingleOperand(ir::Imm{u64{0x5555555555555555}}))
                                  .SetType(ir::ValueType::U64);
                value = __ Sub(value, SingleOperand(shifted)).SetType(ir::ValueType::U64);
                shifted = __ LsrImm(value, ir::Imm{2}).SetType(ir::ValueType::U64);
                shifted = __ And(shifted, SingleOperand(ir::Imm{u64{0x3333333333333333}}))
                                  .SetType(ir::ValueType::U64);
                value = __ And(value, SingleOperand(ir::Imm{u64{0x3333333333333333}}))
                                .SetType(ir::ValueType::U64);
                value = __ Add(value, SingleOperand(shifted)).SetType(ir::ValueType::U64);
                shifted = __ LsrImm(value, ir::Imm{4}).SetType(ir::ValueType::U64);
                value = __ Add(value, SingleOperand(shifted)).SetType(ir::ValueType::U64);
                return __ And(value, SingleOperand(ir::Imm{u64{0x0F0F0F0F0F0F0F0F}}))
                           .SetType(ir::ValueType::U64);
            }
            const u64 masks[] = {0x5555555555555555, 0x3333333333333333, 0x0F0F0F0F0F0F0F0F};
            for (u32 stage = 0; stage < 3; ++stage) {
                auto right = __ LsrImm(value, ir::Imm{1u << stage}).SetType(ir::ValueType::U64);
                right = __ And(right, SingleOperand(ir::Imm{masks[stage]})).SetType(ir::ValueType::U64);
                auto left = __ And(value, SingleOperand(ir::Imm{masks[stage]})).SetType(ir::ValueType::U64);
                left = __ LslImm(left, ir::Imm{1u << stage}).SetType(ir::ValueType::U64);
                value = __ Or(left, SingleOperand(right)).SetType(ir::ValueType::U64);
            }
            return value;
        };
        low = apply(input_low);
        if (q) high = apply(input_high);
    } else {
        const u32 source_bits = narrow ? bits * 2 : bits;
        const u64 mask = (u64{1} << bits) - 1;
        const u64 source_mask = source_bits == 64 ? ~u64{0} : (u64{1} << source_bits) - 1;
        auto saturated = ImmValue(0, ir::ValueType::U8);
        const u32 lanes = (narrow ? 64 : q ? 128 : 64) / bits;
        for (u32 lane = 0; lane < lanes; ++lane) {
            const u32 source_lane = group_bits ?
                lane / (group_bits / bits) * (group_bits / bits) +
                (group_bits / bits - 1 - lane % (group_bits / bits)) : lane;
            const u32 position = source_lane * source_bits;
            auto value = __ LsrImm(position < 64 ? input_low : input_high, ir::Imm{position % 64})
                              .SetType(ir::ValueType::U64);
            value = __ And(value, SingleOperand(ir::Imm{source_mask})).SetType(ir::ValueType::U64);
            if (count) {
                if (op == NEON_CLS) {
                    auto sign = __ TestBit(value, ir::Imm{bits - 1}).SetType(ir::ValueType::U8);
                    auto flip = __ Select(sign, ImmValue(mask, ir::ValueType::U64), zero)
                                    .SetType(ir::ValueType::U64);
                    value = __ Xor(value, SingleOperand(flip)).SetType(ir::ValueType::U64);
                }
                value = __ CountLeadingZeros64(value).SetType(ir::ValueType::U64);
                value = __ Sub(value, SingleOperand(ir::Imm{64u - bits + (op == NEON_CLS ? 1u : 0u)}))
                              .SetType(ir::ValueType::U64);
            } else if (narrow && op != NEON_XTN) {
                const bool signed_source = op != NEON_UQXTN;
                if (signed_source) value = SignExtendValue(value, source_bits);
                auto trimmed = __ And(value, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
                if (op == NEON_SQXTN) trimmed = SignExtendValue(trimmed, bits);
                auto mismatch = __ Xor(value, SingleOperand(trimmed)).SetType(ir::ValueType::U64);
                auto overflow = __ TestNotZero(mismatch).SetType(ir::ValueType::U8);
                saturated = __ Or(saturated, SingleOperand(overflow)).SetType(ir::ValueType::U8);
                ir::Value limit = ImmValue(mask, ir::ValueType::U64);
                if (signed_source) {
                    auto sign = __ TestBit(value, ir::Imm{63}).SetType(ir::ValueType::U8);
                    limit = __ Select(sign, ImmValue(op == NEON_SQXTN ? u64{1} << (bits - 1) : 0,
                                                     ir::ValueType::U64),
                                            ImmValue(op == NEON_SQXTN ? (u64{1} << (bits - 1)) - 1 : mask,
                                                     ir::ValueType::U64)).SetType(ir::ValueType::U64);
                }
                value = __ Select(overflow, limit, value).SetType(ir::ValueType::U64);
            }
            value = __ And(value, SingleOperand(ir::Imm{mask})).SetType(ir::ValueType::U64);
            value = __ LslImm(value, ir::Imm{lane * bits % 64}).SetType(ir::ValueType::U64);
            auto& half = lane * bits < 64 ? low : high;
            half = __ Or(half, SingleOperand(value)).SetType(ir::ValueType::U64);
        }
        if (narrow && op != NEON_XTN) {
            ir::Uniform status{u32(offsetof(ThreadContext64, fpsr)), ir::ValueType::U32};
            auto old = __ LoadUniform<ir::U32>(status);
            auto qc = __ Select(saturated, ImmValue(1u << 27, ir::ValueType::U32),
                                          ImmValue(0, ir::ValueType::U32)).SetType(ir::ValueType::U32);
            __ StoreUniform(status, __ Or<ir::U32>(old, SingleOperand(qc)));
        }
    }
    if (narrow && q) WriteVHalves(instr->GetRd(), ReadVHalf(instr->GetRd(), false), low);
    else WriteVHalves(instr->GetRd(), low, high);
    return true;
}

void A64Decoder::VisitNEON2RegMiscFP16(const Instruction* instr) {
    if (TryNEONFPEstimate(instr, true)) return;
    if (TryNEONFPInteger(instr, true)) return;
    if (TryNEONFPCompare(instr, true, true)) return;
    if (TryNEONFP16Unary(instr)) return;
    const auto op = instr->Mask(NEON2RegMiscFP16Mask);
    if (op != NEON_FABS_H && op != NEON_FNEG_H) {
        Interrupt(InterruptReason::FALLBACK, current_pc);
        return;
    }
    auto input = ReadVRegister(instr->GetRn(), ir::ValueType::V128);
    auto constant = [&](u64 half) {
        return __ VecLoadConst(ir::Imm{half}, ir::Imm{half}).SetType(ir::ValueType::V128);
    };
    auto signs = constant(0x8000800080008000);
    auto magnitude = __ VecAndNot(input, signs).SetType(ir::ValueType::V128);
    // FEAT_AFP alternate handling preserves the original sign of every NaN,
    // including signaling NaNs. Neither operation processes FP exceptions,
    // flushes subnormals, quiets NaNs nor consults the rounding mode.
    auto nan = __ VecCmpGt(magnitude, constant(0x7C007C007C007C00), ir::Imm{16})
                       .SetType(ir::ValueType::V128);
    auto fpcr = __ LoadUniform<ir::U32>(ir::Uniform{u32(offsetof(ThreadContext64, fpcr)), ir::ValueType::U32});
    auto alternate = __ TestBit(fpcr, ir::Imm{1}).SetType(ir::ValueType::U8);
    auto preserved = __ Select(alternate, nan, __ VecSharedZero().SetType(ir::ValueType::V128))
                             .SetType(ir::ValueType::V128);
    signs = __ VecAndNot(signs, preserved).SetType(ir::ValueType::V128);
    auto result = op == NEON_FABS_H ? __ VecAndNot(input, signs) : __ VecXor(input, signs);
    result.SetType(ir::ValueType::V128);
    if (!instr->GetNEONQ()) result = __ BitCast(result).SetType(ir::ValueType::V64);
    WriteVRegister(instr->GetRd(), result);
}

#undef __
}  // namespace swift::arm64
