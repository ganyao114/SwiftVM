#pragma once
#include "compile.h"
#include "allocate.h"
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
};

bool RestoreGuestState(execution::FaultContext&,
                       const FaultSite&,
                       std::span<const std::byte>) noexcept;
}  // namespace swift::runtime::backend::x64
