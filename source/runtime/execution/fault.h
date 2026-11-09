#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <signal.h>
#include <sys/types.h>

#include "guest_execution.h"
#include "runtime/arch/x86/state.h"
#include "runtime/backend/code.h"
#include "runtime/exception.h"

namespace swift {
struct Failure;
}

namespace swift::runtime::platform {
bool EnsureManagedSignal(int) noexcept;
}

namespace swift::runtime::execution {
struct NativeRunContext;
using x86::GuestState;
using NativeCode = backend::NativeCode;

// Owned by the guest thread. The host handler borrows it through this host
// thread's activation; native exceptions return through the root ABI.
struct FaultContext {
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
    bool native_nzcv{};  // Linked raw PF/AF ABI; reserved bits remain in flags_offset's shadow.
    void* state{};
    std::uint64_t* guest_pc{};
    // Borrowed fixed storage; signal adapters retain host failures without formatting.
    swift::Failure* host_failure{};
    pid_t thread_id{};
    bool registered{};
    volatile sig_atomic_t signal_number{}, signal_code{};
    std::atomic<std::uintptr_t> host_pc{}, address{};
    std::atomic<std::uint64_t> unaligned_recoveries{}, unaligned_patches{};
    std::atomic<std::uint64_t> smc_write_faults{};
    // A protected dispatch view covers every indexed L1 access. Only this
    // thread's selected pointer changes; shared executable code stays intact.
    std::atomic<std::uintptr_t> dispatch_interrupt_base{};
    std::size_t dispatch_interrupt_bytes{};
    std::atomic<std::uintptr_t> dispatch_resume_l1{};
    std::atomic<std::uint64_t> dispatch_interrupts{};
    bool (*patch_ordered)(const NativeCode&,
                          backend::NativeOffset,
                          backend::NativeOffset,
                          std::uintptr_t) noexcept {};
    bool (*recover_native)(FaultContext&, int, int, std::uintptr_t, void*) noexcept {};
    std::uintptr_t page_size{};  // Queried once at registration, never from the handler.
    std::uint64_t service_ns{};  // Fixed registration/restoration work, including rollback.
    int restore_error{};
    // Actual private XMM bank in v16..v31; fatal recovery reads FPSIMD state.
    std::uint32_t pinned_vector_offset{UINT32_MAX};
    // Optional borrowed raw FP status slot. The host adapter captures it
    // from the signal frame before leaving native code; the VM merges it.
    std::uint64_t* guest_fp_status{};
    NativeRunContext* native{};
    GuestISA guest_isa{GuestISA::X86_64};
    GuestExceptionInfo exception;
    bool exception_pending{};
    // The host adapter passes only routed asynchronous events. The platform
    // guest OS owns queue/mask policy, including per-signo RT pressure.
    bool (*queue_signal)(void*, int, siginfo_t*, void*) noexcept {};
    void* signal_opaque{};
};

void RecordGuestFault(FaultContext&,
                      const backend::FaultSite&,
                      int signal,
                      int code,
                      std::uintptr_t host_pc,
                      std::uintptr_t address) noexcept;

// Semantic access failures replace the previous event's access hints. Called
// only on failure; successful direct accesses perform no recovery bookkeeping.
void RecordMemoryFault(FaultContext&,
                       std::uint64_t address,
                       GuestExceptionAccess,
                       std::size_t bytes = 0) noexcept;

bool IsDispatchInterrupt(const FaultContext&, std::uintptr_t address) noexcept;

// The scope owns execution and the thread activation. Process handlers have
// their own lifetime and never borrow this stack object.
class FaultScope {
public:
    FaultScope(FaultContext&, GuestState&);
    FaultScope(FaultContext&, void* uniforms, std::uint64_t& pc);
    ~FaultScope();
    FaultScope(const FaultScope&) = delete;
    FaultScope& operator=(const FaultScope&) = delete;

private:
    GuestExecutionLock execution_lock;
    FaultContext& context;
    static void Handle(int, siginfo_t*, void*) noexcept;
    static void Chain(int, siginfo_t*, void*) noexcept;
    friend bool platform::EnsureManagedSignal(int) noexcept;
};
}  // namespace swift::runtime::execution
