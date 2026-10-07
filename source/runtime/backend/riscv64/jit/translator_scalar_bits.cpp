#include "translator.h"

#include <array>
#include <stdexcept>

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

namespace {

constexpr auto crc32c_table = [] {
    std::array<u32, 256> table{};
    for (u32 i = 0; i < table.size(); ++i) {
        auto value = i;
        for (u32 bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (0x82f63b78u & (0u - (value & 1)));
        table[i] = value;
    }
    return table;
}();

u64 FieldMask(u64 lsb, u64 width) {
    if (lsb >= 64 || width > 64 - lsb)
        throw std::runtime_error("invalid RV64 scalar bit field");
    return width == 64 ? UINT64_MAX : ((u64{1} << width) - 1) << lsb;
}

u8 FloatConditionTable(ir::Cond cond) {
    u8 table{};
    for (u32 packed = 0; packed < 8; ++packed) {
        const bool n = packed == 1, z = packed == 4, c = !n, v = packed == 7;
        bool take{};
        switch (cond) {
            case ir::Cond::EQ: take = z; break;
            case ir::Cond::NE: take = !z; break;
            case ir::Cond::CS: take = c; break;
            case ir::Cond::CC: take = !c; break;
            case ir::Cond::MI: take = n; break;
            case ir::Cond::PL: take = !n; break;
            case ir::Cond::VS: take = v; break;
            case ir::Cond::VC: take = !v; break;
            case ir::Cond::HI: take = c && !z; break;
            case ir::Cond::LS: take = !c || z; break;
            case ir::Cond::GE: take = n == v; break;
            case ir::Cond::LT: take = n != v; break;
            case ir::Cond::GT: take = !z && n == v; break;
            case ir::Cond::LE: take = z || n != v; break;
            case ir::Cond::AL: case ir::Cond::NV: take = true; break;
        }
        table |= u8(take) << packed;
    }
    return table;
}

}  // namespace

bool JitTranslator::EmitScalarBits(ir::Inst* inst) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::ByteSwap: case O::CountLeadingZeros64: case O::CountTrailingZeros64:
        case O::CountLeadingZeros32: case O::CountTrailingZeros32: case O::PopCount:
        case O::BitExtract: case O::BitInsert: case O::BitClear: case O::Crc32c:
        case O::LocalParitySet: case O::FCmpCondSet: break;
        default: return false;
    }
    auto& as = context.GetMasm();
    if (ir::IsFloatValueType(inst->ReturnType())) return false;
    const auto result = context.ResultRegister(inst);
    const auto source = inst->GetArg<ir::Value>(0);
    bool normalized = false;
    if (op == O::BitExtract) {
        const auto lsb = inst->GetArg<ir::Imm>(1).Get(), width = inst->GetArg<ir::Imm>(2).Get();
        (void)FieldMask(lsb, width);
        if (!width) as.MV(result, x0);
        else {
            const auto reg = context.SourceRegister(source, t0);
            as.SRLI(result, reg, lsb);
            context.Mask(result, width);
        }
        normalized = width <= ir::GetValueSizeByte(inst->ReturnType()) * 8;
    } else if (op == O::BitInsert || op == O::BitClear) {
        const auto lsb = inst->GetArg<ir::Imm>(op == O::BitInsert ? 2 : 1).Get();
        const auto width = inst->GetArg<ir::Imm>(op == O::BitInsert ? 3 : 2).Get();
        const auto mask = FieldMask(lsb, width);
        context.Read(t0, source);
        as.LI(t2, ~mask);
        as.AND(result, t0, t2);
        if (op == O::BitInsert) {
            context.Read(t1, inst->GetArg<ir::Value>(1));
            as.SLLI(t1, t1, lsb);
            as.NOT(t2, t2); as.AND(t1, t1, t2); as.OR(result, result, t1);
        }
    } else if (op == O::LocalParitySet) {
        context.Read(result, source);
        for (u32 shift : {4u, 2u, 1u}) {
            as.SRLI(t1, result, shift); as.XOR(result, result, t1);
        }
        as.ANDI(result, result, 1);
        if (!inst->GetArg<ir::Imm>(1).Get()) as.XORI(result, result, 1);
        normalized = true;
    } else if (op == O::FCmpCondSet) {
        context.Read(t1, source); as.ANDI(t1, t1, 7);
        as.LI(result, FloatConditionTable(inst->GetArg<ir::Cond>(1)));
        as.SRL(result, result, t1); as.ANDI(result, result, 1);
        normalized = true;
    } else if (op == O::Crc32c) {
        const auto width = inst->GetArg<ir::Imm>(2).Get();
        if (width != 8 && width != 16 && width != 32 && width != 64)
            throw std::runtime_error("invalid RV64 CRC32C width");
        context.Read(t0, source); context.Mask(t0, 32);
        context.Read(t1, inst->GetArg<ir::Value>(1));
        as.LI(t2, reinterpret_cast<u64>(crc32c_table.data()));
        for (u32 byte = 0; byte < width / 8; ++byte) {
            context.EnsureSpace();
            as.XOR(t3, t0, t1); as.ANDI(t3, t3, 255); as.SLLI(t3, t3, 2);
            as.ADD(t3, t2, t3); as.LWU(t3, 0, t3); as.SRLI(t0, t0, 8); as.XOR(t0, t0, t3);
            if (byte + 1 != width / 8) as.SRLI(t1, t1, 8);
        }
        as.MV(result, t0);
        normalized = ir::GetValueSizeByte(inst->ReturnType()) >= 4;
    } else if (op == O::PopCount) {
        if (context.Features().zbb) as.CPOP(result, context.SourceRegister(source, t0));
        else {
            context.Read(t0, source);
            as.LI(t2, 0x5555555555555555ULL);
            as.SRLI(t1, t0, 1); as.AND(t1, t1, t2); as.SUB(t0, t0, t1);
            as.LI(t2, 0x3333333333333333ULL);
            as.AND(t1, t0, t2); as.SRLI(t0, t0, 2); as.AND(t0, t0, t2); as.ADD(t0, t0, t1);
            as.SRLI(t1, t0, 4); as.ADD(t0, t0, t1);
            as.LI(t2, 0x0f0f0f0f0f0f0f0fULL); as.AND(t0, t0, t2);
            as.LI(t2, 0x0101010101010101ULL); as.MUL(t0, t0, t2); as.SRLI(result, t0, 56);
        }
        normalized = true;
    } else if (op == O::ByteSwap) {
        const auto width = inst->GetArg<ir::Imm>(1).Get();
        if (width != 16 && width != 32 && width != 64)
            throw std::runtime_error("invalid RV64 byte-swap width");
        normalized = width <= ir::GetValueSizeByte(inst->ReturnType()) * 8;
        if (context.Features().zbb) {
            as.REV8(result, context.SourceRegister(source, t0));
            if (width != 64) as.SRLI(result, result, 64 - width);
        } else {
            context.Read(t0, source);
            if (width == 16) {
                as.SRLI(t1, t0, 8); as.ANDI(t1, t1, 255);
                as.ANDI(result, t0, 255); as.SLLI(result, result, 8); as.OR(result, result, t1);
            } else {
                as.LI(t2, width == 64 ? 0x00ff00ff00ff00ffULL : 0x00ff00ffULL);
                as.SRLI(t1, t0, 8); as.AND(t1, t1, t2);
                as.AND(t0, t0, t2); as.SLLI(t0, t0, 8); as.OR(t0, t0, t1);
                if (width == 64) {
                    as.LI(t2, 0x0000ffff0000ffffULL);
                    as.SRLI(t1, t0, 16); as.AND(t1, t1, t2);
                    as.AND(t0, t0, t2); as.SLLI(t0, t0, 16); as.OR(t0, t0, t1);
                    as.SRLI(t1, t0, 32); as.SLLI(result, t0, 32); as.OR(result, result, t1);
                } else {
                    as.SLLI(t1, t0, 48); as.SRLI(t1, t1, 32); as.SRLI(result, t0, 16);
                    as.OR(result, result, t1);
                }
            }
        }
    } else {
        const bool word = op == O::CountLeadingZeros32 || op == O::CountTrailingZeros32;
        const bool leading = op == O::CountLeadingZeros32 || op == O::CountLeadingZeros64;
        const u32 bits = word ? 32 : 64;
        if (context.Features().zbb) {
            const auto reg = context.SourceRegister(source, t0);
            if (leading && word) as.CLZW(result, reg);
            else if (leading) as.CLZ(result, reg);
            else if (word) as.CTZW(result, reg);
            else as.CTZ(result, reg);
        } else {
            context.Read(a0, source); context.Mask(a0, bits);
            Label done;
            as.LI(result, bits); as.BEQ(a0, x0, &done); as.MV(result, x0);
            if (leading && word) as.SLLI(a0, a0, 32);
            for (u32 shift = bits / 2; shift; shift /= 2) {
                Label next;
                if (leading) as.SRLI(t1, a0, 64 - shift);
                else as.SLLI(t1, a0, 64 - shift);
                as.BNE(t1, x0, &next);
                as.ADDI(result, result, shift);
                if (leading) as.SLLI(a0, a0, shift);
                else as.SRLI(a0, a0, shift);
                as.Bind(&next);
            }
            as.Bind(&done);
        }
        normalized = true;
    }
    context.Write(inst, result, normalized);
    return true;
}

}  // namespace swift::runtime::backend::riscv64
