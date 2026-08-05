// VEX integer and data-rearrangement handlers (AVX / AVX2), both widths.
//
// Everything here is driven by VexInsn (vex_decoder.h), never by distorm's
// _DInst.  That is not a stylistic choice: over 117 representative AVX/AVX2
// encodings the bundled distorm returns I_UNDEFINED for 40 -- including EVERY
// opcode in this file that is AVX2-only (vpbroadcast*, vpermd, vpermq,
// vperm2i128, vinserti128, vextracti128, vpblendd, vpsllvd/vpsrlvd/vpsravd) --
// and silently drops VEX.L on 38 more, among them vpblendw and vpalignr, which
// this file also implements.  A handler reading _DInst could not tell a 256-bit
// vpalignr from a 128-bit one.
//
// ---------------------------------------------------------------------------
// CONTRACTS
// ---------------------------------------------------------------------------
// C1: a YMM register is NEVER one IR value.  Bits 127:0 live in
//     ThreadContext64::xmms[i] and bits 255:128 in ymm_high[i], and a 256-bit
//     operation is two independent V128 operations.  This file adds no IR
//     opcode and no backend change; every result is built from the existing
//     Vec* set in runtime/ir/ir.inc.
// C3: a VEX.128 form zeroes bits 255:128 of its destination (VexWrite128 does
//     it).  A VEX.256 form writes both halves and must NOT (VexWrite256).
//
// The split is only sound because AVX2 defines nearly all of this family PER
// 128-BIT LANE.  Six instructions here are genuinely cross-lane, and each one
// is expressible anyway for a specific reason:
//
//   vbroadcasti128, vpbroadcastb/w/d/q  one V128 written to both halves.
//   vinserti128, vextracti128           whole-lane moves; the lane index is
//                                       imm8, i.e. known at decode time.
//   vperm2i128                          whole-lane selection, imm8-driven, so
//                                       each output half is a compile-time
//                                       choice among four inputs.
//   vpermq                              imm8-driven qword permutation: each
//                                       output qword is a KNOWN source qword,
//                                       so VecExtract64 + VecDup64 + VecZip
//                                       rebuild the half.
//   vpmovsx*/vpmovzx*                   at VEX.256 the source's high 64 bits
//                                       feed the destination's high lane, which
//                                       is exactly VecZip's Zip2 half.
//   vpermd                              the ONE case with runtime cross-lane
//                                       indices.  Handled with two 128-bit
//                                       table lookups plus an OR; see
//                                       DecodeAvxIntPermd for why the index
//                                       fix-up is needed.
//
// ---------------------------------------------------------------------------
// INTEGRATION (decoder.h and the VEX dispatch are the main line's files)
// ---------------------------------------------------------------------------
// This file reuses VexAddress / VexLoadVec / VexLoadVec256 / VexWrite128 /
// VexWrite256 as decoder_avx_fp.cc already declares them in decoder.h; it
// defines none of them, so nothing there needs to change.  What it does need is
// these declarations added to the private section of X64Decoder, next to the
// DecodeAvxFp* block:
//
//     using AvxIntBinFn = ir::Value (*)(ir::Assembler*, ir::Value, ir::Value, u32, u32);
//     using AvxIntUnFn = ir::Value (*)(ir::Assembler*, ir::Value, u32);
//     bool DecodeAvxInt(const VexInsn& v);
//     void DecodeAvxIntBinary(const VexInsn& v, AvxIntBinFn fn, u32 param);
//     void DecodeAvxIntUnary(const VexInsn& v, AvxIntUnFn fn, u32 param);
//     void DecodeAvxIntShiftCount(const VexInsn& v, u32 kind, u32 lane_bits);
//     void DecodeAvxIntShiftImm(const VexInsn& v, u32 kind, u32 lane_bits);
//     void DecodeAvxIntExtend(const VexInsn& v, u32 src_bits, u32 dst_bits, bool is_signed);
//     void DecodeAvxIntBroadcast(const VexInsn& v, u32 element_bits);
//     void DecodeAvxIntBroadcast128(const VexInsn& v);
//     void DecodeAvxIntInsert128(const VexInsn& v);
//     void DecodeAvxIntExtract128(const VexInsn& v);
//     void DecodeAvxIntPerm2i128(const VexInsn& v);
//     void DecodeAvxIntPermq(const VexInsn& v);
//     void DecodeAvxIntPermd(const VexInsn& v);
//     void DecodeAvxIntBlendv(const VexInsn& v);
//     void DecodeAvxIntZeroDst(const VexInsn& v);
//     ir::Value AvxIntNarrowSrc(const VexInsn& v, u32 bytes);
//
// Plus one line at the VEX dispatch already in Decode(), which today reads
// `if (DecodeAvxFp(vex))`:
//
//     if (DecodeAvxInt(vex) || DecodeAvxFp(vex)) {
//
// Order does not matter -- the two families share no (map, opcode) pair, and
// every path in DecodeAvxInt that returns false does so BEFORE emitting any IR,
// so a decline never leaves a half-built block behind.  Also add
// decoder_avx_int.cc to source/runtime/frontend/x86/CMakeLists.txt and
// fuzz/avx_int_test.cpp to source/tests/CMakeLists.txt.
//
// `pc` must already point PAST the instruction when a handler runs -- the
// RIP-relative address in VexAddress depends on it, exactly as GetAddress does
// on the legacy path.  The existing dispatch already advances it.
//
// ---------------------------------------------------------------------------
// KNOWN DEVIATIONS
// ---------------------------------------------------------------------------
//  * A 32-byte memory operand is two 16-byte accesses, so a page-straddling
//    fault is not indivisible and the reported fault address can be base+16.
//    decoder_avx.cc documents this at length; it is unfixable under C1.
//  * The xmm/m128 shift-count forms read only the low 8 bytes of their memory
//    operand rather than 16.  The upper 8 bytes are architecturally ignored, so
//    this is observable only as a fault that hardware would take and this does
//    not.
//  * vpalignr with imm8 >= 32 and vperm2i128 with both lanes zeroed produce a
//    constant, and their memory operand is then NOT read.  Same class of
//    deviation: a fault hardware would take is skipped.  Loading a value no
//    result depends on is worse -- dead loads break the register allocator.
//  * VEX.vvvv is not checked against 1111 on the two-operand forms, where a
//    non-1111 field is architecturally #UD.  Consistent with the rest of the
//    front end, which does not enforce reserved-field #UD either.
//  * No alignment check anywhere; matching the existing SSE/AVX paths.

#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/vex_decoder.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

// Handler parameters are packed into one u32 because the two dispatch helpers
// take a single opaque value: bits 15:0 are a lane/element width in bits, bits
// 31:16 a per-operation flag (signedness, high/low half, shift kind, imm8).
constexpr u32 Pack(u32 lane, u32 flag = 0) { return lane | (flag << 16); }
constexpr u32 Lane(u32 param) { return param & 0xFFFF; }
constexpr u32 Flag(u32 param) { return param >> 16; }

// `value`, repeated into every `lane_bits`-wide field of a qword.
constexpr u64 Replicate(u32 lane_bits, u64 value) {
    const u64 mask = lane_bits >= 64 ? ~u64(0) : ((u64(1) << lane_bits) - 1);
    u64 out = 0;
    for (u32 shift = 0; shift < 64; shift += lane_bits) {
        out |= (value & mask) << shift;
    }
    return out;
}

// Materialize an arbitrary 128-bit constant.  There is no vector-immediate IR
// opcode, so this goes through the scalar path: VecDup64 broadcasts a GPR into
// both qwords, and the 64-bit Zip1 takes lane 0 of each operand, which selects
// one qword from each of two broadcasts.
ir::Value VecConst(ir::Assembler* as, u64 lo, u64 hi) {
    auto low = as->VecDup64(as->LoadImm(ir::Imm(lo)).SetType(ir::ValueType::U64))
                       .SetType(ir::ValueType::V128);
    if (lo == hi) {
        return low;
    }
    auto high = as->VecDup64(as->LoadImm(ir::Imm(hi)).SetType(ir::ValueType::U64))
                        .SetType(ir::ValueType::V128);
    return as->VecZip(low, high, ir::Imm(64u), ir::Imm(0u)).SetType(ir::ValueType::V128);
}

// The V128 whose low qword is q0 and high qword is q1.
ir::Value VecPairQ(ir::Assembler* as, ir::Value q0, ir::Value q1) {
    auto a = as->VecDup64(q0.SetType(ir::ValueType::U64)).SetType(ir::ValueType::V128);
    auto b = as->VecDup64(q1.SetType(ir::ValueType::U64)).SetType(ir::ValueType::V128);
    return as->VecZip(a, b, ir::Imm(64u), ir::Imm(0u)).SetType(ir::ValueType::V128);
}

ir::Value VecZeroConst(ir::Assembler* as) { return VecConst(as, 0, 0); }

// --- the per-lane binary operations ---------------------------------------
// Every one of these takes the two 128-bit lane values and returns the lane
// result, so the same function serves the VEX.128 form and each half of the
// VEX.256 form.  `half` is 0 for a VEX.128 result and the lane index for a
// VEX.256 one; only the blends look at it.

ir::Value OpMul(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecMul(a, b, ir::Imm(Lane(param))).SetType(ir::ValueType::V128);
}

ir::Value OpMulHigh(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecMulHigh16(a, b, ir::Imm(Flag(param))).SetType(ir::ValueType::V128);
}

ir::Value OpMadd(ir::Assembler* as, ir::Value a, ir::Value b, u32, u32) {
    return as->VecMadd16(a, b).SetType(ir::ValueType::V128);
}

ir::Value OpSadbw(ir::Assembler* as, ir::Value a, ir::Value b, u32, u32) {
    return as->VecAbsDiffSum8(a, b).SetType(ir::ValueType::V128);
}

ir::Value OpAvg(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecAvg(a, b, ir::Imm(Lane(param))).SetType(ir::ValueType::V128);
}

ir::Value OpSatAdd(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecSatAdd(a, b, ir::Imm(Lane(param)), ir::Imm(Flag(param)))
            .SetType(ir::ValueType::V128);
}

ir::Value OpSatSub(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecSatSub(a, b, ir::Imm(Lane(param)), ir::Imm(Flag(param)))
            .SetType(ir::ValueType::V128);
}

ir::Value OpMin(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecMin(a, b, ir::Imm(Lane(param)), ir::Imm(Flag(param)))
            .SetType(ir::ValueType::V128);
}

ir::Value OpMax(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecMax(a, b, ir::Imm(Lane(param)), ir::Imm(Flag(param)))
            .SetType(ir::ValueType::V128);
}

ir::Value OpPack(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecPack(a, b, ir::Imm(Lane(param)), ir::Imm(Flag(param)))
            .SetType(ir::ValueType::V128);
}

// PACKUSDW cannot use VecPack: the backend honours the unsigned-destination
// flag only for 16-bit sources and emits SQXTN for 32-bit ones regardless, and
// SQXTN saturates into the SIGNED 16-bit range -- every value in 0x8000..0xFFFF
// would come back as 0x7FFF.  Clamp each signed dword into [0, 0xFFFF] first
// (which makes the narrowing exact) and then gather the low halfwords.
// VecTableLookup8's control is masked with 0x8F, so an index of 0x80 selects
// nothing and yields a zero byte, which is what lets the two halves be OR-ed.
ir::Value OpPackUsdw(ir::Assembler* as, ir::Value a, ir::Value b, u32, u32) {
    auto zero = VecZeroConst(as);
    const u64 cap = Replicate(32, 0xFFFF);
    auto ceiling = VecConst(as, cap, cap);
    const auto lanes = ir::Imm(32u);
    const auto sign = ir::Imm(1u);
    auto ca = as->VecMin(as->VecMax(a, zero, lanes, sign).SetType(ir::ValueType::V128),
                         ceiling,
                         lanes,
                         sign)
                      .SetType(ir::ValueType::V128);
    auto cb = as->VecMin(as->VecMax(b, zero, lanes, sign).SetType(ir::ValueType::V128),
                         ceiling,
                         lanes,
                         sign)
                      .SetType(ir::ValueType::V128);
    // Bytes 0,1,4,5,8,9,12,13 are the low halfword of each dword.
    constexpr u64 kGather = 0x0D0C090805040100ull;
    constexpr u64 kNone = 0x8080808080808080ull;
    auto from_a = as->VecTableLookup8(ca, VecConst(as, kGather, kNone))
                          .SetType(ir::ValueType::V128);
    auto from_b = as->VecTableLookup8(cb, VecConst(as, kNone, kGather))
                          .SetType(ir::ValueType::V128);
    return as->VecOr(from_a, from_b).SetType(ir::ValueType::V128);
}

ir::Value OpZip(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    return as->VecZip(a, b, ir::Imm(Lane(param)), ir::Imm(Flag(param)))
            .SetType(ir::ValueType::V128);
}

// PSIGN: dst = b < 0 ? -a : (b == 0 ? 0 : a).  The zero case is a separate
// clause in the Intel definition, not a consequence of the negation, so it
// needs its own mask.
ir::Value OpSign(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    const auto lanes = ir::Imm(Lane(param));
    auto zero = VecZeroConst(as);
    auto negative = as->VecCmpGt(zero, b, lanes).SetType(ir::ValueType::V128);
    auto is_zero = as->VecCmpEq(b, zero, lanes).SetType(ir::ValueType::V128);
    auto negated = as->VecSub(zero, a, lanes).SetType(ir::ValueType::V128);
    // VecAndNot(x, y) is x AND NOT y.
    auto picked = as->VecOr(as->VecAnd(negated, negative).SetType(ir::ValueType::V128),
                            as->VecAndNot(a, negative).SetType(ir::ValueType::V128))
                          .SetType(ir::ValueType::V128);
    return as->VecAndNot(picked, is_zero).SetType(ir::ValueType::V128);
}

// PABS.  max(x, -x) is exact including the INT_MIN case, where x86 leaves
// INT_MIN unchanged because the true absolute value does not fit -- and
// max(INT_MIN, -INT_MIN) is INT_MIN for the same wrapping reason.
ir::Value OpAbs(ir::Assembler* as, ir::Value a, u32 param) {
    const auto lanes = ir::Imm(Lane(param));
    auto zero = VecZeroConst(as);
    auto negated = as->VecSub(zero, a, lanes).SetType(ir::ValueType::V128);
    return as->VecMax(a, negated, lanes, ir::Imm(1u)).SetType(ir::ValueType::V128);
}

// vpblendw (element 16, one 8-bit control shared by both lanes) and vpblendd
// (element 32, control bits 3:0 for the low lane and 7:4 for the high one).
// The control is a decode-time constant, so the selector becomes a constant
// mask and the blend becomes three bitwise ops.
ir::Value OpBlendImm(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32 half) {
    const u32 imm = Flag(param);
    const u32 element = Lane(param);
    const u32 bytes = element / 8;
    const u32 count = 128 / element;
    u64 mask[2] = {0, 0};
    for (u32 i = 0; i < count; ++i) {
        const u32 bit = element == 16 ? i : (half * 4 + i);
        if (((imm >> bit) & 1) == 0) {
            continue;
        }
        for (u32 byte = 0; byte < bytes; ++byte) {
            const u32 position = i * bytes + byte;
            mask[position / 8] |= u64(0xFF) << ((position % 8) * 8);
        }
    }
    auto selector = VecConst(as, mask[0], mask[1]);
    return as->VecOr(as->VecAnd(b, selector).SetType(ir::ValueType::V128),
                     as->VecAndNot(a, selector).SetType(ir::ValueType::V128))
            .SetType(ir::ValueType::V128);
}

// vpalignr: shift the 256-bit concatenation a:b (a is the HIGH half) right by
// imm8 bytes and keep the low 128 bits, per 128-bit lane.  Structured like
// DecodePalignr, but building a V128 result instead of writing register halves.
ir::Value OpAlignr(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32) {
    const u32 imm = Flag(param);
    ir::Value cache[4];
    bool cached[4] = {false, false, false, false};
    // Extract lazily: a qword no output depends on must not be materialized.
    auto source = [&](u32 index) -> ir::Value {
        if (!cached[index]) {
            cache[index] = as->VecExtract64(index < 2 ? b : a, ir::Imm(index & 1u))
                                   .SetType(ir::ValueType::U64);
            cached[index] = true;
        }
        return cache[index];
    };
    const u32 first = imm / 8;
    const u32 shift = (imm % 8) * 8;
    auto out = [&](u32 which) -> ir::Value {
        const u32 index = first + which;
        if (index > 3) {
            return as->LoadImm(ir::Imm(u64(0))).SetType(ir::ValueType::U64);
        }
        if (shift == 0) {
            return source(index);
        }
        auto low = as->LsrImm(source(index), ir::Imm(u64(shift))).SetType(ir::ValueType::U64);
        if (index + 1 > 3) {
            return low;
        }
        auto high = as->LslImm(source(index + 1), ir::Imm(u64(64 - shift)))
                            .SetType(ir::ValueType::U64);
        return as->Or(low, ir::Operand{high}).SetType(ir::ValueType::U64);
    };
    auto q0 = out(0);
    auto q1 = out(1);
    return VecPairQ(as, q0, q1);
}

// vpsllv/vpsrlv/vpsrav: every lane shifts by ITS OWN count.  The IR's packed
// shifts take one scalar count for the whole vector (EmitVecShiftLeft dups a
// GPR), so there is no direct form.  Decompose instead: for each bit k of the
// count, conditionally apply a constant shift of 2^k.  That is exact because
// shifting by c is shifting by each set power of two in turn.
//
// Out-of-range counts are the part x86 gets "wrong" relative to every other
// ISA: the count is NOT reduced modulo the lane width.  For the logical shifts
// a count >= width yields zero, applied here as a final mask; for the
// arithmetic shift it yields the sign bits, obtained by clamping the count to
// width-1 up front, which needs no extra masking.
//
// The 64-bit forms follow the Intel definition ("IF COUNT_SRC[63:0] < 64"),
// which is deliberately NOT what Rosetta 2 does: Rosetta truncates the
// VPSLLVQ / VPSRLVQ count to 32 bits and shifts by 1 for a count of
// 0x0000000100000001 where hardware produces zero.  avx_int_test.cpp excludes
// the two affected reference rows and records the probe that established it.
ir::Value OpShiftVar(ir::Assembler* as, ir::Value a, ir::Value counts, u32 param, u32) {
    const u32 lane = Lane(param);
    const u32 kind = Flag(param);  // 0 = left, 1 = logical right, 2 = arithmetic right
    const auto lanes = ir::Imm(lane);
    ir::Value effective = counts;
    if (kind == 2) {
        const u64 cap = Replicate(lane, lane - 1);
        effective = as->VecMin(counts, VecConst(as, cap, cap), lanes, ir::Imm(0u))
                            .SetType(ir::ValueType::V128);
    }
    ir::Value value = a;
    for (u32 bit = 0; (u32(1) << bit) < lane; ++bit) {
        const u64 step = u64(1) << bit;
        const u64 rep = Replicate(lane, step);
        auto probe = VecConst(as, rep, rep);
        auto selected = as->VecCmpEq(as->VecAnd(effective, probe).SetType(ir::ValueType::V128),
                                     probe,
                                     lanes)
                                .SetType(ir::ValueType::V128);
        auto amount = as->LoadImm(ir::Imm(step)).SetType(ir::ValueType::U64);
        ir::Value shifted;
        if (kind == 0) {
            shifted = as->VecShiftLeft(value, amount, lanes).SetType(ir::ValueType::V128);
        } else if (kind == 1) {
            shifted = as->VecShiftRight(value, amount, lanes).SetType(ir::ValueType::V128);
        } else {
            shifted = as->VecShiftRightArithmetic(value, amount, lanes)
                              .SetType(ir::ValueType::V128);
        }
        value = as->VecOr(as->VecAnd(shifted, selected).SetType(ir::ValueType::V128),
                          as->VecAndNot(value, selected).SetType(ir::ValueType::V128))
                        .SetType(ir::ValueType::V128);
    }
    if (kind != 2) {
        const u64 over = Replicate(lane, ~u64(0) << (lane == 64 ? 6 : (lane == 32 ? 5 : 4)));
        auto in_range = as->VecCmpEq(
                                as->VecAnd(counts, VecConst(as, over, over))
                                        .SetType(ir::ValueType::V128),
                                VecZeroConst(as),
                                lanes)
                                .SetType(ir::ValueType::V128);
        value = as->VecAnd(value, in_range).SetType(ir::ValueType::V128);
    }
    return value;
}

}  // namespace

// ---------------------------------------------------------------------------
// Operand plumbing
// ---------------------------------------------------------------------------

// The r/m operand of a form whose SOURCE is narrower than its destination
// (vpmovsx/vpmovzx, vpbroadcast*).  Reading the architectural number of bytes
// rather than a full V128 matters: a 1-byte vpbroadcastb at the end of a page
// must not fault, and a 16-byte load there would.
ir::Value X64Decoder::AvxIntNarrowSrc(const VexInsn& v, u32 bytes) {
    if (v.RmIsRegister()) {
        return XmmRead(XmmOf(v.rm));
    }
    auto address = ir::Operand{VexAddress(v)};
    if (bytes >= 16) {
        return MemLoad(address, ir::ValueType::V128, VexTsoOrdered(v));
    }
    auto raw = MemLoad(address, GetSize(bytes * 8), VexTsoOrdered(v));
    ir::Value widened = raw;
    if (bytes != 8) {
        widened = __ ZeroExtend64(raw);
    }
    // VecDup64 puts the bytes in the low qword, which is all the consumers
    // (Zip1 chains and VecDup64 broadcasts) ever read.
    return __ VecDup64(widened).SetType(ir::ValueType::V128);
}

// dst = src1 (VEX.vvvv) OP src2 (r/m), computed per 128-bit lane.
void X64Decoder::DecodeAvxIntBinary(const VexInsn& v, AvxIntBinFn fn, u32 param) {
    if (v.l) {
        auto a_lo = XmmRead(XmmOf(v.vvvv));
        auto a_hi = YmmHighRead(v.vvvv);
        auto b = VexLoadVec256(v);
        auto lo = fn(assembler, a_lo, b.lo, param, 0);
        auto hi = fn(assembler, a_hi, b.hi, param, 1);
        VexWrite256(v.reg, lo, hi);
        return;
    }
    auto a = XmmRead(XmmOf(v.vvvv));
    auto b = VexLoadVec(v);
    VexWrite128(v.reg, fn(assembler, a, b, param, 0));
}

// dst = f(r/m), full-width source, per 128-bit lane.
void X64Decoder::DecodeAvxIntUnary(const VexInsn& v, AvxIntUnFn fn, u32 param) {
    if (v.l) {
        auto src = VexLoadVec256(v);
        VexWrite256(v.reg, fn(assembler, src.lo, param), fn(assembler, src.hi, param));
        return;
    }
    VexWrite128(v.reg, fn(assembler, VexLoadVec(v), param));
}

void X64Decoder::DecodeAvxIntZeroDst(const VexInsn& v) {
    auto zero = VecConst(assembler, 0, 0);
    if (v.l) {
        VexWrite256(v.reg, zero, VecConst(assembler, 0, 0));
    } else {
        VexWrite128(v.reg, zero);
    }
}

// ---------------------------------------------------------------------------
// Shifts
// ---------------------------------------------------------------------------

// vpsllw/d/q, vpsrlw/d/q, vpsraw/d with an xmm/m128 count.  The count operand
// is 128 bits WIDE EVEN AT VEX.256 and only its low qword is read, so this must
// not go through VexLoadVec256 -- doing so would load 32 bytes for a 16-byte
// operand.
void X64Decoder::DecodeAvxIntShiftCount(const VexInsn& v, u32 kind, u32 lane_bits) {
    ir::Value count = v.RmIsRegister()
                              ? XmmLo(XmmOf(v.rm))
                              : MemLoad(ir::Operand{VexAddress(v)},
                                        ir::ValueType::U64,
                                        VexTsoOrdered(v));
    const auto lanes = ir::Imm(lane_bits);
    const auto apply = [&](ir::Value value) -> ir::Value {
        if (kind == 0) {
            return __ VecShiftLeft(value, count, lanes).SetType(ir::ValueType::V128);
        }
        if (kind == 1) {
            return __ VecShiftRight(value, count, lanes).SetType(ir::ValueType::V128);
        }
        return __ VecShiftRightArithmetic(value, count, lanes).SetType(ir::ValueType::V128);
    };
    if (v.l) {
        VexWrite256(v.reg, apply(XmmRead(XmmOf(v.vvvv))), apply(YmmHighRead(v.vvvv)));
    } else {
        VexWrite128(v.reg, apply(XmmRead(XmmOf(v.vvvv))));
    }
}

// The 0F 71/72/73 groups.  These are NDD forms: the destination is VEX.vvvv,
// the source is r/m and ModRM.reg is the /n opcode extension.  Reading them the
// usual way round silently swaps source and destination.
void X64Decoder::DecodeAvxIntShiftImm(const VexInsn& v, u32 kind, u32 lane_bits) {
    const auto lanes = ir::Imm(lane_bits);
    const bool use_imm = VecLoweringEnabled(features_.vec_imm_shift);
    // Keep the disabled path byte-for-byte equivalent at IR level: VEX.256
    // shares one scalar count between both 128-bit lanes.
    ir::Value count;
    if (!use_imm) {
        count = __ LoadImm(ir::Imm(u64(v.imm8))).SetType(ir::ValueType::U64);
    }
    const auto apply = [&](ir::Value value) -> ir::Value {
        if (use_imm) {
            if (kind == 0) {
                return __ VecShiftLeftImm(value, ir::Imm(u64(v.imm8)), lanes)
                        .SetType(ir::ValueType::V128);
            }
            if (kind == 1) {
                return __ VecShiftRightImm(value, ir::Imm(u64(v.imm8)), lanes)
                        .SetType(ir::ValueType::V128);
            }
            return __ VecShiftRightArithmeticImm(
                            value, ir::Imm(u64(v.imm8)), lanes)
                    .SetType(ir::ValueType::V128);
        }
        if (kind == 0) {
            return __ VecShiftLeft(value, count, lanes).SetType(ir::ValueType::V128);
        }
        if (kind == 1) {
            return __ VecShiftRight(value, count, lanes).SetType(ir::ValueType::V128);
        }
        return __ VecShiftRightArithmetic(value, count, lanes).SetType(ir::ValueType::V128);
    };
    if (v.l) {
        VexWrite256(v.vvvv, apply(XmmRead(XmmOf(v.rm))), apply(YmmHighRead(v.rm)));
    } else {
        VexWrite128(v.vvvv, apply(XmmRead(XmmOf(v.rm))));
    }
}

void X64Decoder::DecodeAvxIntByteShift(const VexInsn& v, bool left) {
    const auto apply = [&](ir::Value value) {
        return VecByteShiftLowered(assembler, value, v.imm8, left);
    };
    if (v.l) {
        VexWrite256(v.vvvv,
                    apply(XmmRead(XmmOf(v.rm))),
                    apply(YmmHighRead(v.rm)));
    } else {
        // VexWrite128 supplies the required upper-128 zero even for count=0.
        VexWrite128(v.vvvv, apply(XmmRead(XmmOf(v.rm))));
    }
}

// ---------------------------------------------------------------------------
// Widening
// ---------------------------------------------------------------------------

// vpmovsx{bw,bd,bq,wd,wq,dq} and the vpmovzx twins.  Widening is a chain of
// interleaves with a filler vector: Zip1 of {value, filler} at width w produces
// 2w-wide lanes whose upper half is the filler, which IS zero-extension when
// the filler is zero and sign-extension when it is the lane's sign mask.
//
// At VEX.256 the LAST step splits: Zip1 takes the source's low 64 bits into the
// destination's low lane and Zip2 its high 64 bits into the high lane, which is
// exactly the architectural mapping.
void X64Decoder::DecodeAvxIntExtend(const VexInsn& v, u32 src_bits, u32 dst_bits, bool is_signed) {
    const u32 elements = (v.l ? 256u : 128u) / dst_bits;
    auto value = AvxIntNarrowSrc(v, elements * (src_bits / 8));
    ir::Value lo, hi;
    for (u32 width = src_bits; width < dst_bits; width *= 2) {
        const auto lanes = ir::Imm(width);
        auto filler = is_signed ? __ VecCmpGt(VecConst(assembler, 0, 0), value, lanes)
                                          .SetType(ir::ValueType::V128)
                                : VecConst(assembler, 0, 0);
        if (width * 2 == dst_bits) {
            lo = __ VecZip(value, filler, lanes, ir::Imm(0u)).SetType(ir::ValueType::V128);
            if (v.l) {
                hi = __ VecZip(value, filler, lanes, ir::Imm(1u)).SetType(ir::ValueType::V128);
            }
        } else {
            value = __ VecZip(value, filler, lanes, ir::Imm(0u)).SetType(ir::ValueType::V128);
        }
    }
    if (v.l) {
        VexWrite256(v.reg, lo, hi);
    } else {
        VexWrite128(v.reg, lo);
    }
}

// ---------------------------------------------------------------------------
// Broadcasts and lane moves
// ---------------------------------------------------------------------------

// vpbroadcastb/w/d/q.  The source element is the LOW element of the source's
// low 128-bit lane and fills the whole destination, so both halves receive the
// identical V128 -- cross-lane, but trivially so.
void X64Decoder::DecodeAvxIntBroadcast(const VexInsn& v, u32 element_bits) {
    ir::Value element;
    if (v.RmIsRegister()) {
        element = XmmLo(XmmOf(v.rm)).SetType(ir::ValueType::U64);
        if (element_bits < 64) {
            element = __ And(element, ir::Operand{ir::Imm((u64(1) << element_bits) - 1)})
                              .SetType(ir::ValueType::U64);
        }
    } else {
        auto raw = MemLoad(ir::Operand{VexAddress(v)},
                           GetSize(element_bits),
                           VexTsoOrdered(v));
        element = raw;
        if (element_bits != 64) {
            element = __ ZeroExtend64(raw);
        }
    }
    // Fill the qword by repeated doubling; VecDup64 then covers 128 bits.
    for (u32 width = element_bits; width < 64; width *= 2) {
        element = __ Or(element, ir::Operand{__ LslImm(element, ir::Imm(u64(width)))
                                                     .SetType(ir::ValueType::U64)})
                          .SetType(ir::ValueType::U64);
    }
    auto value = __ VecDup64(element).SetType(ir::ValueType::V128);
    if (v.l) {
        VexWrite256(v.reg, value, __ VecDup64(element).SetType(ir::ValueType::V128));
    } else {
        VexWrite128(v.reg, value);
    }
}

// vbroadcasti128 ymm, m128.  Memory operand only; the register form is #UD.
void X64Decoder::DecodeAvxIntBroadcast128(const VexInsn& v) {
    auto address = VexAddress(v);
    auto lo = MemLoad(ir::Operand{address}, ir::ValueType::V128, VexTsoOrdered(v));
    // A second load rather than reusing `lo`: both halves are separate IR
    // values under C1 and sharing one would tie the two register writes to a
    // single live range across the whole block.
    auto hi = MemLoad(ir::Operand{address}, ir::ValueType::V128, VexTsoOrdered(v));
    VexWrite256(v.reg, lo, hi);
}

// vinserti128 ymm1, ymm2, xmm3/m128, imm8: src1 with one 128-bit lane replaced.
// imm8 bit 0 picks the lane, so only the OTHER half of src1 is ever read.
void X64Decoder::DecodeAvxIntInsert128(const VexInsn& v) {
    const bool high = (v.imm8 & 1) != 0;
    auto inserted = VexLoadVec(v);
    if (high) {
        VexWrite256(v.reg, XmmRead(XmmOf(v.vvvv)), inserted);
    } else {
        VexWrite256(v.reg, inserted, YmmHighRead(v.vvvv));
    }
}

// vextracti128 xmm1/m128, ymm2, imm8.  The DESTINATION is the r/m operand and
// the source is ModRM.reg -- the reverse of every other shape in this file.
// A register destination is an xmm, so C3 applies to it.
void X64Decoder::DecodeAvxIntExtract128(const VexInsn& v) {
    const bool high = (v.imm8 & 1) != 0;
    auto value = high ? YmmHighRead(v.reg) : XmmRead(XmmOf(v.reg));
    if (v.RmIsRegister()) {
        VexWrite128(v.rm, value);
    } else {
        MemStore(ir::Operand{VexAddress(v)},
                 value.SetType(ir::ValueType::V128),
                 VexTsoOrdered(v));
    }
}

// vperm2i128 ymm1, ymm2, ymm3/m256, imm8.  Each output 128-bit lane is any of
// the four input lanes or zero, selected by imm8 -- a decode-time constant, so
// each half is just a choice between already-existing values.  Only the sources
// a lane actually selects are loaded: a dead 256-bit load would both cost a
// memory access and upset the register allocator.
void X64Decoder::DecodeAvxIntPerm2i128(const VexInsn& v) {
    const u32 imm = v.imm8;
    const bool zero_lo = (imm & 0x08) != 0;
    const bool zero_hi = (imm & 0x80) != 0;
    const u32 sel_lo = imm & 3;
    const u32 sel_hi = (imm >> 4) & 3;
    const bool need_a = (!zero_lo && sel_lo < 2) || (!zero_hi && sel_hi < 2);
    const bool need_b = (!zero_lo && sel_lo >= 2) || (!zero_hi && sel_hi >= 2);
    ir::Value a_lo, a_hi;
    if (need_a) {
        a_lo = XmmRead(XmmOf(v.vvvv));
        a_hi = YmmHighRead(v.vvvv);
    }
    VecHalves b{};
    if (need_b) {
        b = VexLoadVec256(v);
    }
    const auto pick = [&](u32 selector, bool zero) -> ir::Value {
        if (zero) {
            return VecConst(assembler, 0, 0);
        }
        switch (selector) {
            case 0:
                return a_lo;
            case 1:
                return a_hi;
            case 2:
                return b.lo;
            default:
                return b.hi;
        }
    };
    auto lo = pick(sel_lo, zero_lo);
    auto hi = pick(sel_hi, zero_hi);
    VexWrite256(v.reg, lo, hi);
}

// vpermq ymm1, ymm2/m256, imm8.  A free permutation of the four qwords, so
// three of the four selectors can name the other 128-bit lane.  imm8 is a
// decode-time constant, so each output qword is a KNOWN source qword and the
// half can be rebuilt scalar-wise; the two whole-lane cases skip that entirely.
void X64Decoder::DecodeAvxIntPermq(const VexInsn& v) {
    const u32 imm = v.imm8;
    auto src = VexLoadVec256(v);
    ir::Value cache[4];
    bool cached[4] = {false, false, false, false};
    const auto qword = [&](u32 index) -> ir::Value {
        if (!cached[index]) {
            cache[index] = __ VecExtract64(index < 2 ? src.lo : src.hi, ir::Imm(index & 1u))
                                   .SetType(ir::ValueType::U64);
            cached[index] = true;
        }
        return cache[index];
    };
    const auto build = [&](u32 half) -> ir::Value {
        const u32 s0 = (imm >> (half * 4)) & 3;
        const u32 s1 = (imm >> (half * 4 + 2)) & 3;
        if (s0 == 0 && s1 == 1) {
            return src.lo;
        }
        if (s0 == 2 && s1 == 3) {
            return src.hi;
        }
        return VecPairQ(assembler, qword(s0), qword(s1));
    };
    auto lo = build(0);
    auto hi = build(1);
    VexWrite256(v.reg, lo, hi);
}

// vpermd ymm1, ymm2, ymm3/m256.  The only instruction in this family whose
// cross-lane routing is a RUNTIME value: output dword i is src2 dword
// (src1 dword i)[2:0], and that index freely names either 128-bit lane.
//
// Under C1 src2 is two V128 values, so the lookup is done twice and OR-ed --
// which works because a byte index outside 0..15 must yield zero.  The catch is
// that VecTableLookup8 masks its control with 0x8F (it exists to serve PSHUFB,
// where bits 6:4 are architecturally ignored), so byte index 20 would be read
// as 4, not as out-of-range.  Bit 7 is the only bit that survives the mask and
// puts an index out of range, so the "wrong lane" bytes are marked with it
// explicitly instead of relying on their value.
void X64Decoder::DecodeAvxIntPermd(const VexInsn& v) {
    auto index_lo = XmmRead(XmmOf(v.vvvv));
    auto index_hi = YmmHighRead(v.vvvv);
    auto table = VexLoadVec256(v);

    constexpr u64 kSeven = Replicate(32, 7);
    constexpr u64 kTimesFour = 0x0404040404040404ull;
    constexpr u64 kByteInDword = 0x0302010003020100ull;
    constexpr u64 kBit4 = 0x1010101010101010ull;
    constexpr u64 kBit7 = 0x8080808080808080ull;

    const auto lane = [&](ir::Value index) -> ir::Value {
        // dword index -> byte indices.  index & 7 is at most 7, so multiplying
        // the dword by 0x04040404 puts 4*index in every byte of it without
        // carrying between bytes; adding {0,1,2,3} completes the four bytes.
        auto masked = __ VecAnd(index, VecConst(assembler, kSeven, kSeven))
                              .SetType(ir::ValueType::V128);
        auto scaled = __ VecMul(masked,
                                VecConst(assembler, kTimesFour, kTimesFour),
                                ir::Imm(32u))
                              .SetType(ir::ValueType::V128);
        auto bytes = __ VecAdd(scaled,
                               VecConst(assembler, kByteInDword, kByteInDword),
                               ir::Imm(8u))
                             .SetType(ir::ValueType::V128);
        // 0x80 exactly where the index names the HIGH source lane.
        auto high_flag =
                __ VecShiftLeft(__ VecAnd(bytes, VecConst(assembler, kBit4, kBit4))
                                        .SetType(ir::ValueType::V128),
                                __ LoadImm(ir::Imm(u64(3))).SetType(ir::ValueType::U64),
                                ir::Imm(8u))
                        .SetType(ir::ValueType::V128);
        auto low_flag = __ VecXor(high_flag, VecConst(assembler, kBit7, kBit7))
                                .SetType(ir::ValueType::V128);
        auto from_low = __ VecTableLookup8(
                                table.lo,
                                __ VecOr(bytes, high_flag).SetType(ir::ValueType::V128))
                                .SetType(ir::ValueType::V128);
        auto from_high = __ VecTableLookup8(
                                 table.hi,
                                 __ VecOr(bytes, low_flag).SetType(ir::ValueType::V128))
                                 .SetType(ir::ValueType::V128);
        return __ VecOr(from_low, from_high).SetType(ir::ValueType::V128);
    };
    auto lo = lane(index_lo);
    auto hi = lane(index_hi);
    VexWrite256(v.reg, lo, hi);
}

// vpblendvb ymm1, ymm2, ymm3/m256, ymm4.  The selector register is the HIGH
// NIBBLE of the trailing /is4 byte, not an operand slot.  Only bit 7 of each
// selector byte matters, so an arithmetic shift right by 7 turns the selector
// into the 0x00 / 0xFF mask the bitwise blend needs.
void X64Decoder::DecodeAvxIntBlendv(const VexInsn& v) {
    const u32 selector = v.is4_register;
    const auto expand = [&](ir::Value raw) -> ir::Value {
        return __ VecShiftRightArithmetic(
                        raw, __ LoadImm(ir::Imm(u64(7))).SetType(ir::ValueType::U64), ir::Imm(8u))
                .SetType(ir::ValueType::V128);
    };
    const auto blend = [&](ir::Value a, ir::Value b, ir::Value mask) -> ir::Value {
        return __ VecOr(__ VecAnd(b, mask).SetType(ir::ValueType::V128),
                        __ VecAndNot(a, mask).SetType(ir::ValueType::V128))
                .SetType(ir::ValueType::V128);
    };
    if (v.l) {
        auto a_lo = XmmRead(XmmOf(v.vvvv));
        auto a_hi = YmmHighRead(v.vvvv);
        auto b = VexLoadVec256(v);
        auto m_lo = expand(XmmRead(XmmOf(selector)));
        auto m_hi = expand(YmmHighRead(selector));
        auto lo = blend(a_lo, b.lo, m_lo);
        auto hi = blend(a_hi, b.hi, m_hi);
        VexWrite256(v.reg, lo, hi);
        return;
    }
    auto a = XmmRead(XmmOf(v.vvvv));
    auto b = VexLoadVec(v);
    VexWrite128(v.reg, blend(a, b, expand(XmmRead(XmmOf(selector)))));
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

bool X64Decoder::DecodeAvxInt(const VexInsn& v) {
    if (!AvxEnabled() || !v.valid) {
        return false;
    }
    // Every opcode below is a 66-prefixed form except vpshuflw (F2) and
    // vpshufhw (F3), which share opcode 0x70 with vpshufd.
    const bool p66 = v.pp == VexPP::P66;
    const bool wide = v.l;

    switch (v.map) {
        case VexMap::Map0F:
            if (v.opcode == 0x70) {
                switch (v.pp) {
                    case VexPP::P66:
                        DecodeAvxIntUnary(
                                v,
                                [](ir::Assembler* as, ir::Value a, u32 param) {
                                    return VecShuffle32Lowered(
                                            as, a, param & 0xffu,
                                            (param & 0x100u) != 0);
                                },
                                v.imm8 | (features_.vec_const_cache ? 0x100u : 0u));
                        return true;
                    case VexPP::PF2:  // vpshuflw
                    case VexPP::PF3:  // vpshufhw
                        DecodeAvxIntUnary(
                                v,
                                [](ir::Assembler* as, ir::Value a, u32 param) {
                                    return as->VecShuffle16(a,
                                                            ir::Imm(Lane(param)),
                                                            ir::Imm(Flag(param)))
                                            .SetType(ir::ValueType::V128);
                                },
                                Pack(v.imm8, v.pp == VexPP::PF3 ? 1u : 0u));
                        return true;
                    default:
                        return false;
                }
            }
            if (!p66) {
                return false;
            }
            switch (v.opcode) {
                // ---- unpack / pack ------------------------------------------
                case 0x60:
                    DecodeAvxIntBinary(v, OpZip, Pack(8, 0));
                    return true;
                case 0x61:
                    DecodeAvxIntBinary(v, OpZip, Pack(16, 0));
                    return true;
                case 0x62:
                    DecodeAvxIntBinary(v, OpZip, Pack(32, 0));
                    return true;
                case 0x6C:
                    DecodeAvxIntBinary(v, OpZip, Pack(64, 0));
                    return true;
                case 0x68:
                    DecodeAvxIntBinary(v, OpZip, Pack(8, 1));
                    return true;
                case 0x69:
                    DecodeAvxIntBinary(v, OpZip, Pack(16, 1));
                    return true;
                case 0x6A:
                    DecodeAvxIntBinary(v, OpZip, Pack(32, 1));
                    return true;
                case 0x6D:
                    DecodeAvxIntBinary(v, OpZip, Pack(64, 1));
                    return true;
                case 0x63:  // vpacksswb
                    DecodeAvxIntBinary(v, OpPack, Pack(16, 0));
                    return true;
                case 0x67:  // vpackuswb
                    DecodeAvxIntBinary(v, OpPack, Pack(16, 1));
                    return true;
                case 0x6B:  // vpackssdw
                    DecodeAvxIntBinary(v, OpPack, Pack(32, 0));
                    return true;
                // ---- shift by imm8 (NDD: destination is VEX.vvvv) -----------
                case 0x71:
                case 0x72:
                case 0x73: {
                    if (!v.RmIsRegister()) {
                        return false;  // the group forms require mod == 11
                    }
                    const u32 lane = v.opcode == 0x71 ? 16u : (v.opcode == 0x72 ? 32u : 64u);
                    switch (v.reg & 7) {
                        case 2:
                            DecodeAvxIntShiftImm(v, 1, lane);
                            return true;
                        case 4:
                            if (lane == 64) {
                                return false;  // vpsraq is AVX-512 only
                            }
                            DecodeAvxIntShiftImm(v, 2, lane);
                            return true;
                        case 6:
                            DecodeAvxIntShiftImm(v, 0, lane);
                            return true;
                        case 3:
                            if (lane == 64 &&
                                VecLoweringEnabled(features_.vec_byteshift_ext)) {
                                DecodeAvxIntByteShift(v, false);
                                return true;
                            }
                            return false;
                        case 7:
                            if (lane == 64 &&
                                VecLoweringEnabled(features_.vec_byteshift_ext)) {
                                DecodeAvxIntByteShift(v, true);
                                return true;
                            }
                            return false;
                        default:
                            return false;
                    }
                }
                // ---- shift by an xmm/m128 count ----------------------------
                case 0xD1:
                    DecodeAvxIntShiftCount(v, 1, 16);
                    return true;
                case 0xD2:
                    DecodeAvxIntShiftCount(v, 1, 32);
                    return true;
                case 0xD3:
                    DecodeAvxIntShiftCount(v, 1, 64);
                    return true;
                case 0xE1:
                    DecodeAvxIntShiftCount(v, 2, 16);
                    return true;
                case 0xE2:
                    DecodeAvxIntShiftCount(v, 2, 32);
                    return true;
                case 0xF1:
                    DecodeAvxIntShiftCount(v, 0, 16);
                    return true;
                case 0xF2:
                    DecodeAvxIntShiftCount(v, 0, 32);
                    return true;
                case 0xF3:
                    DecodeAvxIntShiftCount(v, 0, 64);
                    return true;
                // ---- multiply ----------------------------------------------
                case 0xD5:  // vpmullw
                    DecodeAvxIntBinary(v, OpMul, Pack(16));
                    return true;
                case 0xE4:  // vpmulhuw
                    DecodeAvxIntBinary(v, OpMulHigh, Pack(16, 0));
                    return true;
                case 0xE5:  // vpmulhw
                    DecodeAvxIntBinary(v, OpMulHigh, Pack(16, 1));
                    return true;
                case 0xF5:  // vpmaddwd
                    DecodeAvxIntBinary(v, OpMadd, 0);
                    return true;
                case 0xF6:  // vpsadbw
                    DecodeAvxIntBinary(v, OpSadbw, 0);
                    return true;
                // ---- average -----------------------------------------------
                case 0xE0:
                    DecodeAvxIntBinary(v, OpAvg, Pack(8));
                    return true;
                case 0xE3:
                    DecodeAvxIntBinary(v, OpAvg, Pack(16));
                    return true;
                // ---- saturating add / sub ----------------------------------
                case 0xDC:
                    DecodeAvxIntBinary(v, OpSatAdd, Pack(8, 0));
                    return true;
                case 0xDD:
                    DecodeAvxIntBinary(v, OpSatAdd, Pack(16, 0));
                    return true;
                case 0xEC:
                    DecodeAvxIntBinary(v, OpSatAdd, Pack(8, 1));
                    return true;
                case 0xED:
                    DecodeAvxIntBinary(v, OpSatAdd, Pack(16, 1));
                    return true;
                case 0xD8:
                    DecodeAvxIntBinary(v, OpSatSub, Pack(8, 0));
                    return true;
                case 0xD9:
                    DecodeAvxIntBinary(v, OpSatSub, Pack(16, 0));
                    return true;
                case 0xE8:
                    DecodeAvxIntBinary(v, OpSatSub, Pack(8, 1));
                    return true;
                case 0xE9:
                    DecodeAvxIntBinary(v, OpSatSub, Pack(16, 1));
                    return true;
                // ---- signed word min / max ---------------------------------
                case 0xEA:
                    DecodeAvxIntBinary(v, OpMin, Pack(16, 1));
                    return true;
                case 0xEE:
                    DecodeAvxIntBinary(v, OpMax, Pack(16, 1));
                    return true;
                default:
                    return false;
            }

        case VexMap::Map0F38:
            if (!p66) {
                return false;
            }
            switch (v.opcode) {
                // ---- sign / absolute value ---------------------------------
                case 0x08:
                    DecodeAvxIntBinary(v, OpSign, Pack(8));
                    return true;
                case 0x09:
                    DecodeAvxIntBinary(v, OpSign, Pack(16));
                    return true;
                case 0x0A:
                    DecodeAvxIntBinary(v, OpSign, Pack(32));
                    return true;
                case 0x1C:
                    DecodeAvxIntUnary(v, OpAbs, Pack(8));
                    return true;
                case 0x1D:
                    DecodeAvxIntUnary(v, OpAbs, Pack(16));
                    return true;
                case 0x1E:
                    DecodeAvxIntUnary(v, OpAbs, Pack(32));
                    return true;
                // ---- sign / zero extension ---------------------------------
                case 0x20:
                    DecodeAvxIntExtend(v, 8, 16, true);
                    return true;
                case 0x21:
                    DecodeAvxIntExtend(v, 8, 32, true);
                    return true;
                case 0x22:
                    DecodeAvxIntExtend(v, 8, 64, true);
                    return true;
                case 0x23:
                    DecodeAvxIntExtend(v, 16, 32, true);
                    return true;
                case 0x24:
                    DecodeAvxIntExtend(v, 16, 64, true);
                    return true;
                case 0x25:
                    DecodeAvxIntExtend(v, 32, 64, true);
                    return true;
                case 0x30:
                    DecodeAvxIntExtend(v, 8, 16, false);
                    return true;
                case 0x31:
                    DecodeAvxIntExtend(v, 8, 32, false);
                    return true;
                case 0x32:
                    DecodeAvxIntExtend(v, 8, 64, false);
                    return true;
                case 0x33:
                    DecodeAvxIntExtend(v, 16, 32, false);
                    return true;
                case 0x34:
                    DecodeAvxIntExtend(v, 16, 64, false);
                    return true;
                case 0x35:
                    DecodeAvxIntExtend(v, 32, 64, false);
                    return true;
                case 0x2B:  // vpackusdw
                    DecodeAvxIntBinary(v, OpPackUsdw, 0);
                    return true;
                // ---- signed / unsigned min and max -------------------------
                case 0x38:
                    DecodeAvxIntBinary(v, OpMin, Pack(8, 1));
                    return true;
                case 0x39:
                    DecodeAvxIntBinary(v, OpMin, Pack(32, 1));
                    return true;
                case 0x3A:
                    DecodeAvxIntBinary(v, OpMin, Pack(16, 0));
                    return true;
                case 0x3C:
                    DecodeAvxIntBinary(v, OpMax, Pack(8, 1));
                    return true;
                case 0x3D:
                    DecodeAvxIntBinary(v, OpMax, Pack(32, 1));
                    return true;
                case 0x3E:
                    DecodeAvxIntBinary(v, OpMax, Pack(16, 0));
                    return true;
                case 0x40:  // vpmulld
                    DecodeAvxIntBinary(v, OpMul, Pack(32));
                    return true;
                // ---- AVX2 per-lane variable shifts -------------------------
                case 0x45:
                    DecodeAvxIntBinary(v, OpShiftVar, Pack(v.w ? 64u : 32u, 1));
                    return true;
                case 0x46:
                    if (v.w) {
                        return false;  // vpsravq is AVX-512 only
                    }
                    DecodeAvxIntBinary(v, OpShiftVar, Pack(32, 2));
                    return true;
                case 0x47:
                    DecodeAvxIntBinary(v, OpShiftVar, Pack(v.w ? 64u : 32u, 0));
                    return true;
                // ---- broadcasts --------------------------------------------
                case 0x58:
                    DecodeAvxIntBroadcast(v, 32);
                    return true;
                case 0x59:
                    DecodeAvxIntBroadcast(v, 64);
                    return true;
                case 0x5A:  // vbroadcasti128
                    if (!wide || v.RmIsRegister()) {
                        return false;
                    }
                    DecodeAvxIntBroadcast128(v);
                    return true;
                case 0x78:
                    DecodeAvxIntBroadcast(v, 8);
                    return true;
                case 0x79:
                    DecodeAvxIntBroadcast(v, 16);
                    return true;
                case 0x36:  // vpermd
                    if (!wide || v.w) {
                        return false;
                    }
                    DecodeAvxIntPermd(v);
                    return true;
                default:
                    return false;
            }

        case VexMap::Map0F3A:
            if (!p66) {
                return false;
            }
            switch (v.opcode) {
                case 0x00:  // vpermq
                    if (!wide || !v.w) {
                        return false;
                    }
                    DecodeAvxIntPermq(v);
                    return true;
                case 0x02:  // vpblendd
                    if (v.w) {
                        return false;
                    }
                    DecodeAvxIntBinary(v, OpBlendImm, Pack(32, v.imm8));
                    return true;
                case 0x0E:  // vpblendw
                    DecodeAvxIntBinary(v, OpBlendImm, Pack(16, v.imm8));
                    return true;
                case 0x0F:  // vpalignr
                    if (v.imm8 >= 32) {
                        // The whole concatenation is shifted out.  Skipping the
                        // operand loads is deliberate; see KNOWN DEVIATIONS.
                        DecodeAvxIntZeroDst(v);
                        return true;
                    }
                    DecodeAvxIntBinary(v, OpAlignr, Pack(0, v.imm8));
                    return true;
                case 0x38:  // vinserti128
                    if (!wide || v.w) {
                        return false;
                    }
                    DecodeAvxIntInsert128(v);
                    return true;
                case 0x39:  // vextracti128
                    if (!wide || v.w) {
                        return false;
                    }
                    DecodeAvxIntExtract128(v);
                    return true;
                case 0x46:  // vperm2i128
                    if (!wide || v.w) {
                        return false;
                    }
                    DecodeAvxIntPerm2i128(v);
                    return true;
                case 0x4C:  // vpblendvb
                    if (v.w) {
                        return false;
                    }
                    DecodeAvxIntBlendv(v);
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
