#include "faultable_atomic.h"

#include <csignal>
#include <ucontext.h>

#if defined(__x86_64__)
#include <cpuid.h>
#elif defined(__riscv) && __riscv_xlen == 64
#include "runtime/backend/riscv64/atomic_helpers.h"
#elif defined(__loongarch64)
#include "runtime/backend/loongarch64/atomic_helpers.h"
#endif

#if defined(__aarch64__) || defined(__x86_64__)
#if defined(__aarch64__)
#define SVM_ATOMIC_HOST A64
#else
#define SVM_ATOMIC_HOST X64
#endif
#define SVM_JOIN_NAME_INNER(prefix, name) Svm##prefix##CompareExchange##name
#define SVM_JOIN_NAME(prefix, name) SVM_JOIN_NAME_INNER(prefix, name)
#define SVM_ATOMIC_NAME(name) SVM_JOIN_NAME(SVM_ATOMIC_HOST, name)
extern "C" std::uint64_t SVM_ATOMIC_NAME(64)(
        std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t*, std::uint64_t*) noexcept;
extern "C" std::uint64_t SVM_ATOMIC_NAME(128)(std::uint64_t,
                                              const swift::runtime::AtomicValue*,
                                              const swift::runtime::AtomicValue*,
                                              swift::runtime::AtomicValue*,
                                              std::uint64_t*) noexcept;
extern "C" const char SVM_ATOMIC_NAME(64Begin)[], SVM_ATOMIC_NAME(64End)[],
        SVM_ATOMIC_NAME(64Fault)[], SVM_ATOMIC_NAME(128Begin)[], SVM_ATOMIC_NAME(128End)[],
        SVM_ATOMIC_NAME(128Fault)[];
#endif

namespace swift::runtime::platform {
bool HasAtomic128() noexcept {
#if defined(__aarch64__)
    return true;  // Successful LDXP/STXP retains the baseline pair-CAS contract.
#elif defined(__x86_64__)
    static const bool supported = [] {
        unsigned a{}, b{}, c{}, d{};
        return __get_cpuid(1, &a, &b, &c, &d) && (c & bit_CMPXCHG16B);
    }();
    return supported;
#else
    return false;
#endif
}

bool HasUnalignedAtomic64() noexcept {
#if defined(__x86_64__)
    return true;  // Actual locked CMPXCHG, including split cache-line cycles.
#else
    return false;
#endif
}

AtomicStatus CompareExchange64(std::uint64_t address,
                               std::uint64_t expected,
                               std::uint64_t desired,
                               std::uint64_t& observed,
                               std::uint64_t& fault_address) noexcept {
#if defined(__aarch64__) || defined(__x86_64__)
    return static_cast<AtomicStatus>(
            SVM_ATOMIC_NAME(64)(address, expected, desired, &observed, &fault_address));
#elif defined(__riscv) && __riscv_xlen == 64
    return static_cast<AtomicStatus>(backend::riscv64::SvmRVCompareExchange64(
            address, expected, desired, &observed, &fault_address));
#elif defined(__loongarch64)
    return static_cast<AtomicStatus>(backend::loongarch64::SvmLACompareExchange64(
            address, expected, desired, &observed, &fault_address));
#else
    return AtomicStatus::Unsupported;
#endif
}

AtomicStatus CompareExchange128(std::uint64_t address,
                                const AtomicValue& expected,
                                const AtomicValue& desired,
                                AtomicValue& observed,
                                std::uint64_t& fault_address) noexcept {
#if defined(__aarch64__) || defined(__x86_64__)
    if (HasAtomic128())
        return static_cast<AtomicStatus>(
                SVM_ATOMIC_NAME(128)(address, &expected, &desired, &observed, &fault_address));
#endif
    return AtomicStatus::Unsupported;
}

bool RecoverAtomicFault(int signal, std::uint64_t address, void* raw) noexcept {
    if (signal != SIGSEGV && signal != SIGBUS) return false;
    auto& machine = *static_cast<ucontext_t*>(raw);
#if defined(__aarch64__)
    auto& pc = machine.uc_mcontext.pc;
    auto& fault_argument = machine.uc_mcontext.regs[5];
#elif defined(__x86_64__)
    auto& pc = machine.uc_mcontext.gregs[REG_RIP];
    auto& fault_argument = machine.uc_mcontext.gregs[REG_R9];
#elif defined(__riscv) && __riscv_xlen == 64
    auto& pc = machine.uc_mcontext.__gregs[0];
    auto& fault_argument = machine.uc_mcontext.__gregs[15];
#elif defined(__loongarch64)
    auto& pc = machine.uc_mcontext.__pc;
    auto& fault_argument = machine.uc_mcontext.__gregs[9];
#else
    return false;
#endif
    const auto recover = [&](const char* begin, const char* end, const char* fault) {
        const auto first = reinterpret_cast<std::uintptr_t>(begin);
        if (static_cast<std::uintptr_t>(pc) < first ||
            static_cast<std::uintptr_t>(pc) >= reinterpret_cast<std::uintptr_t>(end))
            return false;
        pc = reinterpret_cast<std::uintptr_t>(fault);
        fault_argument = address;
        return true;
    };
#if defined(__aarch64__) || defined(__x86_64__)
    return recover(SVM_ATOMIC_NAME(64Begin), SVM_ATOMIC_NAME(64End), SVM_ATOMIC_NAME(64Fault)) ||
           recover(SVM_ATOMIC_NAME(128Begin), SVM_ATOMIC_NAME(128End), SVM_ATOMIC_NAME(128Fault));
#elif defined(__riscv) && __riscv_xlen == 64
    return recover(backend::riscv64::SvmRVCompareExchange64Begin,
                   backend::riscv64::SvmRVCompareExchange64End,
                   backend::riscv64::SvmRVCompareExchange64Fault);
#elif defined(__loongarch64)
    return recover(backend::loongarch64::SvmLACompareExchange64Begin,
                   backend::loongarch64::SvmLACompareExchange64End,
                   backend::loongarch64::SvmLACompareExchange64Fault);
#endif
}
}  // namespace swift::runtime::platform

#if defined(SVM_ATOMIC_HOST)
#undef SVM_ATOMIC_NAME
#undef SVM_JOIN_NAME
#undef SVM_JOIN_NAME_INNER
#undef SVM_ATOMIC_HOST
#endif
