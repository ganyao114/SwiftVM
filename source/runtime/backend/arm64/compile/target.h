#pragma once

#include "runtime/compile/rules/value_rules.h"
#include "runtime/compile/passes/loop_hoist.h"
#include "runtime/backend/register_mask.h"

namespace swift::runtime::ir {
class Block;
class HIRFunction;
}

namespace swift::runtime::backend::arm64 {

[[nodiscard]] bool SupportsIR(const ir::Block* block);
[[nodiscard]] bool SupportsIR(const ir::HIRFunction* function);
[[nodiscard]] bool IsAddSubImmediate(u64 value);
[[nodiscard]] bool IsLogicalImmediate(u64 value, u32 width);
[[nodiscard]] compile::BuilderCapabilities BuilderTarget();
void ConfigureGuestStatePolicy(compile::GuestStateLayout& layout);
[[nodiscard]] compile::LoopHoistTarget LoopHoistPolicy(const GPRSMask& reserved);

}  // namespace swift::runtime::backend::arm64
