// The last scattered VEX gaps: vroundps/pd/ss/sd, vdpps/vdppd and the VARIABLE
// form of vpermilpd.
//
// These are not a performance nicety.  An unimplemented VEX opcode reaches
// FALLBACK -> IllegalCode and kills the guest process outright.  `round[sp]d $9`
// is what C's floor/ceil/trunc/rint become once SSE4.1 is available -- it is
// literally the body of glibc's __floor_sse41 -- and the VEX spelling is what
// any -mavx build of that code emits; vdpps is what compilers emit for a short
// dot product and what every _mm_dp_ps intrinsic becomes.  Missing them is a
// crash, not a slowdown.
//
// ---------------------------------------------------------------------------
// INTEGRATION -- decoder.h and decoder.cc belong to the main line, so the
// additions they need are spelled out here as well as applied.  All are purely
// additive and sit alongside the equivalent lines the other VEX families carry.
// ---------------------------------------------------------------------------
//
// (1) decoder.h, private section of X64Decoder, next to the DecodeAvxBlend
//     block:
//
//         // ---- VEX round / dot-product / permilpd-var (decoder_avx_misc.cc)
//         bool DecodeAvxMisc(const VexInsn& v);
//         void DecodeAvxRound(const VexInsn& v, u32 lane_bits, bool scalar);
//         void DecodeAvxDotProduct(const VexInsn& v, u32 lane_bits);
//
// (2) decoder.cc, the VEX dispatch inside Decode(): one more `||
//     DecodeAvxMisc(vex)` term in the `avx_on && (...)` chain.  Order among the
//     AVX handlers is free -- the triples claimed below, (0F3A, 66, 08/09/0A/0B
//     and 40/41) and (0F38, 66, 0D), are claimed by no other handler, and every
//     decline happens BEFORE any IR is emitted so a decline never leaves a
//     half-built block behind.
//
// (3) source/runtime/frontend/x86/CMakeLists.txt: add decoder_avx_misc.cc.
//     source/tests/CMakeLists.txt: add fuzz/avx_misc_test.cpp.
//
// (4) One new IR opcode, generic rather than x86-shaped (see the note in
//     runtime/ir/ir.inc):
//         ir.inc                                 INST(VecFRoundInt, ...)
//         backend/arm64/jit/translator_alu.cpp   EmitVecFRoundInt (FRINTN/M/P/Z)
//         backend/interp/interpreter.cpp         RunVecFRoundInt
//
// ---------------------------------------------------------------------------
// VROUNDPS / VROUNDPD / VROUNDSS / VROUNDSD -- the imm8 IS the instruction
// ---------------------------------------------------------------------------
// imm8 (SDM Vol.2, ROUNDPD "Rounding Mode Field"):
//
//     bit 3   MXCSR.PE suppression: do not raise the precision exception
//     bit 2   0 = round per bits 1:0, 1 = round per MXCSR.RC
//     bits1:0 00 nearest-even   01 toward -inf   10 toward +inf   11 toward 0
//
// so the four "obvious" encodings are imm8 = 0/1/2/3, C's floor/ceil/trunc are
// the SUPPRESSING forms 9/10/11 (0x8 | mode), and `rint`/`nearbyint` are 4/12.
// All twelve reachable values are handled: bit 3 is accepted and ignored (this
// runtime raises no SSE exceptions at all and never sets an MXCSR status bit,
// so "suppress the precision exception" is already true), and bit 2 is the one
// that costs something.
//
// BIT 2 IS NOT DECORATION.  `nearbyint()` compiles to `vroundsd $4` and a guest
// that has called fesetround(FE_UPWARD) legitimately expects a different answer
// from the same instruction bytes.  ldmxcsr is modelled (DecodeMxcsr writes
// ThreadContext64::mxcsr), so MXCSR.RC is real, live guest state and is read
// here: the four roundings are computed and one is selected by RC (bits 14:13),
// branch-free.  That is 4x the FRINT work on the MXCSR-following path only.
//
// The alternative -- treating bit 2 as "nearest-even", the way the rest of this
// front end treats MXCSR -- was rejected because for THIS instruction the
// rounding mode is the entire semantics; getting it wrong is not a rounding
// difference in the last bit, it is floor() returning ceil().
//
// RESIDUAL DEVIATION: the rest of the front end still ignores MXCSR.RC
// (vcvtps2dq, vcvtss2si and the arithmetic all round to nearest), so a guest
// under FE_UPWARD gets a correct vroundps and a nearest-even vcvtps2dq.  That
// is strictly better than before and is called out rather than hidden.
//
// MID-POINTS ARE THE TEST.  round-half-even and round-half-away differ only on
// exact ties, so the reference data drives 0.5/1.5/2.5/-0.5/-1.5/-2.5 through
// every mode: nearest-even gives 0,2,2,-0,-2,-2 where round-half-away would
// give 1,2,3,-1,-2,-3.  A "round" implemented with std::round would pass every
// non-tie input and fail exactly there.
//
// ---------------------------------------------------------------------------
// VDPPS / VDPPD -- the imm8 that must NOT reach the IR
// ---------------------------------------------------------------------------
//     high nibble  which lanes take part in the multiply
//     low nibble   which lanes receive the sum; the others get +0.0
//
// That is a pure x86 encoding convention, so it stays here: the IR only ever
// sees a multiply, a bitwise AND and adds.  Two details are load-bearing.
//
// (a) THE MASK IS APPLIED AFTER THE MULTIPLY, not before.  Zeroing an OPERAND
//     and multiplying gives 0*inf = NaN; ANDing the PRODUCT with zero gives
//     exactly +0.0 for every input, which is what the SDM specifies for a
//     de-selected lane ("DEST <- +0.0").
//
// (b) THE ADDITION TREE IS THE SDM'S, IN ITS ORDER.  For VDPPS the SDM is
//         TMP2 = TMP1[0] + TMP1[1] ; TMP3 = TMP1[2] + TMP1[3] ; TMP4 = TMP2+TMP3
//     and x86 gives operand 1 of an add NaN priority, so the order decides
//     WHICH NaN comes out.  VecUnzip(prod, prod) produces the even stream as
//     operand 1 and the odd stream as operand 2, which reproduces that tree
//     exactly -- and VecFAdd already carries the x86 NaN-priority fixup, so no
//     NaN rule is restated here.
//
// VDPPD IS 128-BIT ONLY (there is no VEX.256 encoding; it is #UD), so the L=1
// form is DECLINED rather than invented.  vdpps has both widths and applies the
// same imm8 to each 128-bit lane independently.
//
// ---------------------------------------------------------------------------
// VPERMILPD, VARIABLE FORM (0F38 0D)
// ---------------------------------------------------------------------------
// The selector is BIT 1 of each 64-bit control element -- not bit 0, and not
// the whole element -- and it picks a qword of src1 WITHIN the same 128-bit
// lane.  decoder_avx_fp.cc has the imm8 form and the vpermilps variable form;
// this is the one member of the quartet that was left out.
//
// Built as a byte-granular table lookup like FpOpPermilPsVar, but the selector
// broadcast is a shift pair rather than a multiply: bit 1 of the qword is moved
// to bit 63 and then smeared back down arithmetically, giving an all-ones or
// all-zeros qword.  The multiply trick FpOpPermilPsVar uses cannot be reused
// because it broadcasts within a 32-bit lane and this selector must reach all
// eight bytes of a 64-bit one.
//
// ---------------------------------------------------------------------------
// CONTRACTS
// ---------------------------------------------------------------------------
// C1  A YMM is never one IR value: every 256-bit form below is two independent
//     V128 operations.  All three families are defined per 128-bit lane, so
//     that decomposition is exact rather than an approximation.
// C3  A VEX.128 destination write zeroes bits 255:128 (VexWrite128 does it); a
//     VEX.256 write fills both halves (VexWrite256).
//
// ---------------------------------------------------------------------------
// KNOWN DEVIATIONS (shared with the rest of the VEX front end)
// ---------------------------------------------------------------------------
//  * No segment override on the memory operand, no alignment check, and VEX.W
//    is not rejected where the SDM specifies W0 -- consistent with every other
//    decoder_avx*.cc file, which likewise decode the reserved encodings rather
//    than raising #UD.
//  * MXCSR's DAZ/FTZ bits are not honoured, and no exception status bit is
//    ever set (which is what makes vround's imm8 bit 3 a no-op).
//  * A 32-byte memory operand is two 16-byte accesses, so a page-straddling
//    fault is not indivisible.
//

#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/vex_decoder.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

constexpr auto kV128 = ir::ValueType::V128;
constexpr auto kU64 = ir::ValueType::U64;

// A 128-bit constant as {lo qword, hi qword}. There is no "materialize vector
// immediate" IR opcode; same construction as decoder_avx_fp.cc's FpConst128.
ir::Value MiscConst128(ir::Assembler* as, u64 lo, u64 hi) {
    auto low = as->VecDup64(as->LoadImm(ir::Imm(lo)).SetType(kU64)).SetType(kV128);
    if (lo == hi) {
        return low;
    }
    auto high = as->VecDup64(as->LoadImm(ir::Imm(hi)).SetType(kU64)).SetType(kV128);
    return as->VecZip(low, high, ir::Imm(64u), ir::Imm(0u)).SetType(kV128);
}

// An all-ones / all-zeros mask in every 64-bit lane, taken from bit `bit` of
// the scalar `word`: broadcast, shift the bit up to the sign position, then
// smear it back down arithmetically.
ir::Value BitLaneMask(ir::Assembler* as, ir::Value word, u32 bit) {
    auto up = as->LoadImm(ir::Imm(u64(63 - bit))).SetType(kU64);
    auto down = as->LoadImm(ir::Imm(u64(63))).SetType(kU64);
    auto spread = as->VecDup64(word).SetType(kV128);
    auto raised = as->VecShiftLeft(spread, up, ir::Imm(64u)).SetType(kV128);
    return as->VecShiftRightArithmetic(raised, down, ir::Imm(64u)).SetType(kV128);
}

// mask ? on : off, bitwise.  VecAndNot(a, b) is a & ~b.
ir::Value MaskSelect(ir::Assembler* as, ir::Value mask, ir::Value on, ir::Value off) {
    auto taken = as->VecAnd(on, mask).SetType(kV128);
    auto left = as->VecAndNot(off, mask).SetType(kV128);
    return as->VecOr(taken, left).SetType(kV128);
}

// One 128-bit lane of VROUNDPS/PD/SS/SD.  `merge` supplies the untouched lanes
// of a scalar form and is ignored otherwise.
ir::Value RoundLane(ir::Assembler* as,
                    ir::Value source,
                    ir::Value merge,
                    u32 lane_bits,
                    u8 imm8,
                    bool scalar) {
    const auto bits = ir::Imm(lane_bits);
    const auto sc = ir::Imm(u32(scalar));
    if ((imm8 & 0x04u) == 0) {
        // imm8[1:0] IS the IR mode: both use 0 nearest-even / 1 down / 2 up /
        // 3 zero, which is not a coincidence -- IEEE 754 lists them in that
        // order and every ISA that encodes them follows it.
        return as->VecFRoundInt(source, merge, bits, ir::Imm(u32(imm8 & 3u)), sc).SetType(kV128);
    }
    // MXCSR.RC (bits 14:13) selects, and it is runtime state, so all four
    // roundings are materialized and one is picked bitwise.
    ir::Uniform uni_mxcsr{offsetof(ThreadContext64, mxcsr), ir::ValueType::U32};
    auto mxcsr = as->ZeroExtend64(as->LoadUniform(uni_mxcsr));
    auto low_bit = BitLaneMask(as, mxcsr, 13);
    auto high_bit = BitLaneMask(as, mxcsr, 14);
    ir::Value candidate[4];
    for (u32 mode = 0; mode < 4; ++mode) {
        candidate[mode] =
                as->VecFRoundInt(source, merge, bits, ir::Imm(mode), sc).SetType(kV128);
    }
    auto lower = MaskSelect(as, low_bit, candidate[1], candidate[0]);
    auto upper = MaskSelect(as, low_bit, candidate[3], candidate[2]);
    return MaskSelect(as, high_bit, upper, lower);
}

// Which lanes a nibble of vdpps/vdppd's imm8 selects, as a 128-bit mask.
void DotMask(u32 lane_bits, u32 nibble, u64& lo, u64& hi) {
    lo = 0;
    hi = 0;
    const u32 count = 128 / lane_bits;
    const u32 per_qword = 64 / lane_bits;
    const u64 ones = lane_bits == 64 ? UINT64_MAX : UINT64_C(0xFFFFFFFF);
    for (u32 lane = 0; lane < count; ++lane) {
        if (((nibble >> lane) & 1u) == 0) {
            continue;
        }
        u64& half = (lane / per_qword) != 0 ? hi : lo;
        half |= ones << ((lane % per_qword) * lane_bits);
    }
}

// One 128-bit lane of VDPPS (lane_bits 32) / VDPPD (lane_bits 64).
// param packs the lane width low and the imm8 high, matching the convention in
// decoder_avx_int.cc.
ir::Value DotProductLane(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    const u32 lane_bits = param & 0xFFFFu;
    const u32 imm8 = param >> 16;
    const auto lanes = ir::Imm(lane_bits);

    u64 mul_lo = 0, mul_hi = 0, dst_lo = 0, dst_hi = 0;
    DotMask(lane_bits, (imm8 >> 4) & 0xFu, mul_lo, mul_hi);
    DotMask(lane_bits, imm8 & 0xFu, dst_lo, dst_hi);

    auto product = as->VecFMul(a, b, lanes).SetType(kV128);
    auto selected = as->VecAnd(product, MiscConst128(as, mul_lo, mul_hi)).SetType(kV128);

    // Pairwise add, twice for f32 and once for f64. VecUnzip's even stream is
    // operand 1, which is what puts the SDM's TMP2 first.
    auto even = as->VecUnzip(selected, selected, lanes, ir::Imm(0u)).SetType(kV128);
    auto odd = as->VecUnzip(selected, selected, lanes, ir::Imm(1u)).SetType(kV128);
    auto sum = as->VecFAdd(even, odd, lanes).SetType(kV128);
    if (lane_bits == 32) {
        auto even2 = as->VecUnzip(sum, sum, lanes, ir::Imm(0u)).SetType(kV128);
        auto odd2 = as->VecUnzip(sum, sum, lanes, ir::Imm(1u)).SetType(kV128);
        sum = as->VecFAdd(even2, odd2, lanes).SetType(kV128);
    }
    // Every lane now holds the total; keep the selected ones and zero the rest.
    return as->VecAnd(sum, MiscConst128(as, dst_lo, dst_hi)).SetType(kV128);
}

// VPERMILPD with a REGISTER control: bit 1 of each 64-bit control element picks
// a qword of src1 within the same 128-bit lane.  Turned into a byte-granular
// Tbl index: idx[8k+j] = sel(k) * 8 + j.
ir::Value PermilPdVar(ir::Assembler* as, ir::Value a, ir::Value control, u32, u32) {
    constexpr u64 kBytes = UINT64_C(0x0706050403020100);
    constexpr u64 kEight = UINT64_C(0x0808080808080808);
    auto up = as->LoadImm(ir::Imm(u64(62))).SetType(kU64);
    auto down = as->LoadImm(ir::Imm(u64(63))).SetType(kU64);
    auto raised = as->VecShiftLeft(control, up, ir::Imm(64u)).SetType(kV128);
    auto mask = as->VecShiftRightArithmetic(raised, down, ir::Imm(64u)).SetType(kV128);
    auto step = as->VecAnd(mask, MiscConst128(as, kEight, kEight)).SetType(kV128);
    auto index = as->VecAdd(MiscConst128(as, kBytes, kBytes), step, ir::Imm(8u)).SetType(kV128);
    return as->VecTableLookup8(a, index).SetType(kV128);
}

}  // namespace

// vroundps / vroundpd (2-operand, both widths) and vroundss / vroundsd
// (3-operand, lane 0 from r/m and the rest of the destination from VEX.vvvv).
void X64Decoder::DecodeAvxRound(const VexInsn& v, u32 lane_bits, bool scalar) {
    if (scalar) {
        auto source = VexLoadScalarVec(v, lane_bits);
        auto merge = XmmRead(XmmOf(v.vvvv));
        VexWrite128(v.reg, RoundLane(assembler, source, merge, lane_bits, v.imm8, true));
        return;
    }
    if (!v.Is256()) {
        auto source = VexLoadVec(v);
        // `merge` is unread when scalar == 0; pass the source rather than a
        // fresh uniform load so no dead value reaches RegAlloc.
        VexWrite128(v.reg, RoundLane(assembler, source, source, lane_bits, v.imm8, false));
        return;
    }
    auto source = VexLoadVec256(v);
    auto lo = RoundLane(assembler, source.lo, source.lo, lane_bits, v.imm8, false);
    auto hi = RoundLane(assembler, source.hi, source.hi, lane_bits, v.imm8, false);
    VexWrite256(v.reg, lo, hi);
}

// vdpps / vdppd: dst = dot(VEX.vvvv, r/m) under imm8, per 128-bit lane.
// DecodeAvxIntBinary already drives both widths with exactly this operand
// shape, including C3's upper-half zeroing at VEX.128.
void X64Decoder::DecodeAvxDotProduct(const VexInsn& v, u32 lane_bits) {
    DecodeAvxIntBinary(v, DotProductLane, lane_bits | (u32(v.imm8) << 16));
}

bool X64Decoder::DecodeAvxMisc(const VexInsn& v) {
    if (!AvxEnabled() || !v.valid) {
        return false;
    }
    switch (v.map) {
        case VexMap::Map0F38:
            // vpermilpd, register control. 0F38 0D exists only with the 66
            // mandatory prefix.
            if (v.opcode != 0x0D || v.pp != VexPP::P66) {
                return false;
            }
            DecodeAvxIntBinary(v, PermilPdVar, 0);
            return true;
        case VexMap::Map0F3A:
            if (v.pp != VexPP::P66) {
                return false;
            }
            switch (v.opcode) {
                case 0x08:  // vroundps
                    DecodeAvxRound(v, 32, false);
                    return true;
                case 0x09:  // vroundpd
                    DecodeAvxRound(v, 64, false);
                    return true;
                case 0x0A:  // vroundss
                    DecodeAvxRound(v, 32, true);
                    return true;
                case 0x0B:  // vroundsd
                    DecodeAvxRound(v, 64, true);
                    return true;
                case 0x40:  // vdpps
                    DecodeAvxDotProduct(v, 32);
                    return true;
                case 0x41:  // vdppd
                    if (v.l) {
                        return false;  // no VEX.256 encoding exists: #UD
                    }
                    DecodeAvxDotProduct(v, 64);
                    return true;
                default:
                    return false;
            }
        default:
            return false;
    }
}

#undef __

}  // namespace swift::x86
