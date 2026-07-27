// Widening 32x32 -> 64 multiplies: vpmuludq / vpmuldq and their legacy SSE
// twins pmuludq / pmuldq.
//
// ---------------------------------------------------------------------------
// WHY THIS NEEDED A NEW IR OPCODE
// ---------------------------------------------------------------------------
// decoder_avx_fp.cc's "STILL NOT MODELLED" list declined this family with an
// accurate diagnosis: VecMul is same-width, and its ARM64 lowering PANICs on a
// 64-bit lane because AArch64 has no MUL for 2D.  No combination of the
// existing Vec* opcodes computes a 64-bit product from 32-bit inputs, so the
// choice was a new opcode or an approximation, and declining was right.
//
// The opcode added for it is VecMulWiden(a, b, src_lane_bits, is_signed):
// destination lane i is the full 2*src_lane_bits product of source lane 2i of
// each operand, at src_lane_bits of 8, 16 or 32.  It is deliberately not
// x86-shaped -- see the note in runtime/ir/ir.inc.  x86's "the low dword of
// each qword" is the same bits as "the even dwords", and it is the second
// spelling that generalizes: SVE2's UMULLB/SMULLB are this instruction exactly,
// and AArch64 NEON's UMULL/UMULL2 reach it through one existing VecZip.
//
// ---------------------------------------------------------------------------
// CONTRACTS
// ---------------------------------------------------------------------------
// C1: a YMM is never one IR value.  VPMULUDQ/VPMULDQ are defined per 128-bit
//     lane (SDM: DEST[191:128] comes from SRC1[159:128] and SRC2[159:128]), so
//     the 256-bit form is literally two independent V128 operations.
// C3: a VEX.128 form zeroes bits 255:128 (VexWrite128 does it); a VEX.256 form
//     writes both halves (VexWrite256).  The LEGACY SSE forms must do NEITHER
//     -- they leave bits 255:128 alone, which is what XmmWrite does and what
//     the pmuludq/pmuldq rows of avx_mul_test.cpp measure against hardware.
//
// ---------------------------------------------------------------------------
// INTEGRATION -- decoder.h and decoder.cc belong to the main line, so the four
// additions they need are spelled out here as well as applied.  They are
// purely additive and sit alongside the equivalent lines the other VEX families
// already carry; nothing below depends on any other in-flight change.
// ---------------------------------------------------------------------------
//
// (1) decoder.h, private section of X64Decoder, next to the DecodeAvxInt block:
//
//         bool DecodeAvxMul(const VexInsn& v);
//         void DecodeAvxMulWiden(const VexInsn& v, bool is_signed);
//         void DecodeSseMulWiden(_DInst& insn, bool is_signed);
//
// (2) decoder.cc, the VEX dispatch inside Decode(): DecodeAvxMul(vex) joins the
//     `avx_on && (...)` chain alongside DecodeAvxInt / DecodeAvxFp / the other
//     VEX families.
//
//     Order is free: DecodeAvxMul claims only (0F, 66, F4) and (0F38, 66, 28),
//     which neither of the other two families touches, and every path that
//     returns false does so BEFORE emitting any IR, so a decline never leaves a
//     half-built block behind.
//
// (3) decoder.cc, the distorm switch: replace
//
//         case I_PMULUDQ:
//             DecodeMuludq(insn);
//             break;
//
//     with
//
//         case I_PMULUDQ:
//             DecodeSseMulWiden(insn, false);
//             break;
//         case I_PMULDQ:
//             DecodeSseMulWiden(insn, true);
//             break;
//
//     I_PMULDQ (SSE4.1, 66 0F 38 28) had NO case at all and fell through to
//     FALLBACK, which becomes IllegalCode for the guest.  I_PMULUDQ had
//     DecodeMuludq in decoder_sse.cc, which is correct but scalar: two 64-bit
//     ANDs and two 64-bit MULs plus the moves in and out of the vector file.
//     Re-pointing it at the vector opcode makes both forms one lowering.
//     DecodeMuludq is then unreferenced and can be deleted from decoder_sse.cc
//     and decoder.h; leaving it costs only dead code.
//
// (4) source/runtime/frontend/x86/CMakeLists.txt: add decoder_avx_mul.cc.
//     source/tests/CMakeLists.txt: add fuzz/avx_mul_test.cpp.
//
// ---------------------------------------------------------------------------
// KNOWN DEVIATIONS (the same ones the rest of the AVX front end carries)
// ---------------------------------------------------------------------------
//  * A 32-byte memory operand is two 16-byte accesses, so a page-straddling
//    fault is not indivisible and the reported fault address can be base+16.
//  * VEX.W is ignored, which is correct: both opcodes are specified WIG.  The
//    reference data covers W=1 encodings to keep that honest.
//  * VEX.vvvv is not checked against 1111 on the legacy forms; consistent with
//    the rest of the front end, which does not enforce reserved-field #UD.

#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/vex_decoder.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

// One 128-bit lane of (V)PMULUDQ / (V)PMULDQ.
//
// SDM, VPMULUDQ:  DEST[63:0] <- SRC1[31:0] * SRC2[31:0]
//                 DEST[127:64] <- SRC1[95:64] * SRC2[95:64]
// which is source dwords 0 and 2 -- the EVEN ones -- widened to 64 bits.
// VPMULDQ is the same selection with signed widening.
ir::Value MulWiden32(ir::Assembler* as, ir::Value a, ir::Value b, bool is_signed) {
    return as->VecMulWiden(a, b, ir::Imm(32u), ir::Imm(is_signed ? 1u : 0u))
            .SetType(ir::ValueType::V128);
}

}  // namespace

// vpmuludq / vpmuldq: dst = src1 (VEX.vvvv) * src2 (r/m), per 128-bit lane.
void X64Decoder::DecodeAvxMulWiden(const VexInsn& v, bool is_signed) {
    if (v.l) {
        auto a_lo = XmmRead(XmmOf(v.vvvv));
        auto a_hi = YmmHighRead(v.vvvv);
        auto b = VexLoadVec256(v);
        // Both halves are computed before either is written: VexWrite256's
        // destination may be one of the sources.
        auto lo = MulWiden32(assembler, a_lo, b.lo, is_signed);
        auto hi = MulWiden32(assembler, a_hi, b.hi, is_signed);
        VexWrite256(v.reg, lo, hi);
        return;
    }
    auto a = XmmRead(XmmOf(v.vvvv));
    auto b = VexLoadVec(v);
    VexWrite128(v.reg, MulWiden32(assembler, a, b, is_signed));
}

// pmuludq / pmuldq: the two-operand SSE forms, dst = dst * r/m.  XmmWrite
// leaves bits 255:128 of the enclosing YMM untouched, which is the legacy rule
// and the opposite of what VexWrite128 does.
void X64Decoder::DecodeSseMulWiden(_DInst& insn, bool is_signed) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto a = XmmRead(dst);
    auto b = LoadSrcVec(insn, insn.ops[1]);
    XmmWrite(dst, MulWiden32(assembler, a, b, is_signed));
}

bool X64Decoder::DecodeAvxMul(const VexInsn& v) {
    if (!AvxEnabled() || !v.valid || v.pp != VexPP::P66) {
        return false;
    }
    // VEX.W is ignored on purpose: both opcodes are WIG.
    if (v.map == VexMap::Map0F && v.opcode == 0xF4) {
        DecodeAvxMulWiden(v, false);  // vpmuludq
        return true;
    }
    if (v.map == VexMap::Map0F38 && v.opcode == 0x28) {
        DecodeAvxMulWiden(v, true);  // vpmuldq
        return true;
    }
    return false;
}

#undef __

}  // namespace swift::x86
