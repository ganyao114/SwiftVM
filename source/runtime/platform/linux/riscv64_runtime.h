#pragma once

#include "runtime/backend/riscv64/compile.h"
#include "runtime/execution/host_backend.h"

namespace swift::runtime::platform {

// Owns the output and lifetime decisions reused by this VM's native backend.
class Riscv64Runtime {
public:
    explicit Riscv64Runtime(bool diagnostic) noexcept;
    execution::HostBackendOps Bind() noexcept;

private:
    backend::riscv64::CompileOptions options;
    backend::CompileResult output;
    backend::riscv64::Allocation allocation;
};

}  // namespace swift::runtime::platform
