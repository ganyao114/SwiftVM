#pragma once

#include "runtime/backend/x64/runtime.h"

namespace swift::runtime::platform {
class X64Host {
public:
    explicit X64Host(bool diagnostic) noexcept;
    execution::HostBackendOps Bind() noexcept;

private:
    backend::x64::Runtime runtime;
};
}  // namespace swift::runtime::platform
