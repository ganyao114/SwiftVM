#include "translator.h"

#include <stdexcept>
#include "runtime/common/crc32.h"

namespace swift::runtime::backend::loongarch64 {
namespace {

u64 FieldMask(u64 lsb, u64 width) {
    if (lsb >= 64 || width > 64 - lsb)
        throw std::runtime_error("invalid LoongArch scalar bit field");
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

void Crc(JitContext& context, ir::Inst* inst, bool castagnoli) {
    const auto raw_width = inst->GetArg<ir::Imm>(2).Get();
    if (raw_width != 8 && raw_width != 16 && raw_width != 32 && raw_width != 64)
        throw std::runtime_error("LoongArch CRC width must be 8,16,32,64");
    const auto width = static_cast<u32>(raw_width);
    const auto seed = inst->GetArg<ir::Value>(0), data = inst->GetArg<ir::Value>(1);
    auto& as = context.GetMasm();
    context.Read(t0, seed); context.Mask(t0, 32);
    context.Read(t1, data);
    if (!castagnoli) {
        context.Mask(t0, IeeeCrcInputBits(seed));
        context.Mask(t1, IeeeCrcInputBits(data));
    }
    if (context.Features().crc32) {
        // rj is the message and rk is the prior CRC. The native result is
        // sign-extended from W, so the canonical U32 write truncates it.
        if (castagnoli) {
            switch (width) {
                case 8: as.crcc_w_b_w(t0, t1, t0); break;
                case 16: as.crcc_w_h_w(t0, t1, t0); break;
                case 32: as.crcc_w_w_w(t0, t1, t0); break;
                case 64: as.crcc_w_d_w(t0, t1, t0); break;
            }
        } else {
            switch (width) {
                case 8: as.crc_w_b_w(t0, t1, t0); break;
                case 16: as.crc_w_h_w(t0, t1, t0); break;
                case 32: as.crc_w_w_w(t0, t1, t0); break;
                case 64: as.crc_w_d_w(t0, t1, t0); break;
            }
        }
    } else {
        as.LI(t2, castagnoli ? 0x82f63b78u : 0xedb88320u);
        for (u32 byte = 0; byte < width / 8; ++byte) {
            as.andi(t3, t1, 255); as.xor_(t0, t0, t3);
            for (u32 bit = 0; bit < 8; ++bit) {
                as.andi(t3, t0, 1); as.sub_d(t3, zero, t3);
                as.and_(t3, t3, t2); as.srli_d(t0, t0, 1);
                as.xor_(t0, t0, t3);
            }
            if (byte + 1 != width / 8) as.srli_d(t1, t1, 8);
        }
    }
    context.Write(inst, t0);
}

}  // namespace

void JitTranslator::EmitBitExtract(ir::Inst* inst) {
    const auto lsb = inst->GetArg<ir::Imm>(1).Get(), width = inst->GetArg<ir::Imm>(2).Get();
    (void)FieldMask(lsb, width);
    context.Read(t0, inst->GetArg<ir::Value>(0));
    if (!width) context.GetMasm().MV(t0, zero);
    else context.GetMasm().bstrpick_d(t0, t0, static_cast<u32>(lsb + width - 1), static_cast<u32>(lsb));
    context.Write(inst, t0);
}

void JitTranslator::EmitBitInsert(ir::Inst* inst) {
    const auto lsb = inst->GetArg<ir::Imm>(2).Get(), width = inst->GetArg<ir::Imm>(3).Get();
    (void)FieldMask(lsb, width);
    context.Read(t0, inst->GetArg<ir::Value>(0));
    if (width) {
        context.Read(t1, inst->GetArg<ir::Value>(1));
        context.GetMasm().bstrins_d(t0, t1, static_cast<u32>(lsb + width - 1), static_cast<u32>(lsb));
    }
    context.Write(inst, t0);
}

void JitTranslator::EmitBitClear(ir::Inst* inst) {
    const auto lsb = inst->GetArg<ir::Imm>(1).Get(), width = inst->GetArg<ir::Imm>(2).Get();
    const auto mask = FieldMask(lsb, width);
    context.Read(t0, inst->GetArg<ir::Value>(0));
    auto& as = context.GetMasm();
    as.LI(t1, ~mask); as.and_(t0, t0, t1);
    context.Write(inst, t0);
}

void JitTranslator::EmitByteSwap(ir::Inst* inst) {
    const auto width = inst->GetArg<ir::Imm>(1).Get();
    if (width != 16 && width != 32 && width != 64)
        throw std::runtime_error("invalid LoongArch byte-swap width");
    context.Read(t0, inst->GetArg<ir::Value>(0));
    context.Mask(t0, static_cast<u32>(width));
    auto& as = context.GetMasm();
    if (width == 16) as.revb_2h(t0, t0);
    else if (width == 32) as.revb_2w(t0, t0);
    else as.revb_d(t0, t0);
    context.Write(inst, t0);
}

#define BIT_COUNT(name, mnemonic, bits) \
    void JitTranslator::Emit##name(ir::Inst* inst) { \
        context.Read(t0, inst->GetArg<ir::Value>(0)); \
        context.Mask(t0, bits); \
        context.GetMasm().mnemonic(t0, t0); \
        context.Write(inst, t0); \
    }
BIT_COUNT(CountLeadingZeros64, clz_d, 64)
BIT_COUNT(CountTrailingZeros64, ctz_d, 64)
BIT_COUNT(CountLeadingZeros32, clz_w, 32)
BIT_COUNT(CountTrailingZeros32, ctz_w, 32)
#undef BIT_COUNT

void JitTranslator::EmitPopCount(ir::Inst* inst) {
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0));
    as.LI(t2, 0x5555555555555555ULL);
    as.srli_d(t1, t0, 1); as.and_(t1, t1, t2); as.sub_d(t0, t0, t1);
    as.LI(t2, 0x3333333333333333ULL);
    as.and_(t1, t0, t2); as.srli_d(t0, t0, 2); as.and_(t0, t0, t2); as.add_d(t0, t0, t1);
    as.srli_d(t1, t0, 4); as.add_d(t0, t0, t1);
    as.LI(t2, 0x0f0f0f0f0f0f0f0fULL); as.and_(t0, t0, t2);
    as.LI(t2, 0x0101010101010101ULL); as.mul_d(t0, t0, t2); as.srli_d(t0, t0, 56);
    context.Write(inst, t0);
}

void JitTranslator::EmitLocalParitySet(ir::Inst* inst) {
    auto& as = context.GetMasm();
    context.Read(t0, inst->GetArg<ir::Value>(0));
    as.andi(t0, t0, 255);
    for (u32 shift : {4u, 2u, 1u}) { as.srli_d(t1, t0, shift); as.xor_(t0, t0, t1); }
    as.andi(t0, t0, 1);
    if (!inst->GetArg<ir::Imm>(1).Get()) as.xori(t0, t0, 1);
    context.Write(inst, t0);
}

void JitTranslator::EmitFCmpCondSet(ir::Inst* inst) {
    auto& as = context.GetMasm();
    context.Read(t1, inst->GetArg<ir::Value>(0)); as.andi(t1, t1, 7);
    as.LI(t0, FloatConditionTable(inst->GetArg<ir::Cond>(1)));
    as.srl_d(t0, t0, t1); as.andi(t0, t0, 1);
    context.Write(inst, t0);
}

void JitTranslator::EmitCrc32(ir::Inst* inst) {
    (void)ValidateIeeeCrc32(inst);
    Crc(context, inst, false);
}

void JitTranslator::EmitCrc32c(ir::Inst* inst) { Crc(context, inst, true); }

}  // namespace swift::runtime::backend::loongarch64
