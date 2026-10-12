#pragma once

#include "runtime/backend/trampolines.h"

namespace swift::runtime::backend::x64 {

class TrampolinesX64 final : public Trampolines {
public:
    explicit TrampolinesX64(const Config& config);
};

}  // namespace swift::runtime::backend::x64
