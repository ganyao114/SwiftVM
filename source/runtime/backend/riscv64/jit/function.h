#pragma once

#include "runtime/backend/function_plan.h"

namespace swift::runtime::backend::riscv64 {

// Owns the flattened, remapped IR through the executable allocation's QSBR
// lifetime. Original HIR blocks retain their canonical instructions.
using FunctionPlan = backend::FunctionPlan;

} // namespace swift::runtime::backend::riscv64
