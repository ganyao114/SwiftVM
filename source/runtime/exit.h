#pragma once

#include <cstdint>
#include "exit.inc"

namespace swift::runtime {

enum class NativeExit : std::uint64_t {
    Continue = SVM_NATIVE_EXIT_CONTINUE,
    Syscall = SVM_NATIVE_EXIT_SYSCALL,
    ResolveFailure = SVM_NATIVE_EXIT_RESOLVE_FAILURE,
    FPEnvironment = SVM_NATIVE_EXIT_FP_ENVIRONMENT,
    X87Fault = SVM_NATIVE_EXIT_X87_FAULT,
    StateService = SVM_NATIVE_EXIT_STATE_SERVICE,
    UnsupportedAtomic = SVM_NATIVE_EXIT_UNSUPPORTED_ATOMIC,
    DivideFault = SVM_NATIVE_EXIT_DIVIDE_FAULT,
    AlignmentFault = SVM_NATIVE_EXIT_ALIGNMENT_FAULT,
    MemoryFault = SVM_NATIVE_EXIT_MEMORY_FAULT,
    FloatingFault = SVM_NATIVE_EXIT_FLOATING_FAULT,
    GuestException = SVM_NATIVE_EXIT_GUEST_EXCEPTION,
    GuestEvents = SVM_NATIVE_EXIT_GUEST_EVENTS,
    SemanticException = SVM_NATIVE_EXIT_SEMANTIC_EXCEPTION,
};

enum class GuestExceptionKind : std::uint8_t {
    Memory,
    Alignment,
    Divide,
    IllegalInstruction,
    FloatingPoint,
    Breakpoint,
    AsyncSignal,
};

enum class NativePhase : std::uint64_t {
    Runtime = SVM_NATIVE_PHASE_RUNTIME,
    Entering = SVM_NATIVE_PHASE_ENTERING,
    Guest = SVM_NATIVE_PHASE_GUEST,
    Helper = SVM_NATIVE_PHASE_HELPER,
    HostWait = SVM_NATIVE_PHASE_HOST_WAIT,
    Leaving = SVM_NATIVE_PHASE_LEAVING,
};

constexpr std::uint64_t ExitValue(NativeExit value) noexcept {
    return static_cast<std::uint64_t>(value);
}

constexpr std::uint64_t SemanticExceptionExit(GuestExceptionKind kind,
                                              unsigned instruction_bytes = 0) noexcept {
    return ExitValue(NativeExit::SemanticException) | (static_cast<std::uint64_t>(kind) << 8) |
           (static_cast<std::uint64_t>(instruction_bytes) << 16);
}

}  // namespace swift::runtime
