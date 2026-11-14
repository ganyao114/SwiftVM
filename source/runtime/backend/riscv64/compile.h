#pragma once

#include "runtime/backend/code.h"

namespace swift::runtime::ir {
class Program;
}

namespace swift::runtime::backend::riscv64 {

struct Allocation;

constexpr ir::TargetInfo KnownTargetInfo() noexcept {
    return {ir::TargetMode::Known,
            0x64,
            static_cast<std::uint64_t>(ir::TargetFeature::ShiftedAddress) |
                    static_cast<std::uint64_t>(ir::TargetFeature::ConditionalUniformStore) |
                    static_cast<std::uint64_t>(ir::TargetFeature::VectorSignedMulWidenLow)};
}

struct CompileOptions {
    std::uint32_t flags_offset{UINT32_MAX}, pc_offset{UINT32_MAX}, mxcsr_offset{UINT32_MAX};
    std::uintptr_t dispatcher{}, exit_handler{};
    std::uint64_t entry_pc{};
    bool native_fp_environment{}, fault_locations{};
    bool relocatable{};  // Explicit persistence recording; ordinary JIT stays unchanged.
    bool zba{}, zbb{}, zbs{}, zicond{}, zbc{}, zbkb{}, zbkc{}, ual{}, ztso{};
};

// Exact RV64G words. The caller owns publication, W^X, I-cache synchronization
// and the lifetime of fault metadata. Linked units share s1=state/s11=flags;
// standalone units implement uint64_t(uint8_t*) with the LP64D C ABI.
backend::CompileResult Compile(const ir::Program&, CompileOptions = {});
void Compile(const ir::Program&, CompileOptions, backend::CompileResult&);
void Compile(const ir::Program&, CompileOptions, backend::CompileResult&, Allocation&);

}  // namespace swift::runtime::backend::riscv64
