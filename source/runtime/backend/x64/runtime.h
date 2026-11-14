#pragma once

#include "allocate.h"
#include "compile.h"
#include "recovery.h"
#include "runtime/execution/host_backend.h"

namespace swift::runtime::backend::x64 {
class Runtime {
public:
    explicit Runtime(bool diagnostic) noexcept;
    execution::HostBackendOps Bind() noexcept;

private:
    CompileOptions options;
    CompileResult output;
    Allocation allocation;
    std::uint64_t features{};
    GuestISA guest_isa{GuestISA::X86_64};
    bool diagnostic{};
};

}  // namespace swift::runtime::backend::x64
