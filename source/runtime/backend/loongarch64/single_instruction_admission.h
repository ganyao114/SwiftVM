#pragma once

#include <cstddef>

namespace swift::runtime::ir { class Block; }

namespace swift::runtime::backend::loongarch64 {

// A positive inventory for a fresh private unit, including audited native and
// typed memory lowerings. This proves shape, storage, SSA and emitter capability;
// Runtime separately requires fresh decode and frontend admission of helper
// identity. This function never executes guest code or invokes a callback.
bool SingleInstructionAdmitted(const ir::Block& block, size_t uniform_bytes) noexcept;

} // namespace swift::runtime::backend::loongarch64
