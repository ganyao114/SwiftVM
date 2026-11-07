#pragma once

#if defined(__riscv) && __riscv_xlen == 64
#include "riscv64_host.h"
#elif defined(__x86_64__)
#include "x64_host.h"
#elif defined(__loongarch64)
#include "loongarch64_host.h"
#else
#include "arm64_host.h"
#endif

namespace swift::runtime::platform {

#if defined(__riscv) && __riscv_xlen == 64
using HostRuntime = Riscv64Host;
#elif defined(__x86_64__)
using HostRuntime = X64Host;
#elif defined(__loongarch64)
using HostRuntime = Loongarch64Host;
#else
using HostRuntime = Arm64Host;
#endif

}  // namespace swift::runtime::platform
