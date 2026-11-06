#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <setjmp.h>
#include <signal.h>
#include <sys/types.h>

#include "runtime/arch/x86/state.h"
#include "runtime/backend/code.h"

namespace swift::runtime::execution {
using x86::GuestState;
using NativeCode = backend::NativeCode;

// Owned by Process, not a mutable automatic in the sigsetjmp frame. Only
// the one registered guest thread may execute code through this context.
struct FaultContext {
    sigjmp_buf jump;
    std::atomic<const NativeCode*> active{};
    // Optional linked-native lookup, read only while active is non-null.
    // The offsets describe the actual private state layout; UINT32_MAX
    // retains the original single-unit/no-pinned-fields behavior.
    const NativeCode* (*lookup_native)(void*, std::uintptr_t) noexcept {};
    void* lookup_opaque{};
    bool (*recover_code_write)(void*, std::uintptr_t) noexcept {};
    void* code_write_opaque{};
    volatile sig_atomic_t smc_pending{};
    std::uint32_t pinned_uniform_offset{UINT32_MAX}, flags_offset{UINT32_MAX};
    std::uint8_t pinned_uniform_slots{8};
    bool native_nzcv{}; // Linked raw PF/AF ABI; reserved bits remain in flags_offset's shadow.
    GuestState* state{};
    pid_t thread_id{};
    bool registered{};
    volatile sig_atomic_t signal_number{}, signal_code{};
    std::atomic<std::uintptr_t> host_pc{}, address{};
    std::atomic<std::uint64_t> unaligned_recoveries{}, unaligned_patches{};
    std::atomic<std::uint64_t> smc_write_faults{};
    bool (*patch_ordered)(const NativeCode&, backend::NativeOffset, backend::NativeOffset, std::uintptr_t) noexcept {};
    bool (*recover_native)(FaultContext&, int, int, std::uintptr_t, void*) noexcept {};
    std::uintptr_t page_size{}; // Queried once at registration, never from the handler.
    std::uint64_t service_ns{}; // Fixed registration/restoration work, including rollback.
    int restore_error{};
    // Actual private XMM bank in v16..v31; fatal recovery reads FPSIMD state.
    std::uint32_t pinned_vector_offset{UINT32_MAX};
};

[[noreturn]] void RaiseGuestFault(FaultContext&, const backend::FaultSite&,
                                int signal, int code, std::uintptr_t host_pc,
                                std::uintptr_t address) noexcept;

// Construct before the one sigsetjmp in Run. Registration and previous
// actions remain unchanged while native code can jump back to that frame.
class FaultScope {
public:
    FaultScope(FaultContext&, GuestState&);
    ~FaultScope();
    FaultScope(const FaultScope&) = delete;
    FaultScope& operator=(const FaultScope&) = delete;

private:
    FaultContext& context;
    std::array<struct sigaction, 3> previous{};
    static void Handle(int, siginfo_t*, void*) noexcept;
    void Chain(int, siginfo_t*, void*) const noexcept;
};
} // namespace swift::runtime::execution
