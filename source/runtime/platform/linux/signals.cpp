#include "signals.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "base/error.h"
#include "base/format.h"
#include "faultable_atomic.h"
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"
#include "signal_time.h"

namespace swift::runtime::platform {
namespace {
struct SignalService {
    std::mutex mutex;
    std::array<struct sigaction, NSIG> previous{};
    std::array<bool, NSIG> installed{};
    std::atomic<HostSignalBroker> broker{};
};

// The process owner outlives every thread signal reader. A handler never
// borrows a VM-owned registry or a mutable automatic FaultScope object.
SignalService service;
// Bionic's static TLS layout requires at least 64-byte PT_TLS alignment on
// ARM64, including small executables that link only the native fault tests.
alignas(64) constinit thread_local execution::FaultContext* current_context{};

sigset_t RecoverySignals() noexcept {
    sigset_t set;
    sigemptyset(&set);
    for (const auto number : {SIGSEGV, SIGBUS, SIGTRAP, SIGILL, SIGFPE}) sigaddset(&set, number);
    return set;
}

class ThreadAltStack {
public:
    void Prepare(std::size_t page_size) {
        if (prepared) return;
        if (sigaltstack(nullptr, &previous)) swift::ThrowSystemError("read host altstack", errno);
        if (!(previous.ss_flags & SS_DISABLE)) {
            prepared = true;
            return;
        }
        std::size_t minimum = MINSIGSTKSZ;
#if defined(AT_MINSIGSTKSZ)
        minimum = std::max(minimum, static_cast<std::size_t>(getauxval(AT_MINSIGSTKSZ)));
#endif
        const auto stack_bytes = (minimum + 65536 + page_size - 1) / page_size * page_size;
        mapping_bytes = stack_bytes + 2 * page_size;
        mapping = mmap(nullptr, mapping_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            mapping = nullptr;
            swift::ThrowSystemError("map host signal stack", errno);
        }
        auto* start = static_cast<std::uint8_t*>(mapping) + page_size;
        stack_t stack{.ss_sp = start, .ss_flags = 0, .ss_size = stack_bytes};
        if (mprotect(start, stack_bytes, PROT_READ | PROT_WRITE) || sigaltstack(&stack, nullptr)) {
            const auto error = errno;
            munmap(mapping, mapping_bytes);
            mapping = nullptr;
            swift::ThrowSystemError("install host signal stack", error);
        }
        prepared = true;
    }

    ~ThreadAltStack() {
        if (!mapping) return;
        // This thread has left every VM activation. Restore its previous
        // stack before releasing the service-owned mapping.
        if (!sigaltstack(&previous, nullptr)) munmap(mapping, mapping_bytes);
    }

private:
    stack_t previous{};
    void* mapping{};
    std::size_t mapping_bytes{};
    bool prepared{};
};

thread_local ThreadAltStack alternate_stack;

bool Synchronous(int number, int code) noexcept {
    return code > 0 && (number == SIGSEGV || number == SIGBUS || number == SIGTRAP ||
                        number == SIGILL || number == SIGFPE);
}

bool PlatformFatalHandler(int number) noexcept {
#if defined(__ANDROID__)
    // Bionic's linker installs debuggerd before main for these fatal signals.
    // It is retained only as a terminal host-crash owner, not a resumable
    // embedding callback. Recovering application handlers need an explicit broker.
    return number == SIGSEGV || number == SIGBUS || number == SIGILL || number == SIGFPE ||
           number == SIGTRAP;
#else
    (void)number;
    return false;
#endif
}
}  // namespace

bool EnsureManagedSignal(int number) noexcept {
    if (number <= 0 || number >= NSIG || number == SIGKILL || number == SIGSTOP) {
        errno = EINVAL;
        return false;
    }
    std::lock_guard lock{service.mutex};
    if (service.installed[number]) return true;
    // Publish the immutable prior action before installing the handler,
    // which another host thread can enter as soon as sigaction succeeds.
    if (sigaction(number, nullptr, &service.previous[number])) return false;
    const auto& prior = service.previous[number];
    if (prior.sa_handler != SIG_DFL && prior.sa_handler != SIG_IGN &&
        !service.broker.load(std::memory_order_acquire) && !PlatformFatalHandler(number)) {
        // Transparent chaining requires the embedder's broker: directly
        // invoking a saved function loses kernel mask/stack/action semantics.
        errno = EBUSY;
        return false;
    }
    struct sigaction action{};
    action.sa_sigaction = execution::FaultScope::Handle;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigfillset(&action.sa_mask);
    if (sigaction(number, &action, nullptr)) return false;
    service.installed[number] = true;
    return true;
}

bool StopGuestProcess(int number) noexcept {
    if (number == SIGSTOP) return syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), number) == 0;
    if (number != SIGTSTP && number != SIGTTIN && number != SIGTTOU) {
        errno = EINVAL;
        return false;
    }
    std::lock_guard lock{service.mutex};
    sigset_t target, previous_mask;
    sigemptyset(&target);
    sigaddset(&target, number);
    if (sigprocmask(SIG_BLOCK, &target, &previous_mask)) return false;
    const bool was_blocked = sigismember(&previous_mask, number) == 1;
    struct sigaction action{}, previous_action;
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    if (sigaction(number, &action, &previous_action)) {
        const auto error = errno;
        if (!was_blocked) sigprocmask(SIG_UNBLOCK, &target, nullptr);
        errno = error;
        return false;
    }
    int error{};
    if (sigprocmask(SIG_UNBLOCK, &target, nullptr) ||
        syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), number))
        error = errno;
    if (sigaction(number, &previous_action, nullptr) && !error) error = errno;
    // A SIGCONT handler may have changed other mask bits (RT queue pressure).
    // Restore only the target signal rather than replacing the entire mask.
    if (sigprocmask(was_blocked ? SIG_BLOCK : SIG_UNBLOCK, &target, nullptr) && !error)
        error = errno;
    if (error) errno = error;
    return error == 0;
}

void SetHostSignalBroker(HostSignalBroker broker) noexcept { service.broker.store(broker); }
}  // namespace swift::runtime::platform

namespace swift::runtime::execution {
FaultScope::FaultScope(FaultContext& context, GuestState& state)
        : FaultScope(context, &state, state.pc) {}

FaultScope::FaultScope(FaultContext& context, void* state, std::uint64_t& pc) : context(context) {
    platform::SignalTimer elapsed{context.service_ns};
    if (platform::current_context)
        swift::ThrowError("guest execution is already active on this host thread",
                          ErrorKind::InvalidState);
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) swift::ThrowSystemError("get native fault page size", errno);
    context.page_size = static_cast<std::uintptr_t>(page_size);
    platform::alternate_stack.Prepare(context.page_size);
    for (const auto number : {SIGSEGV, SIGBUS, SIGTRAP, SIGILL, SIGFPE, SIGURG, SIGCONT})
        if (!platform::EnsureManagedSignal(number))
            swift::ThrowSystemError(swift::Format("install native signal handler {}", number),
                                    errno);
    const auto managed = platform::RecoverySignals();
    sigset_t saved;
    if (sigprocmask(SIG_BLOCK, &managed, &saved))
        swift::ThrowSystemError("block native recovery signals", errno);
    context.state = state;
    context.guest_pc = &pc;
    context.thread_id = static_cast<pid_t>(syscall(SYS_gettid));
    context.registered = true;
    platform::current_context = &context;
    if (sigprocmask(SIG_SETMASK, &saved, nullptr)) {
        const auto error = errno;
        platform::current_context = nullptr;
        context.registered = false;
        swift::ThrowSystemError("restore native signal mask", error);
    }
}

FaultScope::~FaultScope() {
    platform::SignalTimer elapsed{context.service_ns};
    const auto managed = platform::RecoverySignals();
    sigset_t saved;
    const bool blocked = sigprocmask(SIG_BLOCK, &managed, &saved) == 0;
    if (!blocked) context.restore_error = errno;
    platform::current_context = nullptr;
    context.active.store(nullptr, std::memory_order_release);
    context.registered = false;
    if (blocked && sigprocmask(SIG_SETMASK, &saved, nullptr)) context.restore_error = errno;
}

void FaultScope::Chain(int number, siginfo_t* info, void* raw) noexcept {
    if (const auto broker = platform::service.broker.load(); broker && broker(number, info, raw))
        return;
    const auto& old = platform::service.previous[number];
    if (old.sa_handler == SIG_IGN) return;
    if (platform::PlatformFatalHandler(number) && old.sa_handler != SIG_DFL &&
        !platform::service.broker.load(std::memory_order_acquire)) {
        // Terminal handoff to Android's crash facility. Let the kernel create
        // its original frame/action/altstack semantics on the repeated fault;
        // never call the saved handler as an ordinary function.
        sigaction(number, &old, nullptr);
        if (!platform::Synchronous(number, info->si_code))
            syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), number);
        return;
    }
    // An explicit broker owns every custom prior handler. If it declines an
    // unrecognized host failure, use the kernel default instead of pretending
    // a direct C++ function call reproduces host signal delivery.
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(number, &action, nullptr);
    syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), number);
}

void FaultScope::Handle(int number, siginfo_t* info, void* raw) noexcept {
    const auto saved_errno = errno;
    auto* context = platform::current_context;
    if (context) {
        const bool synchronous = platform::Synchronous(number, info->si_code);
        if (synchronous && platform::RecoverAtomicFault(
                                   number, reinterpret_cast<std::uintptr_t>(info->si_addr), raw)) {
            errno = saved_errno;
            return;
        }
        if (!synchronous && context->queue_signal &&
            context->queue_signal(context->signal_opaque, number, info, raw)) {
            errno = saved_errno;
            return;
        }
        const auto* native = context->native;
        if (synchronous && native && native->root_sp &&
            native->phase.load(std::memory_order_acquire) == SVM_NATIVE_PHASE_GUEST &&
            context->recover_native &&
            context->recover_native(*context,
                                    number,
                                    info->si_code,
                                    reinterpret_cast<std::uintptr_t>(info->si_addr),
                                    raw)) {
            errno = saved_errno;
            return;
        }
    }
    Chain(number, info, raw);
    errno = saved_errno;
}
}  // namespace swift::runtime::execution
