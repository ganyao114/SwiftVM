#include "translator.h"

#include <stdexcept>
#include <algorithm>
#include "runtime/backend/context.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

namespace {

u64 Repeated(u32 bits, u64 lane) {
    u64 result{};
    for (u32 offset = 0; offset < 64; offset += bits) result |= lane << offset;
    return result;
}

bool VectorShape(ir::Inst* inst) {
    if (ir::IsFloatValueType(inst->ReturnType())) return true;
    if (inst->GetOp() == O::StoreUniform || inst->GetOp() == O::StoreMemory || inst->GetOp() == O::StoreMemoryTSO)
        return ir::IsFloatValueType(inst->GetArg<ir::Value>(1).Type());
    if (inst->GetOp() == O::BitCast || inst->GetOp() == O::GetResult)
        return ir::IsFloatValueType(inst->GetArg<ir::Value>(0).Type());
    return false;
}

}  // namespace

bool JitTranslator::EmitVector(ir::Inst* inst) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::LoadUniform: case O::StoreUniform: case O::Zero: case O::LoadImm:
        case O::LoadMemory: case O::LoadMemoryTSO:
        case O::StoreMemory: case O::StoreMemoryTSO: case O::Select: case O::SelectZero: case O::CondSelect:
            if (!VectorShape(inst)) return false;
            break;
        case O::BitCast: case O::GetResult:
        case O::VecSharedZero: case O::VecLoadConst:
        case O::Vec4And: case O::Vec4Or: case O::VecAnd: case O::VecOr: case O::VecXor: case O::VecAndNot:
        case O::Vec4Add: case O::Vec4Sub: case O::VecAdd: case O::VecSub: case O::VecAvg:
        case O::VecDup64: case O::VecExtract64: case O::VecExtract16: case O::VecInsert16:
        case O::VecShiftLeft: case O::VecShiftRight: case O::VecShiftRightArithmetic:
        case O::VecShiftLeftImm: case O::VecShiftRightImm: case O::VecShiftRightArithmeticImm:
            break;
        default: return false;
    }
    auto& as = context.GetMasm();
    const bool rvv = context.Features().vector;
    if ((op == O::BitCast || op == O::GetResult) && !ir::IsFloatValueType(inst->ReturnType())) {
        const auto zero_high = [&](const auto& recurse, ir::Value value) -> bool {
            auto* def = value.Def();
            if (def->GetOp() == O::BitCast || def->GetOp() == O::GetResult)
                return recurse(recurse, def->GetArg<ir::Value>(0));
            return !ir::IsFloatValueType(value.Type());
        };
        const auto input = inst->GetArg<ir::Value>(0);
        if (zero_high(zero_high, input)) {
            const auto result = context.ResultRegister(inst);
            context.Read(result, input);
            // BitCast/GetResult copy raw slot bits, even into a narrow scalar.
            context.Write(inst, result, true);
            return true;
        }
    }
    const bool store = op == O::StoreUniform || op == O::StoreMemory || op == O::StoreMemoryTSO;
    const bool extract = op == O::VecExtract64 || op == O::VecExtract16;
    const auto scalar_result = extract ? context.ResultRegister(inst) : x0;
    std::array<GPR, 2> pair{x0, x0};
    Vec vector_result{v3};
    if (!store && !extract) {
        if (rvv) vector_result = context.ResultVector(inst);
        else pair = context.ResultPair(inst);
    }
    const auto write = [&] {
        if (rvv) context.WriteVector(inst, vector_result);
        else context.WritePair(inst, pair);
    };
    const auto vector_from_pair = [&](GPR low, GPR high) {
        context.SetVectorType(64, 2);
        as.VMV(v6, high); as.VSLIDE1UP(vector_result, v6, low);
    };
    if (op == O::Zero || op == O::VecSharedZero || op == O::LoadImm || op == O::VecLoadConst) {
        const u64 low = op == O::Zero || op == O::VecSharedZero ? 0 : inst->GetArg<ir::Imm>(0).Get();
        const u64 high = op == O::VecLoadConst ? inst->GetArg<ir::Imm>(1).Get() : 0;
        if (rvv) {
            context.SetVectorType(64, 2);
            if (low == high) { as.LI(a0, low); as.VMV(vector_result, a0); }
            else { as.LI(a0, low); as.LI(a1, high); vector_from_pair(a0, a1); }
        } else { as.LI(pair[0], low); as.LI(pair[1], high); }
        write(); return true;
    }
    if (op == O::LoadUniform || op == O::StoreUniform || op == O::LoadMemory || op == O::LoadMemoryTSO ||
        op == O::StoreMemory || op == O::StoreMemoryTSO) {
        const u32 size = ir::GetValueSizeByte(store ? inst->GetArg<ir::Value>(1).Type() : inst->ReturnType());
        const bool uniform = op == O::LoadUniform || op == O::StoreUniform;
        const bool ordered = op == O::LoadMemoryTSO || op == O::StoreMemoryTSO;
        if (uniform) context.Address(a0, state, state_offset_uniform_buffer + inst->GetArg<ir::Uniform>(0).GetOffset());
        else { context.Operand(a1, inst->GetArg<ir::Operand>(0)); EmitAddress(size); }
        if (ordered) as.FENCE();
        if (uniform && !rvv && (state_offset_uniform_buffer + inst->GetArg<ir::Uniform>(0).GetOffset()) % std::min(size, 8u) == 0) {
            for (u32 part = 0; part < (size + 7) / 8; ++part) {
                const auto width = std::min(size - part * 8, 8u);
                if (store) context.Store(context.SourcePart(inst->GetArg<ir::Value>(1), part, a2), a0, part * 8, width);
                else context.Load(pair[part], a0, part * 8, width);
            }
            if (!store) { if (size < 16) as.MV(pair[1], x0); write(); }
            return true;
        }
        if (rvv) {
            auto source = vector_result;
            if (store) source = context.SourceVector(inst->GetArg<ir::Value>(1), v1);
            else if (size < 16) { context.SetVectorType(64, 2); as.VMV(source, 0); }
            if (uniform && size == 16 && (state_offset_uniform_buffer + inst->GetArg<ir::Uniform>(0).GetOffset()) % 8 == 0) {
                context.SetVectorType(64, 2);
                if (store) as.VSE64(source, a0); else as.VLE64(source, a0);
            } else {
                context.SetVectorType(8, size);
                if (store) as.VSE8(source, a0); else as.VLE8(source, a0);
            }
        } else {
            if (!store) { as.MV(pair[0], x0); as.MV(pair[1], x0); }
            Label unaligned, done;
            if (size >= 8) {
                as.ANDI(t2, a0, 7); as.BNE(t2, x0, &unaligned);
                for (u32 part = 0; part < size / 8; ++part) {
                    if (store) { const auto source = context.SourcePart(inst->GetArg<ir::Value>(1), part, a2); as.SD(source, part * 8, a0); }
                    else as.LD(pair[part], part * 8, a0);
                }
                as.J(&done);
                as.Bind(&unaligned);
            }
            for (u32 part = 0; part < (size + 7) / 8; ++part) {
                if (store) context.ReadPart(a2, inst->GetArg<ir::Value>(1), part);
                for (u32 byte = part * 8; byte < std::min(size, (part + 1) * 8); ++byte) {
                    if (store) { as.SB(a2, byte, a0); if (byte + 1 < std::min(size, (part + 1) * 8)) as.SRLI(a2, a2, 8); }
                    else { as.LBU(a2, byte, a0); if (byte % 8) as.SLLI(a2, a2, (byte % 8) * 8); as.OR(pair[part], pair[part], a2); }
                }
            }
            as.Bind(&done);
        }
        if (ordered) as.FENCE();
        if (!store) write();
        return true;
    }
    if (op == O::BitCast || op == O::GetResult || op == O::Select || op == O::SelectZero || op == O::CondSelect) {
        if (op == O::BitCast || op == O::GetResult) {
            const auto input = inst->GetArg<ir::Value>(0);
            if (rvv) { const auto source = context.SourceVector(input, v1); context.SetVectorType(64, 2); as.VMV(vector_result, source); }
            else { context.ReadPart(pair[0], input, 0); context.ReadPart(pair[1], input, 1); }
        } else {
            if (op == O::CondSelect) context.Condition(a0, inst->GetArg<ir::Cond>(0));
            else context.Read(a0, inst->GetArg<ir::Value>(0));
            // Read both before branching so both arms agree on vtype and the
            // canonical/register mapping; VMV is a 128-bit copy at VL=2.
            if (rvv) {
                const auto left = context.SourceVector(inst->GetArg<ir::Value>(1), v1);
                const auto right = context.SourceVector(inst->GetArg<ir::Value>(2), v2);
                context.SetVectorType(64, 2);
                Label other, done;
                if (op == O::SelectZero) as.BNE(a0, x0, &other); else as.BEQ(a0, x0, &other);
                as.VMV(vector_result, left); as.J(&done); as.Bind(&other); as.VMV(vector_result, right); as.Bind(&done);
            } else {
                Label other, done;
                if (op == O::SelectZero) as.BNE(a0, x0, &other); else as.BEQ(a0, x0, &other);
                context.ReadPart(pair[0], inst->GetArg<ir::Value>(1), 0); context.ReadPart(pair[1], inst->GetArg<ir::Value>(1), 1);
                as.J(&done); as.Bind(&other);
                context.ReadPart(pair[0], inst->GetArg<ir::Value>(2), 0); context.ReadPart(pair[1], inst->GetArg<ir::Value>(2), 1);
                as.Bind(&done);
            }
        }
        write(); return true;
    }
    if (op == O::VecExtract64 || op == O::VecExtract16) {
        const u32 bits = op == O::VecExtract64 ? 64 : 16;
        const auto lane = inst->GetArg<ir::Imm>(1).Get() & (128 / bits - 1);
        context.ReadPart(scalar_result, inst->GetArg<ir::Value>(0), lane * bits / 64);
        if (lane * bits % 64) as.SRLI(scalar_result, scalar_result, lane * bits % 64);
        context.Mask(scalar_result, bits); context.Write(inst, scalar_result); return true;
    }
    if (op == O::VecDup64 || op == O::VecInsert16) {
        if (op == O::VecDup64) {
            context.Read(a0, inst->GetArg<ir::Value>(0));
            if (rvv) { context.SetVectorType(64, 2); as.VMV(vector_result, a0); }
            else { as.MV(pair[0], a0); as.MV(pair[1], a0); }
        } else if (rvv) {
            const auto source = context.SourceVector(inst->GetArg<ir::Value>(0), v1);
            context.Read(a2, inst->GetArg<ir::Value>(1));
            context.SetVectorType(16, 8); as.VID(v4);
            as.VMSEQ(v0, v4, s32(inst->GetArg<ir::Imm>(2).Get() & 7));
            as.VMERGE(vector_result, source, a2);
        } else {
            context.ReadPart(a0, inst->GetArg<ir::Value>(0), 0); context.ReadPart(a1, inst->GetArg<ir::Value>(0), 1);
            context.Read(a2, inst->GetArg<ir::Value>(1)); context.Mask(a2, 16);
            const u32 lane = inst->GetArg<ir::Imm>(2).Get() & 7, shift = lane * 16 % 64;
            const auto target = lane < 4 ? a0 : a1;
            as.LI(t2, ~(u64{65535} << shift)); as.AND(target, target, t2);
            if (shift) as.SLLI(a2, a2, shift); as.OR(target, target, a2);
            as.MV(pair[0], a0); as.MV(pair[1], a1);
        }
        write(); return true;
    }
    const bool shift = op == O::VecShiftLeft || op == O::VecShiftRight || op == O::VecShiftRightArithmetic ||
            op == O::VecShiftLeftImm || op == O::VecShiftRightImm || op == O::VecShiftRightArithmeticImm;
    const bool immediate = shift && inst->ArgAt(1).IsImm();
    const bool left_shift = op == O::VecShiftLeft || op == O::VecShiftLeftImm;
    const bool arithmetic = op == O::VecShiftRightArithmetic || op == O::VecShiftRightArithmeticImm;
    const bool add = op == O::VecAdd || op == O::Vec4Add;
    const bool sub = op == O::VecSub || op == O::Vec4Sub;
    const bool average = op == O::VecAvg;
    const bool bitwise = !shift && !add && !sub && !average;
    const u32 bits = bitwise ? 64 : op == O::Vec4Add || op == O::Vec4Sub ? 32 : inst->GetArg<ir::Imm>(2).Get();
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) throw std::runtime_error("invalid RV64 vector lane width");
    if (shift && !immediate) context.Read(a0, inst->GetArg<ir::Value>(1));
    if (rvv) {
        const auto left = context.SourceVector(inst->GetArg<ir::Value>(0), v1);
        const auto right = shift ? v2 : context.SourceVector(inst->GetArg<ir::Value>(1), v2);
        context.SetVectorType(bits, 128 / bits);
        if (add) as.VADD(vector_result, left, right);
        else if (sub) as.VSUB(vector_result, left, right);
        else if (average) {
            // RNU rounds an odd sum upward. Cache the CSR setup across
            // native operations; ABI calls invalidate its known value.
            context.SetVectorRounding(0);
            as.VAADDU(vector_result, left, right);
        } else if (shift && immediate) {
            const auto count = inst->GetArg<ir::Imm>(1).Get();
            const auto amount = arithmetic ? std::min(count, u64(bits - 1)) : count;
            if (!arithmetic && amount >= bits) as.VMV(vector_result, 0);
            else if (amount < 32) {
                if (left_shift) as.VSLL(vector_result, left, u32(amount));
                else if (arithmetic) as.VSRA(vector_result, left, u32(amount));
                else as.VSRL(vector_result, left, u32(amount));
            } else {
                as.LI(a0, amount);
                if (left_shift) as.VSLL(vector_result, left, a0);
                else if (arithmetic) as.VSRA(vector_result, left, a0); else as.VSRL(vector_result, left, a0);
            }
        } else if (shift) {
            Label normal, done;
            as.LI(a1, bits); as.BLTU(a0, a1, &normal);
            if (arithmetic) { as.LI(a1, bits - 1); as.VSRA(vector_result, left, a1); } else as.VMV(vector_result, 0);
            as.J(&done); as.Bind(&normal);
            if (left_shift) as.VSLL(vector_result, left, a0);
            else if (arithmetic) as.VSRA(vector_result, left, a0); else as.VSRL(vector_result, left, a0);
            as.Bind(&done);
        } else if (op == O::VecAnd || op == O::Vec4And) as.VAND(vector_result, left, right);
        else if (op == O::VecOr || op == O::Vec4Or) as.VOR(vector_result, left, right);
        else if (op == O::VecXor) as.VXOR(vector_result, left, right);
        else { as.VXOR(v4, right, -1); as.VAND(vector_result, left, v4); }
    } else {
        if ((add || sub) && bits < 64) {
            const auto high = Repeated(bits, u64{1} << (bits - 1));
            as.LI(a4, high); as.LI(a5, ~high);
        }
        for (u32 part = 0; part < 2; ++part) {
            const auto lhs = context.SourcePart(inst->GetArg<ir::Value>(0), part, a2);
            const auto rhs = shift ? a3 : context.SourcePart(inst->GetArg<ir::Value>(1), part, a3);
            const auto result = pair[part];
            if (bitwise) {
                if (op == O::VecAnd || op == O::Vec4And) as.AND(result, lhs, rhs);
                else if (op == O::VecOr || op == O::Vec4Or) as.OR(result, lhs, rhs);
                else if (op == O::VecXor) as.XOR(result, lhs, rhs);
                else if (context.Features().zbb) as.ANDN(result, lhs, rhs);
                else { as.NOT(t2, rhs); as.AND(result, lhs, t2); }
            } else if (average) {
                as.XOR(t2, lhs, rhs); as.SRLI(t2, t2, 1);
                as.LI(t3, Repeated(bits, (u64{1} << (bits - 1)) - 1)); as.AND(t2, t2, t3);
                as.OR(result, lhs, rhs); as.SUB(result, result, t2);
            } else if (add || sub) {
                if (bits == 64) { if (add) as.ADD(result, lhs, rhs); else as.SUB(result, lhs, rhs); }
                else {
                    as.AND(t3, rhs, a5); as.XOR(t4, lhs, rhs);
                    if (add) {
                        as.AND(t5, lhs, a5); as.ADD(result, t5, t3);
                        as.AND(t4, t4, a4); as.XOR(result, result, t4);
                    } else {
                        as.OR(t5, lhs, a4); as.SUB(result, t5, t3);
                        as.NOT(t4, t4); as.AND(t4, t4, a4); as.XOR(result, result, t4);
                    }
                }
            } else if (immediate) {
                const auto count = inst->GetArg<ir::Imm>(1).Get();
                if (!arithmetic) {
                    if (count >= bits) as.MV(result, x0);
                    else {
                        const u64 lane_mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
                        const u64 mask = left_shift ? (lane_mask << count) & lane_mask : lane_mask >> count;
                        if (left_shift) as.SLLI(result, lhs, count); else as.SRLI(result, lhs, count);
                        if (bits < 64 && count) { as.LI(t2, Repeated(bits, mask)); as.AND(result, result, t2); }
                    }
                } else {
                    const u32 amount = std::min(count, u64(bits - 1));
                    as.MV(result, x0);
                    for (u32 lane = 0; lane < 64 / bits; ++lane) {
                        as.SRLI(t3, lhs, lane * bits); context.SignExtend(t3, bits); as.SRAI(t3, t3, amount);
                        context.Mask(t3, bits); if (lane) as.SLLI(t3, t3, lane * bits); as.OR(result, result, t3);
                    }
                }
            } else {
                // Whole-GPR logical lane shifts need one shift and a mask.
                // Signed lanes instead use sign-extended independent lanes.
                Label normal, done;
                as.LI(a1, bits); as.BLTU(a0, a1, &normal);
                if (!arithmetic) as.MV(result, x0);
                else {
                    as.LI(t2, bits - 1); as.MV(result, x0);
                    for (u32 lane = 0; lane < 64 / bits; ++lane) {
                        as.SRLI(t3, lhs, lane * bits); context.SignExtend(t3, bits); as.SRA(t3, t3, t2);
                        context.Mask(t3, bits); if (lane) as.SLLI(t3, t3, lane * bits); as.OR(result, result, t3);
                    }
                }
                as.J(&done); as.Bind(&normal);
                if (arithmetic) {
                    as.MV(result, x0);
                    for (u32 lane = 0; lane < 64 / bits; ++lane) {
                        as.SRLI(t3, lhs, lane * bits); context.SignExtend(t3, bits); as.SRA(t3, t3, a0);
                        context.Mask(t3, bits); if (lane) as.SLLI(t3, t3, lane * bits); as.OR(result, result, t3);
                    }
                } else {
                    as.LI(t2, bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1);
                    if (left_shift) { as.SLL(t2, t2, a0); context.Mask(t2, bits); }
                    else as.SRL(t2, t2, a0);
                    as.MV(t3, x0);
                    for (u32 lane = 0; lane < 64 / bits; ++lane) { as.SLLI(t4, t2, lane * bits); as.OR(t3, t3, t4); }
                    if (left_shift) as.SLL(result, lhs, a0); else as.SRL(result, lhs, a0);
                    as.AND(result, result, t3);
                }
                as.Bind(&done);
            }
        }
    }
    write(); return true;
}

}  // namespace swift::runtime::backend::riscv64
