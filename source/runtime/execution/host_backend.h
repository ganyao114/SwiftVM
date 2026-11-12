#pragma once

#include "runtime/arch/guest.h"
#include "runtime/backend/code.h"
#include "runtime/ir/types.h"

namespace swift::runtime::ir {
class Program;
}

namespace swift::runtime::execution {
struct NativeRunContext;
struct FaultContext;

// Actual per-request execution state. Stable ISA/ABI configuration is bound
// once by the platform; there is no per-operation backend callback.
struct HostCompileRequest {
    std::uint64_t entry_pc{};
    std::uint8_t fp_mode{};
    bool fault_locations{}, private_state{};
    bool relocatable{};
};

struct HostCapabilities {
    bool rcpc_loads{}, rcpc_imm9{}, native_nzcv{}, lse_atomics{}, crc32c{}, afp{}, aes{}, pmull{},
            sha256{}, native_eflags{};
};

struct HostBackendOps {
    void* context{};
    ir::TargetInfo target;
    HostCapabilities capabilities;
    std::uint64_t isa_features{};
    // Physical private-state banks belong to the selected native ABI.
    std::uint8_t pinned_uniform_slots{};
    // Effective slot count when the caller does not request an override.
    std::uint8_t pinned_uniform_default{};
    bool pinned_vectors{};
    // Only backends that bake software DAZ/FTZ into code need separate domains.
    bool specializes_fp_flush{};
    bool specializes_arm_fpcr{};
    bool thread_dispatch_cache{};
    void (*configure)(void*, const GuestLayout&) noexcept {};
    backend::CompileResult& (*compile)(void*, const ir::Program&, HostCompileRequest){};
    bool (*relocate)(void*,
                     std::span<std::uint8_t>,
                     std::span<const backend::Relocation>) noexcept {};
    void (*trim)(void*) noexcept {};
    void (*release)(void*) noexcept {};
    bool (*initialize_link)(void*,
                            std::span<std::uint8_t>,
                            std::uintptr_t,
                            bool,
                            const backend::LinkSite&,
                            std::uintptr_t) noexcept {};
    // Zero succeeds; a failed host synchronization returns its saved error code.
    int (*synchronize_code)(std::uint8_t*, const std::uint8_t*, std::size_t) noexcept {};
    void (*patch_link)(std::uintptr_t*,
                       const std::uintptr_t*,
                       std::uintptr_t,
                       std::uint32_t) noexcept {};
    void (*unlink)(std::uintptr_t*, const std::uintptr_t*, std::uint32_t) noexcept {};
    bool (*patch_fault)(const backend::NativeCode&,
                        backend::NativeOffset,
                        backend::NativeOffset,
                        std::uintptr_t) noexcept {};
    void (*update_fp)(void*, NativeRunContext&, std::uint32_t) noexcept {};
    std::uint32_t (*floating_status)(const NativeRunContext&) noexcept {};
    void (*restore_host_fp)(const NativeRunContext&) noexcept {};
    bool (*recover_native)(FaultContext&, int, int, std::uintptr_t, void*) noexcept {};
};

}  // namespace swift::runtime::execution
