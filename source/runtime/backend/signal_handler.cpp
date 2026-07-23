//
// Host signal handling framework — see signal_handler.h for the design.
//

#include "signal_handler.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>

namespace swift::runtime::backend {

namespace {

constexpr size_t kMaxHandlers = 16;
// 64KB alternate signal stack per thread running guest code.
constexpr size_t kAltStackSize = 64 * 1024;

struct HandlerEntry {
    SignalHandler::FaultCallback cb{};
    void* ctx{};
    int priority{};
};

std::array<HandlerEntry, kMaxHandlers> g_handlers{};
std::atomic<size_t> g_handler_count{0};
std::mutex g_register_lock{};

std::atomic_bool g_installed{false};

std::atomic<SignalHandler::GuestMapProbe> g_guest_probe{nullptr};
std::atomic<void*> g_guest_probe_ctx{nullptr};

// Per-thread alternate stack. Allocated once and intentionally never freed:
// the kernel references it until the thread exits.
thread_local bool g_alt_stack_installed = false;

const char* SignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        default: return "SIGNAL";
    }
}

}  // namespace

void SignalHandler::Install() {
    InstallThreadAltStack();
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        return;
    }
    struct sigaction sa {};
    sa.sa_sigaction = &SignalHandler::HandleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
}

void SignalHandler::InstallThreadAltStack() {
    if (g_alt_stack_installed) {
        return;
    }
    stack_t ss{};
    ss.ss_sp = std::malloc(kAltStackSize);
    ss.ss_size = kAltStackSize;
    ss.ss_flags = 0;
    if (ss.ss_sp && sigaltstack(&ss, nullptr) == 0) {
        g_alt_stack_installed = true;
    }
}

void SignalHandler::RegisterHandler(FaultCallback cb, void* ctx, int priority) {
    std::lock_guard guard(g_register_lock);
    size_t count = g_handler_count.load(std::memory_order_relaxed);
    if (count >= kMaxHandlers) {
        return;
    }
    // Insert sorted by ascending priority; first handler to return true wins.
    size_t pos = count;
    while (pos > 0 && g_handlers[pos - 1].priority > priority) {
        g_handlers[pos] = g_handlers[pos - 1];
        --pos;
    }
    g_handlers[pos] = HandlerEntry{cb, ctx, priority};
    g_handler_count.store(count + 1, std::memory_order_release);
}

void SignalHandler::UnregisterHandler(void* ctx) {
    std::lock_guard guard(g_register_lock);
    size_t count = g_handler_count.load(std::memory_order_relaxed);
    size_t out = 0;
    for (size_t i = 0; i < count; ++i) {
        if (g_handlers[i].ctx != ctx) {
            g_handlers[out++] = g_handlers[i];
        }
    }
    g_handler_count.store(out, std::memory_order_release);
}

void SignalHandler::SetGuestMapProbe(GuestMapProbe probe, void* ctx) {
    g_guest_probe_ctx.store(ctx, std::memory_order_release);
    g_guest_probe.store(probe, std::memory_order_release);
}

bool SignalHandler::IsGuestAddressMapped(std::uintptr_t fault_host_addr) {
    auto probe = g_guest_probe.load(std::memory_order_acquire);
    if (!probe) {
        return false;
    }
    return probe(g_guest_probe_ctx.load(std::memory_order_acquire), fault_host_addr);
}

std::uintptr_t SignalHandler::GetContextPC(const ucontext_t* uctx) {
#if defined(__APPLE__) && defined(__aarch64__)
    return static_cast<std::uintptr_t>(uctx->uc_mcontext->__ss.__pc);
#elif defined(__linux__) && defined(__aarch64__)
    return static_cast<std::uintptr_t>(uctx->uc_mcontext->pc);
#elif defined(__APPLE__) && defined(__x86_64__)
    return static_cast<std::uintptr_t>(uctx->uc_mcontext->__ss.__rip);
#elif defined(__linux__) && defined(__x86_64__)
    return static_cast<std::uintptr_t>(uctx->uc_mcontext.gregs[REG_RIP]);
#else
#    error "SignalHandler: unsupported host platform"
#endif
}

void SignalHandler::SetContextPC(ucontext_t* uctx, std::uintptr_t pc) {
#if defined(__APPLE__) && defined(__aarch64__)
    uctx->uc_mcontext->__ss.__pc = pc;
#elif defined(__linux__) && defined(__aarch64__)
    uctx->uc_mcontext->pc = pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    uctx->uc_mcontext->__ss.__rip = pc;
#elif defined(__linux__) && defined(__x86_64__)
    uctx->uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(pc);
#endif
}

void SignalHandler::SetContextReturnValue(ucontext_t* uctx, u64 value) {
#if defined(__APPLE__) && defined(__aarch64__)
    uctx->uc_mcontext->__ss.__x[0] = value;
#elif defined(__linux__) && defined(__aarch64__)
    uctx->uc_mcontext->regs[0] = value;
#elif defined(__APPLE__) && defined(__x86_64__)
    uctx->uc_mcontext->__ss.__rax = value;
#elif defined(__linux__) && defined(__x86_64__)
    uctx->uc_mcontext.gregs[REG_RAX] = static_cast<greg_t>(value);
#endif
}

void SignalHandler::HandleSignal(int sig, siginfo_t* info, void* raw_uctx) {
    auto* uctx = static_cast<ucontext_t*>(raw_uctx);
    const size_t count = g_handler_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        const HandlerEntry entry = g_handlers[i];
        if (entry.cb && entry.cb(entry.ctx, uctx, sig, info)) {
            return;
        }
    }
    DefaultHandler(sig, info, uctx);
}

void SignalHandler::DefaultHandler(int sig, siginfo_t* info, ucontext_t* uctx) {
    // Best-effort async-signal-safe diagnostics: snprintf into a stack buffer
    // + a single write() to stderr (no stdio buffering, no allocation).
    char buf[256];
    const int len = std::snprintf(buf,
                                  sizeof(buf),
                                  "[SwiftVM] unhandled host fault: %s at pc=%p addr=%p\n",
                                  SignalName(sig),
                                  reinterpret_cast<void*>(GetContextPC(uctx)),
                                  info ? info->si_addr : nullptr);
    if (len > 0) {
        const ssize_t unused = write(STDERR_FILENO, buf, static_cast<size_t>(len));
        (void) unused;
    }
    // Die with the original signal (and a core dump) instead of a bland
    // abort(): restore the default disposition and re-raise.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
    std::_Exit(128 + sig);
}

}  // namespace swift::runtime::backend
