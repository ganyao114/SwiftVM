#pragma once

#include "runtime/backend/context.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::backend::x64 {

// Reject memory forms before code publication: the shared helper's split
// Float80/environment stores do not provide fault-atomic writes on this host.
u64 ValidateX87RegisterOp(ir::Inst* inst, u64 uniform_size = UINT64_MAX);
u64 EvaluateX87Register(State* state, u64 context, u64 command) noexcept;

}  // namespace swift::runtime::backend::x64
