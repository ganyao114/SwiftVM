#pragma once

#include "recovery.h"

#include "allocate.h"
#include "compile.h"
#include "runtime/execution/host_backend.h"

#include <span>

namespace swift::runtime::backend::arm64 {

// Native runtime integration owns reusable compiler state. It is compiled
// with the execution bridge, separate from the OS-independent code writer.
class Runtime {
public:
    Runtime(bool diagnostic, execution::HostCapabilities) noexcept;
    execution::HostBackendOps Bind() noexcept;

private:
    execution::HostCapabilities capabilities;
    std::uint32_t vector_offset{160};
    CompileOptions options;
    backend::CompileResult output;
    Allocation allocation;
};

// Borrowed architectural register bytes, not a Linux ucontext. Every view
// remains owned by the platform's signal frame for this one recovery call.

}  // namespace swift::runtime::backend::arm64
