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
//  * vcmpps/pd/ss/sd accept only imm8 predicates 0..7 (the SSE set: eq, lt,
//    le, unord, neq, nlt, nle, ord).  AVX widened the field to five bits
//    (0..31: the _*_OQ/_*_OS signalling variants plus ge/gt and their
//    negations).  VecFCmpMask masks the predicate with 7, so accepting 8..31
//    would silently compute a DIFFERENT comparison -- e.g. imm8=17 (LT_OQ)
//    would be run as imm8=1 (LT_OS), and imm8=13 (GE_OS) as imm8=5 (NLT_US),
//    which is a different answer whenever an operand is NaN.  Predicates >= 8
//    are therefore declined (block traps as FALLBACK) rather than mis-executed.
//    Closing this needs either a wider VecFCmpMask predicate or a decode-time
//    rewrite of the 8..31 encodings onto swapped-operand 0..7 forms, and both
//    are IR/backend changes that are out of scope here.
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

#include "runtime/frontend/x86/decoder_internal.h"
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
    return __ LoadMemory(ir::Operand{VexAddress(v)}).SetType(ir::ValueType::V128);
}

// The r/m operand as two V128 halves (C1).
X64Decoder::VecHalves X64Decoder::VexLoadVec256(const VexInsn& v) {
    if (v.RmIsRegister()) {
        return {XmmRead(XmmOf(v.rm)), YmmHighRead(v.rm)};
    }
    auto addr = VexAddress(v);
    auto lo = __ LoadMemory(ir::Operand{addr}).SetType(ir::ValueType::V128);
    auto hi = __ LoadMemory(ir::Operand{__ Add(addr, ir::Operand{ir::Imm(u64(16))})})
                      .SetType(ir::ValueType::V128);
    return {lo, hi};
}

// The r/m operand of a SCALAR form as a plain integer container: a U64 holding
// the f32 in its low dword, or the f64.  This is what VecFAddScalar32/64 and
// VecFCmp want (they take the raw bits in a GPR, not a vector).
ir::Value X64Decoder::VexLoadScalar(const VexInsn& v, u32 lane_bits) {
    if (v.RmIsRegister()) {
        return XmmLo(XmmOf(v.rm));
    }
    return __ LoadMemory(ir::Operand{VexAddress(v)})
            .SetType(lane_bits == 32 ? ir::ValueType::U32 : ir::ValueType::U64);
}

// The r/m operand of a scalar form as a V128 with the value in lane 0, which
// is the shape VecFMinMax / VecFCmpMask / VecFUnary need.  Only lane 0 is ever
// read, so broadcasting the memory form is free of consequence.
ir::Value X64Decoder::VexLoadScalarVec(const VexInsn& v, u32 lane_bits) {
    if (v.RmIsRegister()) {
        return XmmRead(XmmOf(v.rm));
    }
    auto raw = __ LoadMemory(ir::Operand{VexAddress(v)})
                       .SetType(lane_bits == 32 ? ir::ValueType::U32 : ir::ValueType::U64);
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
// Callers must have rejected imm8 >= 8 -- see the KNOWN GAPS note.
void X64Decoder::DecodeAvxFpCmpMask(const VexInsn& v, u32 lane_bits, bool scalar) {
    const auto bits = ir::Imm(lane_bits);
    const auto pred = ir::Imm(u32(v.imm8 & 7u));
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
                                       : __ VecDup64(__ LoadMemory(ir::Operand{VexAddress(v)})
                                                             .SetType(ir::ValueType::U64))
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

bool X64Decoder::DecodeAvxFp(const VexInsn& v) {
    if (!AvxEnabled() || !v.valid || v.map != VexMap::Map0F) {
        return false;
    }
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
            if (!v.has_imm8 || (v.imm8 & 0xF8u) != 0) {
                // Predicates 8..31 are AVX-only extensions VecFCmpMask cannot
                // express; declining is the only answer that is not silently
                // wrong.  See KNOWN GAPS.
                return false;
            }
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

#undef __

}  // namespace swift::x86
