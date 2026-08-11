#include "translator.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

namespace {

bool GcmPclMul2Enabled(const FeatureSet& features) {
    // This is a targeted fast path for the high-high PCLMUL selector used by
    // OpenSSL's Karatsuba GHASH fold.  Keep an exact process-level fallback
    // while defaulting to the architecturally equivalent PMULL2 instruction.
    return features.x86_gcm_pclmul2;
}

}  // namespace

namespace {

// This VIXL snapshot has the AArch64 Crypto Extension encodings and feature
// decoder, but deliberately no assembler entry points for them.  Keep the
// encodings here, next to their IR lowerings, rather than adding a parallel
// assembler API to the vendored dependency.  All operands are Q registers.
constexpr u32 kAese = 0x4E284800;
constexpr u32 kAesd = 0x4E285800;
constexpr u32 kAesmc = 0x4E286800;
constexpr u32 kAesimc = 0x4E287800;
// PMULL.1Q Vd, Vn.1D, Vm.1D.  The 0x00c00000 size field is required for
// the 128-bit polynomial-product form; omitting it emits a different crypto
// encoding and corrupts even the low/low PCLMULQDQ case.
constexpr u32 kPmull = 0x0EE0E000;
constexpr u32 kPmull2 = kPmull | 0x40000000;
constexpr u32 kSha256H = 0x5E004000;
constexpr u32 kSha256H2 = 0x5E005000;
constexpr u32 kSha256Su0 = 0x5E282800;
constexpr u32 kSha256Su1 = 0x5E006000;

u32 Crypto2(u32 opcode, const VRegister& dst, const VRegister& src) {
    return opcode | (src.GetCode() << 5) | dst.GetCode();
}

u32 Crypto3(u32 opcode, const VRegister& dst, const VRegister& left, const VRegister& right) {
    return opcode | (right.GetCode() << 16) | (left.GetCode() << 5) | dst.GetCode();
}

}  // namespace

void JitTranslator::EmitVecAesEnc(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto zero = context.GetTmpV();
    __ Eor(zero.V16B(), zero.V16B(), zero.V16B());
    // x86 AESENC = MC(SR(SB(data))) xor key.  ARM AESE has the key xor
    // before SB/SR, so feed it a zero key and add the x86 round key after
    // AESMC.  This is the established ARM/x86 round-order mapping.
    __ Orr(result.V16B(), data.V16B(), data.V16B());
    masm.dci(Crypto2(kAese, result, zero));
    masm.dci(Crypto2(kAesmc, result, result));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesEncLast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto zero = context.GetTmpV();
    __ Eor(zero.V16B(), zero.V16B(), zero.V16B());
    __ Orr(result.V16B(), data.V16B(), data.V16B());
    masm.dci(Crypto2(kAese, result, zero));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesDec(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto zero = context.GetTmpV();
    __ Eor(zero.V16B(), zero.V16B(), zero.V16B());
    __ Orr(result.V16B(), data.V16B(), data.V16B());
    masm.dci(Crypto2(kAesd, result, zero));
    masm.dci(Crypto2(kAesimc, result, result));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesDecLast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto zero = context.GetTmpV();
    __ Eor(zero.V16B(), zero.V16B(), zero.V16B());
    __ Orr(result.V16B(), data.V16B(), data.V16B());
    masm.dci(Crypto2(kAesd, result, zero));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesEncFast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto zero = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    if (context.IsAesChainTied(inst->Id())) {
        ASSERT_MSG(ReproveAesChainTie(inst),
                   "AES ownership-chain proof diverged at IR {}", inst->Id());
    }
    if (result.GetCode() != data.GetCode()) {
        __ Orr(result.V16B(), data.V16B(), data.V16B());
    }
    masm.dci(Crypto2(kAese, result, zero));
    masm.dci(Crypto2(kAesmc, result, result));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesEncLastFast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto zero = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    if (context.IsAesChainTied(inst->Id())) {
        ASSERT_MSG(ReproveAesChainTie(inst),
                   "AES ownership-chain proof diverged at IR {}", inst->Id());
    }
    if (result.GetCode() != data.GetCode()) {
        __ Orr(result.V16B(), data.V16B(), data.V16B());
    }
    masm.dci(Crypto2(kAese, result, zero));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesDecFast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto zero = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    if (result.GetCode() != data.GetCode()) {
        __ Orr(result.V16B(), data.V16B(), data.V16B());
    }
    masm.dci(Crypto2(kAesd, result, zero));
    masm.dci(Crypto2(kAesimc, result, result));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesDecLastFast(ir::Inst* inst) {
    auto data = context.V(inst->GetArg<ir::Value>(0));
    auto key = context.V(inst->GetArg<ir::Value>(1));
    auto zero = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    if (result.GetCode() != data.GetCode()) {
        __ Orr(result.V16B(), data.V16B(), data.V16B());
    }
    masm.dci(Crypto2(kAesd, result, zero));
    __ Eor(result.V16B(), result.V16B(), key.V16B());
}

void JitTranslator::EmitVecAesKeygenAssist(ir::Inst* inst) {
    auto source = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto sbox_shifted = context.GetTmpV();
    auto zero = context.GetTmpV();
    auto control = context.GetTmpV();
    auto rcon = context.GetTmpV();
    auto scratch = context.GetTmpX();
    const u64 rcon_byte = inst->GetArg<ir::Imm>(1).Get() & 0xff;

    __ Eor(zero.V16B(), zero.V16B(), zero.V16B());
    __ Orr(sbox_shifted.V16B(), source.V16B(), source.V16B());
    masm.dci(Crypto2(kAese, sbox_shifted, zero));
    if (context.GetFeatures().keygen_compact) {
        static_assert(state_offset_named_vector_constants == -16);
        masm.ldur(control.Q(),
                  MemOperand(state, state_offset_named_vector_constants));
        __ Tbl(result.V16B(), sbox_shifted.V16B(), control.V16B());
        __ Mov(scratch, rcon_byte << 32);
        __ Dup(rcon.V2D(), scratch);
        __ Eor(result.V16B(), result.V16B(), rcon.V16B());
        return;
    }
    // AESE applies ShiftRows with its S-box.  This is FEX's established
    // AESKEYGENASSIST swizzle in host-little-endian byte order.  It produces
    // {SubWord(X1), RotWord(SubWord(X1)), SubWord(X3),
    //  RotWord(SubWord(X3))}; do not substitute a generic inverse-ShiftRows
    // mask here, because the required two dword pairs have different source
    // positions after AESE.
    __ Mov(scratch, 0x040B0E010B0E0104ULL);
    __ Fmov(control.D(), scratch);
    __ Mov(scratch, 0x0C0306090306090CULL);
    __ Ins(control.V2D(), 1, scratch);
    __ Tbl(result.V16B(), sbox_shifted.V16B(), control.V16B());
    // The x86 destination dwords are {SubWord(X1),
    // RotWord(SubWord(X1))^rcon, SubWord(X3),
    // RotWord(SubWord(X3))^rcon}; rcon affects byte zero of dwords 1 and 3.
    // In the little-endian vector representation those are byte offsets 4
    // and 12, not byte offsets 0 and 8.  FEX expresses the same placement as
    // (RCON << 32) duplicated across the two 64-bit lanes.
    __ Eor(rcon.V16B(), rcon.V16B(), rcon.V16B());
    __ Mov(scratch, rcon_byte << 32);
    __ Fmov(rcon.D(), scratch);
    __ Ins(rcon.V2D(), 1, scratch);
    __ Eor(result.V16B(), result.V16B(), rcon.V16B());
}

void JitTranslator::EmitVecPclMul(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 select = inst->GetArg<ir::Imm>(2).Get();
    // PCLMULQDQ's high-high form maps exactly to ARM64 PMULL2.  Routing it
    // through two lane DUPs plus PMULL inflated the GHASH Karatsuba fold by
    // two host instructions per 0x11 multiply; FEX takes this PMULL2 form.
    if ((select & 0x11) == 0x11 && GcmPclMul2Enabled(context.GetFeatures())) {
        // The VIXL snapshot exposes Pmull2 but emits an unallocated sentinel
        // for it.  Keep this beside the existing raw PMULL encoding instead.
        masm.dci(Crypto3(kPmull2, result, left, right));
        return;
    }
    // PMULL consumes lane 0.  Duplicate a selected high 64-bit lane when the
    // x86 immediate asks for it; the actual multiplication remains one PMULL.
    if (select & 0x01) {
        auto tmp = context.GetTmpV();
        __ Dup(tmp.V2D(), left.V2D(), 1);
        left = tmp;
    }
    if (select & 0x10) {
        auto tmp = context.GetTmpV();
        __ Dup(tmp.V2D(), right.V2D(), 1);
        right = tmp;
    }
    masm.dci(Crypto3(kPmull, result, left, right));
}

void JitTranslator::EmitVecSha256Msg1(ir::Inst* inst) {
    auto destination = context.V(inst->GetArg<ir::Value>(0));
    auto source = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    __ Orr(result.V16B(), destination.V16B(), destination.V16B());
    masm.dci(Crypto2(kSha256Su0, result, source));
}

void JitTranslator::EmitVecSha256Msg2(ir::Inst* inst) {
    auto destination = context.V(inst->GetArg<ir::Value>(0));
    auto source = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto src1 = context.GetTmpV();
    auto dup = context.GetTmpV();
    // FEX: src1 = ext(dst, dst, 3 dwords); src2 = zip2(dup(dst.d[3]), src).
    __ Ext(src1.V16B(), destination.V16B(), destination.V16B(), 12);
    __ Dup(dup.V4S(), destination.V4S(), 3);
    __ Zip2(dup.V2D(), dup.V2D(), source.V2D());
    __ Eor(result.V16B(), result.V16B(), result.V16B());
    masm.dci(Crypto3(kSha256Su1, result, src1, dup));
}

void JitTranslator::EmitVecSha256Rnds2(ir::Inst* inst) {
    auto destination = context.V(inst->GetArg<ir::Value>(0));
    auto source = context.V(inst->GetArg<ir::Value>(1));
    auto xmm0 = context.V(inst->GetArg<ir::Value>(2));
    auto result = context.V(ir::Value{inst});
    auto abcd = context.GetTmpV();
    auto efgh = context.GetTmpV();
    auto h2 = context.GetTmpV();
    auto key = context.GetTmpV();
    // FEX's x86-to-Arm SHA state mapping: Rev64(Zip2(src, dst)) is abcd,
    // Rev64(Zip1(src, dst)) is efgh.  SHA256RNDS2 reads only XMM0[63:0].
    __ Zip2(abcd.V2D(), source.V2D(), destination.V2D());
    __ Rev64(abcd.V4S(), abcd.V4S());
    __ Zip1(efgh.V2D(), source.V2D(), destination.V2D());
    __ Rev64(efgh.V4S(), efgh.V4S());
    __ Dup(key.V2D(), xmm0.V2D(), 0);
    __ Orr(h2.V16B(), efgh.V16B(), efgh.V16B());
    masm.dci(Crypto3(kSha256H2, h2, abcd, key));
    masm.dci(Crypto3(kSha256H, abcd, efgh, key));
    __ Zip2(result.V2D(), h2.V2D(), abcd.V2D());
    __ Rev64(result.V4S(), result.V4S());
}


#undef __

}  // namespace swift::runtime::backend::arm64
