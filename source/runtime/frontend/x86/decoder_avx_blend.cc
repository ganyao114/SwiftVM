// VEX blend / extract-insert-scalar / masked-move handlers (AVX1, both widths).
//
// Scope -- the ten encodings an end-to-end run found missing, each of which
// kills the guest outright today (no interpreter fallback exists: an
// unclaimed VEX opcode traps the block as FALLBACK -> IllegalCode):
//
//   66.0F3A 0C  vblendps    imm8-controlled 32-bit lane blend
//   66.0F3A 0D  vblendpd    imm8-controlled 64-bit lane blend
//   66.0F3A 4A  vblendvps   register-controlled 32-bit lane blend (/is4)
//   66.0F3A 4B  vblendvpd   register-controlled 64-bit lane blend (/is4)
//   66.0F3A 17  vextractps  one 32-bit lane -> r/m32
//   66.0F3A 21  vinsertps   one 32-bit lane <- xmm lane or m32, plus zeroing
//   66.0F38 2C  vmaskmovps  masked 32-bit lane LOAD
//   66.0F38 2D  vmaskmovpd  masked 64-bit lane LOAD
//   66.0F38 2E  vmaskmovps  masked 32-bit lane STORE
//   66.0F38 2F  vmaskmovpd  masked 64-bit lane STORE
//
// Everything decodes from VexInsn (vex_decoder.h); distorm is never consulted,
// for the reasons vex_decoder.h documents (it drops VEX.L silently on 38 of
// 117 measured encodings and returns I_UNDEFINED for 40 more).
//
// ---------------------------------------------------------------------------
// DECLARATIONS FOR decoder.h  (main line merges these into X64Decoder's
// private section; this file deliberately does not own the shared header)
// ---------------------------------------------------------------------------
//
//     // ---- VEX blend / extract / maskmov (decoder_avx_blend.cc) --------
//     bool DecodeAvxBlend(const VexInsn& v);
//     void DecodeAvxBlendVar(const VexInsn& v, u32 lane_bits);
//     void DecodeAvxInsertPs(const VexInsn& v);
//     void DecodeAvxMaskMov(const VexInsn& v, u32 lane_bits, bool store);
//
// DISPATCH PATCH for decoder.cc (the VEX arm of X64Decoder::Decode):
//
//   -  if (DecodeBmi(vex) || (avx_on && (DecodeAvxInt(vex) || DecodeAvxFp(vex)))) {
//   +  if (DecodeBmi(vex) ||
//   +      (avx_on && (DecodeAvxInt(vex) || DecodeAvxFp(vex) || DecodeAvxBlend(vex)))) {
//
// Order is free: none of the ten opcodes above is claimed by DecodeAvxInt or
// DecodeAvxFp (checked opcode by opcode -- 0F3A 0C/0D/17/21/4A/4B and
// 0F38 2C..2F are all unclaimed), so this handler can sit anywhere in the
// chain.  Last is chosen so an existing claim would win rather than silently
// change meaning.
//
// vex_decoder.cc's length table needs NO change: 0F3A is uniformly
// imm8-carrying there and already lists 4A/4B as the /is4 forms, and 0F38 is
// uniformly immediate-free.  Verified opcode by opcode, because a wrong answer
// there desynchronizes the whole decode stream instead of mis-decoding one
// instruction.
//
// ---------------------------------------------------------------------------
// CONTRACTS
// ---------------------------------------------------------------------------
// C1  A YMM is never one IR value: bits 127:0 live in ThreadContext64::xmms[i]
//     and 255:128 in ymm_high[i], and a 256-bit operation is two independent
//     V128 operations.  Every instruction in this file is defined PER LANE with
//     no lane crossing at all -- blends select lane by lane, vextractps and
//     vinsertps are VEX.128-only, and a masked move touches each element on its
//     own -- so the split loses nothing here.  The only cross-half quantity is
//     the imm8 blend control, and that is a decode-time constant: each half
//     simply reads a different bit range of it.
// C3  A VEX.128 form zeroes bits 255:128 of its destination.  The imm8 and
//     variable blends reach that through VexWrite128 / DecodeAvxIntBinary;
//     vinsertps through VexWrite128; the masked LOAD zeroes all four qwords of
//     the destination up front, which covers C3 and the "mask bit 0 -> element
//     reads as zero" rule in one step.  vextractps and the masked STORE have no
//     vector destination.
//
// ---------------------------------------------------------------------------
// WHY THIS FILE ADDS NO IR OPCODE
// ---------------------------------------------------------------------------
// The IR is a multi-ISA middle end (ARM64 / x86 / Slang front ends, ARM64 /
// RISC-V64 back ends), so a new opcode has to be meaningful to another front
// end and naturally implementable by another back end.  Everything here
// decomposes into primitives that already pass that test:
//
//   * A blend is a bitwise SELECT.  `VecOr(VecAnd(b, m), VecAndNot(a, m))` is
//     exactly AArch64's BSL and RISC-V V's vmerge, and the two x86-specific
//     parts -- "imm8 bit j names lane j" and "the /is4 register's per-lane sign
//     bit names lane j" -- are converted to that one uniform lane mask HERE, in
//     the front end, where they belong.  decoder_avx_int.cc's vpblendd /
//     vpblendw / vpblendvb already use the same three ops, so this is the
//     established spelling rather than a new one.
//   * vinsertps's imm8 is three unrelated bit fields (source lane, destination
//     lane, zero mask).  That is pure x86 trivia and stays here; the IR sees
//     only "extract lane", "insert lane", "and with a constant".
//   * A masked memory access becomes a per-element conditional access built
//     from NotGoto / BindLabel / LoadMemory / StoreMemory -- the same shape
//     DecodeMaskmovdqu has used for MASKMOVDQU since before AVX existed.
//
// A dedicated `VecSelect(mask, a, b)` would be a legitimate neutral primitive
// (BSL on AArch64, vmerge.vvm on RVV) and would turn three IR instructions into
// one; it is left out deliberately because the existing three already express
// it on every back end, and an unnecessary opcode is a permanent cost.
//
// ---------------------------------------------------------------------------
// KNOWN DEVIATION -- vmaskmov AND FAULT SUPPRESSION
// ---------------------------------------------------------------------------
// x86 requires that an element whose mask bit is clear neither faults nor is
// written.  That is MODELLED here rather than approximated: each element gets
// its own conditional branch, so a masked-off element issues no memory access
// at all and an unmapped page under it cannot fault.  A full-width load-then-
// select would have been three instructions instead of ~25, and would have
// been wrong exactly where this instruction is used -- the loop tails that mask
// off the elements past the end of an array, which is precisely where the next
// page is unmapped.
//
// Two smaller deviations remain, both stated rather than hidden:
//
//   * Element-at-a-time access is not one indivisible 16/32-byte transaction.
//     x86 does not promise that for these instructions either (they are not
//     locked and cross cache lines), so this is visible only to another thread
//     racing on the same bytes.
//   * A fault on a masked-ON element is raised after the earlier elements have
//     already been written.  For a LOAD whose destination register is also its
//     mask register (`vmaskmovps ymm0, ymm0, [rdi]` is a legal encoding), a
//     guest that handles the fault, maps the page and RESTARTS the instruction
//     would re-read a mask its own partial result had overwritten.  Hardware
//     would restart cleanly.  Fixing it needs values to escape a conditional
//     region, which this IR expresses only through the register file -- i.e.
//     the very thing being written.  The window is a faulting masked load into
//     its own mask register with a resuming SIGSEGV handler.
//
// ---------------------------------------------------------------------------
// OTHER DELIBERATE DEVIATIONS
// ---------------------------------------------------------------------------
//  * No alignment check anywhere, matching every other SSE/AVX path here.
//    (None of these opcodes requires alignment architecturally.)
//  * Segment overrides are not applied to the memory operand -- VexInsn does
//    not carry the segment.  Inherited from VexAddress, shared with every VEX
//    handler.
//  * Reserved-field #UD is enforced only where the field changes MEANING:
//    VEX.W on vblendv*/vmaskmov* (W1 is a different, reserved encoding) and
//    VEX.L / VEX.vvvv on vextractps and vinsertps (both are VEX.128-only).
//    Elsewhere the front end does not raise #UD for reserved bits, and this
//    file does not start.
//  * vblendps/vblendpd always read the r/m operand even when the imm8 selects
//    nothing from it.  That matches hardware (the operand IS read) and avoids
//    a dead load, which the register allocator handles worse than a live one.

#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/vex_decoder.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

// DecodeAvxIntBinary hands its per-half callback ONE opaque u32, so the lane
// width and the imm8 travel packed together.  Same convention as
// decoder_avx_int.cc; kept file-local because that one is too.
constexpr u32 Pack(u32 lane, u32 flag = 0) { return lane | (flag << 16); }
constexpr u32 Lane(u32 param) { return param & 0xFFFF; }
constexpr u32 Flag(u32 param) { return param >> 16; }

// Materialize an arbitrary 128-bit constant.  The IR has no vector-immediate
// opcode, so this goes through the scalar path: VecDup64 broadcasts a GPR into
// both qwords and a 64-bit Zip1 takes lane 0 of each of two broadcasts.
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

// result = (b & mask) | (a & ~mask), lane-agnostic.  This is the neutral
// bit-select every blend in this file funnels into.
ir::Value BitSelect(ir::Assembler* as, ir::Value a, ir::Value b, ir::Value mask) {
    return as->VecOr(as->VecAnd(b, mask).SetType(ir::ValueType::V128),
                     as->VecAndNot(a, mask).SetType(ir::ValueType::V128))
            .SetType(ir::ValueType::V128);
}

// vblendps / vblendpd, as a DecodeAvxIntBinary per-half callback.
//
// a is VEX.vvvv (SRC1, the "not selected" source) and b is the r/m operand
// (SRC2, the "selected" one) -- getting that round the wrong way is invisible
// for symmetric inputs, which is why the reference data uses two distinguishable
// vectors.
//
// The control bit for output lane i of half `half` is imm8[half*count + i],
// with count = 4 for ps and 2 for pd.  That single expression covers all four
// SDM cases: VEX.128 uses imm8[3:0] / imm8[1:0] (half is always 0) and VEX.256
// uses imm8[7:0] / imm8[3:0].  Bits above the used range are ignored, which
// falls out of never reading them.
ir::Value OpBlendFpImm(ir::Assembler* as, ir::Value a, ir::Value b, u32 param, u32 half) {
    const u32 imm = Flag(param);
    const u32 lane_bits = Lane(param);
    const u32 count = 128u / lane_bits;
    const u32 bytes = lane_bits / 8u;
    u64 mask[2] = {0, 0};
    for (u32 i = 0; i < count; ++i) {
        if (((imm >> (half * count + i)) & 1u) == 0) {
            continue;
        }
        for (u32 byte = 0; byte < bytes; ++byte) {
            const u32 position = i * bytes + byte;
            mask[position / 8] |= u64(0xFF) << ((position % 8) * 8);
        }
    }
    return BitSelect(as, a, b, VecConst(as, mask[0], mask[1]));
}

}  // namespace

// vblendvps / vblendvpd -- VEX.NDS.128/256.66.0F3A.W0 4A(4B) /r /is4.
//
// dst = ModRM.reg, SRC1 = VEX.vvvv, SRC2 = r/m, and the SELECTOR is the high
// nibble of the trailing byte (VexInsn::is4_register), not an operand slot.
// Only each lane's most significant bit matters, so an arithmetic shift right
// by lane_bits-1 turns the selector into the 0 / all-ones lane mask the bit
// select needs.  Exactly the shape DecodeAvxIntBlendv uses for vpblendvb, at
// lane widths 32 and 64 instead of 8.
//
// Note that the mask is read as INTEGER lanes even though the instruction is a
// float one: "the sign bit" of an f32 and the msb of a u32 are the same bit,
// and no floating-point interpretation happens anywhere in a blend.
void X64Decoder::DecodeAvxBlendVar(const VexInsn& v, u32 lane_bits) {
    const u32 selector = v.is4_register;
    const auto expand = [&](ir::Value raw) -> ir::Value {
        return __ VecShiftRightArithmetic(
                        raw,
                        __ LoadImm(ir::Imm(u64(lane_bits - 1))).SetType(ir::ValueType::U64),
                        ir::Imm(lane_bits))
                .SetType(ir::ValueType::V128);
    };
    if (v.Is256()) {
        auto a_lo = XmmRead(XmmOf(v.vvvv));
        auto a_hi = YmmHighRead(v.vvvv);
        auto b = VexLoadVec256(v);
        auto lo = BitSelect(assembler, a_lo, b.lo, expand(XmmRead(XmmOf(selector))));
        auto hi = BitSelect(assembler, a_hi, b.hi, expand(YmmHighRead(selector)));
        VexWrite256(v.reg, lo, hi);
        return;
    }
    auto a = XmmRead(XmmOf(v.vvvv));
    auto b = VexLoadVec(v);
    VexWrite128(v.reg, BitSelect(assembler, a, b, expand(XmmRead(XmmOf(selector)))));
}

// vinsertps -- VEX.NDS.128.66.0F3A.WIG 21 /r ib.  VEX.128 only; VEX.256 is #UD.
//
// The imm8 is three unrelated fields:
//   [7:6] COUNT_S -- which 32-bit lane of SRC2 to take.  IGNORED (and read as
//                    0) when SRC2 is memory, because then SRC2 is an m32 and
//                    there is only one lane to take.
//   [5:4] COUNT_D -- which 32-bit lane of the destination it lands in.
//   [3:0] ZMASK   -- lanes forced to zero AFTERWARDS.  Applied last, so a lane
//                    that was just inserted can be zeroed again; that is the
//                    SDM's order and it is observable (imm8 = 0x0F yields zero
//                    whatever the other fields say).
//
// The untouched lanes come from SRC1 = VEX.vvvv, not from the destination:
// VEX made this form non-destructive.  Every index is a decode-time constant,
// so the whole thing is two 16-bit inserts plus, when ZMASK is non-zero, one
// AND with a constant.
void X64Decoder::DecodeAvxInsertPs(const VexInsn& v) {
    const u32 count_d = (v.imm8 >> 4) & 3u;
    const u32 zmask = v.imm8 & 0xFu;

    ir::Value element;
    if (v.RmIsRegister()) {
        const u32 count_s = (v.imm8 >> 6) & 3u;
        auto source = XmmRead(XmmOf(v.rm));
        auto container = __ VecExtract64(source, ir::Imm(count_s / 2)).SetType(ir::ValueType::U64);
        if (count_s % 2 != 0) {
            container = __ LsrImm(container, ir::Imm(32u)).SetType(ir::ValueType::U64);
        }
        element = __ And(container, ir::Operand{ir::Imm(0xFFFFFFFFull)})
                          .SetType(ir::ValueType::U64);
    } else {
        element = __ ZeroExtend64(__ LoadMemory(ir::Operand{VexAddress(v)})
                                          .SetType(ir::ValueType::U32))
                          .SetType(ir::ValueType::U64);
    }

    // VecInsert16 is the IR's only element write; a 32-bit lane is two of them.
    auto result = XmmRead(XmmOf(v.vvvv));
    result = __ VecInsert16(result, element, ir::Imm(count_d * 2)).SetType(ir::ValueType::V128);
    result = __ VecInsert16(result,
                            __ LsrImm(element, ir::Imm(16u)).SetType(ir::ValueType::U64),
                            ir::Imm(count_d * 2 + 1))
                     .SetType(ir::ValueType::V128);

    if (zmask != 0) {
        u64 keep[2] = {~u64(0), ~u64(0)};
        for (u32 lane = 0; lane < 4; ++lane) {
            if (((zmask >> lane) & 1u) != 0) {
                keep[lane / 2] &= ~(0xFFFFFFFFull << ((lane % 2) * 32));
            }
        }
        result = __ VecAnd(result, VecConst(assembler, keep[0], keep[1]))
                         .SetType(ir::ValueType::V128);
    }
    VexWrite128(v.reg, result);
}

// vmaskmovps / vmaskmovpd -- VEX.NDS.128/256.66.0F38.W0 2C/2D (load),
// 2E/2F (store).  The register form of the r/m operand is #UD both ways.
//
//   load   dst = ModRM.reg, mask = VEX.vvvv, source = m128/m256.
//          dst[i] = mask[i].msb ? mem[i] : 0, and bits 255:128 zeroed at L=0.
//   store  destination = m128/m256, mask = VEX.vvvv, source = ModRM.reg.
//          mem[i] written only where mask[i].msb is set.
//
// NOTE ON VEX.vvvv: the mask is a real operand, so vvvv_valid must NOT be
// consulted.  A raw field of 1111b un-inverts to register 0, which for this
// instruction is a genuine xmm0/ymm0 mask -- the "no such operand" marker and
// `vmaskmovps xmm1, xmm0, [rdi]` are indistinguishable, and rejecting on
// vvvv_valid would kill the guest on the second one.
//
// WHY ONE BRANCH PER ELEMENT
// --------------------------
// This is the whole point of the instruction: a masked-off element must not
// fault.  The natural translation -- load all 16/32 bytes, then select -- is
// wrong precisely where vmaskmov is used, namely the tail of a vectorized loop
// where the elements past the end of the array sit on a page that may not be
// mapped.  So each element gets its own test-and-branch and issues no memory
// access when its mask bit is clear.  DecodeMaskmovdqu does the same, per byte,
// for MASKMOVDQU.
//
// The mask and (for a store) the source data are read into IR values BEFORE any
// branch, so they are ordinary SSA values live across the branches rather than
// re-reads of a register file the loop may already have written.  That is what
// makes `vmaskmovps ymm0, ymm0, [rdi]` -- destination aliasing the mask --
// come out right.
void X64Decoder::DecodeAvxMaskMov(const VexInsn& v, u32 lane_bits, bool store) {
    const u32 halves = v.Is256() ? 2u : 1u;
    const u32 lanes_per_half = 128u / lane_bits;
    const u32 qwords = halves * 2u;
    const auto element_type = GetSize(lane_bits);

    // The address is computed once; every element offsets it arithmetically,
    // so a RIP-relative or SIB operand is evaluated a single time.
    auto address = VexAddress(v);

    // Mask qwords, read up front (see the aliasing note above).
    ir::Value mask_q[4];
    for (u32 q = 0; q < qwords; ++q) {
        mask_q[q] = q < 2 ? (q == 0 ? XmmLo(XmmOf(v.vvvv)) : XmmHi(XmmOf(v.vvvv)))
                          : (q == 2 ? YmmHighLo(v.vvvv) : YmmHighHi(v.vvvv));
    }

    ir::Value source_q[4];
    if (store) {
        for (u32 q = 0; q < qwords; ++q) {
            source_q[q] = q < 2 ? (q == 0 ? XmmLo(XmmOf(v.reg)) : XmmHi(XmmOf(v.reg)))
                                : (q == 2 ? YmmHighLo(v.reg) : YmmHighHi(v.reg));
        }
    } else {
        // Zero the WHOLE 256-bit destination first.  For a masked-off element
        // that is the architectural result, and for VEX.128 it is contract C3
        // -- one step covering both.  Doing it before the loop also means the
        // per-element merge never has to preserve anything.
        auto zero = __ LoadImm(ir::Imm(u64(0))).SetType(ir::ValueType::U64);
        XmmLo(XmmOf(v.reg), zero);
        XmmHi(XmmOf(v.reg), zero);
        YmmHighLo(v.reg, zero);
        YmmHighHi(v.reg, zero);
    }

    for (u32 half = 0; half < halves; ++half) {
        for (u32 lane = 0; lane < lanes_per_half; ++lane) {
            const u32 bit_in_half = lane * lane_bits;
            const u32 q = half * 2u + bit_in_half / 64u;
            const u32 shift = bit_in_half % 64u;
            const s32 offset = static_cast<s32>(half * 16u + bit_in_half / 8u);

            auto enabled = __ TestBit(mask_q[q], ir::Imm(shift + lane_bits - 1));
            auto skip = __ NotGoto(enabled);
            if (store) {
                auto value = shift == 0
                                     ? source_q[q]
                                     : __ LsrImm(source_q[q], ir::Imm(shift))
                                               .SetType(ir::ValueType::U64);
                MemStore(ir::Operand{address, offset, ir::OperandPlus},
                         NarrowTo(value, element_type),
                         false);
            } else {
                auto value = MemLoad(ir::Operand{address, offset, ir::OperandPlus},
                                     element_type,
                                     false);
                if (lane_bits == 64) {
                    // The element IS the qword: a plain write, no merge.
                    if (q == 0) {
                        XmmLo(XmmOf(v.reg), value);
                    } else if (q == 1) {
                        XmmHi(XmmOf(v.reg), value);
                    } else if (q == 2) {
                        YmmHighLo(v.reg, value);
                    } else {
                        YmmHighHi(v.reg, value);
                    }
                } else {
                    // Two 32-bit elements share a qword, and each is written in
                    // its own conditional region, so the second has to merge
                    // with whatever the first left behind.
                    auto current = q == 0   ? XmmLo(XmmOf(v.reg))
                                   : q == 1 ? XmmHi(XmmOf(v.reg))
                                   : q == 2 ? YmmHighLo(v.reg)
                                            : YmmHighHi(v.reg);
                    auto merged = __ BitInsert(current,
                                               __ ZeroExtend64(value),
                                               ir::Imm(shift),
                                               ir::Imm(32u))
                                          .SetType(ir::ValueType::U64);
                    if (q == 0) {
                        XmmLo(XmmOf(v.reg), merged);
                    } else if (q == 1) {
                        XmmHi(XmmOf(v.reg), merged);
                    } else if (q == 2) {
                        YmmHighLo(v.reg, merged);
                    } else {
                        YmmHighHi(v.reg, merged);
                    }
                }
            }
            __ BindLabel(skip);
        }
    }
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
// Returns false for anything unmodelled, BEFORE emitting any IR, so a decline
// leaves the block exactly as it was and the caller can restore pc.  A false
// return traps the block as FALLBACK, which for a VEX instruction means the
// guest dies -- so every `return false` here is a claim that the encoding is
// architecturally invalid, not that it is merely inconvenient.
bool X64Decoder::DecodeAvxBlend(const VexInsn& v) {
    if (v.pp != VexPP::P66) {
        return false;  // every opcode in this file is 66-prefixed
    }
    switch (v.map) {
        case VexMap::Map0F3A:
            switch (v.opcode) {
                case 0x0C:  // vblendps
                    DecodeAvxIntBinary(v, OpBlendFpImm, Pack(32, v.imm8));
                    return true;
                case 0x0D:  // vblendpd
                    DecodeAvxIntBinary(v, OpBlendFpImm, Pack(64, v.imm8));
                    return true;
                case 0x17:  // vextractps
                    // VEX.128 only, no VEX.vvvv.  ModRM.reg is the XMM SOURCE
                    // and r/m the r32/m32 DESTINATION, which is the operand
                    // order DecodeAvxFpExtract already expects -- and a 32-bit
                    // element extract to r/m32 is bit-for-bit the same
                    // operation as vpextrd (0F3A 16 W0), so that handler is
                    // reused rather than duplicated.
                    if (v.Is256() || v.vvvv_valid) {
                        return false;
                    }
                    DecodeAvxFpExtract(v, 32);
                    return true;
                case 0x21:  // vinsertps
                    if (v.Is256()) {
                        return false;
                    }
                    DecodeAvxInsertPs(v);
                    return true;
                case 0x4A:  // vblendvps
                case 0x4B:  // vblendvpd
                    if (v.w) {
                        return false;  // W1 is a reserved encoding
                    }
                    DecodeAvxBlendVar(v, v.opcode == 0x4A ? 32u : 64u);
                    return true;
                default:
                    return false;
            }

        case VexMap::Map0F38:
            switch (v.opcode) {
                case 0x2C:  // vmaskmovps, load
                case 0x2D:  // vmaskmovpd, load
                case 0x2E:  // vmaskmovps, store
                case 0x2F:  // vmaskmovpd, store
                    // All four are W0, and all four are #UD with a register
                    // r/m operand -- there is no register form of a masked
                    // MEMORY access.
                    if (v.w || v.RmIsRegister()) {
                        return false;
                    }
                    DecodeAvxMaskMov(v, (v.opcode & 1u) != 0 ? 64u : 32u, v.opcode >= 0x2E);
                    return true;
                default:
                    return false;
            }

        case VexMap::Map0F:
        case VexMap::Invalid:
        default:
            return false;
    }
}

#undef __

}  // namespace swift::x86
