#pragma once

#include "compile.h"
#include "allocate.h"
#include "runtime/execution/host_backend.h"

#include <span>

namespace swift::runtime::backend::riscv64 {

// Native runtime integration owns reusable compiler state. It is compiled
// with the execution bridge, separate from the OS-independent code writer.
class Runtime {
public:
    explicit Runtime(bool diagnostic) noexcept;
    execution::HostBackendOps Bind() noexcept;

private:
    CompileOptions options;
    backend::CompileResult output;
    Allocation allocation;
};

void RestoreGuestState(execution::FaultContext&, std::span<const std::byte> gprs) noexcept;

}  // namespace swift::runtime::backend::riscv64
