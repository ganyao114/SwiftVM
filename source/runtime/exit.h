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
};

constexpr std::uint64_t ExitValue(NativeExit value) noexcept {
    return static_cast<std::uint64_t>(value);
}

enum class AtomicAccess : std::uint64_t {
    Allowed = SVM_ATOMIC_ACCESS_ALLOWED,
    Fault = SVM_ATOMIC_ACCESS_FAULT,
    Unsupported = SVM_ATOMIC_ACCESS_UNSUPPORTED,
};

constexpr std::uint64_t AtomicAccessValue(AtomicAccess value) noexcept {
    return static_cast<std::uint64_t>(value);
}

}  // namespace swift::runtime
