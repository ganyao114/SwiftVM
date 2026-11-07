#pragma once

#include "runtime/backend/arm64/runtime.h"

namespace swift::runtime::platform {
class Arm64Host {
public:
    explicit Arm64Host(bool diagnostic) noexcept;
    execution::HostBackendOps Bind() noexcept;

private:
    backend::arm64::Runtime runtime;
};
}  // namespace swift::runtime::platform
