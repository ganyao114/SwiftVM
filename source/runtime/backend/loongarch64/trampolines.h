#pragma once
#include "runtime/backend/trampolines.h"
namespace swift::runtime::backend::loongarch64 {
class TrampolinesLoongArch64 : public Trampolines {
public:
    explicit TrampolinesLoongArch64(const Config& config);
};
}  // namespace swift::runtime::backend::loongarch64
