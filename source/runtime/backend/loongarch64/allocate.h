#pragma once

#include <cstdint>
#include <vector>

#include "runtime/backend/code.h"
#include "runtime/ir/program.h"

namespace swift::runtime::backend::loongarch64 {

using backend::CompileError;

// One invariant home for the complete native unit. Scalar/FP bits use a
// callee-saved GPR (r23..r30); V128 uses two adjacent GPRs or an aligned 16-byte spill.
// No live SSA register is caller-saved, including across helper calls.
struct Home {
    std::uint32_t definition_position{}, last_use{}, stack_offset{};
    // Optional 16-byte producer payload: packed flags at +0 and an extra
    // scalar result (Div128 remainder / Cpuid upper half) at +8.
    std::uint32_t auxiliary_offset{UINT32_MAX};
    std::uint8_t reg{}, high_reg{};
    bool spilled{};
};

struct Allocation {
    CompileError error{CompileError::None};
    ir::NodeId node{};
    ir::BlockId block{};
    std::vector<Home> homes;
    std::vector<std::uint32_t> block_order;
    // Unit-local SP-relative storage, including auxiliary slots, rounded to
    // LP64D stack alignment. The native root owns its callee-save frame.
    std::uint32_t spill_bytes{};

    explicit operator bool() const noexcept { return error == CompileError::None; }
};

// Builder supplies closed block extents, valid terminals and SSA dominance.
// Phi/local lowering is not part of this writer's input contract.
Allocation AllocateBlock(const ir::Program&);
Allocation AllocateFunction(const ir::Program&);
void AllocateBlock(const ir::Program&, Allocation&);
void AllocateFunction(const ir::Program&, Allocation&);

}  // namespace swift::runtime::backend::loongarch64
