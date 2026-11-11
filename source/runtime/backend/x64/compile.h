#pragma once
#include "runtime/backend/code.h"

namespace swift::runtime::ir {
class Program;
}

namespace swift::runtime::backend::x64 {
struct Allocation;

constexpr ir::TargetInfo KnownTargetInfo() noexcept {
    return {ir::TargetMode::Known,
            0x8664,
            static_cast<std::uint64_t>(ir::TargetFeature::ConditionalUniformStore) |
                    static_cast<std::uint64_t>(ir::TargetFeature::VectorSignedMulWidenLow)};
}

struct CompileOptions {
    std::uint32_t flags_offset{UINT32_MAX}, pc_offset{UINT32_MAX}, mxcsr_offset{UINT32_MAX};
    // Linked guest-register pins: slots 0..1 live in r13 and r14.
    std::uint32_t pinned_uniform_offset{UINT32_MAX};
    std::uint8_t pinned_uniform_slots{};
    std::uintptr_t dispatcher{}, exit_handler{};
    std::uint64_t entry_pc{};
    bool native_fp_environment{}, fault_locations{}, relocatable{};
    bool cx16{true}, ssse3{}, sse41{}, sse42{}, fma{}, aes{}, pclmul{}, sha{};
};

backend::CompileResult Compile(const ir::Program&, CompileOptions = {});
void Compile(const ir::Program&, CompileOptions, backend::CompileResult&);
void Compile(const ir::Program&, CompileOptions, backend::CompileResult&, Allocation&);
}  // namespace swift::runtime::backend::x64
