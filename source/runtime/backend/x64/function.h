#pragma once

#include "runtime/backend/function_plan.h"

namespace swift::runtime::backend::x64 {
bool NeedsFunctionFrame(std::span<ir::HIRBlock* const> blocks);
} // namespace swift::runtime::backend::x64
