// VEX horizontal / pairwise handlers: vhadd*, vhsub*, vphadd*, vphsub* and
// vpmaddubsw, at BOTH VEX.128 and VEX.256.
//
// These are not a performance nicety.  An unimplemented VEX opcode reaches
// FALLBACK -> IllegalCode and kills the guest process outright, and this family
// is what every hand-written SIMD reduction, every dot product and every
// libjpeg/libpng/ffmpeg colour-conversion kernel ends in.  Missing them is a
// crash, not a slowdown.
//
// ---------------------------------------------------------------------------
// WHAT "HORIZONTAL" ACTUALLY MEANS HERE  (measured, not assumed)
// ---------------------------------------------------------------------------
// The trap in this family is the 256-bit result order.  The intuitive reading --
// "all of SRC1's sums, then all of SRC2's" -- is WRONG.  Every one of these
// instructions is defined PER 128-BIT LANE, and within each lane the SRC1 sums
// come first and the SRC2 sums second:
//
//     vhaddps ymm0, ymm1, ymm2
//     ymm0[127:0]   = [ A0+A1, A2+A3, B0+B1, B2+B3 ]     (A = ymm1, B = ymm2)
//     ymm0[255:128] = [ A4+A5, A6+A7, B4+B5, B6+B7 ]
//
// Measured under Rosetta with A = 1,2,4,8,16,32,64,128 and
// B = 256,512,...,32768, which distinguishes every permutation:
//     3 12 768 3072 48 192 12288 49152
// and cross-checked against the Intel SDM's per-lane pseudocode for
// VHADDPS/VHADDPD/VPHADDW/VPHADDD, which agrees.
//
// That shape is exactly "deinterleave, then a lane-wise op":
//
//     even = UZP1(a, b)   = [a0 a2 .. b0 b2 ..]
//     odd  = UZP2(a, b)   = [a1 a3 .. b1 b3 ..]
//     dst  = even OP odd
//
// so the whole family is one new data-movement IR opcode (VecUnzip) plus the
// arithmetic opcodes that already exist.  Nothing about x86 leaks into the IR:
// VecUnzip is the dual of the VecZip that was already there, and the add /
// subtract / saturating add / saturating subtract all stay in VecAdd, VecSub,
// VecFAdd, VecFSub, VecSatAdd and VecSatSub.  Keeping the arithmetic unfused is
// deliberate -- a fused "pairwise add" opcode would need its own copy of the
// x86 NaN-propagation fixup in both backends, and that copy could drift from
// the one VecFAdd already carries.
//
// OPERAND ORDER WITHIN A PAIR IS NOT COSMETIC.
// For the subtracts it is the value: dst = EVEN - ODD (1-2 = -1, measured).
// For the adds it decides which NaN propagates, because x86 gives operand 1
// priority.  Measured under Rosetta with a distinct payload in each element of
// a pair (source/tests/fuzz/avx_hadd_rosetta_ref.inc, pairs f32nan / f64nan):
//
//   QNaN.111 + QNaN.222 -> 7FC00111   the EVEN element
//   QNaN.555 + SNaN.666 -> 7FC00555   even QNaN beats odd SNaN
//   SNaN.777 + QNaN.888 -> 7FC00777   and the mirror: even SNaN, quieted, wins
//   +inf     + -inf     -> FFC00000   the x86 "real indefinite", NOT AArch64's
//                                     default NaN 7FC00000
//
// and the same for vhaddpd / vhsubpd at 64 bits.  So: even-first, uniformly.
//
// The Intel SDM does not settle this.  Its NaN rule is stated for the two
// REGISTER operands, never for the two ELEMENTS of a pair, and its pseudocode
// is not even self-consistent about the element order -- HADDPS is written
// `SRC1[63:32] + SRC1[31:0]` (odd first) while HADDPD is written
// `SRC1[63:0] + SRC1[127:64]` (even first).  Taking the pseudocode as normative
// would make one instruction of a pair disagree with the other, which no real
// implementation does: both Intel and AMD build HADDPS from two shuffles
// feeding one ADDPS, whose first operand is the even stream.  Measurement and
// plausibility agree on even-first; the SDM is silent rather than contradicted.
// RESIDUAL RISK: Rosetta is the only executable reference available here, so
// this one rule rests on an emulator plus reasoning, not on Intel silicon.
//
// Emitting `VecFAdd(even, odd)` gets the whole rule for free -- VecFAdd's NaN
// fixup already implements operand-1 priority and the x86 indefinite.
//
// ---------------------------------------------------------------------------
// VPMADDUBSW: THE ASYMMETRIC ONE
// ---------------------------------------------------------------------------
// dst.word[i] = SaturateToSignedWord( u8(a[2i])*s8(b[2i]) + u8(a[2i+1])*s8(b[2i+1]) )
// The first operand's bytes are UNSIGNED and the second's are SIGNED, and only
// the SUM saturates -- the individual products do not, because they cannot:
//     max  255 *  127 =  32385  <=  32767
//     min  255 * -128 = -32640  >= -32768
// Every u8 x s8 product fits a signed 16-bit lane EXACTLY.  That is what makes
// the decomposition below sound: a plain 16-bit VecMul loses nothing, and a
// single signed 16-bit VecSatAdd then reproduces the architectural saturation.
// Verified against Rosetta at both endpoints (255*-128 twice -> -32768,
// 255*127 twice -> 32767) and on unstructured bytes (35*127 + 43*-128 = -1059).
//
// The unsigned/signed asymmetry lives HERE, in the front end, and not in the
// IR: it is expressed as four in-lane byte extensions (mask for the unsigned
// low byte, logical shift for the unsigned high byte, arithmetic shifts for the
// two signed ones) over existing generic opcodes.  An IR opcode that baked
// "operand 1 unsigned, operand 2 signed, pairwise, saturating" into one node
// would be an x86 instruction wearing an IR costume.
//
// ---------------------------------------------------------------------------
// INTEGRATION -- WHAT THIS FAMILY ADDS TO SHARED FILES
// ---------------------------------------------------------------------------
// Deliberately one declaration and one dispatch term, because every other
// helper is REUSED rather than duplicated: VexLoadVec / VexLoadVec256 /
// VexWrite128 / VexWrite256 come from decoder_avx_fp.cc, and AvxIntBinFn plus
// DecodeAvxIntBinary come from decoder_avx_int.cc -- this family's opcodes have
// exactly that shape, so it needs no driver of its own.
//
// (1) decoder.h, private section of X64Decoder:
//         bool DecodeAvxHadd(const VexInsn& v);
// (2) decoder.cc, the VEX dispatch in Decode(): one more `|| DecodeAvxHadd(vex)`
//     term.  Order among the AVX handlers is free -- no (map, pp, opcode)
//     triple claimed below is claimed by any other handler (0F 7C/7D are unused
//     there, and 0F38 01..07 are unused: the 0x01..0x07 cases in
//     decoder_avx_fp.cc and decoder_avx_int.cc are all on the 0F3A map).  Every
//     decline below happens before any IR is emitted, so a decline never leaves
//     a half-built block behind.
// (3) frontend/x86/CMakeLists.txt: `decoder_avx_hadd.cc`.
//
// Plus the one new IR opcode, generic rather than x86-shaped (it is the dual of
// the VecZip that was already there, and an ARM64 front end would reach for it
// to decode NEON's own UZP1/UZP2):
//     ir.inc                                 INST(VecUnzip, ...)
//     backend/arm64/jit/translator_alu.cpp   EmitVecUnzip  (UZP1/UZP2)
//     backend/interp/interpreter.cpp         RunVecUnzip
//
// ---------------------------------------------------------------------------
// KNOWN DEVIATIONS (shared with the rest of the VEX front end)
// ---------------------------------------------------------------------------
//  * No segment override is applied to the memory operand (VexInsn does not
//    carry one), no alignment check, and VEX.vvvv is not checked for the
//    reserved-value #UD.  Identical to decoder_avx_fp.cc / decoder_avx_int.cc.
//  * MXCSR is not consulted, so DAZ/FTZ are not honoured -- again matching
//    every other packed-float path here.
//

#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/vex_decoder.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

// Same packing convention as decoder_avx_int.cc: bits 15:0 are the lane width
// in bits, bits 31:16 a per-operation selector.
constexpr u32 Pack(u32 lane, u32 flag = 0) { return lane | (flag << 16); }
constexpr u32 Lane(u32 param) { return param & 0xFFFF; }
constexpr u32 Flag(u32 param) { return param >> 16; }

constexpr auto kV128 = ir::ValueType::V128;

// The two interleaved streams of the concatenation {a, b}: Stream(.., false)
// keeps the even-indexed lanes, Stream(.., true) the odd ones.  A lane-wise
// operation over the pair is a pairwise operation over a and b, with a's
// results in the low half of the destination and b's in the high half -- which
// is precisely the 128-bit lane layout every instruction in this file wants.
ir::Value Stream(ir::Assembler* as, ir::Value a, ir::Value b, u32 lane_bits, bool odd) {
    return as->VecUnzip(a, b, ir::Imm(lane_bits), ir::Imm(odd ? 1u : 0u)).SetType(kV128);
}

// --- floating point: vhaddps / vhaddpd / vhsubps / vhsubpd -----------------
// Flag(param): 0 = add, 1 = subtract.  Lane(param): 32 or 64.
// EVEN is operand 1 of the arithmetic, which is what makes the subtract come
// out as (even - odd) and gives the even element NaN priority on the add.
ir::Value OpHorizontalFloat(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    const u32 lane_bits = Lane(param);
    const auto lanes = ir::Imm(lane_bits);
    auto even = Stream(as, a, b, lane_bits, false);
    auto odd = Stream(as, a, b, lane_bits, true);
    return (Flag(param) ? as->VecFSub(even, odd, lanes) : as->VecFAdd(even, odd, lanes))
            .SetType(kV128);
}

// --- integer: vphaddw/d, vphaddsw, vphsubw/d, vphsubsw ---------------------
// Flag(param): 0 = add, 1 = subtract, 2 = saturating add, 3 = saturating sub.
// The saturating forms are signed-only; x86 has no unsigned horizontal form.
enum : u32 { kAdd = 0, kSub = 1, kSatAdd = 2, kSatSub = 3 };

ir::Value OpHorizontalInt(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    const u32 lane_bits = Lane(param);
    const auto lanes = ir::Imm(lane_bits);
    auto even = Stream(as, a, b, lane_bits, false);
    auto odd = Stream(as, a, b, lane_bits, true);
    switch (Flag(param)) {
        case kAdd:
            return as->VecAdd(even, odd, lanes).SetType(kV128);
        case kSub:
            return as->VecSub(even, odd, lanes).SetType(kV128);
        case kSatAdd:
            return as->VecSatAdd(even, odd, lanes, ir::Imm(1u)).SetType(kV128);
        case kSatSub:
            return as->VecSatSub(even, odd, lanes, ir::Imm(1u)).SetType(kV128);
        default:
            break;
    }
    PANIC("invalid horizontal integer op");
    return ir::Value{};
}

// --- vpmaddubsw ------------------------------------------------------------
// Each 16-bit lane holds one byte pair: bits 7:0 are element 2i and bits 15:8
// element 2i+1.  The four in-lane extensions below turn those bytes into four
// 16-bit vectors; see the header for why the 16-bit products are exact.
ir::Value OpMaddUbs(ir::Assembler* as, ir::Value a, ir::Value b, u32, u32) {
    const auto words = ir::Imm(16u);
    auto eight = as->LoadImm(ir::Imm(u64(8))).SetType(ir::ValueType::U64);

    // Unsigned low byte: mask rather than shift-pair, since a zero-extension
    // in place is just an AND and the constant costs one Dup.
    auto byte_mask = as->VecDup64(as->LoadImm(ir::Imm(u64(0x00FF00FF00FF00FFull)))
                                          .SetType(ir::ValueType::U64))
                             .SetType(kV128);
    auto a_low = as->VecAnd(a, byte_mask).SetType(kV128);
    // Unsigned high byte.
    auto a_high = as->VecShiftRight(a, eight, words).SetType(kV128);
    // Signed low byte: shift it up to the top of the lane, then back down
    // arithmetically.
    auto b_low = as->VecShiftRightArithmetic(as->VecShiftLeft(b, eight, words).SetType(kV128),
                                             eight,
                                             words)
                         .SetType(kV128);
    // Signed high byte.
    auto b_high = as->VecShiftRightArithmetic(b, eight, words).SetType(kV128);

    auto product_low = as->VecMul(a_low, b_low, words).SetType(kV128);
    auto product_high = as->VecMul(a_high, b_high, words).SetType(kV128);
    return as->VecSatAdd(product_low, product_high, words, ir::Imm(1u)).SetType(kV128);
}

}  // namespace

// Every opcode here has the plain three-operand shape -- destination in
// ModRM.reg, source 1 in VEX.vvvv, source 2 in ModRM.r/m -- and is defined per
// 128-bit lane, so DecodeAvxIntBinary (decoder_avx_int.cc) already drives both
// widths correctly, including contract C3's upper-half zeroing at VEX.128.
bool X64Decoder::DecodeAvxHadd(const VexInsn& v) {
    if (!AvxEnabled() || !v.valid) {
        return false;
    }
    switch (v.map) {
        case VexMap::Map0F:
            // 7C/7D exist only with a 66 (packed double) or F2 (packed single)
            // mandatory prefix; the no-prefix and F3 slots are #UD.
            if (v.opcode != 0x7C && v.opcode != 0x7D) {
                return false;
            }
            {
                const u32 sub = v.opcode == 0x7D ? 1u : 0u;
                if (v.pp == VexPP::PF2) {  // vhaddps / vhsubps
                    DecodeAvxIntBinary(v, OpHorizontalFloat, Pack(32, sub));
                    return true;
                }
                if (v.pp == VexPP::P66) {  // vhaddpd / vhsubpd
                    DecodeAvxIntBinary(v, OpHorizontalFloat, Pack(64, sub));
                    return true;
                }
            }
            return false;
        case VexMap::Map0F38:
            if (v.pp != VexPP::P66) {
                return false;
            }
            switch (v.opcode) {
                case 0x01:  // vphaddw
                    DecodeAvxIntBinary(v, OpHorizontalInt, Pack(16, kAdd));
                    return true;
                case 0x02:  // vphaddd
                    DecodeAvxIntBinary(v, OpHorizontalInt, Pack(32, kAdd));
                    return true;
                case 0x03:  // vphaddsw
                    DecodeAvxIntBinary(v, OpHorizontalInt, Pack(16, kSatAdd));
                    return true;
                case 0x04:  // vpmaddubsw
                    DecodeAvxIntBinary(v, OpMaddUbs, 0);
                    return true;
                case 0x05:  // vphsubw
                    DecodeAvxIntBinary(v, OpHorizontalInt, Pack(16, kSub));
                    return true;
                case 0x06:  // vphsubd
                    DecodeAvxIntBinary(v, OpHorizontalInt, Pack(32, kSub));
                    return true;
                case 0x07:  // vphsubsw
                    DecodeAvxIntBinary(v, OpHorizontalInt, Pack(16, kSatSub));
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
