#pragma once

#if defined(__riscv) && __riscv_xlen == 64
#include "riscv64_host.h"
#else
#include "arm64_host.h"
#endif

namespace swift::runtime::platform {

#if defined(__riscv) && __riscv_xlen == 64
using HostRuntime = Riscv64Host;
#else
using HostRuntime = Arm64Host;
#endif

}  // namespace swift::runtime::platform
