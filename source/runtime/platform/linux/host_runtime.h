#pragma once

#if defined(__riscv) && __riscv_xlen == 64
#include "riscv64_runtime.h"
#else
#include "arm64_runtime.h"
#endif

namespace swift::runtime::platform {

#if defined(__riscv) && __riscv_xlen == 64
using HostRuntime = Riscv64Runtime;
#else
using HostRuntime = Arm64Runtime;
#endif

}  // namespace swift::runtime::platform
