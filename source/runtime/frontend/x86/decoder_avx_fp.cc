// VEX floating-point instruction handlers (VEX.128 and VEX.256).
//
// Scope: the whole AVX single/double-precision family --
//   arithmetic  vadd/vsub/vmul/vdiv/vsqrt/vmin/vmax  ps, pd, ss, sd
//   logical     vand/vandn/vor/vxor                  ps, pd
//   compare     vcmp ps/pd/ss/sd, vucomis s/d, vcomis s/d
//   convert     vcvtdq2ps, vcvtps2dq, vcvttps2dq, vcvtps2pd, vcvtpd2ps
//   extract     vmovmskps, vmovmskpd
//
// Everything here decodes from VexInsn (vex_decoder.h) and never touches
// distorm's _DInst: the bundled distorm snapshot silently drops VEX.L on 38 of
// the encodings measured, so its operand list cannot be trusted for AVX.
//
// ---------------------------------------------------------------------------
// DECLARATIONS FOR decoder.h  (main line merges these into X64Decoder's
// private section -- this file deliberately does not edit the shared header)
// ---------------------------------------------------------------------------
//
//     // ---- VEX floating point (decoder_avx_fp.cc) ---------------------
//     bool DecodeAvxFp(const VexInsn& v);
//
//     ir::Value VexAddress(const VexInsn& v);
//     ir::Value VexLoadVec(const VexInsn& v);
//     VecHalves VexLoadVec256(const VexInsn& v);
//     ir::Value VexLoadScalar(const VexInsn& v, u32 lane_bits);
//     ir::Value VexLoadScalarVec(const VexInsn& v, u32 lane_bits);
//     void VexWrite128(u32 index, ir::Value value);
//     void VexWrite256(u32 index, ir::Value lo, ir::Value hi);
//
//     void DecodeAvxFpArith(const VexInsn& v, VecFloatOp op, u32 lane_bits);
//     void DecodeAvxFpArithScalar(const VexInsn& v, VecFloatOp op, u32 lane_bits);
//     void DecodeAvxFpBitwise(const VexInsn& v, VecBitwiseOp op);
//     void DecodeAvxFpMinMax(const VexInsn& v, u32 lane_bits, bool maximum, bool scalar);
//     void DecodeAvxFpSqrt(const VexInsn& v, u32 lane_bits, bool scalar);
//     void DecodeAvxFpCmpMask(const VexInsn& v, u32 lane_bits, bool scalar);
//     void DecodeAvxFpComis(const VexInsn& v, u32 lane_bits);
//     void DecodeAvxFpCvtLanewise(const VexInsn& v, u32 kind);
//     void DecodeAvxFpCvtPs2Pd(const VexInsn& v);
//     void DecodeAvxFpCvtPd2Ps(const VexInsn& v);
//     void DecodeAvxFpMovmsk(const VexInsn& v, u32 lane_bits);
//
//     // ---- second wave: scalar moves / conversions / shuffles ---------
//     // (same file; DecodeAvxFp now dispatches Base first, then Fp2)
//     bool DecodeAvxFpBase(const VexInsn& v);
//     bool DecodeAvxFp2(const VexInsn& v);
//
//     void VexWriteHalves(u32 index, ir::Value lo, ir::Value hi);
//     void DecodeAvxFpMovScalar(const VexInsn& v, u32 lane_bits, bool store);
//     void DecodeAvxFpMovLoHi(const VexInsn& v, bool high, bool store);
//     void DecodeAvxFpMovDDup(const VexInsn& v);
//     void DecodeAvxFpCvtScalarFloat(const VexInsn& v, u32 src_bits);
//     void DecodeAvxFpCvtSi2Scalar(const VexInsn& v, u32 dst_bits);
//     void DecodeAvxFpCvtScalar2Si(const VexInsn& v, u32 src_bits, bool truncate);
//     void DecodeAvxFpCvtWiden(const VexInsn& v, u32 kind);
//     void DecodeAvxFpCvtNarrow(const VexInsn& v, u32 kind);
//     void DecodeAvxFpByteShift(const VexInsn& v, bool left);
//     void DecodeAvxFpPTest(const VexInsn& v);
//     void DecodeAvxFpExtract(const VexInsn& v, u32 element_bits);
//     void DecodeAvxFpInsert(const VexInsn& v, u32 element_bits);
//
// The single call site is DecodeAvxFp(v), to be tried from the VEX dispatch
// after the VEX prefix has been decoded and `pc` advanced past the
// instruction.  It returns false for anything it does not model, which must
// trap the block as FALLBACK -- a VEX float executed at the wrong width or
// with the operands swapped silently produces wrong data.
//
// ---------------------------------------------------------------------------
// CONTRACTS
// ---------------------------------------------------------------------------
// C1  A YMM register is never a single IR value.  ARM64 V registers are 128
//     bits wide and RegAlloc maps one IR value onto one host register, so a
//     256-bit operation is two independent V128 operations: bits 127:0 live in
//     ThreadContext64::xmms[i] and bits 255:128 in ymm_high[i].  Every FP
//     operation in this file except the three noted below is lane-wise, so the
//     halves are genuinely independent and the split loses nothing.
//
//     The three that are NOT lane-wise across the 128-bit boundary, and how
//     each is recombined:
//       * vmovmskps/pd ymm -- one GPR result: `lo | hi << lanes_per_half`.
//       * vcvtps2pd ymm, xmm/m128 -- the source is 128 bits and the result
//         256, so the RESULT's high half comes from the SOURCE's high 64 bits.
//       * vcvtpd2ps xmm, ymm/m256 -- the source is 256 bits and the result
//         128, so both source halves feed one 128-bit result (Zip1 over 64-bit
//         lanes concatenates the two 2-float groups).
//
// C3  A VEX.128 instruction zeroes bits 255:128 of its destination.  Every
//     128-bit path below therefore ends in VexWrite128, which does the zeroing.
//     VEX.256 forms write both halves and must NOT zero -- except the two
//     forms whose destination is an XMM even at L=1 (vcvtpd2ps, vmovmskps/pd),
//     which are handled as 128-bit destinations.
//
// ---------------------------------------------------------------------------
// NaN SEMANTICS
// ---------------------------------------------------------------------------
// This is the part of the family ARM64 does not give for free, and it is why
// this file adds no IR opcode: the existing V128 emitters already carry the
// x86 rules, and calling them twice reproduces those rules on both halves.
//
//   * add/sub/mul/div: x86 propagates the SOURCE NaN (operand 1 first,
//     quieted), ARM propagates a default NaN.  translator_alu.cpp's
//     EmitVecFloatNaNFixup patches every VecFAdd/Sub/Mul/Div lane afterwards.
//   * min/max: x86 MINPS/MAXPS return operand 2 whenever EITHER operand is
//     NaN, and also whenever the comparison is false -- so `min(x, NaN)` and
//     `min(NaN, x)` give DIFFERENT answers, and `min(+0,-0)`/`min(-0,+0)` do
//     too.  VecFMinMax is built on Fcmgt+Bsl precisely so it selects operand 2
//     on an unordered compare, matching x86 rather than ARM's FMIN/FMAX.
//     Because VEX makes the form non-destructive, operand 1 is VEX.vvvv here
//     rather than the destination -- getting that backwards is invisible for
//     ordinary values and visible only on NaN and signed zero, which is what
//     the Rosetta reference data in avx_fp_ops.inc exists to catch.
//   * vcmpps with an unordered predicate and vcvt* out-of-range behaviour are
//     likewise already modelled by VecFCmpMask / VecFCvtPacked (the latter
//     produces x86's 0x80000000 "integer indefinite" rather than ARM's
//     saturating FCVT result; this was measured, see avx_fp_test.cpp).
//   * sqrt of a negative is the ONE case still wrong, and it is a backend gap
//     rather than a decode one -- see KNOWN GAPS below.
//
// ---------------------------------------------------------------------------
// KNOWN GAPS (deliberate, not oversights)
// ---------------------------------------------------------------------------
//  * (CLOSED) vcmpps/pd/ss/sd once accepted only imm8 0..7 -- the SSE set --
//    and declined 8..31 because VecFCmpMask masked its predicate with 7 and
//    would have run imm8=17 (LT_OQ) as imm8=1 (LT_OS).  All 32 predicates now
//    decode.  The fix was to stop putting an x86 encoding in the IR at all:
//    VecFCmpMask's predicate is a RELATION SET over {<, ==, >, unordered}, and
//    fp_cmp_predicate.h translates the imm8 into it (both the VEX path and the
//    legacy SSE mnemonics go through that one table).  AVX's signalling-vs-
//    quiet dimension collapses on the way in, because it was measured to
//    change only MXCSR.IE and never the result mask -- see that header.
//    Coverage: source/tests/fuzz/avx_cmp_test.cpp, all 32 predicates x
//    {ps, pd, ss, sd} x {128, 256} against Rosetta.
//  * vsqrtps/pd/ss/sd of a NEGATIVE operand returns the POSITIVE default QNaN
//    (0x7FC00000 / 0x7FF8000000000000) where x86 returns the NEGATIVE "QNaN
//    floating-point indefinite" (0xFFC00000 / 0xFFF8000000000000).  Measured
//    against Rosetta: 112 of the 5856 comparisons in avx_fp_test.cpp differ,
//    all of them exactly this and nothing else.  The cause is that
//    JitTranslator::EmitVecFUnary (backend/arm64/jit/translator_alu.cpp)
//    applies no NaN fixup, unlike EmitVecFAdd/Sub/Mul/Div, whose shared
//    EmitVecFloatNaNFixup already materializes the correct negative constant.
//    It is therefore NOT AVX-specific -- the legacy SSE sqrtps/sqrtpd/sqrtss/
//    sqrtsd handlers go through the same IR opcode and are wrong in the same
//    way.  Fixing it in this file would mean re-deriving the sign per lane in
//    the frontend and would leave SSE broken, so it is left to the backend and
//    pinned by exact count in avx_fp_test.cpp instead.
//  * MXCSR rounding mode, FTZ/DAZ and exception flags are not modelled, as
//    everywhere else in the SSE/AVX front end: results are computed in the
//    host's default round-to-nearest-even with denormals kept.
//  * A 32-byte memory operand is split into two 16-byte accesses, so a
//    page-straddling fault is not indivisible; this is the same deviation
//    decoder_avx.cc documents at length for the 256-bit integer forms.
//  * No alignment check on the aligned forms (none of the opcodes here is an
//    aligned form, so this only matters if the family grows).
//
// ---------------------------------------------------------------------------
// ENCODING NOTE FOR THE VEX LENGTH TABLE
// ---------------------------------------------------------------------------
// Of the opcodes claimed here, exactly one carries an immediate: 0F C2
// (vcmpps/pd/ss/sd).  vex_decoder.cc's ImmediateForm already lists 0xC2 as an
// Imm8 form on the 0F map, and every other opcode in this file (0x2E, 0x2F,
// 0x50, 0x51, 0x54-0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F) is
// immediate-free, matching that table's "everything else on 0F has no
// immediate" default.  This family therefore introduces no length hazard.
//
// The second wave adds these 0F-map opcodes: 0x10, 0x11, 0x12, 0x13, 0x14,
// 0x15, 0x16, 0x17, 0x2A, 0x2C, 0x2D, 0xD7, 0xDA, 0xDE and 0xE6, all
// immediate-free, plus 0xC4 / 0xC5 / 0xC6 and the 0x73 group, all three of
// which kImmediateForms ALREADY lists as Imm8.  On the 0F38 map it adds 0x00,
// 0x0C, 0x17, 0x18, 0x19, 0x1A, 0x29, 0x37, 0x3B and 0x3F (that map is
// uniformly immediate-free) and on 0F3A 0x04, 0x05, 0x06, 0x14, 0x16, 0x18,
// 0x19, 0x20 and 0x22 (that map is uniformly imm8-carrying).  So the table in
// vex_decoder.cc needs NO change for any of them -- checked opcode by opcode,
// because a wrong answer there desynchronizes the whole decode stream rather
// than mis-decoding one instruction.

#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/fp_cmp_predicate.h"
#include "runtime/frontend/x86/vex_decoder.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

// VexInsn register numbers are architectural (0..15) for both the GPR and the
// vector file; distorm's enum happens to use the same order in each block.
_RegisterType GprOf(u32 index, bool is_64bit) {
    return static_cast<_RegisterType>((is_64bit ? R_RAX : R_EAX) + index);
}

}  // namespace

// Fold a VexInsn memory operand into one address value.  The two 128-bit
// halves of a 256-bit access must share this value, so it is computed once and
// offset arithmetically rather than re-derived.
//
// RIP-relative uses `pc`, which the decode loop advances PAST the instruction
// before handlers run -- the same convention GetAddress relies on for the
// legacy path.  Segment overrides are not applied: VexInsn does not carry the
// segment, and an FS/GS-prefixed VEX float does not occur in practice.  If one
// ever does, it decodes here as a flat access, so this is a deviation worth
// knowing about rather than a silent nonexistent case.
ir::Value X64Decoder::VexAddress(const VexInsn& v) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::Address};
    const auto type = is_64bit ? ir::ValueType::U64 : ir::ValueType::U32;
    const auto disp = static_cast<s64>(v.displacement);
    if (v.rip_relative) {
        return __ LoadImm(ir::Imm((pc + static_cast<u64>(disp)) & addr_mask)).SetType(type);
    }
    ir::Value addr;
    bool have = false;
    if (!v.base_none) {
        addr = R(GprOf(v.base, is_64bit));
        have = true;
    }
    if (!v.index_none) {
        auto scaled = R(GprOf(v.index, is_64bit));
        const u32 shift = v.scale == 8 ? 3u : v.scale == 4 ? 2u : v.scale == 2 ? 1u : 0u;
        if (shift != 0) {
            scaled = __ LslImm(scaled, ir::Imm(shift)).SetType(type);
        }
        addr = have ? __ Add(addr, ir::Operand{scaled}) : scaled;
        have = true;
    }
    if (!have) {
        return __ LoadImm(ir::Imm(static_cast<u64>(disp) & addr_mask)).SetType(type);
    }
    if (disp != 0) {
        addr = __ Add(addr, ir::Operand{ir::Imm(static_cast<u64>(disp))});
    }
    return addr.SetType(type);
}

// The r/m operand as one V128 (register form reads the XMM half).
ir::Value X64Decoder::VexLoadVec(const VexInsn& v) {
    if (v.RmIsRegister()) {
        return XmmRead(XmmOf(v.rm));
    }
    return MemLoad(ir::Operand{VexAddress(v)}, ir::ValueType::V128, VexTsoOrdered(v));
}

// The r/m operand as two V128 halves (C1).
X64Decoder::VecHalves X64Decoder::VexLoadVec256(const VexInsn& v) {
    if (v.RmIsRegister()) {
        return {XmmRead(XmmOf(v.rm)), YmmHighRead(v.rm)};
    }
    auto addr = VexAddress(v);
    const bool tso = VexTsoOrdered(v);
    auto lo = MemLoad(ir::Operand{addr}, ir::ValueType::V128, tso);
    auto hi = MemLoad(ir::Operand{__ Add(addr, ir::Operand{ir::Imm(u64(16))})},
                      ir::ValueType::V128,
                      tso);
    return {lo, hi};
}

// The r/m operand of a SCALAR form as a plain integer container: a U64 holding
// the f32 in its low dword, or the f64.  This is what VecFAddScalar32/64 and
// VecFCmp want (they take the raw bits in a GPR, not a vector).
ir::Value X64Decoder::VexLoadScalar(const VexInsn& v, u32 lane_bits) {
    if (v.RmIsRegister()) {
        return XmmLo(XmmOf(v.rm));
    }
    return MemLoad(ir::Operand{VexAddress(v)},
                   lane_bits == 32 ? ir::ValueType::U32 : ir::ValueType::U64,
                   VexTsoOrdered(v));
}

// The r/m operand of a scalar form as a V128 with the value in lane 0, which
// is the shape VecFMinMax / VecFCmpMask / VecFUnary need.  Only lane 0 is ever
// read, so broadcasting the memory form is free of consequence.
ir::Value X64Decoder::VexLoadScalarVec(const VexInsn& v, u32 lane_bits) {
    if (v.RmIsRegister()) {
        return XmmRead(XmmOf(v.rm));
    }
    auto raw = MemLoad(ir::Operand{VexAddress(v)},
                       lane_bits == 32 ? ir::ValueType::U32 : ir::ValueType::U64,
                       VexTsoOrdered(v));
    return __ VecDup64(__ ZeroExtend64(raw)).SetType(ir::ValueType::V128);
}

// C3: a 128-bit destination write zeroes bits 255:128.
void X64Decoder::VexWrite128(u32 index, ir::Value value) {
    XmmWrite(XmmOf(index), value.SetType(ir::ValueType::V128));
    ZeroYmmHigh(index);
}

void X64Decoder::VexWrite256(u32 index, ir::Value lo, ir::Value hi) {
    XmmWrite(XmmOf(index), lo.SetType(ir::ValueType::V128));
    YmmHighWrite(index, hi.SetType(ir::ValueType::V128));
}

// vaddps/pd, vsubps/pd, vmulps/pd, vdivps/pd -- dst = vvvv OP r/m, lane-wise.
void X64Decoder::DecodeAvxFpArith(const VexInsn& v, VecFloatOp op, u32 lane_bits) {
    const auto lanes = ir::Imm(lane_bits);
    const auto emit = [&](ir::Value a, ir::Value b) {
        switch (op) {
            case VecFloatOp::Add: return __ VecFAdd(a, b, lanes);
            case VecFloatOp::Sub: return __ VecFSub(a, b, lanes);
            case VecFloatOp::Mul: return __ VecFMul(a, b, lanes);
            case VecFloatOp::Div: return __ VecFDiv(a, b, lanes);
        }
        PANIC("unreachable float op");
        return ir::Value{};
    };
    if (!v.Is256()) {
        VexWrite128(v.reg, emit(XmmRead(XmmOf(v.vvvv)), VexLoadVec(v)));
        return;
    }
    // Named locals rather than nested calls: argument evaluation order is
    // unspecified and the emitted IR must be reproducible.
    auto a_lo = XmmRead(XmmOf(v.vvvv));
    auto a_hi = YmmHighRead(v.vvvv);
    auto b = VexLoadVec256(v);
    auto lo = emit(a_lo, b.lo);
    auto hi = emit(a_hi, b.hi);
    VexWrite256(v.reg, lo, hi);
}

// vaddss/sd, vsubss/sd, vmulss/sd, vdivss/sd.
//
// dst[lane 0] = src1[lane 0] OP src2[lane 0]; the REST OF src1 is copied
// through unchanged (VecFAddScalar32/64 do that copy), and bits 255:128 are
// zeroed by C3.  src1 is VEX.vvvv, so a scalar VEX op leaves the upper lanes
// of the *third* operand's register alone, not of the destination.
void X64Decoder::DecodeAvxFpArithScalar(const VexInsn& v, VecFloatOp op, u32 lane_bits) {
    auto left = XmmRead(XmmOf(v.vvvv));
    auto right = VexLoadScalar(v, lane_bits);
    ir::Value result;
    if (lane_bits == 64) {
        switch (op) {
            case VecFloatOp::Add: result = __ VecFAddScalar64(left, right); break;
            case VecFloatOp::Sub: result = __ VecFSubScalar64(left, right); break;
            case VecFloatOp::Mul: result = __ VecFMulScalar64(left, right); break;
            case VecFloatOp::Div: result = __ VecFDivScalar64(left, right); break;
        }
    } else {
        switch (op) {
            case VecFloatOp::Add: result = __ VecFAddScalar32(left, right); break;
            case VecFloatOp::Sub: result = __ VecFSubScalar32(left, right); break;
            case VecFloatOp::Mul: result = __ VecFMulScalar32(left, right); break;
            case VecFloatOp::Div: result = __ VecFDivScalar32(left, right); break;
        }
    }
    VexWrite128(v.reg, result);
}

// vandps/pd, vandnps/pd, vorps/pd, vxorps/pd.  These are bit operations; the
// ps/pd distinction is purely the encoding's mandatory prefix and carries no
// semantic difference at all (no NaN handling, no lane width).
void X64Decoder::DecodeAvxFpBitwise(const VexInsn& v, VecBitwiseOp op) {
    const auto emit = [&](ir::Value a, ir::Value b) {
        switch (op) {
            case VecBitwiseOp::Xor: return __ VecXor(a, b);
            case VecBitwiseOp::Or: return __ VecOr(a, b);
            case VecBitwiseOp::And: return __ VecAnd(a, b);
            // VANDNPS: dst = (NOT src1) AND src2.  VecAndNot(x, y) is
            // x AND NOT y, so the operands go in swapped.
            case VecBitwiseOp::AndNot: return __ VecAndNot(b, a);
        }
        PANIC("unreachable bitwise op");
        return ir::Value{};
    };
    if (!v.Is256()) {
        VexWrite128(v.reg, emit(XmmRead(XmmOf(v.vvvv)), VexLoadVec(v)));
        return;
    }
    auto a_lo = XmmRead(XmmOf(v.vvvv));
    auto a_hi = YmmHighRead(v.vvvv);
    auto b = VexLoadVec256(v);
    auto lo = emit(a_lo, b.lo);
    auto hi = emit(a_hi, b.hi);
    VexWrite256(v.reg, lo, hi);
}

// vminps/pd/ss/sd, vmaxps/pd/ss/sd.
//
// x86 defines MIN/MAX as "if (src1 OP src2) then src1 else src2", which makes
// them NOT commutative: both NaN operand orders and both signed-zero orders
// return operand 2.  VecFMinMax reproduces exactly that (Fcmgt + Bsl, so an
// unordered compare falls through to operand 2), which is why this maps onto
// one IR op instead of needing a fixup pass.  Operand 1 is VEX.vvvv.
void X64Decoder::DecodeAvxFpMinMax(const VexInsn& v, u32 lane_bits, bool maximum, bool scalar) {
    const auto bits = ir::Imm(lane_bits);
    const auto max = ir::Imm(u32(maximum));
    const auto sc = ir::Imm(u32(scalar));
    if (scalar) {
        auto left = XmmRead(XmmOf(v.vvvv));
        auto right = VexLoadScalarVec(v, lane_bits);
        VexWrite128(v.reg, __ VecFMinMax(left, right, bits, max, sc));
        return;
    }
    if (!v.Is256()) {
        auto left = XmmRead(XmmOf(v.vvvv));
        auto right = VexLoadVec(v);
        VexWrite128(v.reg, __ VecFMinMax(left, right, bits, max, sc));
        return;
    }
    auto a_lo = XmmRead(XmmOf(v.vvvv));
    auto a_hi = YmmHighRead(v.vvvv);
    auto b = VexLoadVec256(v);
    auto lo = __ VecFMinMax(a_lo, b.lo, bits, max, sc);
    auto hi = __ VecFMinMax(a_hi, b.hi, bits, max, sc);
    VexWrite256(v.reg, lo, hi);
}

// vsqrtps/pd (two operands: dst, r/m) and vsqrtss/sd (three: dst, vvvv, r/m).
// The scalar forms merge lanes 127:32 (or 127:64) from src1 = VEX.vvvv.
void X64Decoder::DecodeAvxFpSqrt(const VexInsn& v, u32 lane_bits, bool scalar) {
    const auto bits = ir::Imm(lane_bits);
    const auto kind = ir::Imm(0u);  // 0 = sqrt
    if (scalar) {
        auto source = VexLoadScalarVec(v, lane_bits);
        auto merge = XmmRead(XmmOf(v.vvvv));
        VexWrite128(v.reg, __ VecFUnary(source, merge, bits, kind, ir::Imm(1u)));
        return;
    }
    if (!v.Is256()) {
        auto source = VexLoadVec(v);
        // `merge` is unread when scalar == 0; pass the source rather than a
        // fresh uniform load so no dead value reaches RegAlloc.
        VexWrite128(v.reg, __ VecFUnary(source, source, bits, kind, ir::Imm(0u)));
        return;
    }
    auto s = VexLoadVec256(v);
    auto lo = __ VecFUnary(s.lo, s.lo, bits, kind, ir::Imm(0u));
    auto hi = __ VecFUnary(s.hi, s.hi, bits, kind, ir::Imm(0u));
    VexWrite256(v.reg, lo, hi);
}

// vcmpps/pd/ss/sd.  imm8 selects the predicate; the result is an all-ones /
// all-zeros mask per lane (scalar forms write lane 0 and copy src1's rest).
//
// AVX widened the predicate field to five bits and defined all 32 encodings,
// so every imm8 0..31 is accepted.  The imm8 is translated into VecFCmpMask's
// RELATION SET here rather than passed through: the IR deliberately does not
// speak x86 imm8, and imm8 bit 4 (signalling vs quiet) is deliberately dropped
// because it changes only MXCSR, which SwiftVM does not model -- both measured
// on hardware and argued in fp_cmp_predicate.h.
void X64Decoder::DecodeAvxFpCmpMask(const VexInsn& v, u32 lane_bits, bool scalar) {
    const auto bits = ir::Imm(lane_bits);
    const auto pred = ir::Imm(X86CmpPredicateToRelation(v.imm8));
    const auto sc = ir::Imm(u32(scalar));
    if (scalar) {
        auto left = XmmRead(XmmOf(v.vvvv));
        auto right = VexLoadScalarVec(v, lane_bits);
        VexWrite128(v.reg, __ VecFCmpMask(left, right, bits, pred, sc));
        return;
    }
    if (!v.Is256()) {
        auto left = XmmRead(XmmOf(v.vvvv));
        auto right = VexLoadVec(v);
        VexWrite128(v.reg, __ VecFCmpMask(left, right, bits, pred, sc));
        return;
    }
    auto a_lo = XmmRead(XmmOf(v.vvvv));
    auto a_hi = YmmHighRead(v.vvvv);
    auto b = VexLoadVec256(v);
    auto lo = __ VecFCmpMask(a_lo, b.lo, bits, pred, sc);
    auto hi = __ VecFCmpMask(a_hi, b.hi, bits, pred, sc);
    VexWrite256(v.reg, lo, hi);
}

// vucomiss/sd and vcomiss/sd: scalar ordered compare into EFLAGS.
//
// UCOMIS and COMIS differ only in which NaN raises #IA (quiet vs signalling);
// SwiftVM models no FP exceptions, so both produce the identical flags and
// share this handler, exactly as the legacy SSE path already does.
//
// The flag-store sequence is duplicated from DecodeUcomis rather than shared:
// that function's signature is bound to distorm's _DInst and decoder_sse.cc is
// not this file's to change.  The two must stay in step; VecFCmp is the single
// source of the compare itself, so only the EFLAGS plumbing is repeated.
void X64Decoder::DecodeAvxFpComis(const VexInsn& v, u32 lane_bits) {
    auto a = XmmLo(XmmOf(v.reg));
    auto b = VexLoadScalar(v, lane_bits);
    auto f = __ VecFCmp(a, b, ir::Imm(lane_bits)).SetType(ir::ValueType::U64);
    // OF / SF / AF cleared; ZF / PF / CF from the compare result.
    __ ClearFlags(ir::Flags::Overflow | ir::Flags::Negate | ir::Flags::AuxiliaryCarry);
    auto one = __ LoadImm(ir::Imm(u64(1)));
    auto zero = __ LoadImm(ir::Imm(u64(0)));
    auto pf = __ And(__ LsrImm(f, ir::Imm(1u)), ir::Operand{ir::Imm(u64(1))});
    auto pv = __ Select(__ TestNotZero(pf), zero, one);
    __ SaveFlags(__ Or(pv, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Parity);
    // ZF after PF: the logical flag producer would otherwise overwrite parity.
    auto zf = __ And(__ LsrImm(f, ir::Imm(2u)), ir::Operand{ir::Imm(u64(1))});
    auto zv = __ Select(__ TestNotZero(zf), zero, one);
    __ SaveFlags(__ Or(zv, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Zero);
    auto cf = __ And(f, ir::Operand{ir::Imm(u64(1))});
    auto cv = __ Add(__ LoadImm(ir::Imm(~u64(0))), ir::Operand{cf});
    __ SaveFlags(cv, ir::Flags::Carry);
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
}

// vcvtdq2ps, vcvtps2dq, vcvttps2dq: source and destination are the same width
// and the conversion is per 32-bit lane, so the two halves are independent.
void X64Decoder::DecodeAvxFpCvtLanewise(const VexInsn& v, u32 kind) {
    const auto k = ir::Imm(kind);
    if (!v.Is256()) {
        VexWrite128(v.reg, __ VecFCvtPacked(VexLoadVec(v), k));
        return;
    }
    auto s = VexLoadVec256(v);
    auto lo = __ VecFCvtPacked(s.lo, k);
    auto hi = __ VecFCvtPacked(s.hi, k);
    VexWrite256(v.reg, lo, hi);
}

// vcvtps2pd: the source is HALF the width of the destination.
//   L=0: xmm <- xmm/m64   (2 floats -> 2 doubles)
//   L=1: ymm <- xmm/m128  (4 floats -> 4 doubles)
// VecFCvtPacked kind 6 widens the LOW two floats of its V128 argument, so the
// 256-bit form's upper result half needs the source's upper 64 bits moved down
// first -- this is one of the three places the halves are not independent.
void X64Decoder::DecodeAvxFpCvtPs2Pd(const VexInsn& v) {
    const auto k = ir::Imm(6u);
    if (!v.Is256()) {
        // 64-bit source.  A register source supplies it in lanes 1:0 already;
        // a memory source is 8 bytes, so it is loaded as a qword and dropped
        // into lane 0 (lane 1 is never read by kind 6's Fcvtl).
        auto source = v.RmIsRegister() ? XmmRead(XmmOf(v.rm))
                                       : __ VecDup64(MemLoad(ir::Operand{VexAddress(v)},
                                                            ir::ValueType::U64,
                                                            VexTsoOrdered(v)))
                                                 .SetType(ir::ValueType::V128);
        VexWrite128(v.reg, __ VecFCvtPacked(source, k));
        return;
    }
    auto source = VexLoadVec(v);  // 128-bit source even though the result is 256
    auto lo = __ VecFCvtPacked(source, k);
    auto upper = __ VecDup64(__ VecExtract64(source, ir::Imm(1u)).SetType(ir::ValueType::U64))
                         .SetType(ir::ValueType::V128);
    auto hi = __ VecFCvtPacked(upper, k);
    VexWrite256(v.reg, lo, hi);
}

// vcvtpd2ps: the source is TWICE the width of the destination.
//   L=0: xmm <- xmm/m128  (2 doubles -> 2 floats, result[127:64] = 0)
//   L=1: xmm <- ymm/m256  (4 doubles -> 4 floats)
// The destination is an XMM in BOTH cases, so C3 applies to both: bits 255:128
// of the destination are zeroed even at VEX.L=1.
void X64Decoder::DecodeAvxFpCvtPd2Ps(const VexInsn& v) {
    const auto k = ir::Imm(7u);
    if (!v.Is256()) {
        // kind 7 is Fcvtn, which writes the low 64 bits and zeroes the rest --
        // the x86 requirement that result[127:64] be zero comes for free.
        VexWrite128(v.reg, __ VecFCvtPacked(VexLoadVec(v), k));
        return;
    }
    auto s = VexLoadVec256(v);
    auto lo = __ VecFCvtPacked(s.lo, k);  // floats for lanes 1:0, in bits 63:0
    auto hi = __ VecFCvtPacked(s.hi, k);  // floats for lanes 3:2, in bits 63:0
    // Zip1 over 64-bit lanes = {lo[63:0], hi[63:0]}, i.e. the four floats in
    // architectural order.
    auto packed = __ VecZip(lo, hi, ir::Imm(64u), ir::Imm(0u));
    VexWrite128(v.reg, packed);
}

// vmovmskps r32, xmm/ymm and vmovmskpd r32, xmm/ymm.
//
// The one FP form whose halves are not independent: a 256-bit source produces
// ONE GPR whose upper bits come from the upper half.  ps has 4 lanes per 128
// bits, pd has 2, so the high half's mask shifts left by 4 or 2 respectively.
// (Compare vpmovmskb, which shifts by 16.)  The destination is always written
// as a 32-bit register, which zeroes the upper 32 bits of the GPR.
void X64Decoder::DecodeAvxFpMovmsk(const VexInsn& v, u32 lane_bits) {
    const auto width = ir::Imm(lane_bits);
    auto mask = __ VecMovMask(XmmRead(XmmOf(v.rm)), width).SetType(ir::ValueType::U32);
    if (v.Is256()) {
        const u32 lanes_per_half = 128 / lane_bits;
        auto high = __ VecMovMask(YmmHighRead(v.rm), width).SetType(ir::ValueType::U32);
        auto shifted = __ LslImm(high, ir::Imm(lanes_per_half)).SetType(ir::ValueType::U32);
        mask = __ Or(mask, ir::Operand{shifted}).SetType(ir::ValueType::U32);
    }
    // Always the 32-bit register form: VEX.W is ignored by VMOVMSKPS/PD, and a
    // 32-bit GPR write zero-extends to 64 bits in long mode.
    R(static_cast<_RegisterType>(R_EAX + v.reg), mask);
}

// Entry point.  The original arithmetic/compare/convert family lives in
// DecodeAvxFpBase (0F map only); everything added by the second wave lives in
// DecodeAvxFp2.  Base is tried first and its `return false` paths -- including
// the ones it takes for an opcode it recognizes but declines, such as F3/F2
// 0x5A -- fall through to Fp2 rather than trapping the block.
bool X64Decoder::DecodeAvxFp(const VexInsn& v) {
    if (!AvxEnabled() || !v.valid) {
        return false;
    }
    if (v.map == VexMap::Map0F && DecodeAvxFpBase(v)) {
        return true;
    }
    return DecodeAvxFp2(v);
}

bool X64Decoder::DecodeAvxFpBase(const VexInsn& v) {
    const bool packed_single = v.pp == VexPP::None;
    const bool packed_double = v.pp == VexPP::P66;
    const bool scalar_single = v.pp == VexPP::PF3;
    const bool scalar_double = v.pp == VexPP::PF2;
    const bool packed = packed_single || packed_double;
    const bool scalar = scalar_single || scalar_double;
    // Lane width for the packed pair, and separately for the scalar pair.
    const u32 packed_bits = packed_double ? 64u : 32u;
    const u32 scalar_bits = scalar_double ? 64u : 32u;

    switch (v.opcode) {
        // ---- three-operand arithmetic ------------------------------------
        case 0x58:  // vadd
        case 0x59:  // vmul
        case 0x5C:  // vsub
        case 0x5E:  // vdiv
        {
            const VecFloatOp op = v.opcode == 0x58   ? VecFloatOp::Add
                                  : v.opcode == 0x59 ? VecFloatOp::Mul
                                  : v.opcode == 0x5C ? VecFloatOp::Sub
                                                     : VecFloatOp::Div;
            if (packed) {
                DecodeAvxFpArith(v, op, packed_bits);
            } else {
                DecodeAvxFpArithScalar(v, op, scalar_bits);
            }
            return true;
        }
        // ---- min / max ---------------------------------------------------
        case 0x5D:  // vmin
        case 0x5F:  // vmax
        {
            const bool maximum = v.opcode == 0x5F;
            DecodeAvxFpMinMax(v, packed ? packed_bits : scalar_bits, maximum, scalar);
            return true;
        }
        // ---- sqrt --------------------------------------------------------
        case 0x51:
            if (packed && v.vvvv_valid) {
                // Packed sqrt is two-operand; VEX.vvvv must encode 1111b.
                return false;
            }
            DecodeAvxFpSqrt(v, packed ? packed_bits : scalar_bits, scalar);
            return true;
        // ---- bitwise (ps/pd only) ----------------------------------------
        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57: {
            if (!packed) {
                return false;  // no ss/sd form exists
            }
            const VecBitwiseOp op = v.opcode == 0x54   ? VecBitwiseOp::And
                                    : v.opcode == 0x55 ? VecBitwiseOp::AndNot
                                    : v.opcode == 0x56 ? VecBitwiseOp::Or
                                                       : VecBitwiseOp::Xor;
            DecodeAvxFpBitwise(v, op);
            return true;
        }
        // ---- compare into a mask -----------------------------------------
        case 0xC2:
            if (!v.has_imm8) {
                return false;
            }
            // All 32 AVX predicates are accepted.  imm8 bits 7:5 are reserved
            // and IGNORED rather than #UD -- the SDM's operation section reads
            // imm8[4:0] and lists no exception for them, and Rosetta was
            // measured executing imm8 = 0x20/0x40/0x80/0xE1/0xFF as 0x00/0x00/
            // 0x00/0x01/0x1F.  X86CmpPredicateToRelation does that masking.
            DecodeAvxFpCmpMask(v, packed ? packed_bits : scalar_bits, scalar);
            return true;
        // ---- compare into EFLAGS -----------------------------------------
        case 0x2E:  // vucomis
        case 0x2F:  // vcomis
            if (!packed || v.vvvv_valid) {
                return false;  // two-operand, ss/sd selected by 66 not F3/F2
            }
            DecodeAvxFpComis(v, packed_bits);
            return true;
        // ---- sign-bit extraction -----------------------------------------
        case 0x50:
            if (!packed || v.vvvv_valid || !v.RmIsRegister()) {
                return false;  // the memory source form is #UD
            }
            DecodeAvxFpMovmsk(v, packed_bits);
            return true;
        // ---- conversions -------------------------------------------------
        case 0x5B:
            if (v.vvvv_valid) {
                return false;
            }
            if (packed_single) {
                DecodeAvxFpCvtLanewise(v, 0);  // vcvtdq2ps
            } else if (packed_double) {
                DecodeAvxFpCvtLanewise(v, 2);  // vcvtps2dq (round per MXCSR)
            } else if (scalar_single) {
                DecodeAvxFpCvtLanewise(v, 3);  // vcvttps2dq (truncate)
            } else {
                return false;  // F2 5B is not an AVX encoding
            }
            return true;
        case 0x5A:
            if (v.vvvv_valid) {
                return false;
            }
            if (packed_single) {
                DecodeAvxFpCvtPs2Pd(v);
            } else if (packed_double) {
                DecodeAvxFpCvtPd2Ps(v);
            } else {
                // F3/F2 5A are vcvtss2sd / vcvtsd2ss: scalar conversions, not
                // part of this family.  Declined, so the block traps rather
                // than running the packed path at the wrong width.
                return false;
            }
            return true;
        default:
            return false;
    }
}

// ===========================================================================
// SECOND WAVE
// ===========================================================================
//
// WHAT THIS ADDS AND WHY IT IS IN THIS FILE
// -----------------------------------------
// The first wave covered the packed/scalar ARITHMETIC of the AVX float family
// but not the MOVES, the SCALAR CONVERSIONS or the SHUFFLES -- which is the
// half a compiler actually emits first.  `-mavx` turns every `float x = *p;`
// into vmovss, every `(double)f` into vcvtss2sd and every `_mm_shuffle_ps`
// into vshufps, so a guest built with AVX enabled hits one of these long
// before it hits vaddps.  Since an unimplemented encoding is not "slow" but
// FATAL (InterruptReason::FALLBACK -> ExitReason::IllegalCode -> the guest
// process is terminated, there is no interpreter fallback), these gaps are
// what decides whether CPUID may advertise AVX at all.
//
// Three groups here are not floating point in the ISA-manual sense:
//
//   * vpmovmskb / vpshufb / vpminub / vpmaxub / vpminud / vpmaxud.  Their
//     VEX.256 forms already worked (through the legacy distorm path in
//     decoder_avx.cc) while their VEX.128 forms were fatal.  That asymmetry is
//     the nastiest shape a gap can take: the wide form passing is exactly what
//     makes the narrow form look covered.  Both widths are claimed here so the
//     two can no longer drift, which also retires the distorm entries for
//     them.
//   * vpsrldq / vpslldq (0F 73 /3 and /7) -- byte shifts, declined by
//     decoder_avx_int.cc's shift group because they are a different operation
//     from the lane shifts it handles.
//   * vptest, vpcmpeqq/vpcmpgtq, vpermilps/pd, vpinsr*/vpextr*.
//
// They live here rather than in decoder_avx_int.cc only because that file
// belongs to another line of work; DecodeAvxInt is tried first and declines
// every one of them, at both widths, before any IR is emitted.
//
// STILL NOT MODELLED (deliberate, with the reason)
// ------------------------------------------------
//  * vpmuldq / vpmuludq (0F38 28, 0F F4).  These are WIDENING 32x32 -> 64
//    multiplies of the even lanes.  The IR has VecMul only at equal width and
//    its backend rejects a 64-bit lane outright (EmitVecMul PANICs on anything
//    but 8/16/32, because AArch64 has no MUL for 2D), and there is no
//    Umull/Smull opcode to reach.  Expressing them would need a new IR
//    opcode, which is out of scope, and no combination of the existing ones
//    computes a 64-bit product -- so they are declined rather than
//    approximated.
//  * FMA3, the gather family, vroundps/pd, vdpps, vmaskmov: out of scope.
//  * vcmp* imm8 8..31: unchanged, see KNOWN GAPS above.
//  * vpermilpd's VARIABLE form (0F38 0D).  The imm8 form is here; the variable
//    form's selector is bit 1 of each 64-bit control lane and is expressible,
//    but it is vanishingly rare next to the imm8 form and is left out rather
//    than shipped untested.
//
// C3 APPLIES TO EVERY REGISTER-DESTINATION FORM BELOW.  The scalar moves are
// where it bites hardest: `vmovss xmm0, [rax]` zeroes bits 255:32 of ymm0,
// which is 28 bytes of collateral no oracle can see without reading the YMM
// back.  avx_fp2_test.cpp reads all 32 bytes of a POISONED destination against
// Rosetta reference data for exactly that reason.

namespace {

// A 128-bit constant, as {lo qword, hi qword}.  There is no "materialize
// vector immediate" IR opcode, so it is built from scalar immediates.
ir::Value FpConst128(ir::Assembler* as, u64 lo, u64 hi) {
    auto low = as->VecDup64(as->LoadImm(ir::Imm(lo)).SetType(ir::ValueType::U64))
                       .SetType(ir::ValueType::V128);
    if (lo == hi) {
        return low;
    }
    auto high = as->VecDup64(as->LoadImm(ir::Imm(hi)).SetType(ir::ValueType::U64))
                        .SetType(ir::ValueType::V128);
    return as->VecZip(low, high, ir::Imm(64u), ir::Imm(0u)).SetType(ir::ValueType::V128);
}

// Parameter packing for the DecodeAvxIntBinary / DecodeAvxIntUnary callbacks,
// matching decoder_avx_int.cc's convention (lane width low, flag high).
constexpr u32 FpPack(u32 lane, u32 flag = 0) { return lane | (flag << 16); }
constexpr u32 FpLane(u32 param) { return param & 0xFFFF; }
constexpr u32 FpFlag(u32 param) { return param >> 16; }

ir::Value FpOpMin(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecMin(a, b, ir::Imm(FpLane(param)), ir::Imm(FpFlag(param)))
            .SetType(ir::ValueType::V128);
}

ir::Value FpOpMax(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecMax(a, b, ir::Imm(FpLane(param)), ir::Imm(FpFlag(param)))
            .SetType(ir::ValueType::V128);
}

ir::Value FpOpZip(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecZip(a, b, ir::Imm(FpLane(param)), ir::Imm(FpFlag(param)))
            .SetType(ir::ValueType::V128);
}

ir::Value FpOpCmpEq(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecCmpEq(a, b, ir::Imm(FpLane(param))).SetType(ir::ValueType::V128);
}

ir::Value FpOpCmpGt(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecCmpGt(a, b, ir::Imm(FpLane(param))).SetType(ir::ValueType::V128);
}

// VPSHUFB: dst[i] = ctrl[i] bit 7 ? 0 : table[ctrl[i] & 15], per 128-bit lane.
// VecTableLookup8 masks its control with 0x8F, which is that rule exactly --
// including the "index is taken modulo 16 WITHIN the lane" part that makes the
// 256-bit form two independent lookups rather than one 32-entry table.
ir::Value FpOpShufB(ir::Assembler* as, ir::Value table, ir::Value control, u32, u32) {
    return as->VecTableLookup8(table, control).SetType(ir::ValueType::V128);
}

// VSHUFPS: result lanes 1:0 come from src1, lanes 3:2 from src2, each selected
// by its own 2-bit field of imm8.  VecShuffle32 permutes WITHIN one register,
// so the two sources are shuffled separately and their low halves zipped: the
// Zip1-over-64-bit of {a[i0], a[i1], -, -} and {b[i2], b[i3], -, -} is exactly
// {a[i0], a[i1], b[i2], b[i3]}.  Passing imm >> 4 for the second shuffle puts
// imm's bits 5:4 and 7:6 into the lane-0 and lane-1 selector positions.
ir::Value FpOpShufPs(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    auto from_a = as->VecShuffle32(a, ir::Imm(param & 0xFFu)).SetType(ir::ValueType::V128);
    auto from_b = as->VecShuffle32(b, ir::Imm((param >> 4) & 0x0Fu)).SetType(ir::ValueType::V128);
    return as->VecZip(from_a, from_b, ir::Imm(64u), ir::Imm(0u)).SetType(ir::ValueType::V128);
}

// VSHUFPD: one bit per result qword.  The 256-bit form uses a DIFFERENT pair of
// imm8 bits for its upper 128-bit lane (bits 3:2 rather than 1:0), which is why
// this reads `half` -- the only op in the group that is not lane-uniform.
ir::Value FpOpShufPd(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32 half) {
    const u32 sel_a = (param >> (half * 2)) & 1u;
    const u32 sel_b = (param >> (half * 2 + 1)) & 1u;
    auto pick_a = as->VecDup64(as->VecExtract64(a, ir::Imm(sel_a)).SetType(ir::ValueType::U64))
                          .SetType(ir::ValueType::V128);
    auto pick_b = as->VecDup64(as->VecExtract64(b, ir::Imm(sel_b)).SetType(ir::ValueType::U64))
                          .SetType(ir::ValueType::V128);
    return as->VecZip(pick_a, pick_b, ir::Imm(64u), ir::Imm(0u)).SetType(ir::ValueType::V128);
}

// VPERMILPS with a REGISTER control: each 32-bit control lane's bits 1:0 pick a
// dword of src1 within the same 128-bit lane.  Turned into a byte-granular Tbl
// index vector:  idx[4k+j] = (ctrl[k] & 3) * 4 + j.
//
// The selector has to end up in ALL FOUR bytes of its lane, which is what makes
// the multiply 32-bit rather than 8-bit: masking leaves (ctrl & 3) in the
// lane's LOW byte and zero in the other three, so an 8-bit multiply scales only
// that one byte and the other three stay 0 -- which reads as "take source byte
// 0", i.e. every lane but the first comes out of src1 lane 0.  A 32-bit
// multiply by 0x04040404 instead broadcasts it: v * 0x04040404 is 0x00000000,
// 0x04040404, 0x08080808 or 0x0C0C0C0C, every byte being v*4.  That is exact
// because v <= 3 keeps each byte under 0x10 so nothing carries between them,
// and adding 0x03020100 per byte then supplies j, again carry-free.
//
// (This is not hypothetical: the 8-bit version was written first and the
// Rosetta differential caught it on the very control vector a compiler would
// generate.)
ir::Value FpOpPermilPsVar(ir::Assembler* as, ir::Value a, ir::Value ctrl, u32, u32) {
    constexpr u64 kThree = 0x0000000300000003ull;
    constexpr u64 kScale = 0x0404040404040404ull;
    constexpr u64 kLane = 0x0302010003020100ull;
    auto sel = as->VecAnd(ctrl, FpConst128(as, kThree, kThree)).SetType(ir::ValueType::V128);
    auto scaled = as->VecMul(sel, FpConst128(as, kScale, kScale), ir::Imm(32u))
                          .SetType(ir::ValueType::V128);
    auto index = as->VecAdd(scaled, FpConst128(as, kLane, kLane), ir::Imm(8u))
                         .SetType(ir::ValueType::V128);
    return as->VecTableLookup8(a, index).SetType(ir::ValueType::V128);
}

// VPERMILPS with an imm8 control: the SAME imm8 permutes each 128-bit lane.
ir::Value FpOpPermilPsImm(ir::Assembler* as, ir::Value a, u32 param) {
    return as->VecShuffle32(a, ir::Imm(param & 0xFFu)).SetType(ir::ValueType::V128);
}

// VPERMILPD with an imm8 control: one bit per result qword, per 128-bit lane.
// Selecting qwords is selecting PAIRS of dwords, so this rides on VecShuffle32
// rather than needing a 64-bit shuffle: qword b maps to dwords 2b and 2b+1.
// The 256-bit form takes bits 3:2 for its upper lane, which the caller folds in
// by passing the already-shifted nibble.
ir::Value FpOpPermilPdImm(ir::Assembler* as, ir::Value a, u32 param) {
    const u32 lo = param & 1u;
    const u32 hi = (param >> 1) & 1u;
    const u32 control = (2 * lo) | ((2 * lo + 1) << 2) | ((2 * hi) << 4) | ((2 * hi + 1) << 6);
    return as->VecShuffle32(a, ir::Imm(control)).SetType(ir::ValueType::V128);
}

}  // namespace

// A 128-bit destination written as two qwords, then C3.  Used by the scalar
// moves and conversions, whose result is a MERGE of two registers' halves
// rather than one vector value.
void X64Decoder::VexWriteHalves(u32 index, ir::Value lo, ir::Value hi) {
    XmmLo(XmmOf(index), lo);
    XmmHi(XmmOf(index), hi);
    ZeroYmmHigh(index);
}

// vmovss / vmovsd -- VEX.LIG.F3(F2).0F.WIG 10 /r and 11 /r.
//
// THREE different instructions share these two opcodes, and they do not merely
// differ in operand order:
//
//   10 /r, mod != 11   LOAD:   dst[lane-1:0] = m; dst[255:lane] = 0.  There is
//                              no merge source at all -- the bytes above the
//                              scalar are ZEROED, not taken from anywhere.
//                              (VEX.vvvv must be 1111b.)
//   10 /r, mod == 11   MERGE:  dst[lane-1:0] = src2[lane-1:0];
//                              dst[127:lane] = src1[127:lane];  bits 255:128
//                              zeroed.  src1 is VEX.vvvv, so the preserved
//                              lanes come from a THIRD register, not from dst
//                              the way legacy SSE MOVSS does it.
//   11 /r, mod == 11   MERGE, with dst and src2 SWAPPED: the destination is
//                              ModRM.rm and the scalar source is ModRM.reg.
//   11 /r, mod != 11   STORE:  m = src[lane-1:0].  No register is written, so
//                              no zeroing happens.
//
// Getting the load form wrong is invisible until the register is read as a YMM
// (or as a full XMM), and getting the 11 /r register form's direction wrong
// writes an entirely different architectural register.
void X64Decoder::DecodeAvxFpMovScalar(const VexInsn& v, u32 lane_bits, bool store) {
    const auto type = lane_bits == 32 ? ir::ValueType::U32 : ir::ValueType::U64;
    if (!v.RmIsRegister()) {
        if (store) {
            auto value = XmmLo(XmmOf(v.reg));
            MemStore(ir::Operand{VexAddress(v)}, NarrowTo(value, type), VexTsoOrdered(v));
            return;
        }
        auto raw = MemLoad(ir::Operand{VexAddress(v)}, type, VexTsoOrdered(v));
        auto lo = lane_bits == 32 ? __ ZeroExtend64(raw).SetType(ir::ValueType::U64) : raw;
        VexWriteHalves(v.reg, lo, __ LoadImm(ir::Imm(u64(0))));
        return;
    }
    // Register form.  0x11 reverses which of reg/rm is the destination.
    const u32 dst = store ? v.rm : v.reg;
    const u32 src2 = store ? v.reg : v.rm;
    auto merge_lo = XmmLo(XmmOf(v.vvvv));
    auto merge_hi = XmmHi(XmmOf(v.vvvv));
    auto scalar = XmmLo(XmmOf(src2));
    ir::Value lo;
    if (lane_bits == 32) {
        lo = __ Or(__ And(merge_lo, ir::Operand{ir::Imm(0xFFFFFFFF00000000ull)}),
                   ir::Operand{__ And(scalar, ir::Operand{ir::Imm(0xFFFFFFFFull)})});
    } else {
        lo = scalar;
    }
    VexWriteHalves(dst, lo, merge_hi);
}

// The 0x12 / 0x13 / 0x16 / 0x17 block: vmovlps, vmovhps, vmovlpd, vmovhpd,
// vmovlhps, vmovhlps.  `high` selects the 0x16/0x17 pair, `store` the odd
// opcode.
//
// The register (mod == 11) forms are the confusing pair, and their names are
// the wrong way round relative to the opcode:
//   0F 12 /r reg-reg is VMOVHLPS -- it moves src2's HIGH qword to dst's LOW.
//   0F 16 /r reg-reg is VMOVLHPS -- src2's LOW qword to dst's HIGH.
// Both take the other half from VEX.vvvv and both zero bits 255:128.
//
// The memory forms are a 64-bit access at a 128-bit register: vmovlps loads
// into the low qword and keeps vvvv's high qword; vmovhps the reverse.  Their
// stores write 8 bytes from the low / high qword respectively and touch no
// register.  All of these are VEX.128 only -- an L=1 encoding is #UD, and is
// declined here rather than run at the wrong width.
void X64Decoder::DecodeAvxFpMovLoHi(const VexInsn& v, bool high, bool store) {
    if (store) {
        auto value = high ? XmmHi(XmmOf(v.reg)) : XmmLo(XmmOf(v.reg));
        MemStore(ir::Operand{VexAddress(v)}, value, VexTsoOrdered(v));
        return;
    }
    ir::Value from_rm;
    if (v.RmIsRegister()) {
        // vmovhlps reads src2's HIGH qword; vmovlhps reads its LOW qword.
        from_rm = high ? XmmLo(XmmOf(v.rm)) : XmmHi(XmmOf(v.rm));
    } else {
        from_rm = MemLoad(ir::Operand{VexAddress(v)},
                          ir::ValueType::U64,
                          VexTsoOrdered(v));
    }
    if (high) {
        VexWriteHalves(v.reg, XmmLo(XmmOf(v.vvvv)), from_rm);
    } else {
        VexWriteHalves(v.reg, from_rm, XmmHi(XmmOf(v.vvvv)));
    }
}

// vmovddup -- VEX.128/256.F2.0F.WIG 12 /r.
//
// Each 128-bit lane duplicates ITS OWN low qword, so the 256-bit form is not a
// broadcast of one value: bits 255:128 come from the source's bits 191:128,
// not from its bits 63:0.  The memory operand is m64 at L=0 but m256 at L=1 --
// the one form in this file whose operand size is not simply 16 << L.
void X64Decoder::DecodeAvxFpMovDDup(const VexInsn& v) {
    if (!v.Is256()) {
        auto value = VexLoadScalar(v, 64);
        VexWrite128(v.reg, __ VecDup64(value).SetType(ir::ValueType::V128));
        return;
    }
    auto source = VexLoadVec256(v);
    auto lo = __ VecDup64(__ VecExtract64(source.lo, ir::Imm(0u)).SetType(ir::ValueType::U64))
                      .SetType(ir::ValueType::V128);
    auto hi = __ VecDup64(__ VecExtract64(source.hi, ir::Imm(0u)).SetType(ir::ValueType::U64))
                      .SetType(ir::ValueType::V128);
    VexWrite256(v.reg, lo, hi);
}

// vcvtss2sd / vcvtsd2ss -- VEX.LIG.F3(F2).0F.WIG 5A /r.
//
// `src_bits` is the SOURCE width.  The converted scalar replaces the low lane;
// the rest of the low 128 bits comes from VEX.vvvv (not from the destination,
// which is the legacy SSE rule) and bits 255:128 are zeroed.
//
// Note the asymmetry in what "the rest" means: vcvtss2sd produces 64 bits and
// keeps only bits 127:64, while vcvtsd2ss produces 32 and keeps bits 127:32 --
// so the f32 case has to merge inside a qword rather than replace one.
void X64Decoder::DecodeAvxFpCvtScalarFloat(const VexInsn& v, u32 src_bits) {
    auto source = VexLoadScalar(v, src_bits);
    auto converted = __ VecFCvtScalar(source, ir::Imm(src_bits));
    auto merge_lo = XmmLo(XmmOf(v.vvvv));
    auto merge_hi = XmmHi(XmmOf(v.vvvv));
    if (src_bits == 32) {
        // -> f64: the whole low qword is the result.
        VexWriteHalves(v.reg, converted.SetType(ir::ValueType::U64), merge_hi);
        return;
    }
    auto lo = __ Or(__ And(merge_lo, ir::Operand{ir::Imm(0xFFFFFFFF00000000ull)}),
                    ir::Operand{__ And(__ ZeroExtend64(converted),
                                       ir::Operand{ir::Imm(0xFFFFFFFFull)})});
    VexWriteHalves(v.reg, lo, merge_hi);
}

// vcvtsi2ss / vcvtsi2sd -- VEX.LIG.F3(F2).0F.W0/W1 2A /r.
//
// VEX.W picks the SOURCE integer width (32 or 64), which is the only place it
// is load-bearing here; `dst_bits` is the float width the opcode's mandatory
// prefix picks.  The r/m operand is a GENERAL-PURPOSE register or memory, not
// a vector -- the one form in this file where ModRM.rm names a GPR.
void X64Decoder::DecodeAvxFpCvtSi2Scalar(const VexInsn& v, u32 dst_bits) {
    const u32 src_bits = v.w ? 64u : 32u;
    ir::Value source;
    if (v.RmIsRegister()) {
        source = R(GprOf(v.rm, v.w));
    } else {
        source = MemLoad(ir::Operand{VexAddress(v)},
                         GetSize(src_bits),
                         VexTsoOrdered(v));
    }
    auto converted = __ VecFCvtIntToFloat(source, ir::Imm(src_bits), ir::Imm(dst_bits));
    auto merge_lo = XmmLo(XmmOf(v.vvvv));
    auto merge_hi = XmmHi(XmmOf(v.vvvv));
    if (dst_bits == 64) {
        VexWriteHalves(v.reg, converted.SetType(ir::ValueType::U64), merge_hi);
        return;
    }
    auto lo = __ Or(__ And(merge_lo, ir::Operand{ir::Imm(0xFFFFFFFF00000000ull)}),
                    ir::Operand{__ And(__ ZeroExtend64(converted),
                                       ir::Operand{ir::Imm(0xFFFFFFFFull)})});
    VexWriteHalves(v.reg, lo, merge_hi);
}

// vcvtss2si / vcvtsd2si (round per MXCSR, modelled as nearest-even) and
// vcvttss2si / vcvttsd2si (truncate) -- VEX.LIG.F3(F2).0F.W0/W1 2C and 2D /r.
//
// The destination is a GPR whose width is VEX.W.  Out-of-range and NaN inputs
// must produce x86's "integer indefinite" (0x80000000 / 0x8000...0) rather
// than AArch64's saturating FCVT result; VecFCvtFloatToInt already carries
// that rule, which is why this is a two-line handler.
void X64Decoder::DecodeAvxFpCvtScalar2Si(const VexInsn& v, u32 src_bits, bool truncate) {
    const u32 width = v.w ? 64u : 32u;
    auto source = VexLoadScalar(v, src_bits);
    auto result = __ VecFCvtFloatToInt(source,
                                       ir::Imm(src_bits),
                                       ir::Imm(width),
                                       ir::Imm(truncate ? 0u : 1u));
    R(GprOf(v.reg, v.w), result.SetType(width == 64 ? ir::ValueType::U64 : ir::ValueType::U32));
}

// vcvtdq2pd (and the same shape as vcvtps2pd): the SOURCE is half the
// destination's width.
//   L=0: xmm <- xmm/m64    L=1: ymm <- xmm/m128
// VecFCvtPacked's widening kinds read the LOW half of their V128 argument, so
// the 256-bit result's upper half needs the source's upper 64 bits moved down
// first -- the halves are genuinely not independent here.
void X64Decoder::DecodeAvxFpCvtWiden(const VexInsn& v, u32 kind) {
    const auto k = ir::Imm(kind);
    if (!v.Is256()) {
        auto source = v.RmIsRegister() ? XmmRead(XmmOf(v.rm))
                                       : __ VecDup64(MemLoad(ir::Operand{VexAddress(v)},
                                                            ir::ValueType::U64,
                                                            VexTsoOrdered(v)))
                                                 .SetType(ir::ValueType::V128);
        VexWrite128(v.reg, __ VecFCvtPacked(source, k));
        return;
    }
    auto source = VexLoadVec(v);  // 128-bit source, 256-bit result
    auto lo = __ VecFCvtPacked(source, k);
    auto upper = __ VecDup64(__ VecExtract64(source, ir::Imm(1u)).SetType(ir::ValueType::U64))
                         .SetType(ir::ValueType::V128);
    auto hi = __ VecFCvtPacked(upper, k);
    VexWrite256(v.reg, lo, hi);
}

// vcvtpd2dq / vcvttpd2dq (the same shape as vcvtpd2ps): the DESTINATION is
// half the source's width, and is an XMM at BOTH VEX.L values -- so contract
// C3 applies even to the L=1 encoding, and getting that wrong leaves the old
// upper half of a 256-bit register live.
//   L=0: xmm <- xmm/m128, result[127:64] = 0
//   L=1: xmm <- ymm/m256
void X64Decoder::DecodeAvxFpCvtNarrow(const VexInsn& v, u32 kind) {
    const auto k = ir::Imm(kind);
    if (!v.Is256()) {
        // The narrowing kinds write 64 bits and zero the rest, which is the
        // architectural result[127:64] = 0 for free.
        VexWrite128(v.reg, __ VecFCvtPacked(VexLoadVec(v), k));
        return;
    }
    auto source = VexLoadVec256(v);
    auto lo = __ VecFCvtPacked(source.lo, k);
    auto hi = __ VecFCvtPacked(source.hi, k);
    VexWrite128(v.reg, __ VecZip(lo, hi, ir::Imm(64u), ir::Imm(0u)));
}

// vpsrldq / vpslldq -- VEX.NDD.128/256.66.0F.WIG 73 /3 ib and /7 ib.
//
// These shift the whole 128-bit lane by imm8 BYTES (not bits, and not per
// element), zero-filling; at 256 bits each lane shifts independently, so this
// is NOT a 32-byte shift.  imm8 >= 16 shifts everything out.
//
// There is no byte-shift IR opcode, but VecTableLookup8 is one: its control is
// masked with 0x8F, so a control byte with bit 7 set produces a ZERO byte and
// one with bits 3:0 = k produces source byte k.  A constant control vector
// therefore expresses either direction exactly, including the "shifted in from
// nowhere" zeros, without a compare or a mask.
//
// The encoding is NDD: the DESTINATION is VEX.vvvv and the source is ModRM.rm
// (the reverse of the usual assignment), matching the lane-shift group in
// decoder_avx_int.cc.
void X64Decoder::DecodeAvxFpByteShift(const VexInsn& v, bool left) {
    const u32 count = v.imm8;
    u64 control[2] = {0, 0};
    for (u32 byte = 0; byte < 16; ++byte) {
        u32 index;
        if (left) {
            index = byte >= count ? byte - count : 0x80u;
        } else {
            index = byte + count < 16 ? byte + count : 0x80u;
        }
        control[byte / 8] |= u64(index) << ((byte % 8) * 8);
    }
    auto mask = FpConst128(assembler, control[0], control[1]);
    if (v.Is256()) {
        // A second constant rather than reusing `mask`: the two halves are
        // separate IR values under C1, and sharing one would stretch a single
        // live range across both writes.
        auto mask_hi = FpConst128(assembler, control[0], control[1]);
        auto lo = __ VecTableLookup8(XmmRead(XmmOf(v.rm)), mask).SetType(ir::ValueType::V128);
        auto hi = __ VecTableLookup8(YmmHighRead(v.rm), mask_hi).SetType(ir::ValueType::V128);
        VexWrite256(v.vvvv, lo, hi);
        return;
    }
    VexWrite128(v.vvvv, __ VecTableLookup8(XmmRead(XmmOf(v.rm)), mask));
}

// vptest -- VEX.128/256.66.0F38.WIG 17 /r.
//
//   ZF = ((SRC AND DEST) == 0)          DEST = ModRM.reg, SRC = r/m
//   CF = ((SRC AND NOT DEST) == 0)
//   OF = AF = PF = SF = 0
//
// Note DEST is only READ -- vptest writes no register, just EFLAGS.
//
// Reducing a vector to "are any bits set" has no IR opcode, so the two 64-bit
// halves are OR-ed into a scalar; at 256 bits all four qwords fold into one.
// That is exact, because only zero / non-zero is asked for.
//
// The EFLAGS plumbing mirrors DecodeAvxFpComis exactly, including the order:
// PF is written before ZF because the value-producing instruction the ZF save
// attaches to also republishes the parity byte.
void X64Decoder::DecodeAvxFpPTest(const VexInsn& v) {
    const auto fold = [&](ir::Value vec) {
        auto lo = __ VecExtract64(vec, ir::Imm(0u)).SetType(ir::ValueType::U64);
        auto hi = __ VecExtract64(vec, ir::Imm(1u)).SetType(ir::ValueType::U64);
        return __ Or(lo, ir::Operand{hi}).SetType(ir::ValueType::U64);
    };
    ir::Value both, notdest;
    if (v.Is256()) {
        auto dest_lo = XmmRead(XmmOf(v.reg));
        auto dest_hi = YmmHighRead(v.reg);
        auto src = VexLoadVec256(v);
        auto and_lo = __ VecAnd(src.lo, dest_lo).SetType(ir::ValueType::V128);
        auto and_hi = __ VecAnd(src.hi, dest_hi).SetType(ir::ValueType::V128);
        // VecAndNot(x, y) is x AND NOT y.
        auto andn_lo = __ VecAndNot(src.lo, dest_lo).SetType(ir::ValueType::V128);
        auto andn_hi = __ VecAndNot(src.hi, dest_hi).SetType(ir::ValueType::V128);
        both = __ Or(fold(and_lo), ir::Operand{fold(and_hi)}).SetType(ir::ValueType::U64);
        notdest = __ Or(fold(andn_lo), ir::Operand{fold(andn_hi)}).SetType(ir::ValueType::U64);
    } else {
        auto dest = XmmRead(XmmOf(v.reg));
        auto src = VexLoadVec(v);
        both = fold(__ VecAnd(src, dest).SetType(ir::ValueType::V128));
        notdest = fold(__ VecAndNot(src, dest).SetType(ir::ValueType::V128));
    }
    __ ClearFlags(ir::Flags::Overflow | ir::Flags::Negate | ir::Flags::AuxiliaryCarry);
    auto one = __ LoadImm(ir::Imm(u64(1)));
    auto zero = __ LoadImm(ir::Imm(u64(0)));
    // PF is architecturally 0.  SaveFlags(x, Parity) sets PF from the EVEN
    // parity of x's low byte, so a value of 1 (odd) is how PF = 0 is expressed.
    __ SaveFlags(__ Or(one, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Parity);
    // ZF = (both == 0), which is exactly what SaveFlags(x, Zero) computes.
    __ SaveFlags(__ Or(both, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Zero);
    auto cf = __ Select(__ TestNotZero(notdest), zero, one);
    auto cv = __ Add(__ LoadImm(ir::Imm(~u64(0))), ir::Operand{cf});
    __ SaveFlags(cv, ir::Flags::Carry);
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
}

// vpextrb / vpextrw / vpextrd / vpextrq.
//
// VecExtract16 / VecExtract64 are the only element reads the IR has, so the
// 8- and 32-bit elements are reached by extracting the containing 16- or
// 64-bit one and shifting.  That is exact rather than approximate: the element
// is a contiguous field of the container at a statically known offset.
//
// vpextrw (0F C5) is register-destination only; the 0F3A forms may also store
// to memory.  A GPR destination is written at 32 bits (or 64 for vpextrq),
// which zero-extends -- x86 defines the sub-32-bit results as zero-extended.
void X64Decoder::DecodeAvxFpExtract(const VexInsn& v, u32 element_bits) {
    auto source = XmmRead(XmmOf(v.reg));
    const u32 count = 128 / element_bits;
    const u32 index = v.imm8 % count;
    ir::Value value;
    if (element_bits == 64) {
        value = __ VecExtract64(source, ir::Imm(index)).SetType(ir::ValueType::U64);
    } else if (element_bits == 32) {
        auto container = __ VecExtract64(source, ir::Imm(index / 2)).SetType(ir::ValueType::U64);
        if (index % 2 != 0) {
            container = __ LsrImm(container, ir::Imm(32u)).SetType(ir::ValueType::U64);
        }
        value = __ And(container, ir::Operand{ir::Imm(0xFFFFFFFFull)})
                        .SetType(ir::ValueType::U64);
    } else if (element_bits == 16) {
        auto word = __ VecExtract16(source, ir::Imm(index)).SetType(ir::ValueType::U32);
        value = __ ZeroExtend64(word).SetType(ir::ValueType::U64);
    } else {
        auto word = __ VecExtract16(source, ir::Imm(index / 2)).SetType(ir::ValueType::U32);
        auto container = __ ZeroExtend64(word).SetType(ir::ValueType::U64);
        if (index % 2 != 0) {
            container = __ LsrImm(container, ir::Imm(8u)).SetType(ir::ValueType::U64);
        }
        value = __ And(container, ir::Operand{ir::Imm(0xFFull)}).SetType(ir::ValueType::U64);
    }
    if (v.RmIsRegister()) {
        // vpextrq is the only 64-bit destination; everything else writes the
        // 32-bit register, which zeroes bits 63:32.
        R(GprOf(v.rm, element_bits == 64), value);
        return;
    }
    MemStore(ir::Operand{VexAddress(v)},
             NarrowTo(value, GetSize(element_bits)),
             VexTsoOrdered(v));
}

// vpinsrb / vpinsrw / vpinsrd / vpinsrq.
//
// dst = src1 (VEX.vvvv) with one element replaced by the r/m operand; bits
// 255:128 zeroed.  VecInsert16 is the only element write the IR has, so a
// 32- or 64-bit element becomes two or four 16-bit inserts and an 8-bit one
// becomes a read-modify-write of its containing halfword.  Every step is over
// a statically known lane, so nothing here is approximate.
//
// The r/m operand is a GPR or memory of the ELEMENT's width (vpinsrw's
// register form reads a 32-bit GPR but uses only its low 16 bits).
void X64Decoder::DecodeAvxFpInsert(const VexInsn& v, u32 element_bits) {
    const u32 count = 128 / element_bits;
    const u32 index = v.imm8 % count;
    ir::Value value;
    if (v.RmIsRegister()) {
        value = R(GprOf(v.rm, element_bits == 64));
    } else {
        value = MemLoad(ir::Operand{VexAddress(v)},
                        GetSize(element_bits),
                        VexTsoOrdered(v));
        if (element_bits != 64) {
            value = __ ZeroExtend64(value).SetType(ir::ValueType::U64);
        }
    }
    auto result = XmmRead(XmmOf(v.vvvv));
    if (element_bits == 8) {
        const u32 lane = index / 2;
        auto old_word = __ VecExtract16(result, ir::Imm(lane)).SetType(ir::ValueType::U32);
        auto old = __ ZeroExtend64(old_word).SetType(ir::ValueType::U64);
        const u64 keep = index % 2 == 0 ? 0xFF00ull : 0x00FFull;
        auto byte = __ And(value, ir::Operand{ir::Imm(0xFFull)}).SetType(ir::ValueType::U64);
        if (index % 2 != 0) {
            byte = __ LslImm(byte, ir::Imm(8u)).SetType(ir::ValueType::U64);
        }
        auto merged = __ Or(__ And(old, ir::Operand{ir::Imm(keep)}), ir::Operand{byte});
        VexWrite128(v.reg, __ VecInsert16(result, merged, ir::Imm(lane)));
        return;
    }
    const u32 halfwords = element_bits / 16;
    for (u32 i = 0; i < halfwords; ++i) {
        auto part = i == 0 ? value
                           : __ LsrImm(value, ir::Imm(i * 16u)).SetType(ir::ValueType::U64);
        result = __ VecInsert16(result, part, ir::Imm(index * halfwords + i))
                         .SetType(ir::ValueType::V128);
    }
    VexWrite128(v.reg, result);
}

bool X64Decoder::DecodeAvxFp2(const VexInsn& v) {
    const bool p_none = v.pp == VexPP::None;
    const bool p66 = v.pp == VexPP::P66;
    const bool pf3 = v.pp == VexPP::PF3;
    const bool pf2 = v.pp == VexPP::PF2;

    switch (v.map) {
        case VexMap::Map0F:
            switch (v.opcode) {
                // ---- vmovss / vmovsd (and NOT vmovups/vmovupd) -------------
                // pp none / 66 on these opcodes are vmovups / vmovupd, which
                // the legacy path still owns; declining leaves them alone.
                case 0x10:
                case 0x11: {
                    if (!pf3 && !pf2) {
                        return false;
                    }
                    DecodeAvxFpMovScalar(v, pf3 ? 32u : 64u, v.opcode == 0x11);
                    return true;
                }
                // ---- the low/high qword move block -------------------------
                case 0x12:
                case 0x16: {
                    const bool high = v.opcode == 0x16;
                    if (pf3) {  // vmovsldup (12) / vmovshdup (16)
                        if (v.vvvv_valid) {
                            return false;
                        }
                        DecodeAvxIntUnary(
                                v,
                                [](ir::Assembler* as, ir::Value a, u32 param) {
                                    return as->VecDupPairs32(a, ir::Imm(param))
                                            .SetType(ir::ValueType::V128);
                                },
                                high ? 1u : 0u);
                        return true;
                    }
                    if (pf2) {  // vmovddup (12 only)
                        if (high || v.vvvv_valid) {
                            return false;
                        }
                        DecodeAvxFpMovDDup(v);
                        return true;
                    }
                    // vmovlps / vmovhps / vmovlpd / vmovhpd / vmovhlps /
                    // vmovlhps: VEX.128 only, and the 66-prefixed forms have
                    // no register variant (mod == 11 is #UD there).
                    if (v.Is256() || (p66 && v.RmIsRegister()) || (!p_none && !p66)) {
                        return false;
                    }
                    DecodeAvxFpMovLoHi(v, high, false);
                    return true;
                }
                case 0x13:
                case 0x17: {
                    // vmovlps / vmovhps / vmovlpd / vmovhpd, store forms:
                    // memory destination only, VEX.128 only, no VEX.vvvv.
                    if ((!p_none && !p66) || v.Is256() || v.RmIsRegister() || v.vvvv_valid) {
                        return false;
                    }
                    DecodeAvxFpMovLoHi(v, v.opcode == 0x17, true);
                    return true;
                }
                // ---- unpack ------------------------------------------------
                case 0x14:
                case 0x15: {
                    if (!p_none && !p66) {
                        return false;
                    }
                    DecodeAvxIntBinary(v,
                                       FpOpZip,
                                       FpPack(p66 ? 64u : 32u, v.opcode == 0x15 ? 1u : 0u));
                    return true;
                }
                // ---- scalar int -> float -----------------------------------
                case 0x2A:
                    if (!pf3 && !pf2) {
                        return false;
                    }
                    DecodeAvxFpCvtSi2Scalar(v, pf3 ? 32u : 64u);
                    return true;
                // ---- scalar float -> int -----------------------------------
                case 0x2C:
                case 0x2D:
                    if ((!pf3 && !pf2) || v.vvvv_valid) {
                        return false;
                    }
                    DecodeAvxFpCvtScalar2Si(v, pf3 ? 32u : 64u, v.opcode == 0x2C);
                    return true;
                // ---- scalar float <-> float --------------------------------
                case 0x5A:
                    if (!pf3 && !pf2) {
                        return false;  // the packed forms belong to Base
                    }
                    DecodeAvxFpCvtScalarFloat(v, pf3 ? 32u : 64u);
                    return true;
                // ---- byte shifts of the whole lane -------------------------
                case 0x73: {
                    if (!p66 || !v.RmIsRegister()) {
                        return false;
                    }
                    const u32 group = v.reg & 7;
                    if (group != 3 && group != 7) {
                        return false;  // the lane shifts belong to DecodeAvxInt
                    }
                    DecodeAvxFpByteShift(v, group == 7);
                    return true;
                }
                // ---- element insert / extract ------------------------------
                case 0xC4:  // vpinsrw
                    if (!p66 || v.Is256()) {
                        return false;
                    }
                    DecodeAvxFpInsert(v, 16);
                    return true;
                case 0xC5:  // vpextrw (register destination only)
                    if (!p66 || v.Is256() || !v.RmIsRegister() || v.vvvv_valid) {
                        return false;
                    }
                    // ModRM.reg is the GPR and ModRM.rm the vector source, so
                    // the operands are the other way round from every other
                    // form here; DecodeAvxFpExtract expects reg = vector.
                    {
                        VexInsn swapped = v;
                        swapped.reg = v.rm;
                        swapped.rm = v.reg;
                        DecodeAvxFpExtract(swapped, 16);
                    }
                    return true;
                // ---- shuffle -----------------------------------------------
                case 0xC6:
                    if (!p_none && !p66) {
                        return false;
                    }
                    DecodeAvxIntBinary(v, p66 ? FpOpShufPd : FpOpShufPs, v.imm8);
                    return true;
                // ---- vpmovmskb ---------------------------------------------
                case 0xD7:
                    if (!p66 || v.vvvv_valid || !v.RmIsRegister()) {
                        return false;  // the memory source form is #UD
                    }
                    // Lane width 8 gives 16 bits per half, and the 256-bit
                    // combine is `lo | hi << 16` -- which is what
                    // DecodeAvxFpMovmsk computes from 128 / lane_bits.
                    DecodeAvxFpMovmsk(v, 8);
                    return true;
                // ---- unsigned byte min / max -------------------------------
                case 0xDA:
                    if (!p66) {
                        return false;
                    }
                    DecodeAvxIntBinary(v, FpOpMin, FpPack(8, 0));
                    return true;
                case 0xDE:
                    if (!p66) {
                        return false;
                    }
                    DecodeAvxIntBinary(v, FpOpMax, FpPack(8, 0));
                    return true;
                // ---- the 0xE6 conversion trio ------------------------------
                case 0xE6:
                    if (v.vvvv_valid) {
                        return false;
                    }
                    if (pf3) {
                        DecodeAvxFpCvtWiden(v, 1);  // vcvtdq2pd
                    } else if (p66) {
                        DecodeAvxFpCvtNarrow(v, 5);  // vcvttpd2dq (truncate)
                    } else if (pf2) {
                        DecodeAvxFpCvtNarrow(v, 4);  // vcvtpd2dq (round)
                    } else {
                        return false;  // no unprefixed 0F E6 exists
                    }
                    return true;
                default:
                    return false;
            }

        case VexMap::Map0F38:
            if (!p66) {
                return false;
            }
            switch (v.opcode) {
                case 0x00:  // vpshufb
                    DecodeAvxIntBinary(v, FpOpShufB, 0);
                    return true;
                case 0x0C:  // vpermilps, register control
                    if (v.w) {
                        return false;
                    }
                    DecodeAvxIntBinary(v, FpOpPermilPsVar, 0);
                    return true;
                case 0x17:  // vptest
                    if (v.vvvv_valid) {
                        return false;
                    }
                    DecodeAvxFpPTest(v);
                    return true;
                // ---- broadcasts.  vbroadcastss/sd are architecturally the
                // same operation as vpbroadcastd/q, and vbroadcastf128 the
                // same as vbroadcasti128, so these reuse those handlers.
                case 0x18:
                    if (v.w || v.vvvv_valid) {
                        return false;
                    }
                    DecodeAvxIntBroadcast(v, 32);
                    return true;
                case 0x19:
                    if (v.w || v.vvvv_valid || !v.Is256()) {
                        return false;  // vbroadcastsd is VEX.256 only
                    }
                    DecodeAvxIntBroadcast(v, 64);
                    return true;
                case 0x1A:
                    if (v.w || v.vvvv_valid || !v.Is256() || v.RmIsRegister()) {
                        return false;  // VEX.256, memory source only
                    }
                    DecodeAvxIntBroadcast128(v);
                    return true;
                case 0x29:  // vpcmpeqq
                    DecodeAvxIntBinary(v, FpOpCmpEq, FpPack(64));
                    return true;
                case 0x37:  // vpcmpgtq
                    DecodeAvxIntBinary(v, FpOpCmpGt, FpPack(64));
                    return true;
                case 0x3B:  // vpminud
                    DecodeAvxIntBinary(v, FpOpMin, FpPack(32, 0));
                    return true;
                case 0x3F:  // vpmaxud
                    DecodeAvxIntBinary(v, FpOpMax, FpPack(32, 0));
                    return true;
                default:
                    return false;
            }

        case VexMap::Map0F3A:
            if (!p66) {
                return false;
            }
            switch (v.opcode) {
                case 0x04:  // vpermilps, imm8 control
                    if (v.w || v.vvvv_valid) {
                        return false;
                    }
                    DecodeAvxIntUnary(v, FpOpPermilPsImm, v.imm8);
                    return true;
                case 0x05: {  // vpermilpd, imm8 control
                    if (v.w || v.vvvv_valid) {
                        return false;
                    }
                    // The upper 128-bit lane uses imm8 bits 3:2, so the two
                    // halves need different controls and DecodeAvxIntUnary's
                    // single param cannot serve both.
                    if (v.Is256()) {
                        auto source = VexLoadVec256(v);
                        auto lo = FpOpPermilPdImm(assembler, source.lo, v.imm8 & 3u);
                        auto hi = FpOpPermilPdImm(assembler, source.hi, (v.imm8 >> 2) & 3u);
                        VexWrite256(v.reg, lo, hi);
                    } else {
                        VexWrite128(v.reg, FpOpPermilPdImm(assembler, VexLoadVec(v), v.imm8 & 3u));
                    }
                    return true;
                }
                case 0x06:  // vperm2f128
                    if (v.w || !v.Is256()) {
                        return false;
                    }
                    DecodeAvxIntPerm2i128(v);
                    return true;
                case 0x14:  // vpextrb
                    if (v.Is256() || v.vvvv_valid) {
                        return false;
                    }
                    DecodeAvxFpExtract(v, 8);
                    return true;
                case 0x16:  // vpextrd / vpextrq
                    if (v.Is256() || v.vvvv_valid) {
                        return false;
                    }
                    DecodeAvxFpExtract(v, v.w ? 64u : 32u);
                    return true;
                case 0x18:  // vinsertf128
                    if (v.w || !v.Is256()) {
                        return false;
                    }
                    DecodeAvxIntInsert128(v);
                    return true;
                case 0x19:  // vextractf128
                    if (v.w || !v.Is256()) {
                        return false;
                    }
                    DecodeAvxIntExtract128(v);
                    return true;
                case 0x20:  // vpinsrb
                    if (v.Is256()) {
                        return false;
                    }
                    DecodeAvxFpInsert(v, 8);
                    return true;
                case 0x22:  // vpinsrd / vpinsrq
                    if (v.Is256()) {
                        return false;
                    }
                    DecodeAvxFpInsert(v, v.w ? 64u : 32u);
                    return true;
                default:
                    return false;
            }

        case VexMap::Invalid:
        default:
            return false;
    }
}

#undef __

}  // namespace swift::x86
