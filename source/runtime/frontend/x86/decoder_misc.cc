#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <time.h>
#include "runtime/frontend/x86/cpuid.h"
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
    // Adjust the architectural counter to the same virtual 1 GHz frequency
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

void ReadGuestTscContext(ThreadContext64* context) {
    const u64 tsc = ReadVirtualTsc();
    context->rax.low.dword = u32(tsc);
    context->rdx.low.dword = u32(tsc >> 32);
}

u64 ReadGuestTsc() { return ReadVirtualTsc(); }

ir::UniformEffectId ReadGuestTscEffects() {
    static constexpr std::array ranges{
            ir::UniformEffectRange{offsetof(ThreadContext64, rax), sizeof(u32)},
            ir::UniformEffectRange{offsetof(ThreadContext64, rdx), sizeof(u32)},
    };
    static constexpr ir::UniformEffectSet effects{ranges.data(), ranges.size()};
    static const auto id = ir::RegisterUniformEffectSet(&effects);
    return id;
}

void GuestRandomContext(u64* destination, u64 width) {
    const u64 value = HostRandom();
    if (width == 16) {
        *destination = (*destination & ~u64(0xFFFF)) | u16(value);
    } else if (width == 32) {
        *destination = u32(value);
    } else {
        *destination = value;
    }
}

u64 GuestRandom() { return HostRandom(); }

}  // namespace

void X64Decoder::DecodeCpuid(_DInst& insn) {
    swift::runtime::PerfLoweringHandlerBegin();
    (void)insn;
    const bool xsave = XsaveEnabled();
    const bool avx = AvxEnabled() && xsave;
    u64 features{};
    if (swift::runtime::GetSvmConfig().x86_64_abi_baseline) {
        features |= CpuidAbiBaseline;
    }
    if (avx) {
        features |= CpuidAvx;
    }
    if (CryptoNiEnabled()) {
        features |= CpuidCrypto;
    }
    if (ShaNiEnabled()) {
        features |= CpuidSha;
    }
    if (Sse4Enabled()) {
        features |= CpuidSse4;
    }
    if (Sse42StrEnabled()) {
        features |= CpuidSse42;
    }
    if (FsgsbaseEnabled()) {
        features |= CpuidFsgsbase;
    }
    if (BmiEnabled()) {
        features |= CpuidBmi;
    }
    if (AdxEnabled()) {
        features |= CpuidAdx;
    }
    if (xsave) {
        features |= CpuidXsave;
        if (swift::runtime::GetSvmConfig().xsave_ymm) {
            features |= CpuidXsaveYmm;
        }
    }

    auto leaf = __ ZeroExtend64(R(_RegisterType::R_EAX));
    auto subleaf = __ ZeroExtend64(R(_RegisterType::R_ECX));
    auto lower = __ Cpuid(leaf,
                          subleaf,
                          ir::Imm(features),
                          ir::Imm(reinterpret_cast<VAddr>(&QueryCpuid)))
                         .SetType(ir::ValueType::U64);
    auto upper = __ CpuidUpper(lower).SetType(ir::ValueType::U64);
    R(_RegisterType::R_EAX,
      __ BitExtract(lower, ir::Imm(0u), ir::Imm(32u)).SetType(ir::ValueType::U32));
    R(_RegisterType::R_EBX,
      __ BitExtract(lower, ir::Imm(32u), ir::Imm(32u)).SetCastType(ir::ValueType::U32));
    R(_RegisterType::R_ECX,
      __ BitExtract(upper, ir::Imm(0u), ir::Imm(32u)).SetType(ir::ValueType::U32));
    R(_RegisterType::R_EDX,
      __ BitExtract(upper, ir::Imm(32u), ir::Imm(32u)).SetCastType(ir::ValueType::U32));
}

void X64Decoder::DecodeTimestamp(bool rdtscp) {
    if (HelperValuesEnabled()) {
        auto tsc = __ CallHostUniformPure(&ReadGuestTsc).SetType(ir::ValueType::U64);
        R(_RegisterType::R_EAX,
          __ BitExtract(tsc, ir::Imm(0u), ir::Imm(32u)).SetType(ir::ValueType::U32));
        R(_RegisterType::R_EDX,
          __ BitExtract(tsc, ir::Imm(32u), ir::Imm(32u))
                  .SetCastType(ir::ValueType::U32));
    } else {
        // Legacy ABI: the helper writes EAX/EDX through ThreadContext64 and
        // W50 metadata describes the affected byte ranges.
        auto context = __ GetUniformAddress(ir::Imm(0)).SetType(ir::ValueType::U64);
        __ CallHostWithUniformEffects(ReadGuestTscEffects(), &ReadGuestTscContext, context);
    }
    if (rdtscp) {
        // A single virtual CPU/core direct is exposed.
        R(_RegisterType::R_ECX, __ LoadImm(ir::Imm(u64(0))));
    }
}

void X64Decoder::DecodeRandomRegister(_RegisterType reg, u32 width) {
    if (HelperValuesEnabled()) {
        auto value = __ CallHostUniformPure(&GuestRandom).SetType(ir::ValueType::U64);
        R(reg, __ BitExtract(value, ir::Imm(0u), ir::Imm(width)).SetType(GetSize(width)));
    } else {
        const auto destination =
                __ GetUniformAddress(ir::Imm(ToReg(x86_regs_table[reg]).GetOffset()))
                        .SetType(ir::ValueType::U64);
        __ CallHost(&GuestRandomContext, destination, __ LoadImm(ir::Imm(u64(width))));
    }
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
