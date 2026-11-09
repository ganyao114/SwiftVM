#include "abi_arm32.h"
#include "guest_signals.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "base/error.h"
#include "runtime/execution/vm.h"
#include "signals.h"
#include "wait_syscall.h"

namespace swift::runtime::platform {
namespace {
using u64 = std::uint64_t;
constexpr u64 Unblockable = (u64{1} << 8) | (u64{1} << 18);

constexpr u64 Bit(int number) noexcept { return u64{1} << (number - 1); }

// Queried only in ordinary context: libc can reserve a different prefix of
// Linux's RT numbers (Bionic and glibc do not expose the same SIGRTMIN).
bool HostRoutable(int number) noexcept {
    return number < 32 || (number >= SIGRTMIN && number <= SIGRTMAX);
}

sigset_t HostMask(u64 bits) noexcept {
    sigset_t mask;
    sigemptyset(&mask);
    for (int n = 1; n <= 64; ++n)
        if ((bits & Bit(n)) && HostRoutable(n)) sigaddset(&mask, n);
    return mask;
}

u64 MaskBits(const sigset_t& mask) noexcept {
    u64 bits{};
    for (int n = 1; n <= 64; ++n)
        if (sigismember(&mask, n) == 1) bits |= Bit(n);
    return bits;
}

struct IngressGuard {
    sigset_t saved{};
    u64& pressure;
    u64 base{};
    bool valid{};

    IngressGuard(u64 owned, u64& pressure) noexcept : pressure(pressure) {
        const auto set = HostMask(owned);
        valid = sigprocmask(SIG_BLOCK, &set, &saved) == 0;
        if (valid) base = MaskBits(saved) & ~pressure;
    }

    ~IngressGuard() {
        if (valid) {
            const auto restore = HostMask(base | pressure);
            sigprocmask(SIG_SETMASK, &restore, nullptr);
        }
    }
};

int SignalNumber(const GuestExceptionInfo& event, GuestISA isa) noexcept {
    if (event.origin == GuestExceptionOrigin::Async) return event.signal_number;
    switch (event.kind) {
        case GuestExceptionKind::Memory:
            return event.signal_number == 7 ? 7 : 11;
        case GuestExceptionKind::Alignment:
            return isa != GuestISA::X86_64 ? 7 : 11;
        case GuestExceptionKind::Divide:
        case GuestExceptionKind::FloatingPoint:
            return 8;
        case GuestExceptionKind::IllegalInstruction:
            return 4;
        case GuestExceptionKind::Breakpoint:
            return 5;
        case GuestExceptionKind::AsyncSignal:
            return event.signal_number;
    }
    return 0;
}

bool IgnoredDefault(int signal) noexcept {
    return signal == 17 || signal == 18 || signal == 23 || signal == 28;
}
}  // namespace

GuestSignals::GuestSignals(execution::VM& vm) noexcept : vm(vm) {
    owned = Bit(4) | Bit(5) | Bit(7) | Bit(8) | Bit(11) | Bit(SIGCONT) | Bit(WakeSignal);
    vm.thread.faults.queue_signal = HostIngress;
    vm.thread.faults.signal_opaque = this;
}

GuestSignals::~GuestSignals() {
    // No registered activation/handler may borrow the VM during destruction.
    if (pressure) {
        const auto set = HostMask(pressure);
        sigprocmask(SIG_UNBLOCK, &set, nullptr);
    }
}

void GuestSignals::Ready() noexcept {
    vm.thread.native.pending_events.store(pending & (~blocked | waited), std::memory_order_release);
    vm.RefreshDispatchView();
}

bool GuestSignals::OwnSignals(u64 signals) {
    for (int number = 1; number <= 64; ++number) {
        if (!(signals & ~owned & Bit(number))) continue;
        if (number >= 32 && realtime[number - 32].records.empty())
            realtime[number - 32].records.resize(4);
        // Guest RT numbers reserved by this host libc retain software FIFOs;
        // never replace debugger/thread handlers with guest signal ingress.
        if (HostRoutable(number) && !EnsureManagedSignal(number)) return false;
        owned |= Bit(number);
    }
    return true;
}

bool GuestSignals::Pending() const noexcept {
    return vm.thread.native.pending_events.load(std::memory_order_acquire) != 0;
}

void GuestSignals::InterruptSyscall(u64 number,
                                    const u64* arguments,
                                    bool restartable,
                                    bool restart_without_handler) noexcept {
    interrupted.resume_pc = vm.PC();
    interrupted.instruction_pc = vm.ISA() == GuestISA::Arm32
                                         ? vm.Arm32State().service_pc
                                         : vm.PC() - (vm.ISA() == GuestISA::X86_64 ? 2u : 4u);
    interrupted.number = number;
    std::copy_n(arguments, 6, interrupted.arguments);
    interrupted.active = true;
    interrupted.restartable = restartable;
    interrupted.restart_without_handler = restart_without_handler;
    interrupted.restart_ready = false;
}

void GuestSignals::RestoreInterruptedCall() noexcept {
    vm.PC() = interrupted.instruction_pc;
    if (vm.ISA() == GuestISA::X86_64)
        vm.State().gprs[x86::RAX] = interrupted.number;
    else if (vm.ISA() == GuestISA::Arm32) {
        vm.Arm32State().r[7] = interrupted.number;
        std::copy_n(interrupted.arguments, 6, vm.Arm32State().r);
    } else
        vm.ArmState().gprs[0] = interrupted.arguments[0];
}

void GuestSignals::CompleteInterruptedWithoutHandler() noexcept {
    if (!interrupted.active) return;
    if (interrupted.restart_without_handler) {
        RestoreInterruptedCall();
        interrupted.restart_ready = true;
    } else {
        interrupted.deadline_valid = false;
    }
    interrupted.active = false;
}

bool GuestSignals::ConsumeRestartDeadline(u64 number,
                                          const u64* arguments,
                                          timespec& deadline) noexcept {
    const bool reuse = interrupted.restart_ready && interrupted.deadline_valid &&
                       interrupted.instruction_pc ==
                               (vm.ISA() == GuestISA::Arm32
                                        ? vm.Arm32State().service_pc
                                        : vm.PC() - (vm.ISA() == GuestISA::X86_64 ? 2u : 4u)) &&
                       interrupted.number == number &&
                       std::equal(arguments, arguments + 6, interrupted.arguments);
    if (reuse) deadline = interrupted.deadline;
    interrupted.restart_ready = interrupted.deadline_valid = false;
    return reuse;
}

void GuestSignals::PreserveFutexDeadline(const timespec& deadline) noexcept {
    interrupted.deadline = deadline;
    interrupted.deadline_valid = true;
}

bool GuestSignals::MatchesRestartContext() const noexcept {
    if (vm.PC() != interrupted.instruction_pc) return false;
    if (vm.ISA() == GuestISA::Arm64)
        return vm.ArmState().gprs[8] == interrupted.number &&
               std::equal(std::begin(interrupted.arguments),
                          std::end(interrupted.arguments),
                          vm.ArmState().gprs);
    if (vm.ISA() == GuestISA::Arm32)
        return vm.Arm32State().r[7] == interrupted.number &&
               std::equal(std::begin(interrupted.arguments),
                          std::end(interrupted.arguments),
                          vm.Arm32State().r);
    const auto& state = vm.State();
    constexpr unsigned registers[]{x86::RDI, x86::RSI, x86::RDX, x86::R10, x86::R8, x86::R9};
    if (state.gprs[x86::RAX] != interrupted.number) return false;
    for (unsigned i = 0; i < 6; ++i)
        if (state.gprs[registers[i]] != interrupted.arguments[i]) return false;
    return true;
}

std::int64_t GuestSignals::PendingMask(u64 output, u64 size) {
    if (size != 8) return -EINVAL;
    IngressGuard guard{owned, pressure};
    if (!guard.valid) return -errno;
    const auto value = pending & blocked;
    return vm.GuestWrite(output, &value, 8) ? 0 : -EFAULT;
}

std::int64_t GuestSignals::Suspend(u64 input, u64 size) {
    if (size != 8) return -EINVAL;
    u64 temporary{};
    if (!vm.memory.CopyIn(&temporary, input, 8)) return -EFAULT;
    temporary &= ~Unblockable;
    {
        IngressGuard guard{owned | temporary, pressure};
        if (!guard.valid) return -errno;
        if (!OwnSignals(temporary)) return -errno;
        if (!restore_suspend_mask) suspend_mask = blocked;
        blocked = temporary;
        restore_suspend_mask = true;
        Ready();
    }
    const u64 arguments[6]{0, 0, 0, 0, 8, 0};
    while (!Pending()) {
        const auto result = WaitSyscall(SYS_ppoll, arguments);
        if (result < 0 && result != -EINTR && result != CancelledBeforeCall) {
            blocked = suspend_mask;
            restore_suspend_mask = false;
            Ready();
            return result;
        }
    }
    return -EINTR;
}

std::int64_t GuestSignals::WaitInfo(u64 input, u64 output, u64 timeout, u64 size, bool time64) {
    if (size != 8) return -EINVAL;
    u64 wanted{};
    if (!vm.memory.CopyIn(&wanted, input, 8)) return -EFAULT;
    wanted &= ~Unblockable;
    timespec duration{};
    if (timeout &&
        !(vm.ISA() == GuestISA::Arm32 ? abi32::ReadTimespec(vm.memory, timeout, duration, time64)
                                      : vm.memory.CopyIn(&duration, timeout, sizeof(duration))))
        return -EFAULT;
    if (timeout && (duration.tv_sec < 0 || duration.tv_nsec < 0 || duration.tv_nsec >= 1000000000))
        return -EINVAL;
    timespec start{};
    if (timeout && clock_gettime(CLOCK_MONOTONIC, &start)) return -errno;
    {
        IngressGuard guard{owned | wanted, pressure};
        if (!guard.valid) return -errno;
        if (!OwnSignals(wanted)) return -errno;
        // Keep the guest's blocked mask intact. A blocked SIG_IGN signal
        // generated during sigwait must still queue, as Linux real_blocked
        // does. The wait interest only makes wanted pending events wake us.
        waited = wanted;
        Ready();
    }
    std::int64_t result{};
    for (;;) {
        GuestExceptionInfo event;
        bool have{};
        {
            IngressGuard guard{owned, pressure};
            const auto wait_mask = blocked;
            blocked = ~wanted;
            have = TakePending(event);
            blocked = wait_mask;
            Ready();
        }
        if (have) {
            const auto compact = abi32::Encode(selected);
            const bool written =
                    !output || (vm.ISA() == GuestISA::Arm32
                                        ? vm.GuestWrite(output, &compact, sizeof(compact))
                                        : vm.GuestWrite(output, &selected, sizeof(selected)));
            result = written ? event.signal_number : -EFAULT;
            selected_number = 0;
            break;
        }
        if (Pending()) {
            result = -EINTR;
            break;
        }
        timespec remaining = duration;
        if (timeout) {
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now)) {
                result = -errno;
                break;
            }
            auto seconds = now.tv_sec - start.tv_sec;
            auto nanos = now.tv_nsec - start.tv_nsec;
            if (nanos < 0) {
                --seconds;
                nanos += 1000000000;
            }
            if (seconds > duration.tv_sec ||
                (seconds == duration.tv_sec && nanos >= duration.tv_nsec)) {
                result = -EAGAIN;
                break;
            }
            remaining.tv_sec -= seconds;
            remaining.tv_nsec -= nanos;
            if (remaining.tv_nsec < 0) {
                --remaining.tv_sec;
                remaining.tv_nsec += 1000000000;
            }
        }
        const u64 arguments[6]{0, 0, timeout ? reinterpret_cast<u64>(&remaining) : 0, 0, 8, 0};
        result = WaitSyscall(SYS_ppoll, arguments);
        if (!result && timeout) {
            result = -EAGAIN;
            break;
        }
        if (result < 0 && result != -EINTR && result != CancelledBeforeCall) break;
    }
    {
        IngressGuard guard{owned, pressure};
        waited = 0;
        Ready();
    }
    return result;
}

bool GuestSignals::Enqueue(int number, const GuestSignalInfo& info) noexcept {
    if (number < 1 || number > 64) return false;
    // Linux generates these side effects even when the signal is blocked or
    // ignored. A pending stop must not survive a later continue, or vice versa.
    if (number == SIGCONT)
        pending &= ~(Bit(SIGSTOP) | Bit(SIGTSTP) | Bit(SIGTTIN) | Bit(SIGTTOU));
    else if (number == SIGSTOP || number == SIGTSTP || number == SIGTTIN || number == SIGTTOU)
        pending &= ~Bit(SIGCONT);
    // A blocked signal must remain pending even if its current disposition
    // ignores it: sigwait can consume it and the action can change before
    // unblocking. Changing an action to SIG_IGN separately purges old pending.
    if (!(blocked & Bit(number)) &&
        (actions[number].handler == 1 || (!actions[number].handler && IgnoredDefault(number)))) {
        Ready();
        return true;
    }
    if (number < 32) {
        if (!(pending & Bit(number))) standard[number] = info;
    } else {
        auto& queue = realtime[number - 32];
        if (queue.count == queue.records.size()) return false;
        queue.records[(queue.first + queue.count) % queue.records.size()] = info;
        ++queue.count;
    }
    pending |= Bit(number);
    Ready();
    return true;
}

std::int64_t GuestSignals::Queue(int number, const GuestSignalInfo& info) {
    if (number < 1 || number > 64) return -EINVAL;
    IngressGuard guard{owned, pressure};
    if (!guard.valid) return -errno;
    if (number >= 32 && realtime[number - 32].records.empty())
        realtime[number - 32].records.resize(4);
    if (!Enqueue(number, info)) return -EAGAIN;
    if (number >= 32 && HostRoutable(number) && (owned & Bit(number)) &&
        realtime[number - 32].count == realtime[number - 32].records.size() &&
        !(guard.base & Bit(number)))
        pressure |= Bit(number);
    return 0;
}

bool GuestSignals::TakePending(GuestExceptionInfo& event) noexcept {
    IngressGuard guard{owned, pressure};
    if (!guard.valid) return false;
    const auto ready = pending & ~blocked;
    if (!ready) {
        // A cancelled request can leave an in-flight protected lookup. Its
        // event exit must also restore the view when there is nothing to pop.
        Ready();
        return false;
    }
    int number = 1;
    while (!(ready & Bit(number))) ++number;
    if (number < 32) {
        selected = standard[number];
        pending &= ~Bit(number);
    } else {
        auto& queue = realtime[number - 32];
        selected = queue.records[queue.first];
        queue.first = (queue.first + 1) % queue.records.size();
        if (!--queue.count) pending &= ~Bit(number);
        pressure &= ~Bit(number);
    }
    selected_number = number;
    event = {};
    event.isa = vm.ISA();
    event.kind = GuestExceptionKind::AsyncSignal;
    event.origin = GuestExceptionOrigin::Async;
    event.signal_number = number;
    event.signal_code = selected.code;
    event.instruction_pc = CodeAddress(vm.ISA(), vm.PC());
    Ready();
    return true;
}

GuestSignalStack GuestSignals::StackView() const noexcept {
    auto view = stack;
    if (!(view.flags & 2) && OnSignalStack(vm.SP(), stack)) view.flags |= 1;
    return view;
}

std::int64_t GuestSignals::Action(int number, u64 input, u64 output, u64 size) {
    if (number < 1 || number > 64 || size != 8 || (input && (Bit(number) & Unblockable)))
        return -EINVAL;
    // The new action and ownership must become visible before its newly
    // installed host ingress can run on this thread.
    IngressGuard guard{owned | (input ? Bit(number) : 0), pressure};
    if (!guard.valid) return -errno;
    auto action = actions[number];
    if (input) {
        if (vm.ISA() == GuestISA::Arm32) {
            abi32::Action compact;
            if (!vm.memory.CopyIn(&compact, input, sizeof(compact))) return -EFAULT;
            action = abi32::Decode(compact);
        } else if (!vm.memory.CopyIn(&action, input, sizeof(action)))
            return -EFAULT;
    }
    const u64 supported = 0xdc000007 | (vm.ISA() == GuestISA::Arm32 ? 0x02000000 : 0);
    if (input && (action.flags & ~supported)) return -EINVAL;
    const auto compact = abi32::Encode(actions[number]);
    if (output &&
        !(vm.ISA() == GuestISA::Arm32 ? vm.GuestWrite(output, &compact, sizeof(compact))
                                      : vm.GuestWrite(output, &actions[number], sizeof(action))))
        return -EFAULT;
    if (!input) return 0;
    if (!OwnSignals(Bit(number))) return -errno;
    action.mask &= ~Unblockable;
    actions[number] = action;
    if (action.handler == 1 || (!action.handler && IgnoredDefault(number))) {
        pending &= ~Bit(number);
        pressure &= ~Bit(number);
        if (number >= 32) realtime[number - 32].first = realtime[number - 32].count = 0;
    }
    Ready();
    return 0;
}

std::int64_t GuestSignals::Mask(int how, u64 input, u64 output, u64 size) {
    if (size != 8 || (input && (how < 0 || how > 2))) return -EINVAL;
    u64 next{};
    if (input && !vm.memory.CopyIn(&next, input, 8)) return -EFAULT;
    next &= ~Unblockable;
    const auto updated = how == 0 ? blocked | next : how == 1 ? blocked & ~next : next;
    IngressGuard guard{owned | (input ? updated : 0), pressure};
    if (!guard.valid) return -errno;
    if (output && !vm.GuestWrite(output, &blocked, 8)) return -EFAULT;
    if (input) {
        if (!OwnSignals(updated)) return -errno;
        blocked = updated;
    }
    Ready();
    return 0;
}

std::int64_t GuestSignals::AltStack(u64 input, u64 output) {
    IngressGuard guard{owned, pressure};
    if (!guard.valid) return -errno;
    const auto old = StackView();
    GuestSignalStack next;
    if (input) {
        if (vm.ISA() == GuestISA::Arm32) {
            abi32::Stack compact;
            if (!vm.memory.CopyIn(&compact, input, sizeof(compact))) return -EFAULT;
            next = abi32::Decode(compact);
        } else if (!vm.memory.CopyIn(&next, input, sizeof(next)))
            return -EFAULT;
    }
    const auto compact = abi32::Encode(old);
    if (output && !(vm.ISA() == GuestISA::Arm32 ? vm.GuestWrite(output, &compact, sizeof(compact))
                                                : vm.GuestWrite(output, &old, sizeof(old))))
        return -EFAULT;
    if (!input) return 0;
    if (OnSignalStack(vm.SP(), stack)) return -EPERM;
    if (next.flags & ~0x80000002u) return -EINVAL;
    if (!(next.flags & 2)) {
        const auto minimum = vm.ISA() == GuestISA::Arm64 ? 5120u : 2048u;
        if (next.size < minimum) return -ENOMEM;
        const auto limit = u64{1} << (vm.ISA() == GuestISA::Arm32 ? 32 : 48);
        if (next.address >= limit || next.size > limit - next.address) return -EINVAL;
    }
    next.padding = 0;
    stack = next;
    return 0;
}

u64 GuestSignals::Restorer(bool realtime) {
    const auto offset = vm.ISA() == GuestISA::Arm32 && !realtime ? 16u : 0u;
    if (restorer) return restorer + offset;
    const auto address = vm.memory.Map(
            0, vm.memory.PageSize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (address == UINT64_MAX) return 0;
    constexpr std::uint8_t x86_code[]{0xb8, 15, 0, 0, 0, 0x0f, 0x05, 0x0f, 0x0b};
    constexpr std::uint32_t arm_code[]{0xd2801168, 0xd4000001, 0xd4200000};
    constexpr std::uint32_t arm32_code[]{
            0xe3a070ad, 0xef000000, 0xe7f000f0, 0xe320f000, 0xe3a07077, 0xef000000, 0xe7f000f0};
    const bool written = vm.ISA() == GuestISA::X86_64
                                 ? vm.memory.CopyOut(address, x86_code, sizeof(x86_code))
                         : vm.ISA() == GuestISA::Arm32
                                 ? vm.memory.CopyOut(address, arm32_code, sizeof(arm32_code))
                                 : vm.memory.CopyOut(address, arm_code, sizeof(arm_code));
    if (!written || vm.memory.Protect(address, vm.memory.PageSize(), PROT_READ | PROT_EXEC))
        return 0;
    restorer = address;
    return restorer + offset;
}

void GuestSignals::Terminate(int number) noexcept {
    vm.thread.stop_reason = GuestStopReason::GuestSignalTermination;
    vm.thread.termination_signal = number;
    vm.thread.exit_code = 128 + number;
    vm.thread.exited = true;
}

SignalDelivery GuestSignals::Deliver(const GuestExceptionInfo& event) {
    const int number = SignalNumber(event, vm.ISA());
    if (number < 1 || number > 64) return SignalDelivery::Unhandled;
    GuestSignalAction action;
    GuestSignalInfo info{};
    SignalFrameContext frame_context;
    {
        IngressGuard guard{owned, pressure};
        if (!guard.valid) return SignalDelivery::Unhandled;
        action = actions[number];
        if (interrupted.restart_ready) {
            if (MatchesRestartContext()) {
                vm.PC() = interrupted.resume_pc;
                if (vm.ISA() == GuestISA::X86_64)
                    vm.State().gprs[x86::RAX] = static_cast<u64>(-EINTR);
                else if (vm.ISA() == GuestISA::Arm32)
                    vm.Arm32State().r[0] = std::uint32_t(-EINTR);
                else
                    vm.ArmState().gprs[0] = static_cast<u64>(-EINTR);
                interrupted.active = true;
            } else {
                interrupted.deadline_valid = false;
            }
            interrupted.restart_ready = false;
        }
        // Decide restart using this actual delivery, not a prior pending peek.
        if (interrupted.active && action.handler > 1) {
            if (interrupted.restartable && (action.flags & 0x10000000)) RestoreInterruptedCall();
            interrupted.active = false;
            interrupted.deadline_valid = false;
        }
        frame_context = {restore_suspend_mask ? suspend_mask : blocked, StackView()};
        if (event.origin == GuestExceptionOrigin::Async && selected_number == number) {
            info = selected;
            selected_number = 0;
        } else {
            info.number = number;
            info.code = event.signal_code;
        }
    }
    const bool synchronous = event.origin != GuestExceptionOrigin::Async;
    if (synchronous && ((frame_context.mask & Bit(number)) || action.handler == 1))
        action.handler = 0;
    if (action.handler == 1 || (!action.handler && IgnoredDefault(number)))
        return SignalDelivery::Resume;
    if (!action.handler && (number == 19 || number == 20 || number == 21 || number == 22)) {
        if (!vm.options.forward_process_signals) return SignalDelivery::Unhandled;
        if (!StopGuestProcess(number)) swift::ThrowSystemError("stop guest process", errno);
        return SignalDelivery::Resume;
    }
    if (!action.handler) {
        Terminate(number);
        return SignalDelivery::Terminate;
    }
    if (event.origin != GuestExceptionOrigin::Async) {
        if (!info.code)
            info.code = event.kind == GuestExceptionKind::Memory
                                ? (vm.memory.Contains(event.address, 1, 0) ? 2 : 1)
                                : 1;
        if (event.address_valid) std::memcpy(info.payload.data(), &event.address, 8);
    }
    const auto return_pc = (action.flags & 0x04000000) && action.restorer
                                   ? action.restorer
                                   : Restorer(vm.ISA() != GuestISA::Arm32 || (action.flags & 4));
    if (!return_pc) {
        Terminate(11);
        return SignalDelivery::Terminate;
    }
    EncodedSignalFrame frame;
    bool valid{};
    if (vm.ISA() == GuestISA::X86_64) {
        x86::GuestState handler;
        valid = EncodeX86SignalFrame(vm.State(),
                                     event,
                                     info,
                                     action,
                                     frame_context,
                                     return_pc,
                                     frame,
                                     handler) &&
                vm.GuestWrite(frame.address, frame.bytes.data(), frame.bytes.size());
        if (valid) {
            const auto previous = vm.State();
            vm.State() = handler;
            try {
                vm.RestoreGuestFP();
            } catch (...) {
                vm.State() = previous;
                vm.RestoreGuestFP();
                throw;
            }
        }
    } else if (vm.ISA() == GuestISA::Arm32) {
        arm32::GuestState handler;
        valid = EncodeArm32SignalFrame(vm.Arm32State(),
                                       event,
                                       info,
                                       action,
                                       frame_context,
                                       return_pc,
                                       frame,
                                       handler) &&
                vm.GuestWrite(frame.address, frame.bytes.data(), frame.bytes.size());
        if (valid) {
            vm.Arm32State() = handler;
            vm.RestoreGuestFP();
        }
    } else {
        arm64::GuestState handler;
        valid = EncodeArm64SignalFrame(vm.ArmState(),
                                       event,
                                       info,
                                       action,
                                       frame_context,
                                       return_pc,
                                       frame,
                                       handler) &&
                vm.GuestWrite(frame.address, frame.bytes.data(), frame.bytes.size());
        if (valid) {
            const auto previous = vm.ArmState();
            vm.ArmState() = handler;
            try {
                vm.RestoreGuestFP();
            } catch (...) {
                vm.ArmState() = previous;
                vm.RestoreGuestFP();
                throw;
            }
        }
    }
    if (!valid) {
        Terminate(11);
        return SignalDelivery::Terminate;
    }
    {
        IngressGuard guard{owned, pressure};
        if (!guard.valid) return SignalDelivery::Unhandled;
        blocked |= action.mask;
        if (!(action.flags & 0x40000000)) blocked |= Bit(number);
        blocked &= ~Unblockable;
        restore_suspend_mask = false;
        if (action.flags & 0x80000000) actions[number].handler = 0;
        if (stack.flags & 0x80000000) stack = {};
        Ready();
    }
    return SignalDelivery::Resume;
}

bool GuestSignals::ReturnFrame(bool realtime) {
    SignalFrameContext restored;
    const auto valid_stack = [&] {
        const auto limit = u64{1} << (vm.ISA() == GuestISA::Arm32 ? 32 : 48);
        return !(restored.stack.flags & ~0x80000003u) &&
               ((restored.stack.flags & 2) ||
                (restored.stack.address < limit &&
                 restored.stack.size <= limit - restored.stack.address));
    };
    if (vm.ISA() == GuestISA::X86_64) {
        x86::GuestState state;
        if (!DecodeX86SignalFrame(vm.memory, vm.State(), state, restored) || !valid_stack())
            return false;
        const auto previous = vm.State();
        vm.State() = state;
        try {
            vm.RestoreGuestFP();
        } catch (...) {
            vm.State() = previous;
            vm.RestoreGuestFP();
            throw;
        }
    } else if (vm.ISA() == GuestISA::Arm32) {
        arm32::GuestState state;
        if (!DecodeArm32SignalFrame(vm.memory, vm.Arm32State(), state, restored, realtime) ||
            !valid_stack())
            return false;
        vm.Arm32State() = state;
        vm.RestoreGuestFP();
    } else {
        arm64::GuestState state;
        if (!DecodeArm64SignalFrame(vm.memory, vm.ArmState(), state, restored) || !valid_stack())
            return false;
        const auto previous = vm.ArmState();
        vm.ArmState() = state;
        try {
            vm.RestoreGuestFP();
        } catch (...) {
            vm.ArmState() = previous;
            vm.RestoreGuestFP();
            throw;
        }
    }
    {
        IngressGuard guard{owned, pressure};
        if (!guard.valid) return false;
        blocked = restored.mask & ~Unblockable;
        restored.stack.flags &= ~1u;
        stack = restored.stack;
        Ready();
    }
    return true;
}

bool GuestSignals::HostIngress(void* opaque, int number, siginfo_t* info, void* raw) noexcept {
    return static_cast<GuestSignals*>(opaque)->Ingress(number, info, raw);
}

bool GuestSignals::Ingress(int number, siginfo_t* host, void* raw) noexcept {
    if (number < 1 || number > 64) return false;
    if (!(owned & Bit(number))) return false;
    const bool wake = number == WakeSignal && host && host->si_code == SI_QUEUE &&
                      host->si_value.sival_ptr == this;
    if (!wake) {
        GuestSignalInfo info{};
        info.number = number;
        if (host) {
            info.code = host->si_code;
            const auto pid = static_cast<std::int32_t>(host->si_pid);
            const auto uid = static_cast<std::uint32_t>(host->si_uid);
            std::memcpy(info.payload.data(), &pid, 4);
            std::memcpy(info.payload.data() + 4, &uid, 4);
            const auto value = reinterpret_cast<u64>(host->si_value.sival_ptr);
            std::memcpy(info.payload.data() + 8, &value, 8);
        }
        if (!Enqueue(number, info)) return false;
        if (number >= 32 && realtime[number - 32].count == realtime[number - 32].records.size()) {
            auto& resume_mask = static_cast<ucontext_t*>(raw)->uc_sigmask;
            if (sigismember(&resume_mask, number) != 1) {
                pressure |= Bit(number);
                sigaddset(&resume_mask, number);
            }
        }
    }
    if (wait_active && Pending()) CancelHostWait(raw);
    return true;
}

std::int64_t GuestSignals::WaitSyscall(u64 number, const u64* args) {
    IngressGuard guard{owned, pressure};
    if (!guard.valid) return -errno;
    if (Pending()) return CancelledBeforeCall;
    HostWaitRequest request;
    request.number = number;
    std::copy_n(args, 6, request.arguments);
    request.mask = (guard.base | pressure) & ~Bit(WakeSignal);
    wait_active = true;
    vm.thread.native.phase.store(SVM_NATIVE_PHASE_HOST_WAIT, std::memory_order_release);
    const auto result = SvmHostWaitSyscall(&request);
    const auto ingress = HostMask(owned);
    if (sigprocmask(SIG_BLOCK, &ingress, nullptr)) {
        wait_active = false;
        vm.thread.native.phase.store(SVM_NATIVE_PHASE_RUNTIME, std::memory_order_release);
        return -errno;
    }
    wait_active = false;
    vm.thread.native.phase.store(SVM_NATIVE_PHASE_RUNTIME, std::memory_order_release);
    return result;
}
}  // namespace swift::runtime::platform
