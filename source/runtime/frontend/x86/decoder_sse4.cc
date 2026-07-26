// Legacy (non-VEX) SSE3 / SSSE3 / SSE4.1 / SSE4.2 instruction handlers.
//
// WHY THIS FILE EXISTS
// --------------------
// A 78-opcode probe over the legacy 66 0F 38 / 66 0F 3A / 0F 7C / 0F 7D / 0F D0
// encodings found 67 of them unimplemented.  An unimplemented legacy opcode is
// not a slowdown: X64Decoder::DecodeSwitch returns false, Decode() raises
// InterruptReason::FALLBACK and the runtime turns that into
// ExitReason::IllegalCode.  There is no interpreter fallback for an
// undecodable instruction, so the guest process dies.
//
// Only eleven of the family were reachable before this file: pshufb, palignr,
// pmuldq, pminud, pmaxud, pextrw, haddps, hsubps, movsldup, movshdup, movddup.
//
// The exposure is not hypothetical.  `-msse4.2` is the default floor for a
// large amount of shipping x86-64 (it is x86-64-v2), pcmpistri alone appears
// 302 times in the measured instruction census, and CPUID already advertises
// AVX -- and no real CPU has ever shipped AVX without SSE4.2, so any guest that
// dispatches on AVX has already been promised this whole family.
//
// ---------------------------------------------------------------------------
// INTEGRATION -- decoder.h and decoder.cc belong to the main line
// ---------------------------------------------------------------------------
// (1) decoder.h, private section of X64Decoder (append; nothing is reordered):
//
//         // ---- legacy SSE3/SSSE3/SSE4.1/SSE4.2 (decoder_sse4.cc) --------
//         // Single entry point, called from DecodeSwitch's `default:` arm.
//         bool DecodeSse4(_DInst& insn);
//         // Per-128-bit-lane callbacks, same shape as AvxIntBinFn/AvxIntUnFn.
//         using SseBinFn = ir::Value (*)(ir::Assembler*, ir::Value, ir::Value, u32);
//         using SseUnFn = ir::Value (*)(ir::Assembler*, ir::Value, u32);
//         void DecodeSseBinary(_DInst& insn, SseBinFn fn, u32 param);
//         void DecodeSseUnary(_DInst& insn, SseUnFn fn, u32 param);
//         ir::Value SseNarrowSrc(_DInst& insn, _Operand& op, u32 bytes);
//         void DecodeSseRound(_DInst& insn, u32 lane_bits, bool scalar);
//         void DecodeSsePTest(_DInst& insn);
//         void DecodeSseExtend(_DInst& insn, u32 src_bits, u32 dst_bits, bool is_signed);
//         void DecodeSseBlendVar(_DInst& insn, u32 lane_bits);
//         void DecodeSseInsertPs(_DInst& insn);
//         void DecodeSseExtract(_DInst& insn, u32 element_bits);
//         void DecodeSseInsert(_DInst& insn, u32 element_bits);
//         void DecodeSseMpsadbw(_DInst& insn);
//         void DecodeSsePhminposuw(_DInst& insn);
//
// (2) decoder.cc, X64Decoder::DecodeSwitch -- ONE line, the switch's default
//     arm.  Written this way on purpose: every other agent's change to this
//     switch is a new `case`, so a one-line change to `default` cannot
//     conflict with one, and an opcode another handler claims still wins
//     because control never reaches `default`.
//
//         -        default:
//         -            return false;
//         +        default:
//         +            return DecodeSse4(insn);
//
// (3) source/runtime/frontend/x86/CMakeLists.txt: add `decoder_sse4.cc` to
//     add_library(fronted_x86 ...).
//     source/tests/CMakeLists.txt: add `fuzz/sse4_test.cpp` to swift_test.
//
// (4) NO new IR opcode.  Everything here is expressed with primitives the VEX
//     twins already use; see "WHY THIS FILE ADDS NO IR OPCODE" below.
//
// (5) CPUID.  This file deliberately does NOT touch it.  Turning the feature
//     bits on is the main line's call, and the exact place is the kLeaf1Ecx
//     constant in decoder_misc.cc (X64Decoder::DecodeCpuid):
//
//         static constexpr u32 kLeaf1Ecx = (1u << 13)   // CMPXCHG16B
//                                          | (1u << 22) // MOVBE
//     +                                    | (1u << 0)  // SSE3
//     +                                    | (1u << 9)  // SSSE3
//     +                                    | (1u << 19) // SSE4.1
//     +                                    | (1u << 23) // POPCNT
//                                          | (1u << 30);// RDRAND
//
//     Bit 23 (POPCNT) is safe to set today: I_POPCNT is already implemented in
//     DecodeSwitch.  Do NOT set bit 20 (SSE4.2) until the pcmpXstrY family is
//     implemented -- SSE4.2's advertised contents are POPCNT, CRC32 and those
//     four string instructions, and the four are exactly the part this file
//     leaves out (see "NOT IMPLEMENTED" below).  Bits 0, 9 and 19 are fully
//     backed by what is here and by sse4_test.cpp's 4020 Rosetta rows.
//
//     Note the asymmetry this leaves: AVX is advertised (behind SVM_AVX +
//     SVM_XSAVE) while SSE4.2 is not, which no real CPU has ever done.  That
//     is a reason to finish the string family, not a reason to advertise it.
//
// ---------------------------------------------------------------------------
// THE ONE SEMANTIC DIFFERENCE FROM THE VEX TWINS
// ---------------------------------------------------------------------------
// A legacy SSE write to XMMn leaves bits 255:128 of YMMn UNCHANGED; a VEX.128
// write ZEROES them.  That is the opposite contract, and it is the only
// difference between most of the handlers here and their decoder_avx*.cc
// counterparts.
//
// It is honoured structurally rather than by remembering: XmmWrite / XmmLo /
// XmmHi write only the 128-bit xmm uniform and never touch ymm_high, and
// NOTHING in this file calls ZeroYmmHigh or VexWrite128.  A grep for either
// name in this file must return nothing.  sse4_test.cpp poisons ymm_high
// before every row and requires the poison back afterwards, so a stray
// zeroing fails a test rather than surviving as a latent bug.
//
// The second-most error-prone difference is BLENDVPS / BLENDVPD / PBLENDVB:
// the legacy forms take their mask from an IMPLICIT XMM0, where the VEX forms
// encode a register in the /is4 byte.  distorm reports the implicit operand as
// ops[2] (verified: `66 0F 38 10 CA` yields ops[2] = REG 91 = XMM0 even though
// the encoding has no field for it), and this file reads ops[2] rather than
// assuming XMM0, so a distorm change would surface as a wrong register rather
// than being silently papered over.
//
// ---------------------------------------------------------------------------
// RELATIONSHIP TO THE VEX HANDLERS -- DELIBERATE, ATTRIBUTED DUPLICATION
// ---------------------------------------------------------------------------
// Every lane function below is the same IR sequence as its VEX twin.  It is a
// copy rather than a call because the twins live in anonymous namespaces
// inside decoder_avx_int.cc / _hadd.cc / _blend.cc / _misc.cc, which this
// agent does not own; the alternative (synthesizing a VexInsn from a _DInst
// and calling the VEX handler, then restoring ymm_high) would put a fake
// operand descriptor between distorm and the IR, which is exactly the class of
// bug VexInsn was created to remove.
//
// Each copy names its origin in a comment so the main line can hoist the pair
// into a shared header in one mechanical pass.  The duplication is also
// TESTED rather than trusted: sse4_test.cpp's "legacy/VEX twin agreement"
// section runs the legacy form and the VEX form on identical inputs and
// requires bit-identical low 128 bits, so a copy that drifts from its origin
// fails immediately.  That check is the reason the duplication is acceptable.
//
// ---------------------------------------------------------------------------
// WHY THIS FILE ADDS NO IR OPCODE
// ---------------------------------------------------------------------------
// The IR is a multi-ISA middle end, so an opcode has to be meaningful to
// another front end and naturally implementable by another back end.  Nothing
// here needed one:
//
//   * ROUNDPS/PD/SS/SD is VecFRoundInt, which already exists for vroundps.
//   * The blends are the standard VecAnd / VecAndNot / VecOr bit-select; the
//     two x86-specific selector conventions (imm8 bit j names lane j; the sign
//     bit of XMM0's element j names lane j) are converted to one uniform lane
//     mask HERE, in the front end, where an encoding convention belongs.
//   * MPSADBW's "which 4 bytes are the needle and where the window starts" is
//     pure x86 encoding trivia; the IR sees only table lookups, min, max, sub
//     and add.  Same for INSERTPS's three-field imm8 and DPPS's two nibbles.
//   * PMULHRSW is a 16x16 multiply whose 32-bit product is rounded and
//     narrowed.  Built from VecMul + VecMulHigh16 + VecZip, which reconstruct
//     the exact 32-bit product, then shifts -- no approximation and no new
//     opcode.
//   * PHMINPOSUW returns a VALUE and its INDEX, which no vector primitive in
//     this IR expresses.  Rather than invent an x86-shaped "horizontal min
//     with index", it goes through CallLambda, the same escape hatch the
//     existing legacy SSE handlers use for pshufb/haddps/psadbw-style work.
//
// ---------------------------------------------------------------------------
// NOT IMPLEMENTED (stated, not hidden)
// ---------------------------------------------------------------------------
//   PCMPISTRI / PCMPISTRM / PCMPESTRI / PCMPESTRM  (SSE4.2 string compare)
//     These four are a small interpreter each: imm8 selects one of four
//     aggregation functions over a 16x16 byte/word comparison matrix, then
//     polarity inversion, then either an index into ECX or a mask into XMM0,
//     plus CF/ZF/SF/OF/AF/PF -- and the explicit-length forms read EAX/EDX as
//     SIGNED lengths whose absolute value saturates at 16.  Getting half of
//     that right is worse than not decoding it: today the guest dies with
//     IllegalCode, whereas a half-implementation returns a wrong index and the
//     guest's strlen/strstr silently reads the wrong memory.  They therefore
//     stay unclaimed and keep the current (loud) failure mode.
//     pcmpistri is the single highest-frequency item in the census (302
//     occurrences), so this is the largest remaining gap in the family.
//
//   MONITOR / MWAIT (SSE3): privileged, no guest kernel, deliberately absent.
//
// ---------------------------------------------------------------------------
// KNOWN DEVIATIONS
// ---------------------------------------------------------------------------
//  * No #GP on a misaligned operand.  Every instruction here except MOVNTDQA
//    and LDDQU requires 16-byte alignment of a memory operand; this front end
//    models no alignment check anywhere, so the deviation is inherited rather
//    than introduced.
//  * MOVNTDQA is a plain aligned load: non-temporal hinting has no IR
//    representation and none is architecturally observable.
//  * MXCSR's DAZ/FTZ are not honoured and no exception status bit is ever set
//    (which is what makes ROUND's imm8 bit 3, "suppress precision exception",
//    a genuine no-op here).  MXCSR.RC *is* honoured, by ROUND's imm8 bit 2
//    only, exactly as decoder_avx_misc.cc does it for vround.
//  * The MMX forms of the SSSE3 opcodes (no 66 prefix, MM registers) are
//    REJECTED rather than executed -- this runtime models no MMX register
//    file.  Rejection keeps the existing IllegalCode failure; executing them
//    against the XMM file would silently compute against another register.
//

#include <cstdlib>
#include <cstring>

#include "runtime/frontend/x86/decoder_internal.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

constexpr auto kV128 = ir::ValueType::V128;
constexpr auto kU64 = ir::ValueType::U64;

// Same param packing as decoder_avx_int.cc: lane width in bits 15:0, a
// per-operation flag (or an imm8) in bits 31:16.
constexpr u32 Pack(u32 lane, u32 flag = 0) { return lane | (flag << 16); }
constexpr u32 Lane(u32 param) { return param & 0xFFFFu; }
constexpr u32 Flag(u32 param) { return param >> 16; }

// `value` replicated into every lane_bits-wide field of a qword.
constexpr u64 Replicate(u32 lane_bits, u64 value) {
    const u64 mask = lane_bits == 64 ? ~u64(0) : ((u64(1) << lane_bits) - 1);
    u64 out = 0;
    for (u32 shift = 0; shift < 64; shift += lane_bits) {
        out |= (value & mask) << shift;
    }
    return out;
}

// Materialize an arbitrary 128-bit constant.  Byte-identical to VecConst in
// decoder_avx_int.cc / decoder_avx_blend.cc and MiscConst128 in
// decoder_avx_misc.cc (four copies already exist upstream; this is the fifth
// and they should all become one).
ir::Value VecConst(ir::Assembler* as, u64 lo, u64 hi) {
    auto low = as->VecDup64(as->LoadImm(ir::Imm(lo)).SetType(kU64)).SetType(kV128);
    if (lo == hi) {
        return low;
    }
    auto high = as->VecDup64(as->LoadImm(ir::Imm(hi)).SetType(kU64)).SetType(kV128);
    return as->VecZip(low, high, ir::Imm(64u), ir::Imm(0u)).SetType(kV128);
}

// mask ? on : off, bitwise.  VecAndNot(a, b) is a AND NOT b.
// Same as BitSelect in decoder_avx_blend.cc and MaskSelect in
// decoder_avx_misc.cc.
ir::Value BitSelect(ir::Assembler* as, ir::Value mask, ir::Value on, ir::Value off) {
    auto taken = as->VecAnd(on, mask).SetType(kV128);
    auto left = as->VecAndNot(off, mask).SetType(kV128);
    return as->VecOr(taken, left).SetType(kV128);
}

// An all-ones / all-zeros mask in every 64-bit lane taken from bit `bit` of a
// scalar.  Same as BitLaneMask in decoder_avx_misc.cc.
ir::Value BitLaneMask(ir::Assembler* as, ir::Value word, u32 bit) {
    auto up = as->LoadImm(ir::Imm(u64(63 - bit))).SetType(kU64);
    auto down = as->LoadImm(ir::Imm(u64(63))).SetType(kU64);
    auto spread = as->VecDup64(word).SetType(kV128);
    auto raised = as->VecShiftLeft(spread, up, ir::Imm(64u)).SetType(kV128);
    return as->VecShiftRightArithmetic(raised, down, ir::Imm(64u)).SetType(kV128);
}

// The two 64-bit halves of a 128-bit lane mask in which lane j of width
// `element` is all-ones exactly when bit j of `imm` is set.
void ImmLaneMask(u32 element, u32 imm, u64& lo, u64& hi) {
    lo = 0;
    hi = 0;
    const u32 bytes = element / 8;
    const u32 count = 128 / element;
    for (u32 i = 0; i < count; ++i) {
        if (((imm >> i) & 1u) == 0) {
            continue;
        }
        for (u32 byte = 0; byte < bytes; ++byte) {
            const u32 position = i * bytes + byte;
            (position < 8 ? lo : hi) |= u64(0xFF) << ((position % 8) * 8);
        }
    }
}

// ---------------------------------------------------------------------------
// Per-lane operations.  Each is the IR sequence of the named VEX twin.
// ---------------------------------------------------------------------------

// == decoder_avx_int.cc OpMul
ir::Value OpMul(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    return as->VecMul(a, b, ir::Imm(Lane(param))).SetType(kV128);
}

// == decoder_avx_int.cc OpMin / OpMax
ir::Value OpMin(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    return as->VecMin(a, b, ir::Imm(Lane(param)), ir::Imm(Flag(param))).SetType(kV128);
}

ir::Value OpMax(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    return as->VecMax(a, b, ir::Imm(Lane(param)), ir::Imm(Flag(param))).SetType(kV128);
}

// == decoder_avx_fp.cc FpOpCmpEq / FpOpCmpGt (used for pcmpeqq / pcmpgtq)
ir::Value OpCmpEq(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    return as->VecCmpEq(a, b, ir::Imm(Lane(param))).SetType(kV128);
}

ir::Value OpCmpGt(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    return as->VecCmpGt(a, b, ir::Imm(Lane(param))).SetType(kV128);
}

// == decoder_avx_int.cc OpPackUsdw.  VecPack cannot be used: the back end
// honours the unsigned-destination flag only for 16-bit sources and emits
// SQXTN for 32-bit ones, which saturates into the SIGNED 16-bit range, so
// every value in 0x8000..0xFFFF would come back 0x7FFF.  Clamp into
// [0, 0xFFFF] first (which makes the narrowing exact), then gather the low
// halfwords.  VecTableLookup8 masks its control with 0x8F, so index 0x80
// selects nothing and yields a zero byte -- that is what lets the two halves
// be OR-ed.
ir::Value OpPackUsdw(ir::Assembler* as, ir::Value a, ir::Value b, u32) {
    auto zero = VecConst(as, 0, 0);
    constexpr u64 kCap = Replicate(32, 0xFFFF);
    auto ceiling = VecConst(as, kCap, kCap);
    const auto lanes = ir::Imm(32u);
    const auto sign = ir::Imm(1u);
    auto ca = as->VecMin(as->VecMax(a, zero, lanes, sign).SetType(kV128), ceiling, lanes, sign)
                      .SetType(kV128);
    auto cb = as->VecMin(as->VecMax(b, zero, lanes, sign).SetType(kV128), ceiling, lanes, sign)
                      .SetType(kV128);
    constexpr u64 kGather = 0x0D0C090805040100ull;
    constexpr u64 kNone = 0x8080808080808080ull;
    auto from_a = as->VecTableLookup8(ca, VecConst(as, kGather, kNone)).SetType(kV128);
    auto from_b = as->VecTableLookup8(cb, VecConst(as, kNone, kGather)).SetType(kV128);
    return as->VecOr(from_a, from_b).SetType(kV128);
}

// == decoder_avx_int.cc OpSign.  PSIGN: dst = b < 0 ? -a : (b == 0 ? 0 : a).
// The zero case is a separate clause in the SDM, not a consequence of the
// negation, so it needs its own mask.
ir::Value OpSign(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    const auto lanes = ir::Imm(Lane(param));
    auto zero = VecConst(as, 0, 0);
    auto negative = as->VecCmpGt(zero, b, lanes).SetType(kV128);
    auto is_zero = as->VecCmpEq(b, zero, lanes).SetType(kV128);
    auto negated = as->VecSub(zero, a, lanes).SetType(kV128);
    auto picked = as->VecOr(as->VecAnd(negated, negative).SetType(kV128),
                            as->VecAndNot(a, negative).SetType(kV128))
                          .SetType(kV128);
    return as->VecAndNot(picked, is_zero).SetType(kV128);
}

// == decoder_avx_int.cc OpAbs.  max(x, -x) is exact including INT_MIN, where
// x86 leaves INT_MIN unchanged because the true absolute value does not fit --
// and max(INT_MIN, -INT_MIN) is INT_MIN for the same wrapping reason.
ir::Value OpAbs(ir::Assembler* as, ir::Value a, u32 param) {
    const auto lanes = ir::Imm(Lane(param));
    auto zero = VecConst(as, 0, 0);
    auto negated = as->VecSub(zero, a, lanes).SetType(kV128);
    return as->VecMax(a, negated, lanes, ir::Imm(1u)).SetType(kV128);
}

// == decoder_avx_hadd.cc Stream / OpHorizontalInt / OpHorizontalFloat.
// The two interleaved streams of the concatenation {a, b}: a lane-wise
// operation over the pair is a pairwise operation over a and b, with a's
// results low and b's high -- exactly the 128-bit lane layout these want.
// EVEN is operand 1, which makes the subtract come out (even - odd) and gives
// the even element NaN priority on the float add.
ir::Value Stream(ir::Assembler* as, ir::Value a, ir::Value b, u32 lane_bits, bool odd) {
    return as->VecUnzip(a, b, ir::Imm(lane_bits), ir::Imm(odd ? 1u : 0u)).SetType(kV128);
}

enum : u32 { kHAdd = 0, kHSub = 1, kHSatAdd = 2, kHSatSub = 3 };

ir::Value OpHorizontalInt(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    const u32 lane_bits = Lane(param);
    const auto lanes = ir::Imm(lane_bits);
    auto even = Stream(as, a, b, lane_bits, false);
    auto odd = Stream(as, a, b, lane_bits, true);
    switch (Flag(param)) {
        case kHAdd:
            return as->VecAdd(even, odd, lanes).SetType(kV128);
        case kHSub:
            return as->VecSub(even, odd, lanes).SetType(kV128);
        case kHSatAdd:
            return as->VecSatAdd(even, odd, lanes, ir::Imm(1u)).SetType(kV128);
        case kHSatSub:
            return as->VecSatSub(even, odd, lanes, ir::Imm(1u)).SetType(kV128);
        default:
            break;
    }
    PANIC("invalid horizontal integer op");
    return ir::Value{};
}

ir::Value OpHorizontalFloat(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    const u32 lane_bits = Lane(param);
    const auto lanes = ir::Imm(lane_bits);
    auto even = Stream(as, a, b, lane_bits, false);
    auto odd = Stream(as, a, b, lane_bits, true);
    return (Flag(param) ? as->VecFSub(even, odd, lanes) : as->VecFAdd(even, odd, lanes))
            .SetType(kV128);
}

// == decoder_avx_hadd.cc OpMaddUbs.  Each 16-bit lane holds one byte pair:
// bits 7:0 are element 2i (UNSIGNED from the first operand, SIGNED from the
// second) and bits 15:8 element 2i+1.  Both products fit a 16-bit lane exactly
// (|255 * -128| = 32640 < 32768), so only the final add saturates.
ir::Value OpMaddUbs(ir::Assembler* as, ir::Value a, ir::Value b, u32) {
    const auto words = ir::Imm(16u);
    auto eight = as->LoadImm(ir::Imm(u64(8))).SetType(kU64);
    auto byte_mask = VecConst(as, 0x00FF00FF00FF00FFull, 0x00FF00FF00FF00FFull);
    auto a_low = as->VecAnd(a, byte_mask).SetType(kV128);
    auto a_high = as->VecShiftRight(a, eight, words).SetType(kV128);
    auto b_low = as->VecShiftRightArithmetic(as->VecShiftLeft(b, eight, words).SetType(kV128),
                                             eight,
                                             words)
                         .SetType(kV128);
    auto b_high = as->VecShiftRightArithmetic(b, eight, words).SetType(kV128);
    auto product_low = as->VecMul(a_low, b_low, words).SetType(kV128);
    auto product_high = as->VecMul(a_high, b_high, words).SetType(kV128);
    return as->VecSatAdd(product_low, product_high, words, ir::Imm(1u)).SetType(kV128);
}

// PMULHRSW (SSSE3) -- no VEX twin exists upstream either.
//     DEST.word[i] = (((SRC1.word[i] * SRC2.word[i]) >> 14) + 1) >> 1
// with a SIGNED 16x16 -> 32 multiply and ARITHMETIC shifts of the 32-bit
// temporary.  Doing it in 16-bit lanes is NOT possible: bit 16 of (t + 1)
// becomes bit 15 of the result, so the intermediate must really be 32 bits.
//
// The exact 32-bit product is reconstructed from the two halves the IR already
// provides: VecMul gives bits 15:0 of each product and VecMulHigh16 (signed)
// bits 31:16, and VecZip at 16 bits interleaves them into 32-bit lanes --
// Zip1 for products 0..3 and Zip2 for 4..7.  Then shift, add one, shift, and
// gather the low halfword of each dword (the same 0x80-poisoned table trick
// OpPackUsdw uses).
//
// a = -32768, b = -32768 is the one case that overflows 16 bits: the true
// answer is +32768, and x86 returns 0x8000.  Truncating the 17-bit result to
// the low halfword reproduces that without a special case.
ir::Value OpMulHrsw(ir::Assembler* as, ir::Value a, ir::Value b, u32) {
    const auto words = ir::Imm(16u);
    const auto dwords = ir::Imm(32u);
    auto low = as->VecMul(a, b, words).SetType(kV128);
    auto high = as->VecMulHigh16(a, b, ir::Imm(1u)).SetType(kV128);
    auto product_lo = as->VecZip(low, high, words, ir::Imm(0u)).SetType(kV128);
    auto product_hi = as->VecZip(low, high, words, ir::Imm(1u)).SetType(kV128);
    auto fourteen = as->LoadImm(ir::Imm(u64(14))).SetType(kU64);
    auto one_bit = as->LoadImm(ir::Imm(u64(1))).SetType(kU64);
    auto ones = VecConst(as, Replicate(32, 1), Replicate(32, 1));
    const auto round = [&](ir::Value product) {
        auto shifted = as->VecShiftRightArithmetic(product, fourteen, dwords).SetType(kV128);
        auto bumped = as->VecAdd(shifted, ones, dwords).SetType(kV128);
        return as->VecShiftRightArithmetic(bumped, one_bit, dwords).SetType(kV128);
    };
    constexpr u64 kGather = 0x0D0C090805040100ull;
    constexpr u64 kNone = 0x8080808080808080ull;
    auto from_lo = as->VecTableLookup8(round(product_lo), VecConst(as, kGather, kNone))
                           .SetType(kV128);
    auto from_hi = as->VecTableLookup8(round(product_hi), VecConst(as, kNone, kGather))
                           .SetType(kV128);
    return as->VecOr(from_lo, from_hi).SetType(kV128);
}

// == decoder_avx_int.cc OpBlendImm / decoder_avx_blend.cc OpBlendFpImm.
// BLENDPS / BLENDPD / PBLENDW: imm8 bit j selects lane j from the SOURCE.
// The control is a decode-time constant, so this is three bitwise ops.
ir::Value OpBlendImm(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    u64 lo = 0, hi = 0;
    ImmLaneMask(Lane(param), Flag(param), lo, hi);
    return BitSelect(as, VecConst(as, lo, hi), b, a);
}

// ADDSUBPS / ADDSUBPD (SSE3).  No VEX twin exists upstream.
//     even lanes:  DEST = DEST - SRC
//     odd  lanes:  DEST = DEST + SRC
// Both are computed and blended with a constant lane mask rather than being
// built from a shuffle, so each lane's NaN and signed-zero behaviour is the
// plain VecFSub / VecFAdd one that the rest of the front end already pins.
ir::Value OpAddSub(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    const u32 lane_bits = Lane(param);
    const auto lanes = ir::Imm(lane_bits);
    auto sum = as->VecFAdd(a, b, lanes).SetType(kV128);
    auto difference = as->VecFSub(a, b, lanes).SetType(kV128);
    // Odd lanes (1, 3, ...) take the sum.
    const u32 odd = lane_bits == 32 ? 0xAu : 0x2u;
    u64 lo = 0, hi = 0;
    ImmLaneMask(lane_bits, odd, lo, hi);
    return BitSelect(as, VecConst(as, lo, hi), sum, difference);
}

// == decoder_avx_misc.cc RoundLane.  One 128-bit lane of ROUNDPS/PD/SS/SD;
// `merge` supplies the untouched lanes of a scalar form.
//
// imm8: bit 3 suppresses the precision exception (a no-op here -- this runtime
// sets no MXCSR status bit), bit 2 means "round per MXCSR.RC", bits 1:0 are
// the mode.  imm8[1:0] IS the IR mode: both number them nearest-even / down /
// up / zero, which is not a coincidence (IEEE 754 lists them in that order).
ir::Value RoundLane(ir::Assembler* as,
                    ir::Value source,
                    ir::Value merge,
                    u32 lane_bits,
                    u8 imm8,
                    bool scalar) {
    const auto bits = ir::Imm(lane_bits);
    const auto sc = ir::Imm(u32(scalar));
    if ((imm8 & 0x04u) == 0) {
        return as->VecFRoundInt(source, merge, bits, ir::Imm(u32(imm8 & 3u)), sc).SetType(kV128);
    }
    // MXCSR.RC (bits 14:13) is live guest state (DecodeMxcsr writes it), so all
    // four roundings are materialized and one is picked bitwise.
    ir::Uniform uni_mxcsr{offsetof(ThreadContext64, mxcsr), ir::ValueType::U32};
    auto mxcsr = as->ZeroExtend64(as->LoadUniform(uni_mxcsr));
    auto low_bit = BitLaneMask(as, mxcsr, 13);
    auto high_bit = BitLaneMask(as, mxcsr, 14);
    ir::Value candidate[4];
    for (u32 mode = 0; mode < 4; ++mode) {
        candidate[mode] = as->VecFRoundInt(source, merge, bits, ir::Imm(mode), sc).SetType(kV128);
    }
    auto lower = BitSelect(as, low_bit, candidate[1], candidate[0]);
    auto upper = BitSelect(as, low_bit, candidate[3], candidate[2]);
    return BitSelect(as, high_bit, upper, lower);
}

// == decoder_avx_misc.cc DotMask / DotProductLane.
// DPPS / DPPD imm8: the high nibble names the lanes that take part in the
// multiply, the low nibble the lanes that receive the sum (the rest get +0.0).
//
// Two details are load-bearing.  (a) The mask is applied AFTER the multiply:
// zeroing an operand and multiplying gives 0 * inf = NaN, whereas ANDing the
// PRODUCT with zero gives exactly +0.0, which is what the SDM specifies.
// (b) The addition tree is the SDM's, in its order -- VecUnzip's even stream
// is operand 1, which puts the SDM's TMP2 first and so decides which NaN wins.
ir::Value OpDotProduct(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    const u32 lane_bits = Lane(param);
    const u32 imm8 = Flag(param);
    const auto lanes = ir::Imm(lane_bits);

    u64 mul_lo = 0, mul_hi = 0, dst_lo = 0, dst_hi = 0;
    ImmLaneMask(lane_bits, (imm8 >> 4) & 0xFu, mul_lo, mul_hi);
    ImmLaneMask(lane_bits, imm8 & 0xFu, dst_lo, dst_hi);

    auto product = as->VecFMul(a, b, lanes).SetType(kV128);
    auto selected = as->VecAnd(product, VecConst(as, mul_lo, mul_hi)).SetType(kV128);

    auto even = as->VecUnzip(selected, selected, lanes, ir::Imm(0u)).SetType(kV128);
    auto odd = as->VecUnzip(selected, selected, lanes, ir::Imm(1u)).SetType(kV128);
    auto sum = as->VecFAdd(even, odd, lanes).SetType(kV128);
    if (lane_bits == 32) {
        auto even2 = as->VecUnzip(sum, sum, lanes, ir::Imm(0u)).SetType(kV128);
        auto odd2 = as->VecUnzip(sum, sum, lanes, ir::Imm(1u)).SetType(kV128);
        sum = as->VecFAdd(even2, odd2, lanes).SetType(kV128);
    }
    return as->VecAnd(sum, VecConst(as, dst_lo, dst_hi)).SetType(kV128);
}

// MPSADBW (SSE4.1).  No VEX twin exists upstream.
//     needle  = 4 bytes of SRC starting at (imm8[1:0] * 4)
//     window  = DEST bytes starting at (imm8[2] * 4)
//     DEST.word[i] = sum over j in 0..3 of |window[i + j] - needle[j]|
//
// Where the needle and the window start is pure x86 encoding, so it is
// resolved here into eight constant byte-gather indices.  For each j the two
// operands are gathered STRAIGHT INTO 16-bit lanes: VecTableLookup8's control
// is masked with 0x8F, so an index of 0x80 yields a zero byte, and an index
// vector of {b, 0x80, b+1, 0x80, ...} both selects and zero-extends in one
// step.  |x - y| on values that are known to be 0..255 is then just
// max - min, and the four differences add without overflow (4 * 255 = 1020).
ir::Value OpMpsadbw(ir::Assembler* as, ir::Value a, ir::Value b, u32 param) {
    const u32 imm8 = Flag(param);
    const u32 needle_at = (imm8 & 3u) * 4u;
    const u32 window_at = ((imm8 >> 2) & 1u) * 4u;
    const auto words = ir::Imm(16u);
    ir::Value total{};
    for (u32 j = 0; j < 4; ++j) {
        // Eight consecutive window bytes, zero-extended into eight words.
        u64 window_index[2] = {0, 0};
        for (u32 i = 0; i < 8; ++i) {
            const u32 byte = window_at + i + j;
            // window_at + 7 + 3 == 14 at most, so this never leaves the vector.
            window_index[i / 4] |= u64(byte) << ((i % 4) * 16);
            window_index[i / 4] |= u64(0x80) << ((i % 4) * 16 + 8);
        }
        auto window = as->VecTableLookup8(a, VecConst(as, window_index[0], window_index[1]))
                              .SetType(kV128);
        // One needle byte broadcast into all eight words.
        const u64 needle_word = u64(needle_at + j) | 0x8000ull;
        const u64 needle_qword = Replicate(16, needle_word);
        auto needle = as->VecTableLookup8(b, VecConst(as, needle_qword, needle_qword))
                              .SetType(kV128);
        auto high = as->VecMax(window, needle, words, ir::Imm(0u)).SetType(kV128);
        auto low = as->VecMin(window, needle, words, ir::Imm(0u)).SetType(kV128);
        auto absolute = as->VecSub(high, low, words).SetType(kV128);
        total = j == 0 ? absolute : as->VecAdd(total, absolute, words).SetType(kV128);
    }
    return total;
}

// PHMINPOSUW (SSE4.1): the smallest UNSIGNED word, its INDEX in word 1, and
// zeros above.  "Minimum with its position" has no vector primitive in this
// IR, and inventing one would be inventing an x86 shape, so this is the one
// operation here that goes through CallLambda -- the same escape hatch
// decoder_sse.cc already uses for pshufb/haddps/palignr-class work.
//
// Ties: the SDM takes the LOWEST index, so the comparison must be strict.
u64 Phminposuw64(u64 lo, u64 hi, u64) {
    u32 best = 0xFFFFFFFFu;
    u32 best_index = 0;
    for (u32 i = 0; i < 8; ++i) {
        const u32 word = u32(((i < 4 ? lo : hi) >> ((i % 4) * 16)) & 0xFFFFu);
        if (word < best) {
            best = word;
            best_index = i;
        }
    }
    return u64(best) | (u64(best_index) << 16);
}

}  // namespace

// ---------------------------------------------------------------------------
// Operand plumbing
// ---------------------------------------------------------------------------

// The r/m operand of a form whose SOURCE is narrower than its destination
// (pmovsx / pmovzx).  Reading the architectural number of bytes rather than a
// full V128 matters: a 2-byte pmovsxbq at the end of a page must not fault,
// and a 16-byte load there would.  == AvxIntNarrowSrc in decoder_avx_int.cc.
ir::Value X64Decoder::SseNarrowSrc(_DInst& insn, _Operand& op, u32 bytes) {
    if (op.type == O_REG) {
        return XmmRead(static_cast<_RegisterType>(op.index));
    }
    auto address = ir::Operand{FlatAddress(insn, op)};
    if (bytes >= 16) {
        return __ LoadMemory(address).SetType(ir::ValueType::V128);
    }
    auto raw = __ LoadMemory(address).SetType(GetSize(bytes * 8));
    ir::Value widened = raw;
    if (bytes != 8) {
        widened = __ ZeroExtend64(raw);
    }
    // VecDup64 puts the bytes in the low qword, which is all the Zip1 chain
    // below ever reads.
    return __ VecDup64(widened).SetType(ir::ValueType::V128);
}

// dst(xmm) = fn(dst, src).  The legacy two-operand shape: the destination is
// also source 1.  Bits 255:128 of the underlying YMM are untouched, which is
// the legacy contract.
void X64Decoder::DecodeSseBinary(_DInst& insn, SseBinFn fn, u32 param) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto a = XmmRead(dst);
    auto b = LoadSrcVec(insn, insn.ops[1]);
    XmmWrite(dst, fn(assembler, a, b, param));
}

void X64Decoder::DecodeSseUnary(_DInst& insn, SseUnFn fn, u32 param) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto src = LoadSrcVec(insn, insn.ops[1]);
    XmmWrite(dst, fn(assembler, src, param));
}

// ---------------------------------------------------------------------------
// ROUNDPS / ROUNDPD / ROUNDSS / ROUNDSD
// ---------------------------------------------------------------------------
// The scalar forms differ from their VEX twins in exactly one place: VEX takes
// the untouched lanes from VEX.vvvv, legacy takes them from the destination
// (which is also source 1).
void X64Decoder::DecodeSseRound(_DInst& insn, u32 lane_bits, bool scalar) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    const u8 imm8 = u8(insn.imm.byte);
    if (scalar) {
        auto source = SseNarrowSrc(insn, insn.ops[1], lane_bits / 8);
        auto merge = XmmRead(dst);
        XmmWrite(dst, RoundLane(assembler, source, merge, lane_bits, imm8, true));
        return;
    }
    auto source = LoadSrcVec(insn, insn.ops[1]);
    // `merge` is unread when scalar == 0; pass the source rather than a fresh
    // uniform load so no dead value reaches RegAlloc.
    XmmWrite(dst, RoundLane(assembler, source, source, lane_bits, imm8, false));
}

// ---------------------------------------------------------------------------
// PTEST
// ---------------------------------------------------------------------------
//   ZF = ((SRC AND DEST) == 0)          DEST = ops[0], SRC = ops[1]
//   CF = ((SRC AND NOT DEST) == 0)
//   OF = AF = PF = SF = 0
// DEST is only READ -- ptest writes no register.
//
// == decoder_avx_fp.cc DecodeAvxFpPTest, including the ordering: PF is written
// before ZF because the value-producing instruction the ZF save attaches to
// also republishes the parity byte.
void X64Decoder::DecodeSsePTest(_DInst& insn) {
    const auto fold = [&](ir::Value vec) {
        auto lo = __ VecExtract64(vec, ir::Imm(0u)).SetType(kU64);
        auto hi = __ VecExtract64(vec, ir::Imm(1u)).SetType(kU64);
        return __ Or(lo, ir::Operand{hi}).SetType(kU64);
    };
    auto dest = XmmRead(static_cast<_RegisterType>(insn.ops[0].index));
    auto src = LoadSrcVec(insn, insn.ops[1]);
    auto both = fold(__ VecAnd(src, dest).SetType(kV128));
    // VecAndNot(x, y) is x AND NOT y.
    auto notdest = fold(__ VecAndNot(src, dest).SetType(kV128));
    __ ClearFlags(ir::Flags::Overflow | ir::Flags::Negate | ir::Flags::AuxiliaryCarry);
    auto one = __ LoadImm(ir::Imm(u64(1)));
    auto zero = __ LoadImm(ir::Imm(u64(0)));
    // PF is architecturally 0.  SaveFlags(x, Parity) sets PF from the EVEN
    // parity of x's low byte, so a value of 1 (odd) is how PF = 0 is spelled.
    __ SaveFlags(__ Or(one, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Parity);
    __ SaveFlags(__ Or(both, ir::Operand{ir::Imm(u64(0))}), ir::Flags::Zero);
    auto cf = __ Select(__ TestNotZero(notdest), zero, one);
    auto cv = __ Add(__ LoadImm(ir::Imm(~u64(0))), ir::Operand{cf});
    __ SaveFlags(cv, ir::Flags::Carry);
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
}

// ---------------------------------------------------------------------------
// PMOVSX* / PMOVZX*
// ---------------------------------------------------------------------------
// == decoder_avx_int.cc DecodeAvxIntExtend at L=0.  Widening is a chain of
// interleaves with a filler vector: Zip1 of {value, filler} at width w
// produces 2w-wide lanes whose upper half is the filler, which IS
// zero-extension when the filler is zero and sign-extension when it is the
// lane's sign mask.
void X64Decoder::DecodeSseExtend(_DInst& insn, u32 src_bits, u32 dst_bits, bool is_signed) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    const u32 elements = 128u / dst_bits;
    auto value = SseNarrowSrc(insn, insn.ops[1], elements * (src_bits / 8));
    ir::Value result;
    for (u32 width = src_bits; width < dst_bits; width *= 2) {
        const auto lanes = ir::Imm(width);
        auto filler = is_signed
                              ? __ VecCmpGt(VecConst(assembler, 0, 0), value, lanes).SetType(kV128)
                              : VecConst(assembler, 0, 0);
        value = __ VecZip(value, filler, lanes, ir::Imm(0u)).SetType(kV128);
        result = value;
    }
    XmmWrite(dst, result);
}

// ---------------------------------------------------------------------------
// BLENDVPS / BLENDVPD / PBLENDVB -- the implicit-XMM0 forms
// ---------------------------------------------------------------------------
// The mask register is NOT encoded: it is always XMM0.  distorm materializes
// it as ops[2], and that is what is read here so that a distorm change
// surfaces as a wrong register rather than being papered over by a hard-coded
// R_XMM0.  The selector is the TOP BIT of each element (the sign bit for the
// float forms), which is exactly an arithmetic shift right by lane_bits - 1.
//
// == decoder_avx_blend.cc DecodeAvxBlendVar / decoder_avx_int.cc
// DecodeAvxIntBlendv, differing only in where the mask comes from.
void X64Decoder::DecodeSseBlendVar(_DInst& insn, u32 lane_bits) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto a = XmmRead(dst);
    auto b = LoadSrcVec(insn, insn.ops[1]);
    auto selector = XmmRead(static_cast<_RegisterType>(insn.ops[2].index));
    auto shift = __ LoadImm(ir::Imm(u64(lane_bits - 1))).SetType(kU64);
    auto mask = __ VecShiftRightArithmetic(selector, shift, ir::Imm(lane_bits)).SetType(kV128);
    XmmWrite(dst, BitSelect(assembler, mask, b, a));
}

// ---------------------------------------------------------------------------
// INSERTPS
// ---------------------------------------------------------------------------
// imm8 is three unrelated bit fields -- 7:6 source lane (register form only;
// a memory source IS the value and the field is ignored), 5:4 destination
// lane, 3:0 a per-lane zero mask applied AFTER the insert.  That is pure x86
// trivia and stays here; the IR sees an extract, two 16-bit inserts and an AND.
//
// == decoder_avx_blend.cc DecodeAvxInsertPs, with src1 taken from the
// destination instead of VEX.vvvv.
void X64Decoder::DecodeSseInsertPs(_DInst& insn) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    const u32 imm8 = u32(insn.imm.byte);
    const u32 source_lane = (imm8 >> 6) & 3u;
    const u32 dest_lane = (imm8 >> 4) & 3u;
    const u32 zero_mask = imm8 & 0xFu;

    ir::Value word;
    auto& src = insn.ops[1];
    if (src.type == O_REG) {
        auto vec = XmmRead(static_cast<_RegisterType>(src.index));
        auto container = __ VecExtract64(vec, ir::Imm(source_lane / 2)).SetType(kU64);
        if (source_lane % 2 != 0) {
            container = __ LsrImm(container, ir::Imm(32u)).SetType(kU64);
        }
        word = __ And(container, ir::Operand{ir::Imm(0xFFFFFFFFull)}).SetType(kU64);
    } else {
        word = __ ZeroExtend64(
                __ LoadMemory(ir::Operand{FlatAddress(insn, src)}).SetType(ir::ValueType::U32));
    }
    auto value = XmmRead(dst);
    value = __ VecInsert16(value, word, ir::Imm(dest_lane * 2)).SetType(kV128);
    auto upper = __ LsrImm(word, ir::Imm(16u)).SetType(kU64);
    value = __ VecInsert16(value, upper, ir::Imm(dest_lane * 2 + 1)).SetType(kV128);
    if (zero_mask != 0) {
        u64 lo = 0, hi = 0;
        ImmLaneMask(32, (~zero_mask) & 0xFu, lo, hi);
        value = __ VecAnd(value, VecConst(assembler, lo, hi)).SetType(kV128);
    }
    XmmWrite(dst, value);
}

// ---------------------------------------------------------------------------
// PEXTRB / PEXTRD / PEXTRQ / EXTRACTPS
// ---------------------------------------------------------------------------
// == decoder_avx_fp.cc DecodeAvxFpExtract.  VecExtract16 / VecExtract64 are
// the only element reads the IR has, so 8- and 32-bit elements are reached by
// extracting the containing 16- or 64-bit one and shifting -- exact, because
// the element is a contiguous field at a statically known offset.
//
// A GPR destination is written at 32 bits (64 for the Q forms), which
// zero-extends; x86 defines the sub-32-bit results as zero-extended.
void X64Decoder::DecodeSseExtract(_DInst& insn, u32 element_bits) {
    auto source = XmmRead(static_cast<_RegisterType>(insn.ops[1].index));
    const u32 count = 128u / element_bits;
    const u32 index = u32(insn.imm.byte) % count;
    ir::Value value;
    if (element_bits == 64) {
        value = __ VecExtract64(source, ir::Imm(index)).SetType(kU64);
    } else if (element_bits == 32) {
        auto container = __ VecExtract64(source, ir::Imm(index / 2)).SetType(kU64);
        if (index % 2 != 0) {
            container = __ LsrImm(container, ir::Imm(32u)).SetType(kU64);
        }
        value = __ And(container, ir::Operand{ir::Imm(0xFFFFFFFFull)}).SetType(kU64);
    } else {
        auto word = __ VecExtract16(source, ir::Imm(index / 2)).SetType(ir::ValueType::U32);
        auto container = __ ZeroExtend64(word).SetType(kU64);
        if (index % 2 != 0) {
            container = __ LsrImm(container, ir::Imm(8u)).SetType(kU64);
        }
        value = __ And(container, ir::Operand{ir::Imm(0xFFull)}).SetType(kU64);
    }
    auto& out = insn.ops[0];
    if (out.type == O_REG) {
        // distorm already sizes the destination register (32 or 64 bit); the
        // register write follows that, which is what zero-extends the 32-bit
        // forms into the full 64-bit register.
        R(static_cast<_RegisterType>(out.index),
          NarrowTo(value, out.size == 64 ? ir::ValueType::U64 : ir::ValueType::U32));
        return;
    }
    MemStore(ir::Operand{FlatAddress(insn, out)}, NarrowTo(value, GetSize(element_bits)), false);
}

// ---------------------------------------------------------------------------
// PINSRB / PINSRD / PINSRQ
// ---------------------------------------------------------------------------
// == decoder_avx_fp.cc DecodeAvxFpInsert, with src1 the destination itself.
// VecInsert16 is the only element write the IR has, so an 8-bit insert is a
// read-modify-write of the containing halfword and 32/64-bit inserts are two
// or four halfword writes.
void X64Decoder::DecodeSseInsert(_DInst& insn, u32 element_bits) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto& src = insn.ops[1];
    ir::Value value;
    if (src.type == O_REG) {
        // distorm already sizes the source GPR (32 bit for B/D, 64 for Q), the
        // same choice GprOf(v.rm, element_bits == 64) makes on the VEX side.
        value = R(static_cast<_RegisterType>(src.index));
    } else {
        value = __ LoadMemory(ir::Operand{FlatAddress(insn, src)})
                        .SetType(GetSize(element_bits));
        if (element_bits != 64) {
            value = __ ZeroExtend64(value).SetType(kU64);
        }
    }
    const u32 count = 128u / element_bits;
    const u32 index = u32(insn.imm.byte) % count;
    auto target = XmmRead(dst);
    if (element_bits == 8) {
        auto word = __ VecExtract16(target, ir::Imm(index / 2)).SetType(ir::ValueType::U32);
        auto container = __ ZeroExtend64(word).SetType(kU64);
        const u64 keep = index % 2 == 0 ? 0xFF00ull : 0x00FFull;
        auto byte = __ And(value, ir::Operand{ir::Imm(0xFFull)}).SetType(kU64);
        if (index % 2 != 0) {
            byte = __ LslImm(byte, ir::Imm(8u)).SetType(kU64);
        }
        auto merged = __ Or(__ And(container, ir::Operand{ir::Imm(keep)}), ir::Operand{byte});
        target = __ VecInsert16(target, merged, ir::Imm(index / 2)).SetType(kV128);
    } else {
        const u32 words = element_bits / 16;
        for (u32 w = 0; w < words; ++w) {
            auto piece = w == 0 ? value : __ LsrImm(value, ir::Imm(w * 16u)).SetType(kU64);
            target = __ VecInsert16(target, piece, ir::Imm(index * words + w)).SetType(kV128);
        }
    }
    XmmWrite(dst, target);
}

// ---------------------------------------------------------------------------
// MPSADBW / PHMINPOSUW
// ---------------------------------------------------------------------------
void X64Decoder::DecodeSseMpsadbw(_DInst& insn) {
    DecodeSseBinary(insn, OpMpsadbw, Pack(8, u32(insn.imm.byte)));
}

void X64Decoder::DecodeSsePhminposuw(_DInst& insn) {
    auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto src = LoadSrcHalves(insn, insn.ops[1]);
    auto zero = __ LoadImm(ir::Imm(u64(0)));
    auto packed = __ CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<VAddr>(&Phminposuw64)}},
                                src.lo,
                                src.hi,
                                zero);
    // Write the high half first: XmmLo/XmmHi are two independent uniform
    // stores and the order is free, but writing hi first keeps the read of
    // `packed` adjacent to its definition.
    XmmHi(dst, __ LoadImm(ir::Imm(u64(0))));
    XmmLo(dst, packed);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

// True when every vector operand distorm reported is an XMM register or a
// memory reference -- i.e. this is the 66-prefixed SSE form and not the MMX
// one.  The SSSE3 opcodes have both encodings and this runtime models no MMX
// register file, so the MMX form must keep failing loudly rather than being
// executed against the XMM file.
static bool SseXmmForm(const _DInst& insn, u32 vector_ops) {
    u32 seen = 0;
    for (const auto& op : insn.ops) {
        if (op.type == O_NONE) {
            continue;
        }
        if (op.type != O_REG) {
            continue;
        }
        const auto reg = static_cast<_RegisterType>(op.index);
        if (reg >= R_MM0 && reg <= R_MM7) {
            return false;
        }
        if (reg >= R_XMM0 && reg <= R_XMM15) {
            ++seen;
        }
    }
    // A memory operand replaces at most one of the expected XMM registers.
    return seen + 1 >= vector_ops;
}

bool X64Decoder::DecodeSse4(_DInst& insn) {
    // Escape hatch.  Default ON: every opcode below currently kills the guest
    // outright, so the risk of a wrong answer is strictly better than the
    // certainty of a crash -- but a bisectable off switch is worth one getenv.
    static const bool enabled = [] {
        const char* env = std::getenv("SVM_SSE4");
        return !env || std::strcmp(env, "0") != 0;
    }();
    if (!enabled) {
        return false;
    }
    switch (insn.opcode) {
        // ---- SSE4.1 rounding -------------------------------------------
        case I_ROUNDPS:
            DecodeSseRound(insn, 32, false);
            return true;
        case I_ROUNDPD:
            DecodeSseRound(insn, 64, false);
            return true;
        case I_ROUNDSS:
            DecodeSseRound(insn, 32, true);
            return true;
        case I_ROUNDSD:
            DecodeSseRound(insn, 64, true);
            return true;
        // ---- SSE4.1 ptest ----------------------------------------------
        case I_PTEST:
            DecodeSsePTest(insn);
            return true;
        // ---- SSE4.1 sign / zero extension -------------------------------
        case I_PMOVSXBW:
            DecodeSseExtend(insn, 8, 16, true);
            return true;
        case I_PMOVSXBD:
            DecodeSseExtend(insn, 8, 32, true);
            return true;
        case I_PMOVSXBQ:
            DecodeSseExtend(insn, 8, 64, true);
            return true;
        case I_PMOVSXWD:
            DecodeSseExtend(insn, 16, 32, true);
            return true;
        case I_PMOVSXWQ:
            DecodeSseExtend(insn, 16, 64, true);
            return true;
        case I_PMOVSXDQ:
            DecodeSseExtend(insn, 32, 64, true);
            return true;
        case I_PMOVZXBW:
            DecodeSseExtend(insn, 8, 16, false);
            return true;
        case I_PMOVZXBD:
            DecodeSseExtend(insn, 8, 32, false);
            return true;
        case I_PMOVZXBQ:
            DecodeSseExtend(insn, 8, 64, false);
            return true;
        case I_PMOVZXWD:
            DecodeSseExtend(insn, 16, 32, false);
            return true;
        case I_PMOVZXWQ:
            DecodeSseExtend(insn, 16, 64, false);
            return true;
        case I_PMOVZXDQ:
            DecodeSseExtend(insn, 32, 64, false);
            return true;
        // ---- SSE4.1 blends ---------------------------------------------
        case I_BLENDVPS:
            DecodeSseBlendVar(insn, 32);
            return true;
        case I_BLENDVPD:
            DecodeSseBlendVar(insn, 64);
            return true;
        case I_PBLENDVB:
            DecodeSseBlendVar(insn, 8);
            return true;
        case I_BLENDPS:
            DecodeSseBinary(insn, OpBlendImm, Pack(32, u32(insn.imm.byte) & 0xFu));
            return true;
        case I_BLENDPD:
            DecodeSseBinary(insn, OpBlendImm, Pack(64, u32(insn.imm.byte) & 0x3u));
            return true;
        case I_PBLENDW:
            DecodeSseBinary(insn, OpBlendImm, Pack(16, u32(insn.imm.byte)));
            return true;
        // ---- SSE4.1 arithmetic / compare --------------------------------
        case I_PMULLD:
            DecodeSseBinary(insn, OpMul, Pack(32));
            return true;
        case I_PCMPEQQ:
            DecodeSseBinary(insn, OpCmpEq, Pack(64));
            return true;
        case I_PCMPGTQ:  // SSE4.2
            DecodeSseBinary(insn, OpCmpGt, Pack(64));
            return true;
        case I_PACKUSDW:
            DecodeSseBinary(insn, OpPackUsdw, 0);
            return true;
        // ---- SSE4.1 insert / extract ------------------------------------
        case I_INSERTPS:
            DecodeSseInsertPs(insn);
            return true;
        case I_EXTRACTPS:
            DecodeSseExtract(insn, 32);
            return true;
        case I_PEXTRB:
            DecodeSseExtract(insn, 8);
            return true;
        case I_PEXTRD:
            DecodeSseExtract(insn, 32);
            return true;
        case I_PEXTRQ:
            DecodeSseExtract(insn, 64);
            return true;
        case I_PINSRB:
            DecodeSseInsert(insn, 8);
            return true;
        case I_PINSRD:
            DecodeSseInsert(insn, 32);
            return true;
        case I_PINSRQ:
            DecodeSseInsert(insn, 64);
            return true;
        // ---- SSSE3 horizontal / sign / absolute -------------------------
        case I_PHADDW:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseBinary(insn, OpHorizontalInt, Pack(16, kHAdd));
            return true;
        case I_PHADDD:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseBinary(insn, OpHorizontalInt, Pack(32, kHAdd));
            return true;
        case I_PHADDSW:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseBinary(insn, OpHorizontalInt, Pack(16, kHSatAdd));
            return true;
        case I_PHSUBW:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseBinary(insn, OpHorizontalInt, Pack(16, kHSub));
            return true;
        case I_PHSUBD:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseBinary(insn, OpHorizontalInt, Pack(32, kHSub));
            return true;
        case I_PHSUBSW:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseBinary(insn, OpHorizontalInt, Pack(16, kHSatSub));
            return true;
        case I_PSIGNB:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseBinary(insn, OpSign, Pack(8));
            return true;
        case I_PSIGNW:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseBinary(insn, OpSign, Pack(16));
            return true;
        case I_PSIGND:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseBinary(insn, OpSign, Pack(32));
            return true;
        case I_PABSB:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseUnary(insn, OpAbs, Pack(8));
            return true;
        case I_PABSW:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseUnary(insn, OpAbs, Pack(16));
            return true;
        case I_PABSD:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseUnary(insn, OpAbs, Pack(32));
            return true;
        case I_PMADDUBSW:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseBinary(insn, OpMaddUbs, 0);
            return true;
        case I_PMULHRSW:
            if (!SseXmmForm(insn, 2)) return false;
            DecodeSseBinary(insn, OpMulHrsw, 0);
            return true;
        // ---- SSE4.1 min / max -------------------------------------------
        case I_PMINSB:
            DecodeSseBinary(insn, OpMin, Pack(8, 1));
            return true;
        case I_PMINSD:
            DecodeSseBinary(insn, OpMin, Pack(32, 1));
            return true;
        case I_PMINUW:
            DecodeSseBinary(insn, OpMin, Pack(16, 0));
            return true;
        case I_PMAXSB:
            DecodeSseBinary(insn, OpMax, Pack(8, 1));
            return true;
        case I_PMAXSD:
            DecodeSseBinary(insn, OpMax, Pack(32, 1));
            return true;
        case I_PMAXUW:
            DecodeSseBinary(insn, OpMax, Pack(16, 0));
            return true;
        // ---- SSE3 horizontal float / add-subtract -----------------------
        case I_HADDPD:
            DecodeSseBinary(insn, OpHorizontalFloat, Pack(64, 0));
            return true;
        case I_HSUBPD:
            DecodeSseBinary(insn, OpHorizontalFloat, Pack(64, 1));
            return true;
        case I_ADDSUBPS:
            DecodeSseBinary(insn, OpAddSub, Pack(32));
            return true;
        case I_ADDSUBPD:
            DecodeSseBinary(insn, OpAddSub, Pack(64));
            return true;
        // ---- SSE4.1 dot product ------------------------------------------
        case I_DPPS:
            DecodeSseBinary(insn, OpDotProduct, Pack(32, u32(insn.imm.byte)));
            return true;
        case I_DPPD:
            DecodeSseBinary(insn, OpDotProduct, Pack(64, u32(insn.imm.byte)));
            return true;
        // ---- SSE4.1 odds and ends ----------------------------------------
        case I_MPSADBW:
            DecodeSseMpsadbw(insn);
            return true;
        case I_PHMINPOSUW:
            DecodeSsePhminposuw(insn);
            return true;
        case I_MOVNTDQA: {
            // A non-temporal load has no architecturally visible difference
            // from an ordinary one here; the hint has no IR representation.
            auto dst = static_cast<_RegisterType>(insn.ops[0].index);
            XmmWrite(dst, LoadSrcVec(insn, insn.ops[1]));
            return true;
        }
        default:
            return false;
    }
}

#undef __

}  // namespace swift::x86
