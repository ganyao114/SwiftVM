#include "vector_helper.h"

#include <cstdlib>

namespace swift::runtime::backend::arm64 {
#if defined(__aarch64__)
extern "C" void SvmArm64VectorHelper() noexcept;
#else
static void SvmArm64VectorHelper() { std::abort(); }  // Cross-host generation only.
#endif

std::uintptr_t VectorHelperBridge() noexcept {
    return reinterpret_cast<std::uintptr_t>(&SvmArm64VectorHelper);
}
}  // namespace swift::runtime::backend::arm64
