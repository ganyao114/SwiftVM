#pragma once

#include "aarch64/macro-assembler-aarch64.h"
#include "runtime/backend/context.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {

using namespace vixl::aarch64;

inline constexpr u64 kSseAFPGuestFPCRBase = (u64{1} << 1) | (u64{1} << 2);
inline constexpr s32 kSseAFPRuntimeFrameSize = 32;
inline constexpr s32 kSseAFPHostFPCROffset = 0;
inline constexpr s32 kSseAFPGuestFPCROffset = 8;
inline constexpr s32 kSseAFPSourceMXCSROffset = 16;

// Compatibility tags for the two existing translator_mem call sites. The
// retired FPCR-tax profiler no longer records them; keeping the tags here lets
// that emitter remain untouched while all generated counter code disappears.
enum class FpcrTaxCounter : u8 {
    CacheLookup,
    RebuildExecuted,
    CacheHit,
    StoreMxcsr,
    MemoryCopy,
};

// Build the complete guest FPCR from architectural MXCSR state.  Do not use
// the caller's FPCR as a base: DN/FZ/RMode and trap controls are host state.
// x86 RC encodes down/up as 01/10, while Arm FPCR encodes up/down as 01/10,
// so the two source bits deliberately cross on their way to RMode[23:22].
inline void EmitSseAFPGuestFPCRFromMXCSR(MacroAssembler& masm,
                                         const XRegister& result,
                                         const XRegister& mxcsr,
                                         const XRegister& bit) {
    masm.Mov(result, kSseAFPGuestFPCRBase);
    masm.Ubfx(bit, mxcsr, 6, 1);   // MXCSR.DAZ -> FPCR.FIZ[0]
    masm.Orr(result, result, bit);
    masm.Ubfx(bit, mxcsr, 15, 1);  // MXCSR.FTZ -> FPCR.FZ[24]
    masm.Orr(result, result, Operand(bit, LSL, 24));
    masm.Ubfx(bit, mxcsr, 13, 1);  // MXCSR.RC low -> FPCR.RMode high
    masm.Orr(result, result, Operand(bit, LSL, 23));
    masm.Ubfx(bit, mxcsr, 14, 1);  // MXCSR.RC high -> FPCR.RMode low
    masm.Orr(result, result, Operand(bit, LSL, 22));
}

inline void EmitSseAFPGuestFPCR(MacroAssembler& masm,
                                const XRegister& state_reg,
                                const XRegister& result,
                                const XRegister& mxcsr,
                                const XRegister& bit) {
    masm.Ldr(mxcsr.W(),
             MemOperand(state_reg,
                        state_offset_uniform_buffer +
                                offsetof(swift::x86::ThreadContext64, mxcsr)));
    EmitSseAFPGuestFPCRFromMXCSR(masm, result, mxcsr, bit);
}

// Restore guest FPCR after any host boundary. Every path compares the current
// architectural MXCSR with the source cached in this JitRun's stack frame;
// there are deliberately no helper-specific cleanliness exemptions.
inline void EmitSseAFPRestoreGuestFPCRCached(MacroAssembler& masm,
                                             const XRegister& state_reg,
                                             s32 frame_offset,
                                             const XRegister& result,
                                             const XRegister& mxcsr,
                                             const XRegister& bit) {
    Label cached;
    Label apply;
    masm.Ldp(result,
             bit,
             MemOperand(sp, frame_offset + kSseAFPGuestFPCROffset));
    masm.Ldr(mxcsr.W(),
             MemOperand(state_reg,
                        state_offset_uniform_buffer +
                                offsetof(swift::x86::ThreadContext64, mxcsr)));
    masm.Cmp(mxcsr.W(), bit.W());
    masm.B(&cached, eq);
    EmitSseAFPGuestFPCRFromMXCSR(masm, result, mxcsr, bit);
    masm.Stp(result,
             mxcsr,
             MemOperand(sp, frame_offset + kSseAFPGuestFPCROffset));
    masm.B(&apply);
    masm.Bind(&cached);
    masm.Bind(&apply);
    masm.Msr(FPCR, result);
}

template <typename IgnoredCounter>
inline void EmitSseAFPRestoreGuestFPCRCached(MacroAssembler& masm,
                                             const XRegister& state_reg,
                                             s32 frame_offset,
                                             const XRegister& result,
                                             const XRegister& mxcsr,
                                             const XRegister& bit,
                                             IgnoredCounter&&) {
    EmitSseAFPRestoreGuestFPCRCached(
            masm, state_reg, frame_offset, result, mxcsr, bit);
}

inline u64 ReadNativeFPCR() {
#if defined(__aarch64__)
    u64 value{};
    asm volatile("mrs %0, fpcr" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

inline void WriteNativeFPCR(u64 value) {
#if defined(__aarch64__)
    asm volatile("msr fpcr, %0" : : "r"(value) : "memory");
#else
    (void) value;
#endif
}

}  // namespace swift::runtime::backend::arm64
