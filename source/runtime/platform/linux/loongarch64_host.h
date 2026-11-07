#pragma once

#include "runtime/backend/loongarch64/runtime.h"

namespace swift::runtime::platform {
class Loongarch64Host {
public:
    explicit Loongarch64Host(bool diagnostic) noexcept;
    execution::HostBackendOps Bind() noexcept;

private:
    backend::loongarch64::Runtime runtime;
};
}  // namespace swift::runtime::platform
