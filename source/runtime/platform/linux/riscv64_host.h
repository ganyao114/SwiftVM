#pragma once

#include "runtime/backend/riscv64/runtime.h"

namespace swift::runtime::platform {
class Riscv64Host {
public:
    explicit Riscv64Host(bool diagnostic) noexcept;
    execution::HostBackendOps Bind() noexcept;

private:
    backend::riscv64::Runtime runtime;
};
}  // namespace swift::runtime::platform
