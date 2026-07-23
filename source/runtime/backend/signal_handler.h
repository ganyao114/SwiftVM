//
// Host signal handling framework for the SwiftVM runtime backend.
//
// Purpose (Phase 3: exception-handling groundwork):
//  - JIT-compiled guest code dereferences guest memory as [pt + guest_addr]
//    with no bounds checks. A wild guest pointer therefore raises a *host*
//    SIGSEGV/SIGBUS inside JIT code. This framework installs a sigaction
//    handler chain that gives the runtime a chance to convert such faults
//    into a guest-visible PageFatal halt instead of crashing the host.
//  - The same chain is the delivery channel for SMC write-protect faults
//    (Phase 4 registers a higher-priority handler).
//
// Design (modeled on cross86's unified_fault_handler, simplified):
//  - One process-wide sigaction for SIGSEGV/SIGBUS/SIGILL
//    (SA_SIGINFO | SA_ONSTACK) installed lazily by Install().
//  - A fixed-size handler chain, sorted by ascending priority; the first
//    handler that returns true has "handled" the fault (it must have made
//    the ucontext resumable, e.g. by rewinding the PC).
//  - If no handler claims the fault, a default handler prints a minimal
//    async-signal-safe diagnostic (signal, fault addr, PC) and re-raises the
//    signal with SIG_DFL so the process dies with the original signal.
//
// Threading:
//  - The signal mask/action is process-wide, but the alternate signal stack
//    is per-thread: Install() sets one up for the calling thread and
//    InstallThreadAltStack() must be called once on every thread that runs
//    guest code (Runtime::Run does this).
//  - Handler registration is expected at setup time only. The chain is a
//    fixed array appended under a mutex and read lock-free in the signal
//    handler (registration never happens concurrently with guest execution
//    in the current single-threaded drivers).
//

#pragma once

#include <csignal>
#include <cstdint>
#ifdef __APPLE__
// macOS <ucontext.h> is deprecated-gated behind _XOPEN_SOURCE; the sys/
// variant exposes ucontext_t/mcontext_t unconditionally.
#    include <sys/ucontext.h>
#else
#    include <ucontext.h>
#endif
#include "runtime/common/types.h"

namespace swift::runtime::backend {

class SignalHandler {
public:
    // Fault callback. `uctx` is the interrupted context and may be modified
    // (e.g. PC rewound) before returning true. Return true = fault handled,
    // resume execution from `uctx`; false = pass to the next handler.
    using FaultCallback = bool (*)(void* ctx, ucontext_t* uctx, int sig, siginfo_t* info);

    // Optional probe registered by the frontend (translator) layer: given a
    // *host* fault address, returns true if it is backed by a mapped guest
    // page (i.e. the fault is a protection violation, not a wild pointer).
    using GuestMapProbe = bool (*)(void* ctx, std::uintptr_t fault_host_addr);

    // Installs the process-wide sigaction handlers and the calling thread's
    // alternate signal stack. Idempotent.
    static void Install();

    // Sets up the alternate signal stack for the *current* thread.
    // Idempotent per thread. Called by Install() and by Runtime::Run.
    static void InstallThreadAltStack();

    // Registers a fault handler. Lower `priority` runs first (SMC lives at
    // 0; the JIT guest-fault recovery at 100; the built-in default handler
    // is the implicit last resort and is not part of the chain).
    static void RegisterHandler(FaultCallback cb, void* ctx, int priority);

    // Removes every handler registered with this ctx.
    static void UnregisterHandler(void* ctx);

    // Frontend hook: IsGuestAddressMapped() below consults it.
    static void SetGuestMapProbe(GuestMapProbe probe, void* ctx);

    // True if the probe says this host fault address maps to a guest page.
    // Without a probe, conservatively returns false (fault treated as a wild
    // guest pointer when the PC is inside JIT code).
    static bool IsGuestAddressMapped(std::uintptr_t fault_host_addr);

    // --- ucontext accessors (OS/arch abstraction) --------------------------
    static std::uintptr_t GetContextPC(const ucontext_t* uctx);
    static void SetContextPC(ucontext_t* uctx, std::uintptr_t pc);
    // Writes the value a trampoline-style frame would return to its host
    // caller (x0 on arm64 hosts) — used to fabricate a HaltReason.
    static void SetContextReturnValue(ucontext_t* uctx, u64 value);

private:
    static void HandleSignal(int sig, siginfo_t* info, void* raw_uctx);
    static void DefaultHandler(int sig, siginfo_t* info, ucontext_t* uctx);
};

}  // namespace swift::runtime::backend
