#include "vm.h"

#include <csignal>

#include "base/error.h"

namespace swift::runtime::execution {

std::unique_ptr<GuestExceptionSnapshot> VM::CaptureException() const {
    if (thread.stop_reason != GuestStopReason::GuestSignalTermination &&
        thread.stop_reason != GuestStopReason::UnhandledGuestEvent)
        return {};
    auto snapshot = std::make_unique<GuestExceptionSnapshot>();
    snapshot->info = thread.faults.exception;
    if (ISA() == GuestISA::Arm64)
        snapshot->state = thread.native.arm_state;
    else if (ISA() == GuestISA::Arm32)
        snapshot->state = thread.native.arm32_state;
    else
        snapshot->state = thread.native.state;
    return snapshot;
}

void VM::DeliverException(const GuestExceptionInfo& event) {
    thread.faults.exception = event;
    thread.faults.exception_pending = false;
    ClearReservation();
    if (options.exception_handler) {
        const GuestContextView context =
                ISA() == GuestISA::Arm64   ? GuestContextView{&thread.native.arm_state}
                : ISA() == GuestISA::Arm32 ? GuestContextView{&thread.native.arm32_state}
                                           : GuestContextView{&thread.native.state};
        switch (options.exception_handler(options.exception_opaque, context, event)) {
            case GuestExceptionDisposition::Resume:
                RestoreGuestFP();
                return;
            case GuestExceptionDisposition::Stop:
                thread.stop_reason = GuestStopReason::UnhandledGuestEvent;
                thread.exited = true;
                thread.exit_code = 128 + event.signal_number;
                return;
            case GuestExceptionDisposition::GuestOS:
                break;
        }
    }
    switch (signals.Deliver(event)) {
        case platform::SignalDelivery::Resume:
        case platform::SignalDelivery::Terminate:
            return;
        case platform::SignalDelivery::Unhandled:
            thread.stop_reason = GuestStopReason::UnhandledGuestEvent;
            thread.exited = true;
            thread.exit_code = 128 + event.signal_number;
            return;
    }
}

void VM::DeliverPending() {
    while (!thread.exited) {
        GuestExceptionInfo event;
        if (!signals.TakePending(event)) {
            signals.CompleteInterruptedWithoutHandler();
            return;
        }
        DeliverException(event);
        if (!signals.HasInterruptedSyscall()) return;
    }
}

void VM::ServiceArmState() {
    if (ISA() == GuestISA::Arm32 ? HandleArm32StateService() : HandleArmStateService()) return;
    if (thread.bridge_failure.kind != ErrorKind::GuestFault)
        throw swift::RuntimeError(thread.bridge_failure);
    thread.bridge_failure.Clear();
    RaiseSemanticException(thread.faults.exception.kind == GuestExceptionKind::Alignment
                                   ? NativeExit::AlignmentFault
                                   : NativeExit::MemoryFault,
                           PC());
}

void VM::RaiseEncodedException(std::uint64_t encoded) {
    GuestExceptionInfo event;
    event.isa = ISA();
    event.origin = GuestExceptionOrigin::Semantic;
    event.kind = static_cast<GuestExceptionKind>((encoded >> 8) & 255);
    event.instruction_pc = CodeAddress(ISA(), PC());
    if (event.kind == GuestExceptionKind::Breakpoint) {
        if (ISA() == GuestISA::X86_64) event.exception_vector = 3;
        if (ISA() != GuestISA::Arm32) event.instruction_pc -= (encoded >> 16) & 255;
        event.signal_number = SIGTRAP;
        event.signal_code = ISA() == GuestISA::X86_64 ? SI_KERNEL : TRAP_BRKPT;
        if (ISA() != GuestISA::X86_64) {
            event.address = event.instruction_pc;
            event.address_valid = true;
        }
    } else if (event.kind == GuestExceptionKind::IllegalInstruction) {
        if (ISA() == GuestISA::X86_64) event.exception_vector = 6;
        event.signal_number = SIGILL;
        event.signal_code = ISA() == GuestISA::X86_64 ? ILL_ILLOPN : ILL_ILLOPC;
        event.address = event.instruction_pc;
        event.address_valid = true;
    } else
        swift::ThrowError("invalid encoded guest exception", ErrorKind::InvalidState);
    DeliverException(event);
}

void VM::RaiseSemanticException(NativeExit reason, std::uint64_t pc) {
    GuestExceptionInfo event;
    event.isa = ISA();
    event.origin = GuestExceptionOrigin::Semantic;
    event.instruction_pc = CodeAddress(ISA(), pc);
    switch (reason) {
        case NativeExit::DivideFault:
            if (ISA() == GuestISA::X86_64) event.exception_vector = 0;
            event.kind = GuestExceptionKind::Divide;
            event.signal_number = SIGFPE;
            event.signal_code = FPE_INTDIV;
            break;
        case NativeExit::FloatingFault:
        case NativeExit::X87Fault: {
            if (ISA() == GuestISA::X86_64)
                event.exception_vector = reason == NativeExit::X87Fault ? 16 : 19;
            event.kind = GuestExceptionKind::FloatingPoint;
            event.signal_number = SIGFPE;
            auto raised = NumericControl() & ~(NumericControl() >> 7) & 0x3f;
            if (reason == NativeExit::X87Fault) {
                if (ISA() != GuestISA::X86_64)
                    swift::ThrowError("x87 exception for a non-x86 guest", ErrorKind::InvalidState);
                const auto& x87 = thread.native.state.x87;
                raised = x87.status & ~x87.control & 0x3f;
                if (!raised) swift::ThrowError("unsupported x87 command", ErrorKind::Unsupported);
            }
            event.syndrome = raised;
            event.syndrome_valid = true;
            event.signal_code = raised & 1    ? FPE_FLTINV
                                : raised & 4  ? FPE_FLTDIV
                                : raised & 8  ? FPE_FLTOVF
                                : raised & 16 ? FPE_FLTUND
                                              : FPE_FLTRES;
            break;
        }
        case NativeExit::AlignmentFault:
            if (ISA() == GuestISA::X86_64) event.exception_vector = 13;
            event.kind = GuestExceptionKind::Alignment;
            event.signal_number = ISA() != GuestISA::X86_64 ? SIGBUS : SIGSEGV;
            event.signal_code = ISA() != GuestISA::X86_64 ? BUS_ADRALN : SI_KERNEL;
            break;
        case NativeExit::MemoryFault:
            if (ISA() == GuestISA::X86_64) event.exception_vector = 14;
            event.kind = GuestExceptionKind::Memory;
            event.signal_number = SIGSEGV;
            event.address = thread.faults.address.load(std::memory_order_relaxed);
            event.address_valid = true;
            event.access = thread.faults.exception.access;
            event.access_bytes = thread.faults.exception.access_bytes;
            break;
        default:
            swift::ThrowError("invalid semantic guest exception", ErrorKind::InvalidState);
    }
    PC() = pc;
    DeliverException(event);
}

}  // namespace swift::runtime::execution
