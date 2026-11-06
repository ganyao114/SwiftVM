#pragma once

#include "runtime/backend/arm64/compile.h"
#include "runtime/execution/host_backend.h"

namespace swift::runtime::platform {

// Concrete host binding. The VM borrows its operations; this object owns the
// reusable backend output/allocation and dies after the VM has stopped/closed.
class Arm64Runtime {
public:
    explicit Arm64Runtime(bool diagnostic) noexcept;
    execution::HostBackendOps Bind() noexcept;

private:
    execution::HostCapabilities capabilities;
    backend::arm64::CompileOptions options;
    backend::CompileResult output;
    backend::arm64::Allocation allocation;
};

}  // namespace swift::runtime::platform
