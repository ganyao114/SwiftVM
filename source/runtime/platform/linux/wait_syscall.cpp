#include <ucontext.h>
#include "wait_syscall.h"

namespace swift::runtime::platform {
bool CancelHostWait(void* raw) noexcept {
    auto& machine = *static_cast<ucontext_t*>(raw);
#if defined(__aarch64__)
    auto& pc = machine.uc_mcontext.pc;
#elif defined(__x86_64__)
    auto& pc = machine.uc_mcontext.gregs[REG_RIP];
#elif defined(__riscv) && __riscv_xlen == 64
    auto& pc = machine.uc_mcontext.__gregs[0];
#elif defined(__loongarch64)
    auto& pc = machine.uc_mcontext.__pc;
#else
    (void)machine;
    return false;
#endif
    const auto address = static_cast<std::uintptr_t>(pc);
    if (address < reinterpret_cast<std::uintptr_t>(SvmHostWaitBegin) ||
        address >= reinterpret_cast<std::uintptr_t>(SvmHostWaitComplete))
        return false;
    pc = reinterpret_cast<std::uintptr_t>(SvmHostWaitCancelled);
    return true;
}
}  // namespace swift::runtime::platform
