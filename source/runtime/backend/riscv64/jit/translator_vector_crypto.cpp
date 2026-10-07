#include "translator.h"

#include <array>

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

namespace {

constexpr u8 Multiply(u8 a, u8 b) {
    u8 result{};
    for (; b; b >>= 1) {
        if (b & 1) result ^= a;
        a = u8((a << 1) ^ ((a & 128) ? 0x1b : 0));
    }
    return result;
}

constexpr auto Substitution() {
    std::array<u8, 256> result{};
    for (u32 byte = 0; byte < 256; ++byte) {
        u8 power = u8(byte), inverse = byte ? 1 : 0;
        for (u32 exponent = 254; exponent; exponent >>= 1) {
            if (exponent & 1) inverse = Multiply(inverse, power);
            power = Multiply(power, power);
        }
        u8 value = inverse ^ 0x63;
        for (u32 rotate = 1; rotate <= 4; ++rotate)
            value ^= u8((inverse << rotate) | (inverse >> (8 - rotate)));
        result[byte] = value;
    }
    return result;
}

constexpr auto kSub = Substitution();
constexpr auto InverseSubstitution() {
    std::array<u8, 256> result{};
    for (u32 byte = 0; byte < 256; ++byte) result[kSub[byte]] = u8(byte);
    return result;
}
constexpr auto kInvSub = InverseSubstitution();

constexpr auto RoundTable(bool inverse) {
    std::array<std::array<u32, 4>, 256> result{};
    constexpr std::array<u8, 4> encrypt{2, 1, 1, 3}, decrypt{14, 9, 13, 11};
    for (u32 byte = 0; byte < 256; ++byte) for (u32 row = 0; row < 4; ++row)
        for (u32 output = 0; output < 4; ++output)
            result[byte][row] |= u32(Multiply(inverse ? kInvSub[byte] : kSub[byte],
                                            (inverse ? decrypt : encrypt)[(output + 4 - row) % 4])) << (output * 8);
    return result;
}
alignas(64) constexpr auto kEncTable = RoundTable(false);
alignas(64) constexpr auto kDecTable = RoundTable(true);

}  // namespace

bool JitTranslator::EmitVectorCrypto(ir::Inst* inst) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::VecAesEnc: case O::VecAesEncLast: case O::VecAesDec: case O::VecAesDecLast:
        case O::VecAesEncFast: case O::VecAesEncLastFast: case O::VecAesDecFast: case O::VecAesDecLastFast:
        case O::VecAesKeygenAssist: case O::VecPclMul:
        case O::VecSha256Msg1: case O::VecSha256Msg2: case O::VecSha256Rnds2: break;
        default: return false;
    }
    auto& as = context.GetMasm();
    const auto& features = context.Features();
    const bool decrypt = op == O::VecAesDec || op == O::VecAesDecLast ||
            op == O::VecAesDecFast || op == O::VecAesDecLastFast;
    const bool last = op == O::VecAesEncLast || op == O::VecAesDecLast ||
            op == O::VecAesEncLastFast || op == O::VecAesDecLastFast;
    const bool round = op >= O::VecAesEnc && op <= O::VecAesDecLastFast;
    // RVV AES decrypt-middle mixes the round key, unlike AESDEC. Apply that
    // instruction with a zero key and then XOR the already transformed key.
    if (round && features.vector_crypto_aes) {
        const auto result = context.ResultVector(inst);
        const auto data = context.SourceVector(inst->GetArg<ir::Value>(0), v1);
        const auto key = context.SourceVector(inst->GetArg<ir::Value>(1), v2);
        context.SetVectorType(32, 4); as.VMV(result, data);
        if (decrypt && !last) { as.VMV(v4, 0); as.VAESDM_VV(result, v4); as.VXOR(result, result, key); }
        else if (decrypt) as.VAESDF_VV(result, key);
        else if (last) as.VAESEF_VV(result, key);
        else as.VAESEM_VV(result, key);
        context.WriteVector(inst, result); return true;
    }
    if (op == O::VecSha256Rnds2 && features.vector_crypto_sha256) {
        const auto result = context.ResultVector(inst);
        const auto dst = context.SourceVector(inst->GetArg<ir::Value>(0), v1);
        const auto src = context.SourceVector(inst->GetArg<ir::Value>(1), v2);
        const auto key = context.SourceVector(inst->GetArg<ir::Value>(2), v4);
        context.SetVectorType(32, 4); as.VMV(result, dst); as.VSHA2CL(result, src, key);
        context.WriteVector(inst, result); return true;
    }
    if (op == O::VecPclMul && features.vector_crypto_clmul) {
        const auto result = context.ResultVector(inst);
        const auto left = context.SourceVector(inst->GetArg<ir::Value>(0), v1);
        const auto right = context.SourceVector(inst->GetArg<ir::Value>(1), v2);
        const auto control = inst->GetArg<ir::Imm>(2).Get();
        context.SetVectorType(64, 2);
        as.VRGATHER(v4, left, u32(control & 1)); as.VRGATHER(v5, right, u32((control >> 4) & 1));
        as.VCLMUL(result, v4, v5); as.VCLMULH(v6, v4, v5); as.VSLIDEUP(result, v6, 1u);
        context.WriteVector(inst, result); return true;
    }
    if (features.vector && (op == O::VecSha256Msg1 || op == O::VecSha256Msg2)) {
        const auto result = context.ResultVector(inst);
        const auto dst = context.SourceVector(inst->GetArg<ir::Value>(0), v1);
        const auto src = context.SourceVector(inst->GetArg<ir::Value>(1), v2);
        const auto sigma = [&](Vec input, bool second) {
            const u32 r0 = second ? 17 : 7, r1 = second ? 19 : 18, shift = second ? 10 : 3;
            as.VSRL(v5, input, r0); as.VSLL(v6, input, 32 - r0); as.VOR(v5, v5, v6);
            as.VSRL(v6, input, r1); as.VSLL(v7, input, 32 - r1); as.VOR(v6, v6, v7);
            as.VXOR(v5, v5, v6); as.VSRL(v6, input, shift); as.VXOR(v5, v5, v6);
        };
        context.SetVectorType(32, 4);
        if (op == O::VecSha256Msg1) {
            as.VSLIDEDOWN(v4, dst, 1u); as.VRGATHER(v5, src, 0u);
            as.VID(v6); as.VMSEQ(v0, v6, 3); as.VMERGE(v4, v4, v5);
            sigma(v4, false); as.VADD(result, dst, v5);
        } else {
            as.VMV(result, dst); as.VSLIDEDOWN(v4, src, 2u);
            context.SetVectorType(32, 2); sigma(v4, true); as.VADD(result, dst, v5);
            sigma(result, true);
            context.SetVectorType(32, 4); as.VMV(v4, 0); as.VSLIDEUP(v4, v5, 2u); as.VADD(result, result, v4);
        }
        context.WriteVector(inst, result); return true;
    }
    const auto result = context.ResultPair(inst);
    const auto rotate = [&](GPR target, GPR source, u32 shift, GPR scratch) {
        if (features.zbb) as.RORIW(target, source, shift);
        else { as.SRLIW(scratch, source, shift); as.SLLIW(target, source, 32 - shift); as.OR(target, target, scratch); }
    };
    const auto sigma = [&](GPR target, GPR source, u32 kind) {
        if (features.zknh) {
            if (kind == 0) as.SHA256SIG0(target, source);
            else if (kind == 1) as.SHA256SIG1(target, source);
            else if (kind == 2) as.SHA256SUM0(target, source);
            else as.SHA256SUM1(target, source);
        } else {
            const std::array<std::array<u32, 3>, 4> shifts{{{7, 18, 3}, {17, 19, 10}, {2, 13, 22}, {6, 11, 25}}};
            const auto& s = shifts[kind];
            rotate(target, source, s[0], t6); rotate(t4, source, s[1], t6); as.XOR(target, target, t4);
            if (kind < 2) as.SRLIW(t4, source, s[2]); else rotate(t4, source, s[2], t6);
            as.XOR(target, target, t4);
        }
    };
    if (round || op == O::VecAesKeygenAssist) {
        context.ReadPart(a0, inst->GetArg<ir::Value>(0), 0);
        context.ReadPart(a1, inst->GetArg<ir::Value>(0), 1);
        if (round) {
            context.ReadPart(a2, inst->GetArg<ir::Value>(1), 0);
            context.ReadPart(a3, inst->GetArg<ir::Value>(1), 1);
            if (decrypt ? features.zknd : features.zkne) {
                if (decrypt && last) { as.AES64DS(result[0], a0, a1); as.AES64DS(result[1], a1, a0); }
                else if (decrypt) { as.AES64DSM(result[0], a0, a1); as.AES64DSM(result[1], a1, a0); }
                else if (last) { as.AES64ES(result[0], a0, a1); as.AES64ES(result[1], a1, a0); }
                else { as.AES64ESM(result[0], a0, a1); as.AES64ESM(result[1], a1, a0); }
                as.XOR(result[0], result[0], a2); as.XOR(result[1], result[1], a3);
                context.WritePair(inst, result); return true;
            }
        }
        const auto rcon = op == O::VecAesKeygenAssist ? u8(inst->GetArg<ir::Imm>(1).Get()) : 0;
        if (op == O::VecAesKeygenAssist && (features.zkne || features.zknd)) {
            for (u32 part = 0; part < 2; ++part) {
                as.AES64KS1I(t2, part ? a1 : a0, 10); context.Mask(t2, 32);
                rotate(t3, t2, 8, t4); as.XORI(t3, t3, rcon); as.SLLI(t3, t3, 32); as.OR(result[part], t2, t3);
            }
            context.WritePair(inst, result); return true;
        }
        context.HostAddress(a4, reinterpret_cast<u64>(last || !round ? (decrypt ? kInvSub.data() : kSub.data())
                                                       : static_cast<const void*>(decrypt ? kDecTable.data() : kEncTable.data())));
        for (u32 column = 0; column < 4; ++column) {
            as.MV(t3, x0);
            for (u32 row = 0; row < 4; ++row) {
                const u32 input = round ? 4 * ((column + (decrypt ? 4 - row : row)) % 4) + row
                        : 4 * (column < 2 ? 1 : 3) + ((row + (column & 1)) % 4);
                if (input % 8) as.SRLI(t2, input < 8 ? a0 : a1, (input % 8) * 8);
                else as.MV(t2, input < 8 ? a0 : a1);
                as.ANDI(t2, t2, 255);
                if (round && !last) { as.SLLI(t2, t2, 4); as.ADD(t2, t2, a4); as.LWU(t2, row * 4, t2); }
                else { as.ADD(t2, t2, a4); as.LBU(t2, 0, t2); if (row) as.SLLI(t2, t2, row * 8); }
                as.XOR(t3, t3, t2);
            }
            if (!round && (column & 1)) as.XORI(t3, t3, rcon);
            if (column & 1) { as.SLLI(t3, t3, 32); as.OR(result[column / 2], result[column / 2], t3); }
            else as.MV(result[column / 2], t3);
        }
        if (round) { as.XOR(result[0], result[0], a2); as.XOR(result[1], result[1], a3); }
    } else if (op == O::VecPclMul) {
        const auto control = inst->GetArg<ir::Imm>(2).Get();
        context.ReadPart(a0, inst->GetArg<ir::Value>(0), control & 1);
        context.ReadPart(a1, inst->GetArg<ir::Value>(1), (control >> 4) & 1);
        if (features.zbc) { as.CLMUL(result[0], a0, a1); as.CLMULH(result[1], a0, a1); }
        else {
            // A fixed, branchless bit body avoids data-dependent loop length.
            // Four bodies per loop amortize the counter/branch overhead.
            as.MV(result[0], x0); as.MV(result[1], x0); as.MV(a2, x0); as.LI(a3, 16);
            Label loop; as.Bind(&loop);
            for (u32 bit = 0; bit < 4; ++bit) {
                as.ANDI(t3, a1, 1); as.NEG(t3, t3); as.AND(t2, a0, t3); as.XOR(result[0], result[0], t2);
                as.AND(t2, a2, t3); as.XOR(result[1], result[1], t2);
                as.SRLI(t2, a0, 63); as.SLLI(a0, a0, 1); as.SLLI(a2, a2, 1); as.OR(a2, a2, t2); as.SRLI(a1, a1, 1);
            }
            as.ADDI(a3, a3, -1); as.BNE(a3, x0, &loop);
        }
    } else if (op != O::VecSha256Rnds2) {
        context.ReadPart(a0, inst->GetArg<ir::Value>(0), 0); context.ReadPart(a1, inst->GetArg<ir::Value>(0), 1);
        context.ReadPart(a2, inst->GetArg<ir::Value>(1), 0); context.ReadPart(a3, inst->GetArg<ir::Value>(1), 1);
        const auto lane = [&](GPR target, GPR low, GPR high, u32 index) {
            if (index & 1) as.SRLI(target, index < 2 ? low : high, 32);
            else as.MV(target, index < 2 ? low : high);
            context.Mask(target, 32);
        };
        // MSG2's newly computed low words feed its upper words. Keep those
        // intermediates in registers; no SSA homes or callbacks are needed.
        for (u32 index = 0; index < 4; ++index) {
            if (op == O::VecSha256Msg1) lane(t3, index == 3 ? a2 : a0, a1, index == 3 ? 0 : index + 1);
            else if (index < 2) lane(t3, a2, a3, index + 2);
            else lane(t3, result[0], x0, index - 2);
            sigma(t2, t3, op == O::VecSha256Msg1 ? 0 : 1);
            lane(t3, a0, a1, index); as.ADDW(t2, t2, t3); context.Mask(t2, 32);
            if (index & 1) { as.SLLI(t2, t2, 32); as.OR(result[index / 2], result[index / 2], t2); }
            else as.MV(result[index / 2], t2);
        }
    } else {
        // A..H fit in a0..a7. Only the two round keys need transient storage.
        context.ReadPart(t2, inst->GetArg<ir::Value>(0), 0); context.ReadPart(t3, inst->GetArg<ir::Value>(0), 1);
        context.ReadPart(t4, inst->GetArg<ir::Value>(1), 0); context.ReadPart(t5, inst->GetArg<ir::Value>(1), 1);
        context.ReadPart(t6, inst->GetArg<ir::Value>(2), 0); as.ADDI(sp, sp, -16); as.SD(t6, 0, sp);
        as.SRLI(a0, t5, 32); as.MV(a1, t5); as.SRLI(a2, t3, 32); as.MV(a3, t3);
        as.SRLI(a4, t4, 32); as.MV(a5, t4); as.SRLI(a6, t2, 32); as.MV(a7, t2);
        for (u32 i = 0; i < 2; ++i) {
            sigma(t2, a4, 3); as.ADDW(t2, t2, a7);
            as.XOR(t3, a5, a6); as.AND(t3, t3, a4); as.XOR(t3, t3, a6); as.ADDW(t2, t2, t3);
            as.LWU(t3, i * 4, sp); as.ADDW(t2, t2, t3);
            sigma(t3, a0, 2);
            as.XOR(t4, a0, a1); as.AND(t4, t4, a2); as.AND(t5, a0, a1); as.XOR(t4, t4, t5); as.ADDW(t3, t3, t4);
            as.MV(a7, a6); as.MV(a6, a5); as.MV(a5, a4); as.ADDW(a4, a3, t2);
            as.MV(a3, a2); as.MV(a2, a1); as.MV(a1, a0); as.ADDW(a0, t2, t3);
        }
        as.ADDI(sp, sp, 16); context.Mask(a5, 32); context.Mask(a1, 32);
        as.SLLI(a4, a4, 32); as.OR(result[0], a4, a5); as.SLLI(a0, a0, 32); as.OR(result[1], a0, a1);
    }
    context.WritePair(inst, result); return true;
}

}  // namespace swift::runtime::backend::riscv64
