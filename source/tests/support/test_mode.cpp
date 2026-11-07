#include "test_mode.h"

namespace swift::tests {
namespace {
bool native{};
}

bool NativeMode() noexcept { return native; }

void SetNativeMode(bool value) noexcept { native = value; }

NativeISA HostISA() noexcept {
#if defined(__aarch64__) && defined(__linux__)
    return NativeISA::Arm64;
#elif defined(__riscv) && __riscv_xlen == 64 && defined(__linux__)
    return NativeISA::Riscv64;
#else
    return NativeISA::None;
#endif
}

bool CanExecute(NativeISA isa) noexcept {
    if (isa == NativeISA::Host) return HostISA() != NativeISA::None;
    if (isa == NativeISA::Arm64Android) {
#if defined(__ANDROID__) && defined(__aarch64__)
        return true;
#else
        return false;
#endif
    }
    return isa != NativeISA::None && HostISA() == isa;
}
}  // namespace swift::tests
