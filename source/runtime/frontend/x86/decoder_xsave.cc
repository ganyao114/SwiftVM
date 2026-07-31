// XSAVE facility emitters and helpers: XGETBV, XSAVE/XSAVE64,
// XSAVEOPT/XSAVEOPT64, XSAVEC/XSAVEC64, and XRSTOR/XRSTOR64. decoder.cc
// dispatches the diStorm mnemonics (plus its raw XSAVEC predecode) here; the
// shared SVM_XSAVE gate keeps opcode availability coherent with CPUID.
//
// WHAT IS AND IS NOT IMPLEMENTED
// ---------------------------------------------------------------------------
// Implemented: XGETBV(ECX=0), CPUID.0xD subleaves 0/1/2 (in decoder_misc.cc),
// XSAVE, XSAVE64, XSAVEOPT, XSAVEOPT64, XSAVEC, XSAVEC64, XRSTOR, and
// XRSTOR64 -- including the RFBM/XSTATE_BV logic and the INIT-state semantics
// of XRSTOR.
//
// NOT implemented: XSAVES/XRSTORS and XSETBV. XSAVES/XRSTORS can execute only
// at CPL 0 (Intel SDM Vol. 1, Sections 13.11 and 13.12), while SwiftVM runs
// user-mode guests; CPUID.0xD.1:EAX[3] therefore remains clear and their
// opcodes remain #UD. The compacted format is deliberately not modelled:
// as an intentional SDM divergence, XSAVEC uses the standard layout and keeps
// XCOMP_BV zero.
//
// FAULTS.  This runtime has no #GP delivery path (InterruptReason has no
// general-protection reason and no signal is synthesised), so the SDM's #GP
// conditions are approximated:
//   * XGETBV with ECX != 0 exits to the host with InterruptReason::ILL_CODE.
//   * The XSAVE area's 64-byte alignment requirement is not enforced.
//   * XRSTOR does not fault on a non-zero reserved header byte, on XCOMP_BV
//     != 0, or on XSTATE_BV bits outside XCR0; those bits are masked off
//     instead.  Rosetta 2 does not fault on any of these either.
//   * An unmapped XSAVE area raises a guest page fault (PageFatal): the
//     helper validates the whole area up front and reports the refusal
//     through x86::kX87GuestFault, which EmitXsave turns into a block exit.
//     It does NOT partially save/restore first, so unlike hardware nothing
//     before the faulting byte is committed.
// The helpers write guest memory through the raw bias pointer (as X87Fxsave
// already does), so they bypass SMC tracking; an XSAVE area overlapping
// translated code will not invalidate it.

#include <array>
#include <cstring>
#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/x87.h"
#include "runtime/frontend/x86/xsave.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

// Guest -> host pointer for the XSAVE/XRSTOR helpers. Same contract as
// x87.cpp's GuestPointer: the window mask makes host memory unreachable, the
// embedder's mapping oracle turns an unmapped in-window address into nullptr
// so the caller skips the access instead of faulting in an unrecoverable
// host frame. Returns nullptr when [address, address+size) is not backed.
u8* GuestBytes(u64 address, size_t size) {
    return GuestPointer(address, size);
}

// x87 INIT configuration (SDM Table 9-1): the same state FNINIT establishes,
// plus zeroed data registers.  Routed through X87Dispatch rather than written
// field by field so any derived x87 bookkeeping stays consistent.
void InitX87State(u64 context, ThreadContext64& ctx) {
    X87Dispatch(context, MakeX87Command(X87Action::Init), 0);
    for (auto& reg : ctx.x87_regs) {
        reg = X87Reg{};
    }
}

void EmitUndefined(ir::Assembler* assembler, VAddr next_pc) {
    // Same shape as X64Decoder::Interrupt, which is a private member.
    ir::Uniform uni_interrupt{offsetof(ThreadContext64, interrupt), ir::ValueType::U32};
    __ SetLocation(ir::Lambda{ir::Imm{next_pc}});
    __ StoreUniform(uni_interrupt,
                    __ LoadImm(ir::Imm(static_cast<u32>(InterruptReason::ILL_CODE))));
    __ ReturnToHost();
}

ir::UniformEffectId XgetbvEffects() {
    static constexpr std::array ranges{
            ir::UniformEffectRange{offsetof(ThreadContext64, rax), sizeof(u64)},
            ir::UniformEffectRange{offsetof(ThreadContext64, rdx), sizeof(u64)},
            ir::UniformEffectRange{offsetof(ThreadContext64, interrupt),
                                   sizeof(InterruptReason)},
    };
    static constexpr ir::UniformEffectSet effects{ranges.data(), ranges.size()};
    static const auto id = ir::RegisterUniformEffectSet(&effects);
    return id;
}

// RFBM arrives in EDX:EAX.  Read the two dwords out of the guest context and
// splice them into one 64-bit value for the helper.
ir::Value EmitRequestedFeatureMask(ir::Assembler* assembler) {
    ir::Uniform uni_eax{offsetof(ThreadContext64, rax), ir::ValueType::U32};
    ir::Uniform uni_edx{offsetof(ThreadContext64, rdx), ir::ValueType::U32};
    auto low = __ ZeroExtend64(__ LoadUniform(uni_eax).SetType(ir::ValueType::U32));
    auto high = __ ZeroExtend64(__ LoadUniform(uni_edx).SetType(ir::ValueType::U32));
    return __ Or(__ LslImm(high, ir::Imm(32u)), ir::Operand{low}).SetType(ir::ValueType::U64);
}

}  // namespace

u64 XgetbvHelper(u64 context) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context);
    // ECX selects the extended control register.  Only XCR0 exists here:
    // XCR1 (ECX=1) requires CPUID.0xD.1:EAX[2], which is reported clear, so
    // every other index is a #GP.
    if (ctx.rcx.low.dword != 0) {
        ctx.interrupt = InterruptReason::ILL_CODE;
        return 1;
    }
    const u64 xcr0 = GuestXcr0();
    ctx.rax.qword = static_cast<u32>(xcr0);
    ctx.rdx.qword = static_cast<u32>(xcr0 >> 32);
    return 0;
}

u64 XsaveHelper(u64 context, u64 guest_address, u64 requested) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context);
    const u64 xcr0 = GuestXcr0();
    const u64 rfbm = requested & xcr0;
    u8* out = GuestBytes(guest_address, XsaveAreaSize());
    if (!out) return kX87GuestFault;  // unmapped area -> guest #PF

    u64 old_bv{};
    std::memcpy(&old_bv, out + kXsaveXstateBvOffset, sizeof(old_bv));

    if (rfbm & kXstateX87) {
        // X87Fxsave zeroes all 512 legacy bytes before filling in the x87
        // fields, but XSAVE owns only bytes 23:0 and 159:32 there.  The
        // SSE-owned bytes (24..31 MXCSR/MXCSR_MASK, 160..415 XMM) and the
        // software-available tail (416..511) must survive unless their
        // component was requested as well, so carry them across the call.
        u8 keep_mxcsr[8];
        u8 keep_sse[kXsaveLegacySize - kXsaveXmmOffset];
        std::memcpy(keep_mxcsr, out + kXsaveMxcsrOffset, sizeof(keep_mxcsr));
        std::memcpy(keep_sse, out + kXsaveXmmOffset, sizeof(keep_sse));
        X87Fxsave(context, guest_address);
        std::memcpy(out + kXsaveMxcsrOffset, keep_mxcsr, sizeof(keep_mxcsr));
        std::memcpy(out + kXsaveXmmOffset, keep_sse, sizeof(keep_sse));
    }
    // MXCSR and MXCSR_MASK are written when either the SSE or the AVX
    // component is requested: MXCSR is architecturally part of both.
    if (rfbm & (kXstateSse | kXstateYmm)) {
        const u32 mxcsr = ctx.mxcsr;
        const u32 mask = kMxcsrMask;
        std::memcpy(out + kXsaveMxcsrOffset, &mxcsr, sizeof(mxcsr));
        std::memcpy(out + kXsaveMxcsrMaskOffset, &mask, sizeof(mask));
    }
    if (rfbm & kXstateSse) {
        for (u32 i = 0; i < 16; ++i) {
            std::memcpy(out + kXsaveXmmOffset + i * 16, ctx.xmms[i].b, 16);
        }
    }
    if (rfbm & kXstateYmm) {
        for (u32 i = 0; i < 16; ++i) {
            std::memcpy(out + kXsaveYmmOffset + i * 16, ctx.ymm_high[i].b, 16);
        }
    }
    // SDM: XSTATE_BV[i] <- XINUSE[i] where RFBM[i] = 1, unchanged elsewhere.
    // XINUSE tracking is explicitly allowed to be conservative (a component
    // that is in its initial configuration may still report 1), so every
    // supported component is reported in use.  Real hardware behaves the same
    // way here: after `vzeroall`, XSAVE still sets XSTATE_BV[2].
    // Bits outside XCR0 are dropped from the preserved half so the area can
    // never end up in a state XRSTOR would have to reject.
    const u64 new_bv = (old_bv & ~rfbm & xcr0) | rfbm;
    std::memcpy(out + kXsaveXstateBvOffset, &new_bv, sizeof(new_bv));
    // Neither XCOMP_BV nor the 48 reserved header bytes are written: XSAVE
    // (as opposed to XSAVEC/XSAVES) leaves them alone.
    return 0;
}

u64 XsavecHelper(u64 context, u64 guest_address, u64 requested) {
    const u64 status = XsaveHelper(context, guest_address, requested);
    if (status != 0) return status;

    // The simplified XSAVEC contract uses the standard layout, so make the
    // format tag deterministic even when the guest buffer was pre-poisoned.
    u8* out = GuestBytes(guest_address, XsaveAreaSize());
    const u64 xcomp_bv = 0;
    std::memcpy(out + kXsaveXcompBvOffset, &xcomp_bv, sizeof(xcomp_bv));
    return 0;
}

u64 XrstorHelper(u64 context, u64 guest_address, u64 requested) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context);
    const u64 xcr0 = GuestXcr0();
    const u64 rfbm = requested & xcr0;
    const u8* in = GuestBytes(guest_address, XsaveAreaSize());
    if (!in) return kX87GuestFault;  // unmapped area -> guest #PF

    u64 state_bv{};
    std::memcpy(&state_bv, in + kXsaveXstateBvOffset, sizeof(state_bv));
    state_bv &= xcr0;  // SDM would #GP on bits outside XCR0; see the note above.

    // For every requested component: load it when XSTATE_BV says the area
    // holds it, otherwise put it in its INIT configuration.  Components with
    // RFBM[i] = 0 are left completely untouched.
    if (rfbm & kXstateX87) {
        if (state_bv & kXstateX87) {
            X87Fxrstor(context, guest_address);
        } else {
            InitX87State(context, ctx);
        }
    }
    if (rfbm & kXstateSse) {
        for (u32 i = 0; i < 16; ++i) {
            if (state_bv & kXstateSse) {
                std::memcpy(ctx.xmms[i].b, in + kXsaveXmmOffset + i * 16, 16);
            } else {
                std::memset(ctx.xmms[i].b, 0, 16);
            }
        }
    }
    // MXCSR lives in the legacy region and is reloaded whenever the SSE or AVX
    // component is requested, independent of XSTATE_BV.  Confirmed on
    // hardware: XSTATE_BV = 0 with RFBM = 7 still takes MXCSR from memory.
    if (rfbm & (kXstateSse | kXstateYmm)) {
        u32 mxcsr{};
        std::memcpy(&mxcsr, in + kXsaveMxcsrOffset, sizeof(mxcsr));
        // SDM: #GP if any bit outside MXCSR_MASK is set. Mask instead of
        // faulting, consistent with the rest of this file.
        ctx.mxcsr = mxcsr & kMxcsrMask;
    }
    if (rfbm & kXstateYmm) {
        for (u32 i = 0; i < 16; ++i) {
            if (state_bv & kXstateYmm) {
                std::memcpy(ctx.ymm_high[i].b, in + kXsaveYmmOffset + i * 16, 16);
            } else {
                std::memset(ctx.ymm_high[i].b, 0, 16);
            }
        }
    }
    return 0;
}

void EmitXgetbv(ir::Assembler* assembler, VAddr next_pc) {
    // CR4.OSXSAVE is not modelled; the OSXSAVE CPUID bit stands in for it.
    // With the facility hidden, XGETBV is architecturally unavailable.
    if (!XsaveEnabled()) {
        EmitUndefined(assembler, next_pc);
        return;
    }
    auto context = __ GetUniformAddress(ir::Imm(0)).SetType(ir::ValueType::U64);
    auto faulted =
            __ CallHostWithUniformEffects(XgetbvEffects(), &XgetbvHelper, context);
    // ECX != 0 is a #GP.  The helper has already recorded the reason, so the
    // block terminates back to the host on that path and links to the next
    // instruction on the normal one.  Making the exit a terminal (instead of
    // just setting the field) is what actually stops execution.
    __ SetLocation(ir::Lambda{ir::Imm{next_pc}});
    __ If(ir::terminal::If{__ TestNotZero(faulted),
                           ir::terminal::ReturnToHost{},
                           ir::terminal::LinkBlock{next_pc}});
}

void EmitXsave(ir::Assembler* assembler,
               ir::Value address,
               VAddr next_pc,
               VAddr insn_pc,
               bool restore) {
    if (!XsaveEnabled()) {
        EmitUndefined(assembler, next_pc);
        return;
    }
    auto context = __ GetUniformAddress(ir::Imm(0)).SetType(ir::ValueType::U64);
    auto rfbm = EmitRequestedFeatureMask(assembler);
    // XSAVE64/XRSTOR64 differ from the 32-bit forms only in how FIP/FDP are
    // laid out in the legacy region; X87Fxsave already writes the 64-bit form
    // for both FXSAVE and FXSAVE64, so the two share one handler here too.
    ir::Value status;
    if (restore) {
        status = __ CallHost(&XrstorHelper, context, address, rfbm)
                         .SetType(ir::ValueType::U64);
    } else {
        status = __ CallHostUniformPure(&XsaveHelper, context, address, rfbm)
                         .SetType(ir::ValueType::U64);
    }
    // An unmapped XSAVE area is a guest #PF, not a no-op. CheckMemoryAlignment
    // is the runtime's generic "exit the block with PageFatal if (v & mask)"
    // primitive; see decoder_x87.cc's RaiseIfGuestFault.
    __ SetLocation(ir::Lambda{ir::Imm{insn_pc}});
    __ CheckMemoryAlignment(status, ir::Imm(kX87GuestFault));
}

void EmitXsaveopt(ir::Assembler* assembler,
                  ir::Value address,
                  VAddr next_pc,
                  VAddr insn_pc) {
    // XSAVEOPT may omit components in INIT state, but it is also conforming
    // to write every requested component. Reuse the conservative XSAVE path.
    EmitXsave(assembler, address, next_pc, insn_pc, false);
}

void EmitXsavec(ir::Assembler* assembler,
                ir::Value address,
                VAddr next_pc,
                VAddr insn_pc) {
    // SwiftVM does not expose the compacted layout: XCOMP_BV stays zero, so
    // component offsets are the standard-format offsets and save semantics
    // are identical to XSAVE.
    if (!XsaveEnabled()) {
        EmitUndefined(assembler, next_pc);
        return;
    }
    auto context = __ GetUniformAddress(ir::Imm(0)).SetType(ir::ValueType::U64);
    auto rfbm = EmitRequestedFeatureMask(assembler);
    auto status = __ CallHostUniformPure(&XsavecHelper, context, address, rfbm)
                          .SetType(ir::ValueType::U64);
    __ SetLocation(ir::Lambda{ir::Imm{insn_pc}});
    __ CheckMemoryAlignment(status, ir::Imm(kX87GuestFault));
}

}  // namespace swift::x86
