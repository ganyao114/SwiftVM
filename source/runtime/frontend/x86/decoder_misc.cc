#include <array>
#include <chrono>
#include <random>
#include <time.h>
#include "runtime/frontend/x86/decoder_internal.h"
#include "runtime/frontend/x86/xsave.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

u64 ReadVirtualTsc() {
#if defined(__APPLE__)
    // Darwin does not guarantee EL0 access to CNTVCT_EL0. Use its supported
    // monotonic raw clock and expose a stable virtual 1 GHz TSC.
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return u64(ts.tv_sec) * 1'000'000'000ull + u64(ts.tv_nsec);
#elif defined(__aarch64__)
    u64 value, frequency;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    // Normalize the architectural counter to the same virtual 1 GHz frequency
    // exposed through CPUID.15H.
    return static_cast<u64>(
            (static_cast<unsigned __int128>(value) * 1'000'000'000ull) / frequency);
#else
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
#endif
}

u64 HostRandom() {
    // A host helper keeps the nondeterministic read in both the JIT and
    // interpreter execution paths.  random_device is backed by the platform
    // entropy provider on the supported macOS host.
    static thread_local std::random_device random;
    return (u64(random()) << 32) | u64(random());
}

void ReadGuestTsc(ThreadContext64* context) {
    const u64 tsc = ReadVirtualTsc();
    context->rax.low.dword = u32(tsc);
    context->rdx.low.dword = u32(tsc >> 32);
}

void GuestRandom(u64* destination, u64 width) {
    const u64 value = HostRandom();
    if (width == 16) {
        *destination = (*destination & ~u64(0xFFFF)) | u16(value);
    } else if (width == 32) {
        *destination = u32(value);
    } else {
        *destination = value;
    }
}

}  // namespace

void X64Decoder::DecodeCpuid(_DInst& insn) {
    (void)insn;
    // Baseline: SSE2 userland plus the explicitly implemented scalar
    // facilities (CX16, MOVBE, RDRAND, RDSEED, TSC, RDTSCP). AVX-512, ERMS and
    // MMX stay unreported so glibc's ifunc dispatch keeps away from them.
    //
    // AVX/AVX2 and BMI1/BMI2 are advertised only behind their opt-in gates.
    // The discipline this file has always followed is "never advertise what
    // the decoder would #UD on", and it now cuts the other way too: the gates
    // enable the handlers, so CPUID must follow them or the guest is told a
    // lie in the safe direction and simply never uses the code we wrote.
    static constexpr u32 kSse2Edx = (1u << 0)   // FPU
                                    | (1u << 4)   // TSC
                                    | (1u << 8)   // CX8
                                    | (1u << 15)  // CMOV
                                    | (1u << 24)  // FXSR
                                    | (1u << 25)  // SSE
                                    | (1u << 26); // SSE2
    // SSE3 / SSSE3 / SSE4.1 are backed by decoder_sse4.cc (64 mnemonics,
    // 4020 Rosetta rows).  POPCNT is I_POPCNT, long implemented.
    //
    // SSE4.2 (bit 20) follows SVM_SSE42STR and is set below, not here:
    // pcmpistri/pcmpistrm/pcmpestri/pcmpestrm are implemented (6104 Rosetta
    // rows against a second, independent from-the-SDM model).
    static constexpr u32 kLeaf1Ecx = (1u << 13)  // CMPXCHG16B
                                     | (1u << 22)  // MOVBE
                                     | (1u << 30); // RDRAND
    static constexpr u32 kSse4Ecx = (1u << 0)    // SSE3
                                    | (1u << 9)   // SSSE3
                                    | (1u << 19)  // SSE4.1
                                    | (1u << 23); // POPCNT
    static constexpr u32 kLeaf7Ebx = (1u << 18);  // RDSEED
    // XSAVE (ECX.26) and OSXSAVE (ECX.27) travel together: OSXSAVE is
    // CR4.OSXSAVE, which is what makes XGETBV legal and tells the guest the OS
    // really saves the extended state.  Both are gated on SVM_XSAVE, the same
    // switch that enables the XGETBV/XSAVE/XRSTOR handlers, so CPUID can never
    // promise a facility the decoder would #UD on.
    // AVX requires the whole chain, not just its own bit: glibc checks
    // OSXSAVE, then executes XGETBV and requires XCR0[2:1] == 11b, before it
    // will take an AVX path. Advertising AVX without XSAVE/OSXSAVE would be
    // incoherent -- CPUID would claim a facility whose enabling protocol is
    // missing -- so AVX is reported only when BOTH gates are on. SVM_XSAVE
    // alone still reports XSAVE/OSXSAVE, which is coherent on its own.
    const bool avx_reported = AvxEnabled() && XsaveEnabled();
    // FMA (ECX.12) rides on the same condition as AVX and not on one of its
    // own: the FMA3 handler is dispatched from inside the `avx_on` group of the
    // VEX chain in decoder.cc, so it is reachable in exactly the cases AVX is
    // reported.  All 60 mnemonics are implemented at both VEX.L, so the bit
    // does not over-promise.  (The bit is architecturally meaningless without
    // AVX anyway -- FMA3 has no non-VEX encoding.)
    // SSE4.2 (bit 20) is NOT folded into kSse4Ecx: that constant follows
    // SVM_SSE4, and SVM_SSE4=1 SVM_SSE42STR=0 would then advertise a feature
    // whose every instruction declines.  Sse42StrEnabled() is exported for
    // exactly this.
    const u32 leaf1_ecx = kLeaf1Ecx | (Sse4Enabled() ? kSse4Ecx : 0u) |
                          (Sse42StrEnabled() ? (1u << 20) : 0u) |  // SSE4.2
                          (XsaveEnabled() ? ((1u << 26) | (1u << 27)) : 0u) |
                          (avx_reported ? ((1u << 28) | (1u << 12)) : 0u);  // AVX, FMA
    // BMI1 (bit 3) and BMI2 (bit 8) follow SVM_BMI. They are deliberately
    // independent of AVX: glibc's string ifuncs require AVX2 *and* BMI2
    // together, so advertising BMI2 without the AVX2 implementation being
    // ready would select variants we cannot run. Advertising AVX2 without
    // BMI2 is safe -- the string variants stay unselected and memcpy/memset,
    // whose AVX variants are fully covered, still get the fast path.
    const u32 leaf7_ebx = kLeaf7Ebx |
                          (avx_reported ? (1u << 5) : 0u) |       // AVX2
                          (BmiEnabled() ? ((1u << 3) | (1u << 8)) : 0u);
    // CPUID.0xD: the XSAVE state-component enumeration.  Every value is
    // derived from the layout constants in xsave.h, which are the same ones
    // XsaveHelper writes through -- a guest sizing its save area from EBX
    // below therefore cannot under-allocate it.
    const u64 xcr0 = GuestXcr0();
    const u32 xsave_size = XsaveAreaSize();
    static constexpr u32 kExtEdx = (1u << 11)   // SYSCALL/SYSRET
                                   | (1u << 20)   // NX
                                   | (1u << 27)   // RDTSCP
                                   | (1u << 29);  // LM (required by 64 bit guests)
    auto leaf = __ ZeroExtend64(R(_RegisterType::R_EAX));
    // ECX is both an input (the subleaf) and an output, so capture it before
    // the accumulator below overwrites it.
    auto subleaf = __ ZeroExtend64(R(_RegisterType::R_ECX));
    auto is_leaf = [&](u32 n) {
        return __ TestZero(__ Xor(leaf, ir::Operand{ir::Imm(u64(n))}));
    };
    auto is_subleaf = [&](u32 n) {
        return __ TestZero(__ Xor(subleaf, ir::Operand{ir::Imm(u64(n))}));
    };
    // Per-output-register leaf values {eax, ebx, ecx, edx}; unlisted leaves
    // and subleaves yield zeros.
    auto fold = [&](auto cond, std::array<u32, 4> vals) {
        auto pick = [&](_RegisterType reg, u32 v) {
            R(reg, __ Select(cond, __ LoadImm(ir::Imm(u64(v))), R(reg))
                       .SetType(ir::ValueType::U32));
        };
        pick(_RegisterType::R_EAX, vals[0]);
        pick(_RegisterType::R_EBX, vals[1]);
        pick(_RegisterType::R_ECX, vals[2]);
        pick(_RegisterType::R_EDX, vals[3]);
    };
    auto emit = [&](u32 for_leaf, std::array<u32, 4> vals) {
        fold(is_leaf(for_leaf), vals);
    };
    // Subleaf-selected leaves; an unlisted subleaf keeps the zeros below,
    // which is what real hardware reports for CPUID.0xD.3 and above.
    auto emit_sub = [&](u32 for_leaf, u32 for_subleaf, std::array<u32, 4> vals) {
        fold(__ And(is_leaf(for_leaf), ir::Operand{is_subleaf(for_subleaf)}), vals);
    };
    // Start from zeros, then fold each supported leaf in.
    R(_RegisterType::R_EAX, __ LoadImm(ir::Imm(u64(0))));
    R(_RegisterType::R_EBX, __ LoadImm(ir::Imm(u64(0))));
    R(_RegisterType::R_ECX, __ LoadImm(ir::Imm(u64(0))));
    R(_RegisterType::R_EDX, __ LoadImm(ir::Imm(u64(0))));
    emit(0x80000000, {0x80000004, 0, 0, 0});  // max extended leaf
    emit(0x80000001, {0, 0, 0, kExtEdx});
    emit(7, {0, leaf7_ebx, 0, 0});              // RDSEED (+AVX2/BMI when gated on)
    // denominator=1, numerator=1, crystal=1 GHz => virtual TSC frequency 1 GHz.
    emit(0x15, {1, 1, 1'000'000'000u, 0});
    if (XsaveEnabled()) {
        // Subleaf 0: EAX/EDX = the XCR0 bitmap the processor supports, EBX =
        // bytes needed by the components currently enabled in XCR0, ECX = the
        // same for every supported component.  XSETBV is not implemented, so
        // XCR0 is fixed at the supported set and EBX == ECX.
        emit_sub(0xD, 0, {u32(xcr0), xsave_size, xsave_size, u32(xcr0 >> 32)});
        // Subleaf 1: advertise XSAVEOPT[0] and XSAVEC[1]. XGETBV with ECX=1
        // and the privileged XSAVES/XRSTORS pair remain unsupported, so bits
        // 2 and 3 stay clear. The simplified XSAVEC path uses standard offsets
        // (XCOMP_BV=0), so its required area size IS the standard size: SDM
        // §13.2 makes EBX the XSAVEC area size whenever EAX[1]=1, and a guest
        // that allocates from EBX must not under-allocate.
        emit_sub(0xD, 1, {(1u << 0) | (1u << 1), xsave_size, 0, 0});
        if (xcr0 & kXstateYmm) {
            // Subleaf 2: size and offset of the YMM_Hi128 component.
            emit_sub(0xD, 2, {kXsaveYmmSize, kXsaveYmmOffset, 0, 0});
        }
    }
    emit(1, {0x000306C3, 0, leaf1_ecx, kSse2Edx}); // Haswell-ish model + CX16
    // "GenuineIntel" + max basic leaf.
    emit(0, {0x15, 0x756E6547, 0x6C65746E, 0x49656E69});
}

void X64Decoder::DecodeTimestamp(bool rdtscp) {
    // Write through the context instead of returning the counter in an
    // allocatable IR value. This also makes the operation explicitly
    // side-effecting and avoids keeping a volatile value live across the host
    // call in either backend.
    auto context = __ GetUniformAddress(ir::Imm(0)).SetType(ir::ValueType::U64);
    __ CallHost(&ReadGuestTsc, context);
    if (rdtscp) {
        // A single virtual CPU/core identity is exposed.
        R(_RegisterType::R_ECX, __ LoadImm(ir::Imm(u64(0))));
    }
}

void X64Decoder::DecodeRandomRegister(_RegisterType reg, u32 width) {
    const auto destination =
            __ GetUniformAddress(ir::Imm(ToReg(x86_regs_table[reg]).GetOffset()))
                    .SetType(ir::ValueType::U64);
    __ CallHost(&GuestRandom, destination, __ LoadImm(ir::Imm(u64(width))));
    // RDRAND/RDSEED report success in CF and clear OF/SF/ZF/AF/PF.
    __ ClearFlags(ir::Flags::All);
    __ SetCarry(__ LoadImm(ir::Imm(u64(1))));
    carry_ = CarryPolarity::Direct;
    StorePolarity(false);
}

void X64Decoder::DecodeRandom(_DInst& insn) {
    auto& op0 = insn.ops[0];
    DecodeRandomRegister(static_cast<_RegisterType>(op0.index), op0.size);
}

void X64Decoder::DecodeMovbe(_DInst& insn) {
    auto& dst = insn.ops[0];
    auto& src = insn.ops[1];
    // distorm orders both architectural forms semantically:
    //   F0: op0=register, op1=memory (load)
    //   F1: op0=memory,   op1=register (store)
    const u32 width = dst.size;
    auto value = ToValue(Src(insn, src));
    auto swapped = __ ByteSwap(value, ir::Imm(width)).SetType(GetSize(width));
    Dst(insn, dst, swapped);
}

void X64Decoder::DecodeMovnti(_DInst& insn) {
    // The non-temporal cache hint is not architecturally visible here.  x86
    // TSO is stronger than an NT store, so use the ordinary TSO-aware store
    // path for correctness and retain the configured ordering policy.
    Dst(insn, insn.ops[0], Src(insn, insn.ops[1]));
}

void X64Decoder::DecodeXlat(_DInst& insn) {
    auto address = __ Add(R(_RegisterType::R_RBX),
                          ir::Operand{__ ZeroExtend64(R(_RegisterType::R_AL))});
    auto value = MemLoad(ir::Operand{address}, ir::ValueType::U8, TsoOrdered(insn));
    R(_RegisterType::R_AL, value);
}

void X64Decoder::DecodeBswap(_DInst& insn) {
    // bswap r32/r64: reverse byte order. No flags affected.
    auto& op0 = insn.ops[0];
    u64 width = op0.size ? op0.size : 32;
    auto src = ToValue(Src(insn, op0));
    auto result = __ ByteSwap(src, ir::Imm(width)).SetType(GetSize(width));
    Dst(insn, op0, result);
}

}  // namespace swift::x86
