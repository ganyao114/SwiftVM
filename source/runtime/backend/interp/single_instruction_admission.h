#pragma once

#include <cstddef>

namespace swift::runtime::ir {
class Block;
}

namespace swift::runtime::backend::interp {

// Positive implementation inventory for the private single-instruction
// interpreter. This validates capabilities and raw SSA/storage safety, not
// guest instruction count or arbitrary host helper identity. The fresh unit
// and the frontend's helper admission remain separate caller obligations.
bool SingleInstructionAdmitted(const ir::Block& block, size_t uniform_bytes);

}  // namespace swift::runtime::backend::interp
