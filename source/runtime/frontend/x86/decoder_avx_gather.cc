//
// AVX2 gather (VSIB) handlers -- vpgatherdd/qd/dq/qq and vgatherdps/qps/dpd/qpd,
// both VEX.128 and VEX.256.
//
// ---------------------------------------------------------------------------
// WHY THERE IS NO NEW IR OPCODE HERE
// ---------------------------------------------------------------------------
// The obvious shape for a gather is one IR instruction, and the obvious way to
// implement that instruction is a host helper reached through CallLambda (what
// decoder_bmi.cc does for PDEP/PEXT).  Both were rejected, for reasons that are
// about this runtime's memory model rather than about taste:
//
//   * A host helper does its loads with a raw host pointer.  In the JIT the
//     faulting host PC would then be inside the helper, AddressSpace::
//     LookupFault would not find it in any JIT code buffer, and a wild guest
//     pointer would kill the HOST process instead of raising PageFatal
//     (Runtime::Impl::HandleFault).  In the interpreter it would bypass
//     guest_addr_limit and interp_range_check entirely -- RunLoadMemory's
//     wild-pointer guard exists precisely because the interpreter has no
//     signal handler.  Trading a guest-visible fault for a host crash is a
//     strictly worse deviation than anything the plain-IR route costs.
//   * An IR opcode that carried gather's x86-specific baggage -- clearing the
//     mask register as a side effect, VSIB's index-register-inside-the-SIB-byte
//     encoding, the partial-completion fault model -- would be an x86
//     instruction wearing an IR name.  This IR serves ARM64 / x86 / Slang
//     front ends and ARM64 / RISC-V64 back ends; the mask-clear in particular
//     is an architectural quirk of x86's gather and belongs in the frontend,
//     which is where it is done below (an ordinary write of zero).
//
// So gather is built from the IR that already exists, in exactly the shape
// DecodeMaskmovdqu (decoder_sse.cc) already uses for the mirror-image
// instruction: per element, TestNotZero on the mask bit, NotGoto, the memory
// access, BindLabel.  MASKMOVDQU's comment states the reason in one line --
// "masked-off bytes must not introduce memory faults" -- and for gather that
// property is not a nicety, it is the whole ballgame (see MEASURED SEMANTICS).
//
// Every load therefore goes through MemLoad, which selects LoadMemoryTSO in
// AcqRel mode and otherwise keeps the existing LoadMemory IR. The JIT fault
// table and interpreter range check work unchanged, with no gather-specific
// backend path.
//
// ---------------------------------------------------------------------------
// MEASURED SEMANTICS (Rosetta, arch -x86_64, macOS 27 -- not assumed)
// ---------------------------------------------------------------------------
// 1. The mask is the MOST SIGNIFICANT BIT of each mask element and nothing
//    else: 0x7FFFFFFF and 0x40000000 read as off, 0xFFFFFFFF and 0xC0000000 as
//    on.  Mask element size is the DESTINATION element size, not the index
//    size -- vpgatherqd's mask lanes are dwords 0..N-1, which is easy to get
//    backwards and was checked directly.
// 2. A masked-off element leaves the destination element UNCHANGED (not
//    zeroed) and performs NO memory access.  Measured with wild indices
//    (0x20000000 * 4 = +2 GiB, far outside the mapping) in the masked-off
//    lanes: the instruction completed without a fault and the enabled lanes
//    were gathered correctly.  With the mask all zero the destination came
//    back byte-identical and again no fault.
// 3. The mask register is ZEROED by the instruction -- all 256 bits, including
//    the upper half for a VEX.128 form.  Loops depend on this.
// 4. Indices are SIGNED: index -1 with scale 4 read base-4.
// 5. Duplicate indices are fine (two lanes read the same address).
// 6. Destination bits above N*element_bits are zeroed: vpgatherqd xmm writes
//    dst[63:0] and zeroes dst[255:64]; every VEX.128 form zeroes dst[255:128].
// 7. vpgatherdq/vgatherdpd take their dword indices from the LOW half of the
//    index register even at VEX.256 (the operand is vm32x, an XMM).
//
// ---------------------------------------------------------------------------
// KNOWN DEVIATION: THE FAULT MODEL (read this before trusting a faulting gather)
// ---------------------------------------------------------------------------
// Hardware makes a faulting gather RESTARTABLE: the elements completed before
// the faulting one are written, THEIR mask bits are cleared, the remaining
// mask bits survive, and the same instruction re-executes after the handler
// returns.  SwiftVM cannot express that, and not because of gather: NO memory
// access in this runtime is restartable.  Runtime::Impl::HandleFault rewinds
// the interrupted context to the trampoline's return-host label and reports
// HaltReason::PageFatal -- its own comment says "the faulting instruction is
// never re-executed" -- and there is no guest signal delivery, so a faulting
// access ends the guest.  A faulting gather therefore behaves exactly like a
// faulting `mov`: fatal, loudly.
//
// MEASURED, not argued: a `vpgatherdd ymm` with an unmapped ENABLED lane and a
// `mov eax, [rsi]` with the same unmapped address were run through this
// runtime side by side.  Both exit the JIT with ExitReason::PageFatal (2), and
// both die identically in the interpreter.  Gather is not a special case of
// the fault model; it is the same case.
//
// What that costs, precisely:
//   * The architectural register state at the moment of the fault differs.
//     Here the destination holds the elements completed so far (they are
//     written as the loop goes) but the mask register still holds its ORIGINAL
//     value, because the mask is zeroed after the last element.  On hardware
//     the completed prefix of the mask would be cleared.  This is observable
//     only to something that inspects the guest after a fatal fault -- a
//     debugger or a core dump -- because the guest cannot run again.
//   * It does NOT cost a wrong answer on any non-faulting gather, and it does
//     NOT introduce faults hardware would not take: point 2 above is the
//     property that matters for real code (a masked tail loop routinely leaves
//     garbage in the masked-off index lanes), and the per-element branch
//     preserves it exactly.  avx_gather_test.cpp asserts it directly.
//
// ---------------------------------------------------------------------------
// KNOWN DEVIATION: #UD SHAPES ARE DECLINED, NOT RAISED
// ---------------------------------------------------------------------------
// The SDM makes it #UD if any two of {destination, index, mask} name the same
// register, and #UD if the r/m operand is a register (VSIB requires a memory
// operand with a SIB byte).  Those forms return false here, which traps the
// block as FALLBACK and ends the guest -- the same outcome an unhandled #UD
// has, reached by a different route.  Declining is the honest answer: the
// alternative is to invent a result for an encoding that has none.
//
// ---------------------------------------------------------------------------
// VSIB AND THE SHARED VEX DECODER (no change needed there -- verified)
// ---------------------------------------------------------------------------
// vex_decoder.cc parses the ModRM/SIB of a VSIB form as an ordinary SIB.  All
// 22 addressing shapes an assembler emits for this family were decoded and
// compared against llvm-objdump: length, mod/disp8/disp32 selection, the
// base=rbp/r13 forced-disp8 case, the base=r12 case, the no-base
// (mod=00, SIB.base=101, disp32) case, VEX.X for index >= 8, VEX.B, VEX.W,
// VEX.L and vvvv all come back correct.  ONE encoding needs interpretation
// rather than a fix:
//
//     SIB.index == 100b with VEX.X == 0 is "no index register" under a plain
//     SIB, and vex_decoder.cc reports index_none = true for it.  Under VSIB
//     there is no such thing as "no index" -- that encoding names vector
//     register 4.  The raw bits are recoverable without ambiguity, since
//     index_none can ONLY be set when index_field == 4 and VEX.X == 0, i.e.
//     index register (4 | 0<<3) == 4, so the handler below simply reads
//     index_none as "the index is register 4".
//
// That is a latent trap for any future VSIB user (an FMA scatter, AVX-512),
// not a defect for this one.  If the main line ever wants it removed, the
// place is vex_decoder.cc's SIB block, which would need to know that the
// opcode is a VSIB form; it is deliberately not changed from here.
//
// ---------------------------------------------------------------------------
// INTEGRATION (decoder.h / decoder.cc / CMakeLists.txt are the main line's)
// ---------------------------------------------------------------------------
// Add to the private section of X64Decoder in decoder.h, next to the
// DecodeAvxInt block:
//
//     bool DecodeAvxGather(const VexInsn& v);
//     void DecodeAvxGatherOp(const VexInsn& v, u32 element_bits, u32 index_bits,
//                            u32 index_reg);
//     ir::Value GatherSlotRead(u32 reg, u32 slot);
//     void GatherSlotWrite(u32 reg, u32 slot, ir::Value value);
//
// Add the handler to the chain of AVX handlers in the VEX dispatch in
// Decode().  At the time of writing that chain reads
//
//     (DecodeAvxMul(vex) || DecodeAvxInt(vex) || DecodeAvxFp(vex) ||
//      DecodeAvxHadd(vex) || DecodeAvxBlend(vex))
//
// and it is enough to prepend `DecodeAvxGather(vex) || `.  Position in the
// chain does not matter: 66.0F38 90..93 is claimed by nothing else, and every
// false return below happens BEFORE any IR is emitted, so a decline never
// leaves a half-built block behind.
//
// Add decoder_avx_gather.cc to source/runtime/frontend/x86/CMakeLists.txt and
// fuzz/avx_gather_test.cpp to source/tests/CMakeLists.txt.
//
// `pc` must already point PAST the instruction when the handler runs; the
// existing VEX dispatch guarantees that.
//

#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/vex_decoder.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

// VexInsn register numbers are architectural (0..15); distorm's enum uses the
// same order within each block.  Local copy: decoder_avx_fp.cc's identical
// helper lives in that file's anonymous namespace.
_RegisterType GatherGprOf(u32 index, bool is_64bit) {
    return static_cast<_RegisterType>((is_64bit ? R_RAX : R_EAX) + index);
}

}  // namespace

// A 256-bit vector register as four independent 64-bit slots (contract C1: a
// YMM is never one IR value).  Slots 0/1 are the XMM half, 2/3 the ymm_high
// half.  Gather needs this granularity because its destination is written one
// ELEMENT at a time under a branch, so no whole-register value ever exists.
ir::Value X64Decoder::GatherSlotRead(u32 reg, u32 slot) {
    switch (slot) {
        case 0: return XmmLo(XmmOf(reg));
        case 1: return XmmHi(XmmOf(reg));
        case 2: return YmmHighLo(reg);
        default: return YmmHighHi(reg);
    }
}

void X64Decoder::GatherSlotWrite(u32 reg, u32 slot, ir::Value value) {
    switch (slot) {
        case 0: XmmLo(XmmOf(reg), value); break;
        case 1: XmmHi(XmmOf(reg), value); break;
        case 2: YmmHighLo(reg, value); break;
        default: YmmHighHi(reg, value); break;
    }
}

// The whole family, parameterized by the two element widths.  N, the number of
// gathered elements, is VL / max(element_bits, index_bits) -- the wider of the
// two is what runs out of register first:
//
//   opcode 90 W0 dd   32/32   VEX.128 -> 4   VEX.256 -> 8
//   opcode 91 W0 qd   32/64   VEX.128 -> 2   VEX.256 -> 4   (dst is an XMM)
//   opcode 90 W1 dq   64/32   VEX.128 -> 2   VEX.256 -> 4   (index is an XMM)
//   opcode 91 W1 qq   64/64   VEX.128 -> 2   VEX.256 -> 4
//
// 92/93 (vgather*ps/pd) move the same bits as 90/91; nothing here is
// float-aware, because a gather does no arithmetic.
void X64Decoder::DecodeAvxGatherOp(const VexInsn& v, u32 element_bits, u32 index_bits,
                                   u32 index_reg) {
    const u32 vl = v.Is256() ? 256u : 128u;
    const u32 n = vl / (element_bits > index_bits ? element_bits : index_bits);
    const u32 dst = v.reg;
    const u32 mask = v.vvvv;
    const auto addr_type = is_64bit ? ir::ValueType::U64 : ir::ValueType::U32;
    const auto load_type = element_bits == 32 ? ir::ValueType::U32 : ir::ValueType::U64;

    // Read every source slot ONCE, before the destination is touched.  #UD
    // guarantees dst is neither the index nor the mask, so nothing below can
    // invalidate these -- but they must still be materialized up front, since
    // the destination writes are interleaved with the reads in program order.
    const u32 index_slots = (n * index_bits + 63u) / 64u;
    const u32 mask_slots = (n * element_bits + 63u) / 64u;
    ir::Value index_slot[4];
    ir::Value mask_slot[4];
    for (u32 s = 0; s < index_slots; ++s) {
        index_slot[s] = GatherSlotRead(index_reg, s);
    }
    for (u32 s = 0; s < mask_slots; ++s) {
        mask_slot[s] = GatherSlotRead(mask, s);
    }
    ir::Value base;
    const bool have_base = !v.base_none;
    if (have_base) {
        base = R(GatherGprOf(v.base, is_64bit));
    }

    // Destination slots above the gathered elements are zeroed.  This covers
    // contract C3 (a VEX.128 form zeroes bits 255:128) AND the narrower cases
    // measured above -- vpgatherqd VEX.128 writes 8 bytes and zeroes 24 -- so
    // ZeroYmmHigh is not called separately.  Done before the element loop: the
    // two ranges are disjoint, and this keeps the zeroing out of the branchy
    // region where the uniform cache is invalidated anyway.
    const u32 used_slots = n * element_bits / 64u;
    if (used_slots < 4) {
        auto zero = __ LoadImm(ir::Imm(u64(0)));
        for (u32 s = used_slots; s < 4; ++s) {
            GatherSlotWrite(dst, s, zero);
        }
    }

    const u32 scale_shift = v.scale == 8 ? 3u : v.scale == 4 ? 2u : v.scale == 2 ? 1u : 0u;
    const auto disp = static_cast<s64>(v.displacement);

    for (u32 i = 0; i < n; ++i) {
        // ---- mask bit: the msb of mask element i, nothing else -------------
        const u32 m_slot = (i * element_bits) / 64u;
        const u32 m_shift = (i * element_bits) % 64u;
        const u64 m_bit = u64(1) << (m_shift + element_bits - 1u);
        auto enabled = __ TestNotZero(
                __ And(mask_slot[m_slot], ir::Operand{ir::Imm(m_bit)}).SetType(ir::ValueType::U64));
        auto skip = __ NotGoto(enabled);

        // ---- address: base + SignExtend(index[i]) * scale + disp -----------
        // Everything from here to BindLabel is skipped when the element is
        // masked off, which is what keeps a garbage index in a masked-off lane
        // from faulting.
        ir::Value offset;
        if (index_bits == 64) {
            offset = index_slot[i];
        } else {
            // Sign-extend dword i out of its slot.  Shift-left-then-arithmetic-
            // shift-right rather than SignExtend(value.SetType(U32)): SetType
            // rewrites the DEFINING instruction's return type (ir::Value::
            // SetType calls Def()->SetReturn), so retyping a shared LoadUniform
            // would corrupt the other element that reads the same slot.
            const u32 i_slot = (i * 32u) / 64u;
            const bool high = (i % 2u) != 0;
            auto wide = high ? index_slot[i_slot]
                             : __ LslImm(index_slot[i_slot], ir::Imm(32u))
                                       .SetType(ir::ValueType::U64);
            offset = __ AsrImm(wide, ir::Imm(32u)).SetType(ir::ValueType::U64);
        }
        ir::Value scaled = scale_shift == 0
                                   ? offset
                                   : __ LslImm(offset, ir::Imm(scale_shift))
                                             .SetType(ir::ValueType::U64);
        ir::Value addr = have_base ? __ Add(base, ir::Operand{scaled}).SetType(addr_type) : scaled;
        if (disp != 0) {
            addr = __ Add(addr, ir::Operand{ir::Imm(static_cast<u64>(disp))}).SetType(addr_type);
        }
        if (!is_64bit) {
            // 32-bit addressing: the effective address wraps at 4 GiB.  Also
            // normalizes the type when the address is a bare scaled index.
            addr = __ And(addr, ir::Operand{ir::Imm(static_cast<u64>(addr_mask))})
                           .SetType(addr_type);
        }

        auto loaded = MemLoad(ir::Operand{addr}, load_type, VexTsoOrdered(v));

        // ---- merge into the destination element ---------------------------
        // The destination slot is re-read HERE rather than hoisted: an earlier
        // element may or may not have written it, and UniformEliminationPass
        // treats Goto / NotGoto / BindLabel as cache barriers precisely so a
        // load inside a branchy region is not folded to a value stored on some
        // other path.
        const u32 d_slot = (i * element_bits) / 64u;
        if (element_bits == 64) {
            GatherSlotWrite(dst, d_slot, loaded);
        } else {
            const u32 d_shift = (i * element_bits) % 64u;
            auto current = GatherSlotRead(dst, d_slot);
            GatherSlotWrite(dst, d_slot,
                            __ BitInsert(current, loaded, ir::Imm(d_shift), ir::Imm(32u))
                                    .SetType(ir::ValueType::U64));
        }
        __ BindLabel(skip);
    }

    // ---- the mask register is zeroed, all 256 bits --------------------------
    // Architecturally this is a per-element clear that happens whether or not
    // the element was enabled; zeroing the whole register at the end is the
    // same thing for any gather that completes, which is every gather that
    // does not end the guest (see the fault-model note above).  Kept in the
    // frontend deliberately: "clear the mask register" is an x86 quirk and has
    // no business inside a shared IR opcode.
    {
        auto zero = __ LoadImm(ir::Imm(u64(0)));
        for (u32 s = 0; s < 4; ++s) {
            GatherSlotWrite(mask, s, zero);
        }
    }
}

// Entry point.  Returns false -- before emitting ANY IR -- for everything not
// modelled, which traps the block as FALLBACK.
bool X64Decoder::DecodeAvxGather(const VexInsn& v) {
    if (!AvxEnabled() || !v.valid) {
        return false;
    }
    if (v.map != VexMap::Map0F38 || v.pp != VexPP::P66) {
        return false;
    }
    switch (v.opcode) {
        case 0x90:  // vpgatherdd (W0) / vpgatherdq (W1)
        case 0x91:  // vpgatherqd (W0) / vpgatherqq (W1)
        case 0x92:  // vgatherdps (W0) / vgatherdpd (W1)
        case 0x93:  // vgatherqps (W0) / vgatherqpd (W1)
            break;
        default:
            return false;
    }
    // VSIB: the r/m operand MUST be memory with a SIB byte.  mod == 11 is #UD,
    // and so is a non-SIB memory form (there is no way to encode the vector
    // index without the SIB byte).  RIP-relative cannot occur -- it needs
    // ModRM.rm == 101b, not 100b -- but is rejected rather than assumed away.
    if (v.RmIsRegister() || !v.has_sib || v.rip_relative) {
        return false;
    }
    // The mask operand lives in VEX.vvvv; the "no operand" encoding (1111b)
    // has no meaning for this family.
    if (!v.vvvv_valid) {
        return false;
    }
    // SIB.index == 100b with VEX.X == 0 is "no index" for a plain SIB and is
    // reported as index_none; under VSIB it names vector register 4.  See the
    // header note -- the reconstruction is exact, not a guess.
    const u32 index_reg = v.index_none ? 4u : v.index;
    const u32 element_bits = v.w ? 64u : 32u;
    const u32 index_bits = (v.opcode & 1u) ? 64u : 32u;
    // #UD when any two of destination / index / mask are the same register.
    if (v.reg == index_reg || v.reg == v.vvvv || index_reg == v.vvvv) {
        return false;
    }
    DecodeAvxGatherOp(v, element_bits, index_bits, index_reg);
    return true;
}

}  // namespace swift::x86
