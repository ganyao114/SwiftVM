// FMA3: the vfmadd / vfmsub / vfnmadd / vfnmsub family, all three operand
// orders (132 / 213 / 231), packed and scalar, single and double, VEX.128 and
// VEX.256 -- plus vfmaddsub / vfmsubadd.
//
// ---------------------------------------------------------------------------
// WHY THIS FAMILY MATTERS MORE THAN ITS SIZE SUGGESTS
// ---------------------------------------------------------------------------
// Every other AVX gap needs hand-written intrinsics or a hand-written kernel to
// show up in a real binary.  FMA3 does not: gcc and clang emit it from ordinary
// `a * b + c` source at -O2 as soon as -mavx2/-mfma is on, so a plain numeric
// loop compiled for a modern -march contains it.  An unimplemented instruction
// here is not a corner case; it is FALLBACK -> IllegalCode, which kills the
// guest process.
//
// ---------------------------------------------------------------------------
// THE ONE THING THAT CANNOT BE APPROXIMATED
// ---------------------------------------------------------------------------
// FMA is FUSED.  The exact product a*b enters the addition unrounded and the
// whole expression is rounded once.  Spelling it VecFMul followed by VecFAdd
// rounds twice, and the difference is not a subtlety.  Worked example, taken
// from lane 0 of avx_fma_test.cpp's `fuse32a` input and checked against the
// bytes real x86 produced for `vfmadd231ps`:
//
//     a = b = 0x3F800800 = 1 + 2^-12       c = 0xBF800400 = -(1 + 2^-13)
//
// The exact product is 1 + 2^-11 + 2^-24, and 2^-24 is EXACTLY half an ulp at
// 1.0, so rounding it first is a tie that round-to-nearest-even resolves
// downwards to 1 + 2^-11 -- the 2^-24 is gone.  The addition then cancels the
// leading 1 and what is left is 3*2^-13 = 0x39C00000.  Fused, the 2^-24
// survives the cancellation into a binade where it is representable, and the
// answer is 0x39C00800.  Eight mantissa bits of difference, from ordinary
// nearby-to-1 operands.
//
// This is measured, not argued: replacing the fused step with a multiply and
// an add in BOTH back ends moves 630 of the 3360 reference rows (see the
// mutation log at the end of this comment).
//
// This is why the work needed a new IR opcode rather than a composition:
//
//     VecFMulAdd(a, b, c, lane_bits, flags)
//         dst.lane = round( +-(a.lane * b.lane) +- c.lane )     one rounding
//         flags bit 0: negate the product   bit 1: negate the addend
//
// which is as platform-neutral as vector opcodes get -- AArch64 FMLA/FMLS (and
// scalar FMADD/FMSUB/FNMADD/FNMSUB, which cover all four sign combinations
// outright), RISC-V vfmacc/vfnmacc/vfmsac/vfnmsac, 32-bit ARM VFMA/VFMS.  The
// x86 132/213/231 numbering is deliberately NOT in the IR: it names which
// ENCODED operand plays which arithmetic role, which is a decoding fact, so it
// is resolved here into a single (a, b, c) order.  See runtime/ir/ir.inc.
//
// ---------------------------------------------------------------------------
// THE OPERAND-ORDER NUMBERING, MEASURED RATHER THAN REMEMBERED
// ---------------------------------------------------------------------------
// With op1 = ModRM.reg (which is also the destination), op2 = VEX.vvvv and
// op3 = ModRM.r/m, the digits name the multiplicand, the multiplier and the
// addend in that order:
//
//     132   dst = op1 * op3 + op2
//     213   dst = op2 * op1 + op3
//     231   dst = op2 * op3 + op1
//
// It is easy to get backwards and the two wrong assignments are not symmetric,
// so this is not taken on trust.  The reference data loads three DIFFERENT
// values into three DIFFERENT registers (op1 = C = 5, op2 = A = 2, op3 = B = 3
// in lane 0 of the `order32` input), for which the three orders give 17, 13 and
// 11 -- and real x86 answered 17 for 132, 13 for 213 and 11 for 231.  Making
// the 132 opcodes decode as 213 moves 908 of the 3360 rows.
//
// ---------------------------------------------------------------------------
// THE OPCODE SPACE IS REGULAR, SO THE DISPATCH IS ARITHMETIC
// ---------------------------------------------------------------------------
// All of FMA3 is VEX.66.0F38 with W0 = single and W1 = double:
//
//     high nibble  0x9_ -> 132     0xA_ -> 213     0xB_ -> 231
//     low nibble   6 addsub  7 subadd
//                  8/9 madd   A/B msub   C/D fnmadd   E/F fnmsub
//                  (odd = the SCALAR form; even = packed)
//
// so (low >> 1) & 3 is the sign kind and (low & 1) is scalarness.  Writing the
// dispatch as a 60-entry table instead would make a single typo produce a
// silently wrong instruction rather than a compile error, and there is no
// irregularity in the space for a table to capture.
//
// ---------------------------------------------------------------------------
// CONTRACTS
// ---------------------------------------------------------------------------
// C1: a YMM is never one IR value.  FMA is lane-wise, so a VEX.256 form is two
//     independent V128 operations; both halves are computed before either is
//     written because the destination register is ALSO a source here.
// C3: a VEX.128 form zeroes bits 255:128 (VexWrite128 / VexWriteHalves do it).
//     The scalar forms zero them too -- and additionally preserve the rest of
//     the destination's low 128 bits, which is why they do not go through
//     VexWrite128.
//
// ---------------------------------------------------------------------------
// NaN PROPAGATION PRIORITY -- MEASURED, BECAUSE IT IS NOT OBVIOUS
// ---------------------------------------------------------------------------
// With three source operands there are two plausible priority rules, and they
// disagree: "the first NaN in the ENCODED operand order (op1, op2, op3)" and
// "the first NaN in the ARITHMETIC order (multiplicand, multiplier, addend)".
// They coincide only for 231.  avx_fma_test.cpp's `nanprio*` inputs put NaNs
// with distinguishable payloads in two or three positions at once -- the only
// arrangement in which the rule is observable at all -- and real x86 answered
// with the ARITHMETIC order: `vfmadd231ps` with a NaN in VEX.vvvv and another
// in the destination returns vvvv's, while `vfmadd132ps` on the same registers
// returns the DESTINATION's, because 132 makes the destination the
// multiplicand.  That is what VecFMulAdd implements (a, then b, then c), so
// the front end needs no per-order fixup.
//
// ---------------------------------------------------------------------------
// THE SCALAR MERGE, AND WHY IT IS NOT AN IR FLAG
// ---------------------------------------------------------------------------
// VFMADD*SS/SD compute lane 0 and copy the remaining lanes from the
// DESTINATION register.  The destination is op1 -- which is the multiplicand
// for 132, the multiplier for 213 and the addend for 231 -- so no single "the
// upper lanes come from operand N" rule covers the three orders, and an IR flag
// would have had to smuggle the x86 numbering into the IR to work.  Instead the
// packed opcode is evaluated over the full 128 bits and lane 0 is merged here.
// The discarded upper lanes cost nothing observable: this runtime does not
// model MXCSR exception flags, so a lane that is computed and thrown away has
// no side effect.  The memory operand of a scalar form is still loaded as 4 or
// 8 bytes (VexLoadScalarVec), never 16, so a scalar FMA one dword below a page
// boundary does not fault.
//
// ---------------------------------------------------------------------------
// TO BE MERGED INTO decoder.h / decoder.cc BY THE MAIN LINE
// ---------------------------------------------------------------------------
// (1) decoder.h, private section of X64Decoder, next to the DecodeAvxMul block:
//
//         // ---- FMA3 (decoder_avx_fma.cc) --------------------------------
//         bool DecodeAvxFma(const VexInsn& v);
//         void DecodeAvxFmaPacked(const VexInsn& v, u32 order, u32 flags, u32 lane_bits);
//         void DecodeAvxFmaScalar(const VexInsn& v, u32 order, u32 flags, u32 lane_bits);
//         void DecodeAvxFmaAddSub(const VexInsn& v, u32 order, bool sub_even, u32 lane_bits);
//
// (2) decoder.cc, the VEX dispatch inside Decode(): DecodeAvxFma(vex) joins the
//     `avx_on && (...)` chain, e.g.
//
//         (avx_on &&
//          (DecodeAvxMul(vex) || DecodeAvxFma(vex) || DecodeAvxInt(vex) ||
//           DecodeAvxFp(vex) || DecodeAvxHadd(vex) || DecodeAvxBlend(vex) ||
//           DecodeAvxGather(vex)))
//
//     Position is free.  DecodeAvxFma claims only (0F38, 66) opcodes
//     0x96..0x9F, 0xA6..0xAF and 0xB6..0xBF, which no other family touches:
//     DecodeAvxInt's 0F38 cases stop below 0x40, and the nearest neighbour --
//     DecodeAvxGather -- owns 0x90..0x93, four opcodes below where FMA starts.
//     Every path that returns false does so before emitting any IR, so a
//     decline never leaves a half-built block behind.
//
// (3) source/runtime/frontend/x86/CMakeLists.txt: add decoder_avx_fma.cc.
//     source/tests/CMakeLists.txt: add fuzz/avx_fma_test.cpp.
//
// (4) CPUID leaf 1 ECX bit 12 (FMA) is NOT set by this change.  Advertising it
//     is the main line's call once this is merged; until then a guest that
//     checks CPUID will not use FMA, while one that uses it unconditionally
//     (or was built with -mfma) now runs instead of dying.
//
// ---------------------------------------------------------------------------
// KNOWN DEVIATIONS
// ---------------------------------------------------------------------------
//  * MXCSR is not modelled, so neither the rounding mode nor the exception
//    flags an FMA raises are observable.  Everything here is round-to-nearest-
//    even, which is the default MXCSR and what every other FP path in this
//    front end already assumes.
//  * A 32-byte memory operand is two 16-byte accesses, so a page-straddling
//    fault is not indivisible and the reported fault address can be base+16.
//    Shared with the whole 256-bit front end.
//  * VEX.vvvv is required to be present (it names a real source in every FMA
//    encoding) but no reserved-field #UD is raised for other fields, matching
//    the rest of the front end.
//  * Scalar forms are VEX.LIG: VEX.L is ignored rather than rejected, which is
//    what the SDM specifies.  The reference data covers both L values of every
//    scalar mnemonic, so this is measured rather than assumed.
//
// ---------------------------------------------------------------------------
// MUTATION LOG -- what the reference data actually catches
// ---------------------------------------------------------------------------
// Each number is REFERENCE ROWS that stopped matching real x86, out of the
// 3360 in avx_fma_rosetta_ref.inc, with the rest of the tree unchanged.  (The
// test's `mismatches` counter is twice these: every row is run on both the JIT
// and the interpreter, and every mutation below broke both.)
//
//   132 decoded as 213 (OrderOperands)                            908 rows
//   fused step replaced by multiply-then-add, BOTH back ends       630 rows
//   product/addend sign bits not swapped (msub <-> fnmadd)        1128 rows
//   VEX.128 packed writes without zeroing 255:128 (contract C3)    672 rows
//   scalar merge taken from VEX.vvvv instead of the destination   1296 rows
//   vfmaddsub/vfmsubadd lane parity exchanged                      576 rows
//   NaN propagation priority reversed to c, b, a, both back ends   336 rows
//
// The fusion mutation is the one that matters most and it is also the one a
// differential can most easily fail to catch, so avx_fma_test.cpp does not
// merely rely on the rows: it recomputes every `fuse*` row the double-rounded
// way and REQUIRES, per mnemonic, that hardware disagreed.  If a future
// regeneration produced data that no longer distinguished the two, that
// assertion fires instead of the file silently going blind.

#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/vex_decoder.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

// The three arithmetic roles, resolved from the encoded operands.
struct FmaOperands {
    ir::Value a;  // multiplicand
    ir::Value b;  // multiplier
    ir::Value c;  // addend
};

// 132 -> op1*op3 + op2 ; 213 -> op2*op1 + op3 ; 231 -> op2*op3 + op1.
FmaOperands OrderOperands(u32 order, ir::Value op1, ir::Value op2, ir::Value op3) {
    switch (order) {
        case 0:
            return {op1, op3, op2};
        case 1:
            return {op2, op1, op3};
        default:
            return {op2, op3, op1};
    }
}

ir::Value Fma(ir::Assembler* as, const FmaOperands& o, u32 lane_bits, u32 flags) {
    return as->VecFMulAdd(o.a, o.b, o.c, ir::Imm(lane_bits), ir::Imm(flags))
            .SetType(ir::ValueType::V128);
}

// Interleave two whole-vector results so that the EVEN lanes come from `even`
// and the ODD lanes from `odd`.  UZP1 collects the even lanes of one operand
// into the low half, UZP2 the odd lanes of the other, and ZIP1 re-interleaves
// them; the same three steps work at 32- and 64-bit lanes, so vfmaddsub needs
// no width special case.
ir::Value InterleaveEvenOdd(ir::Assembler* as, ir::Value even, ir::Value odd, u32 lane_bits) {
    const auto bits = ir::Imm(lane_bits);
    auto e = as->VecUnzip(even, even, bits, ir::Imm(0u)).SetType(ir::ValueType::V128);
    auto o = as->VecUnzip(odd, odd, bits, ir::Imm(1u)).SetType(ir::ValueType::V128);
    return as->VecZip(e, o, bits, ir::Imm(0u)).SetType(ir::ValueType::V128);
}

}  // namespace

// vfmadd/vfmsub/vfnmadd/vfnmsub, packed, 128- and 256-bit.
void X64Decoder::DecodeAvxFmaPacked(const VexInsn& v, u32 order, u32 flags, u32 lane_bits) {
    if (!v.Is256()) {
        auto op1 = XmmRead(XmmOf(v.reg));
        auto op2 = XmmRead(XmmOf(v.vvvv));
        auto op3 = VexLoadVec(v);
        VexWrite128(v.reg, Fma(assembler, OrderOperands(order, op1, op2, op3), lane_bits, flags));
        return;
    }
    // Named locals: the destination is also a source, so both halves must be
    // computed before either is written, and argument evaluation order is
    // unspecified.
    auto op1_lo = XmmRead(XmmOf(v.reg));
    auto op1_hi = YmmHighRead(v.reg);
    auto op2_lo = XmmRead(XmmOf(v.vvvv));
    auto op2_hi = YmmHighRead(v.vvvv);
    auto op3 = VexLoadVec256(v);
    auto lo = Fma(assembler, OrderOperands(order, op1_lo, op2_lo, op3.lo), lane_bits, flags);
    auto hi = Fma(assembler, OrderOperands(order, op1_hi, op2_hi, op3.hi), lane_bits, flags);
    VexWrite256(v.reg, lo, hi);
}

// vfmaddsub / vfmsubadd, packed only.  `sub_even` selects vfmaddsub, whose
// EVEN lanes subtract the addend and whose odd lanes add it (SDM: DEST[63:0]
// uses `-`, DEST[127:64] uses `+`); vfmsubadd is the mirror image.
void X64Decoder::DecodeAvxFmaAddSub(const VexInsn& v, u32 order, bool sub_even, u32 lane_bits) {
    // Bit 1 of the flag word negates the addend; bit 0 (negate the product) is
    // never set by this family.
    const u32 add_flags = 0u;
    const u32 sub_flags = 2u;
    const u32 even_flags = sub_even ? sub_flags : add_flags;
    const u32 odd_flags = sub_even ? add_flags : sub_flags;
    const auto emit = [&](ir::Value op1, ir::Value op2, ir::Value op3) {
        const auto o = OrderOperands(order, op1, op2, op3);
        auto even = Fma(assembler, o, lane_bits, even_flags);
        auto odd = Fma(assembler, o, lane_bits, odd_flags);
        return InterleaveEvenOdd(assembler, even, odd, lane_bits);
    };
    if (!v.Is256()) {
        auto op1 = XmmRead(XmmOf(v.reg));
        auto op2 = XmmRead(XmmOf(v.vvvv));
        auto op3 = VexLoadVec(v);
        VexWrite128(v.reg, emit(op1, op2, op3));
        return;
    }
    auto op1_lo = XmmRead(XmmOf(v.reg));
    auto op1_hi = YmmHighRead(v.reg);
    auto op2_lo = XmmRead(XmmOf(v.vvvv));
    auto op2_hi = YmmHighRead(v.vvvv);
    auto op3 = VexLoadVec256(v);
    auto lo = emit(op1_lo, op2_lo, op3.lo);
    auto hi = emit(op1_hi, op2_hi, op3.hi);
    VexWrite256(v.reg, lo, hi);
}

// vfmadd*ss / *sd: lane 0 is computed, the rest of the destination's low 128
// bits are preserved and bits 255:128 are zeroed.  VEX.L is ignored (LIG).
void X64Decoder::DecodeAvxFmaScalar(const VexInsn& v, u32 order, u32 flags, u32 lane_bits) {
    auto op1 = XmmRead(XmmOf(v.reg));
    auto op2 = XmmRead(XmmOf(v.vvvv));
    // 4 or 8 bytes for a memory operand, never 16.
    auto op3 = VexLoadScalarVec(v, lane_bits);
    // The merge sources are read BEFORE the write below; both are plain
    // uniform reads of the destination register, whose lane 0 is also an
    // operand of the FMA in at least one of the three orders.
    auto dest_lo = XmmLo(XmmOf(v.reg));
    auto dest_hi = XmmHi(XmmOf(v.reg));
    auto result = Fma(assembler, OrderOperands(order, op1, op2, op3), lane_bits, flags);
    auto result_lo = __ VecExtract64(result, ir::Imm(0u)).SetType(ir::ValueType::U64);
    ir::Value lo;
    if (lane_bits == 32) {
        lo = __ Or(__ And(dest_lo, ir::Operand{ir::Imm(u64(0xFFFFFFFF00000000))}),
                   ir::Operand{__ And(result_lo, ir::Operand{ir::Imm(u64(0xFFFFFFFF))})});
    } else {
        lo = result_lo;
    }
    VexWriteHalves(v.reg, lo, dest_hi);
}

bool X64Decoder::DecodeAvxFma(const VexInsn& v) {
    if (!AvxEnabled() || !v.valid || v.map != VexMap::Map0F38 || v.pp != VexPP::P66) {
        return false;
    }
    // Every FMA3 encoding names a source in VEX.vvvv; 1111 there is not a
    // valid form of this family.
    if (!v.vvvv_valid) {
        return false;
    }
    u32 order;
    switch (v.opcode & 0xF0) {
        case 0x90:
            order = 0;  // 132
            break;
        case 0xA0:
            order = 1;  // 213
            break;
        case 0xB0:
            order = 2;  // 231
            break;
        default:
            return false;
    }
    const u32 low = v.opcode & 0x0Fu;
    // W0 = single precision, W1 = double, uniformly across the family.
    const u32 lane_bits = v.w ? 64u : 32u;
    if (low == 0x6 || low == 0x7) {
        DecodeAvxFmaAddSub(v, order, low == 0x6, lane_bits);
        return true;
    }
    if (low < 0x8) {
        // 0x90..0x95 / 0xA0..0xA5 / 0xB0..0xB5 are not FMA (0x90..0x93 are the
        // gathers, which have their own handler and their own VSIB shape).
        return false;
    }
    // 8/9 madd, A/B msub, C/D fnmadd, E/F fnmsub -- and the low bit is
    // scalarness.  The IR flag word negates the product in bit 0 and the
    // addend in bit 1, so the two bits of `kind` land swapped.
    const u32 kind = (low >> 1) & 3u;
    const u32 flags = ((kind >> 1) & 1u) | ((kind & 1u) << 1);
    if ((low & 1u) != 0) {
        DecodeAvxFmaScalar(v, order, flags, lane_bits);
    } else {
        DecodeAvxFmaPacked(v, order, flags, lane_bits);
    }
    return true;
}

#undef __

}  // namespace swift::x86
