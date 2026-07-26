#pragma once

#include <cstdlib>
#include <cstring>
#include "runtime/frontend/x86/decoder.h"

namespace swift::x86 {

// ---------------------------------------------------------------------------
// XSAVE state components and XSAVE-area layout (Intel SDM Vol. 1 ch. 13).
//
// Every number below was cross-checked against real x86-64 hardware through
// Rosetta 2; the probes and their results are described in
// source/tests/fuzz/xsave_test.cpp.  Hardware reports
//   CPUID.0xD.0 -> EAX=0x7 EBX=832 ECX=832 EDX=0
//   CPUID.0xD.2 -> EAX=256 (size) EBX=576 (offset)
// and XSAVE(rfbm=7) writes exactly [0..415] (legacy) + [512..519] (XSTATE_BV)
// + [576..831] (YMM_Hi128), leaving XCOMP_BV and the rest of the header alone.
//
// The CPUID.0xD values DecodeCpuid reports are DERIVED from these constants,
// so the advertised area size and the bytes XSAVE actually writes cannot drift
// apart -- a guest that sizes its buffer from CPUID would otherwise overrun it.
// ---------------------------------------------------------------------------
constexpr u64 kXstateX87 = 1ull << 0;
constexpr u64 kXstateSse = 1ull << 1;
constexpr u64 kXstateYmm = 1ull << 2;

// Legacy (FXSAVE) region.  SDM 13.4.1 splits it between two components:
// x87 owns bytes 23:0 and 159:32, SSE owns 31:24 (MXCSR + MXCSR_MASK) and
// 415:160 (XMM0-15).  Bytes 511:416 are software-available and XSAVE never
// writes them.
constexpr u32 kXsaveLegacySize = 512;
constexpr u32 kXsaveMxcsrOffset = 24;
constexpr u32 kXsaveMxcsrMaskOffset = 28;
constexpr u32 kXsaveXmmOffset = 160;
constexpr u32 kXsaveXmmSize = 256;
// XSAVE header: XSTATE_BV at +0, XCOMP_BV at +8, 48 reserved bytes.  XSAVE
// writes the XSTATE_BV qword and nothing else in it.
constexpr u32 kXsaveHeaderOffset = 512;
constexpr u32 kXsaveHeaderSize = 64;
constexpr u32 kXsaveXstateBvOffset = 512;
// State component 2, YMM_Hi128: bits 255:128 of the sixteen YMM registers.
constexpr u32 kXsaveYmmOffset = 576;
constexpr u32 kXsaveYmmSize = 256;

// MXCSR_MASK reported in the legacy area; matches the value X87Fxsave writes
// for FXSAVE, so both instructions stay consistent.
constexpr u32 kMxcsrMask = 0x0000FFFF;

// Architectural defaults used when XRSTOR takes a component to its INIT
// configuration (SDM 13.8 / Table 9-1).
constexpr u16 kX87InitFcw = 0x037F;
constexpr u16 kX87InitFtw = 0xFFFF;  // full tag word: every slot empty

// ---------------------------------------------------------------------------
// Runtime configuration.
//
// Both queries hit getenv every time instead of caching in a function-local
// static.  They are only consulted while DECODING cpuid / xgetbv / xsave --
// a handful of times per translated block -- and the differential test has to
// flip them between sections; a cached static would freeze whichever value the
// first translation in the process happened to observe.
// ---------------------------------------------------------------------------
inline bool XsaveEnvOn(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "0") != 0;
}

// Master switch for the XSAVE facility: CPUID.1:ECX.XSAVE[26]/OSXSAVE[27],
// CPUID leaf 0xD, and the XGETBV / XSAVE / XRSTOR opcodes.  Default OFF, which
// reproduces the pre-existing behaviour exactly (feature bits clear, XGETBV
// #UD, XSAVE/XRSTOR unhandled).  Architecturally OSXSAVE is CR4.OSXSAVE, and
// XGETBV is #UD when it is clear -- so gating the opcodes on the same switch
// that advertises the bit keeps CPUID and behaviour coherent in both settings.
inline bool XsaveEnabled() { return XsaveEnvOn("SVM_XSAVE"); }

// XCR0 as the guest observes it through XGETBV and CPUID.0xD.  x87 and SSE are
// always set (bit 0 is architecturally always 1 and SSE state is implemented);
// the YMM bit follows the AVX decoder switch, because a guest that sees
// XCR0.YMM will emit VEX and both must therefore agree.  XSETBV is not
// implemented, so XCR0 is fixed and equals the supported bitmap.
//
// SVM_XSAVE_YMM overrides that default when set.  It exists because
// X64Decoder::AvxEnabled() caches SVM_AVX in a function-local static on first
// use: a test that sets SVM_AVX to reach the YMM component would leave the
// AVX decoder enabled for the whole process even after restoring the
// variable.  The XSAVE differential test therefore drives this bit directly
// and never touches SVM_AVX.
inline u64 GuestXcr0() {
    const char* ymm = std::getenv("SVM_XSAVE_YMM");
    const bool with_ymm = ymm ? std::strcmp(ymm, "0") != 0 : XsaveEnvOn("SVM_AVX");
    return kXstateX87 | kXstateSse | (with_ymm ? kXstateYmm : 0);
}

// Bytes required by the components enabled in XCR0: CPUID.0xD.0:EBX and ECX.
inline u32 XsaveAreaSize() {
    return (GuestXcr0() & kXstateYmm) ? (kXsaveYmmOffset + kXsaveYmmSize)
                                      : (kXsaveHeaderOffset + kXsaveHeaderSize);
}

// ---------------------------------------------------------------------------
// Host helpers (defined in decoder_xsave.cc).  They take the guest context and
// a guest address; RFBM arrives as an explicit argument because XSAVE/XRSTOR
// take it in EDX:EAX and the decoder assembles it from the IR register values.
// ---------------------------------------------------------------------------
u64 XsaveHelper(u64 context, u64 guest_address, u64 rfbm);
u64 XrstorHelper(u64 context, u64 guest_address, u64 rfbm);
u64 XgetbvHelper(u64 context);

// ---------------------------------------------------------------------------
// Emitters.  Deliberately free functions taking the assembler rather than
// X64Decoder members, so that adding the XSAVE family needs no new declaration
// in the shared decoder.h.  `next_pc` is the decoder's `pc`, which at handler
// time already points past the instruction.
// ---------------------------------------------------------------------------
void EmitXgetbv(ir::Assembler* assembler, VAddr next_pc);
void EmitXsave(ir::Assembler* assembler, ir::Value address, VAddr next_pc, bool restore);

}  // namespace swift::x86
