#include "runtime/execution/fault.h"
#include <cerrno>
#include "signal_time.h"
#include "base/error.h"
#include <sys/syscall.h>
#include <unistd.h>

namespace swift::runtime::execution {
namespace {
constexpr std::array signals{SIGSEGV, SIGBUS, SIGTRAP};

std::atomic<FaultScope*> registered_scope{};  // One service, not a process directory.
static_assert(std::atomic<FaultScope*>::is_always_lock_free &&
              std::atomic<const NativeCode*>::is_always_lock_free &&
              std::atomic<std::uintptr_t>::is_always_lock_free &&
              std::atomic<std::uint64_t>::is_always_lock_free);

sigset_t ManagedSignals() noexcept {
    sigset_t set;
    sigemptyset(&set);
    for (const auto signal : signals) sigaddset(&set, signal);
    return set;
}
}  // namespace

FaultScope::FaultScope(FaultContext& context, GuestState& state)
        : FaultScope(context, &state, state.pc) {}

FaultScope::FaultScope(FaultContext& context, void* state, std::uint64_t& pc) : context(context) {
    platform::SignalTimer elapsed{context.service_ns};
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) swift::ThrowSystemError("get native fault page size", errno);
    context.page_size = static_cast<std::uintptr_t>(page_size);
    const auto managed = ManagedSignals();
    sigset_t saved;
    if (sigprocmask(SIG_BLOCK, &managed, &saved))
        swift::ThrowSystemError("block native fault signals", errno);
    FaultScope* empty{};
    if (!registered_scope.compare_exchange_strong(empty, this)) {
        sigprocmask(SIG_SETMASK, &saved, nullptr);
        swift::ThrowError("only one guest fault service may be registered",
                          ErrorKind::InvalidState);
    }
    context.state = state;
    context.guest_pc = &pc;
    context.thread_id = static_cast<pid_t>(syscall(SYS_gettid));
    struct sigaction action{};
    action.sa_sigaction = Handle;
    action.sa_flags = SA_SIGINFO;
    action.sa_mask = managed;
    std::size_t installed{};
    for (; installed < signals.size(); ++installed)
        if (sigaction(signals[installed], &action, &previous[installed])) break;
    if (installed != signals.size()) {
        const auto saved_error = errno;
        while (installed) {
            --installed;
            sigaction(signals[installed], &previous[installed], nullptr);
        }
        registered_scope.store(nullptr);
        sigprocmask(SIG_SETMASK, &saved, nullptr);
        swift::ThrowSystemError("install native fault handlers", saved_error);
    }
    context.registered = true;
    if (sigprocmask(SIG_SETMASK, &saved, nullptr)) {
        const auto saved_error = errno;
        context.registered = false;
        registered_scope.store(nullptr);
        for (std::size_t i = 0; i < signals.size(); ++i)
            sigaction(signals[i], &previous[i], nullptr);
        swift::ThrowSystemError("restore signal mask after fault registration", saved_error);
    }
}

FaultScope::~FaultScope() {
    platform::SignalTimer elapsed{context.service_ns};
    const auto managed = ManagedSignals();
    sigset_t saved;
    const bool blocked = sigprocmask(SIG_BLOCK, &managed, &saved) == 0;
    if (!blocked) context.restore_error = errno;
    context.active.store(nullptr);
    context.registered = false;
    for (std::size_t i = 0; i < signals.size(); ++i)
        if (sigaction(signals[i], &previous[i], nullptr)) context.restore_error = errno;
    registered_scope.store(nullptr);
    if (blocked && sigprocmask(SIG_SETMASK, &saved, nullptr)) context.restore_error = errno;
}

void FaultScope::Chain(int signal, siginfo_t* info, void* raw) const noexcept {
    std::size_t index{};
    while (signals[index] != signal) ++index;
    const auto& old = previous[index];
    if (old.sa_handler == SIG_IGN) return;
    if (old.sa_handler == SIG_DFL) {
        sigaction(signal, &old, nullptr);
        syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), signal);
    } else if (old.sa_flags & SA_SIGINFO)
        old.sa_sigaction(signal, info, raw);
    else
        old.sa_handler(signal);
}

void FaultScope::Handle(int signal, siginfo_t* info, void* raw) noexcept {
    const auto saved_errno = errno;
    auto* scope = registered_scope.load();
    if (!scope) {
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(signal, &action, nullptr);
        syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), signal);
        return;
    }
    auto& context = scope->context;
    if (info->si_code > 0 && context.thread_id == static_cast<pid_t>(syscall(SYS_gettid)) &&
        context.recover_native &&
        context.recover_native(context,
                               signal,
                               info->si_code,
                               reinterpret_cast<std::uintptr_t>(info->si_addr),
                               raw)) {
        errno = saved_errno;
        return;
    }
    scope->Chain(signal, info, raw);
    errno = saved_errno;
}

}  // namespace swift::runtime::execution
