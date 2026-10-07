#pragma once

#include "runtime/backend/trampolines.h"

namespace swift::runtime::backend::riscv64 {

class TrampolinesRiscv64 : public Trampolines {
public:
    explicit TrampolinesRiscv64(const Config& config);
};

}  // namespace swift::runtime::backend::riscv64
